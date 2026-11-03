// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "app_model/bounded_string.hpp"
#include "app_model/device_data.hpp"
#include "config/config.hpp"
#include "network/wifi.hpp"
#include "secrets/secrets.hpp"
#include "storage/key_value.hpp"

#include "provisioning_flow.hpp"

// WHAT THE RADIO MUST BE PUT BACK TO WHEN A PROVISIONING CANDIDATE IS ABANDONED — Architecture
// Spec §13.3 step 7, "retain or restore previous valid configuration on failure".
//
// INTERNAL HEADER of the app_core component. Pure — no ESP-IDF, no driver, no clock, no global — so
// every rule below is host-tested against the same code the firmware runs. app_core.cpp is not
// host-compiled, and this is the half of restoration that can be.
//
// ⛔ THE GAP THIS CLOSES. §13.3's staged pattern already obtains the "retain" half by construction:
// stage_pair() writes both candidates to the inactive slots and moves no pointer, so a transaction
// that fails leaves the previous pair active in FLASH. What it never did was put the RADIO back.
// begin_transaction() hands the candidate's credential to the driver before anything is proved, so
// a candidate that fails, or a session that is abandoned before it commits, leaves the station
// joined to a network the device has not persisted and cannot explain — and on the next boot it
// joins the previous one instead, which is a device that changes networks when it is power-cycled.
//
// ⬅️ AND THE RESTORE TARGET IS ALREADY IN FLASH, WHICH IS WHY NOTHING NEW IS STORED HERE. Because
// no activation happened, the ACTIVE generations still name the previous pair — the same numbers
// stage_pair() copies into StagedPair::previous_* and activate_pair() writes into the recovery
// record. So the exact pair is READ, never reconstructed: the previous design's `active + 1`
// arithmetic is the mistake candidate.hpp exists to record, and no arithmetic appears here either.
//
// ⛔ THREE ANSWERS, AND THE THIRD IS WHY THIS IS NOT A BOOL. "Rejoin the previous pair", "there was
// never a previous pair", and "a previous pair may exist and could not be established" are three
// different facts about the device, and only the third is a failure. Collapsing the second into the
// third would report a restore failure on every first-time device; collapsing the third into the
// second would take the station down and call it success while the store was telling us nothing.

namespace prd::app_core::wifi_restore {

enum class Action : std::uint8_t {
    // A whole previous pair is named at exact generations. Rejoin it, which is an ordinary station
    // submission — invariants 6 and 7 retire the abandoned candidate on the way.
    Restore,

    // There is no previous pair, and there is nothing at risk. PM ruling 1 of checkpoint 3e: the
    // station must not be left joined to the unproven candidate, so it is withdrawn to Disabled.
    // ⛔ THIS IS A SUCCESS, NOT A FAILURE. It is the first-provisioning case.
    Withdraw,

    // ⛔ THE STORE WOULD NOT GIVE A TRUSTWORTHY ANSWER, so no restore target can be established: a
    // failed read, a record that does not validate, one that needs a migration this firmware cannot
    // perform, or an active record damaged enough that the previous known-good generation had to be
    // used in its place.
    //
    // ⛔ WHEN THIS IS THE ANSWER BEFORE THE RADIO IS DISTURBED, THE CANDIDATE IS REFUSED OUTRIGHT and
    // esp_wifi is never touched — PM ruling, 2026-08-27. Nothing about the device changes, which is
    // why the outcome is Failure::WifiRollbackUnavailable and NOT WifiRestoreFailed: no restoration
    // was attempted, so no statement about the user's network is available to make.
    Unavailable,
};

[[nodiscard]] std::string_view log_label(Action action);

// The exact identity of the pair to restore. Every field is a generation or a profile some record
// actually carries; nothing here is derived.
struct Target {
    Action action{Action::Withdraw};

    // The EXACT generations, meaningful only for Restore. kNoGeneration otherwise.
    std::uint32_t configuration_generation{config::kNoGeneration};
    std::uint32_t secret_generation{secrets::kNoGeneration};

    // The profile id both records name. 0 unless Restore.
    std::uint32_t profile{0};
};

// Reads what is active and decides which of the three answers applies.
//
// ⛔ THE WHOLE-PAIR PREDICATE IS stage_pair()'s, DELIBERATELY THE SAME ONE. Both halves loaded
// cleanly, a non-zero Wi-Fi profile id, and both records naming it. Two different notions of "the
// previous pair" in one firmware is how the recovery record and the restore would come to disagree
// about which pair that is, and this project has already paid for one such divergence.
//
// It writes nothing, activates nothing and erases nothing.
[[nodiscard]] Target target_for(storage::KeyValue& store);

// The station credential of a Restore target, loaded from the EXACT generations it names.
//
// ⛔ IT CARRIES A PASSPHRASE, SO IT HAS A LIFETIME AND A wipe(). §13.5 forbids a credential in a
// log, in AppState and in a crash dump where avoidable; the caller hands this straight to the
// station and wipes it on every path out, success and refusal alike.
//
// ⬅️ AND IT CARRIES ONLY THE TWO FIELDS THE RADIO NEEDS. The stored record also holds a printer API
// key and password; a restore has no use for them, so they never leave the store on this path — the
// loaded record is wiped inside credential_for() before it returns.
struct Credential {
    // False when the exact generations did not yield a whole pair after all: a record that has since
    // become unreadable, one that no longer names the target profile, or an empty network name. The
    // strings are then empty, and the caller reports the restore as failed.
    bool loaded{false};

    app_model::BoundedString<32> ssid;
    app_model::BoundedString<64> password;
};

// Loads BOTH halves at the exact generations `target` names and requires both to carry
// `target.profile`.
//
// ⬅️ WHY THE CONFIGURATION HALF IS READ WHEN ONLY THE CREDENTIAL IS USED. It is what makes the
// answer a PAIR rather than a credential that happens to exist: the pairing handle lives in both
// records, and a check that read one of them would accept a secret left behind by a profile whose
// configuration is long gone. Loading by generation rather than by pointer is what makes it exact.
//
// Returns an unloaded Credential for any action other than Restore.
[[nodiscard]] Credential credential_for(storage::KeyValue& store, const Target& target);

// Overwrites a Credential in place through a volatile pointer, so the compiler cannot elide the
// store. Byte-for-byte the mechanism network::radio::wipe(), provisioning::scrub::bytes() and
// app_core.cpp's own wipe_bytes() use.
//
// ⬅️ WHY THIS COMPONENT'S FOURTH COPY OF A THREE-LINE LOOP IS THE RIGHT SHAPE HERE. app_core.cpp
// already has wipe_bytes(), but it lives in that file's anonymous namespace and that file is the
// component's ESP-IDF adapter translation unit, which the host suite cannot compile. A credential
// wipe that can only be reviewed and never executed by a test is exactly the class of cleanup this
// project has learned not to trust — BoundedString::clear() writes one terminator and leaves the
// rest of the buffer intact, which is why clearing is not wiping.
void wipe(Credential& credential);

// Whether the previous pair `stage_pair()` captured is the SAME pair this transaction retained
// before it disturbed the radio.
//
// ⛔ WHY THIS IS CHECKED RATHER THAN ASSUMED. Both numbers come from the same two activation
// pointers, so they agree whenever both reads succeed — and that is exactly why a disagreement is
// worth catching: it means the store moved underneath a live transaction, and NEITHER value can then
// be trusted as a restore target. An `assert` would not do: assertions are compiled out of the
// release profile, and this must hold in the profile a user runs.
//
// A `Withdraw` target agrees with two kNoGeneration captures — that is what "there was no previous
// pair" looks like from both sides. An `Unavailable` target agrees with nothing: it was never
// established, so there is nothing for a capture to match.
[[nodiscard]] bool agrees(const Target& target, std::uint32_t staged_previous_configuration,
                          std::uint32_t staged_previous_secret);

// --- Routing one station update to its one consumer -------------------------
//
// ⛔ THIS EXISTS BECAUSE THE HAND-WRITTEN VERSION OF IT WAS THE DEFECT. app_core.cpp discarded every
// station evidence record whenever no CANDIDATE transaction was running, which silently included
// every record a restoration was waiting for — so an accepted restore that later failed reached
// nobody. The condition was three tokens long, lived in a translation unit the host suite cannot
// compile, and no negative control could reach it. It is a POLICY, so it lives here where mutating
// it fails a test.

enum class Route : std::uint8_t {
    // Nothing to route, or nobody is listening. The slot is cleared — which is also R3's original
    // purpose: an answer must not sit in it until some later transaction picks it up.
    Discard,

    // The candidate validation machine owns it. It applies §8.1's identity check itself, so evidence
    // charged to another tag advances its time and decides nothing.
    CandidateValidation,

    // The restore supervisor owns it, on the same terms.
    RestoreSupervision,
};

[[nodiscard]] std::string_view log_label(Route route);

struct Routing {
    // Whether this update carried a candidate-evidence record at all.
    bool evidence{false};

    // Whether a provisioning transaction is running. Its verdict machine is the newest instruction
    // and outranks a restoration, which is why a user submission disarms the supervisor rather than
    // racing it.
    bool transaction_running{false};

    bool restore_armed{false};
};

// ⛔ THE ONE DECISION, AND THE PRECEDENCE IS THE WHOLE OF IT. A transaction outranks a restoration
// because a submission is the newer instruction; the two are mutually exclusive in production
// because begin_transaction() disarms the supervisor before it submits, and a host test pins that
// the router still answers deterministically if they ever overlap.
[[nodiscard]] Route route_for(const Routing& routing);

// --- The restore supervisor -------------------------------------------------
//
// ⛔ WHAT IT IS FOR. `network::station::submit()` and `withdraw()` both report ACCEPTANCE of an
// asynchronous intent. Retirement, association, DHCP, §11.3's ladder and §16.2's give-up all happen
// afterwards, and §13.3 step 7 says "retain or RESTORE previous valid configuration on failure" —
// a completed act. §8.2 says the same thing about the mechanism: "effects are executed by
// `app_core`, then completion or failure is returned as another event". This is what holds the
// restore between the two.
//
// ⛔ AND IT HAS NO CLOCK, NO TIMER, NO DEADLINE AND NO RETRY. Every path is already bounded by the
// station's own accepted baselines — the completion deadline, the no-lease interval, §11.3's ladder
// and §16.2's give-up — so waiting for the station's OWN terminal answer needs no second bound and
// introduces no new number requiring acceptance. Nothing here reads or compares a time, which is the
// R11 class of defect made unrepresentable rather than guarded against.
//
// PURE, AND HOST-TESTED. Two levels and one record go in; a typed result comes out.

class Supervisor {
  public:
    enum class Phase : std::uint8_t {
        Idle,

        // A previous-pair submission was accepted. Waiting for `Leased` (rejoined) or `Unreachable`
        // (the network did not come back) charged to THIS tag.
        Restoring,

        // A withdrawal was accepted. Waiting for the station to actually settle to Disabled — PM
        // ruling, 2026-08-27: acceptance is not completion.
        Withdrawing,
    };

    enum class Result : std::uint8_t {
        // Nothing has settled. Also what an unarmed supervisor answers.
        Pending,

        // The previous pair is rejoined and holds a lease.
        Rejoined,

        // The station settled to Disabled and there was no previous pair to go back to. PM ruling 1's
        // prescribed SUCCESS, not a failure.
        Withdrawn,

        // ⛔ A RESTORE WAS TRIED AND THE NETWORK DID NOT COME BACK. The one thing that maps to
        // Failure::WifiRestoreFailed.
        RestoreFailed,

        // The station settled to Disabled after a rollback target that could not be established or
        // could not be loaded. Maps to Failure::WifiRollbackUnavailable.
        RollbackUnavailable,

        // ⛔ ENDED WITHOUT A RESTORE VERDICT, AND DELIBERATELY SILENT. A driver stall, lost evidence,
        // an exclusive operation or a newer user submission. The first two are already the typed
        // WifiUnavailable Device Fault and PM ruling 3 keeps this vocabulary out of them; the last two
        // are instructions that outrank the restoration.
        Abandoned,
    };

    // A previous-pair submission was accepted for `tag`.
    void arm_restore(network::station::Tag tag);

    // A withdrawal was accepted. `carry` is the failure to report once the station settles: None for
    // the no-previous-pair case, WifiRollbackUnavailable when the target could not be established or
    // loaded and the unexplained candidate is being taken down instead.
    //
    // ⛔ `already_reported` IS WHAT STOPS ONE FAILURE BEING PUBLISHED TWICE. On the staging-disagreement
    // path the candidate's own settlement has already told the browser this exact typed failure — it
    // had to, because that settlement is what moves the page off Validating — so the terminal must
    // RECORD the outcome without publishing it again. Every other path reaches the terminal with this
    // false, and the terminal is then the one and only publication.
    void arm_withdrawal(provisioning_flow::Failure carry, bool already_reported);

    // Ends the supervision with no restore verdict. Used for the exclusive-operation barrier and for
    // a user submission that supersedes an in-flight restoration.
    [[nodiscard]] Result abandon();

    // One step. `phase` and `fault` are the CURRENT published network levels, not edges: a station
    // that was already Disabled when a withdrawal was accepted publishes no change, so a supervisor
    // waiting for an edge would wait for ever. `evidence`/`tag`/`outcome` are the routed record.
    //
    // ⛔ IT ASKS THE STATION NOTHING. Every input is either a fact this class was given when it armed
    // or a fact already crossing the network seam each tick, so no new query is added to a component
    // that must not learn about stored pairs.
    [[nodiscard]] Result observe(app_model::NetworkPhase phase, app_model::NetworkFault fault,
                                 bool evidence, network::station::Tag tag,
                                 network::station::Outcome outcome);

    [[nodiscard]] bool armed() const {
        return phase_ != Phase::Idle;
    }

    [[nodiscard]] Phase phase() const {
        return phase_;
    }

    // The identity this restoration is judged by. kNoTag while withdrawing — a withdrawal produces no
    // candidate evidence and is judged by the published phase instead.
    [[nodiscard]] network::station::Tag tag() const {
        return tag_;
    }

    // Whether the failure this cleanup carries has already been published to the browser.
    //
    // ⬅️ IT DELIBERATELY SURVIVES THE SETTLE, unlike `carry_`. conclude_restore() reads it in the same
    // breath as the terminal result it was handed, so clearing it inside settle() would make it
    // unreadable at exactly the one moment it is needed. Every arm_*() overwrites it, so a stale
    // value cannot outlive the cleanup it belonged to.
    [[nodiscard]] bool already_reported() const {
        return already_reported_;
    }

  private:
    [[nodiscard]] Result settle(Result result);

    Phase phase_{Phase::Idle};
    network::station::Tag tag_{network::station::kNoTag};
    provisioning_flow::Failure carry_{provisioning_flow::Failure::None};
    bool already_reported_{false};
};

[[nodiscard]] std::string_view log_label(Supervisor::Result result);

// --- What a terminal result may be told to whom ------------------------------

enum class Publication : std::uint8_t {
    // Recorded on the device only. Either there is nothing to report, or the surface that could have
    // received it is gone and pretending otherwise would be the lie this exists to prevent.
    None,

    // Publish SetupState::Failed with `failure`'s label on the live privileged surface.
    SetupFailed,
};

struct Report {
    Publication publication{Publication::None};

    // The typed terminal outcome, recorded whether or not it is published. Failure::None for the two
    // successful results and for an abandonment.
    provisioning_flow::Failure failure{provisioning_flow::Failure::None};
};

// The three facts about the moment a terminal result arrives in.
//
// ⛔ A NAMED STRUCT BECAUSE THREE ADJACENT BOOLEANS ARE ONLY TOLD APART BY THEIR ORDER AT THE CALL
// SITE. Swapping any two would compile and would quietly publish into an expired surface, or suppress
// a first publication, or report a Device Fault twice. Same remedy backoff.hpp's Jitter,
// server_view.hpp's CsrfToken and escaping.hpp's FieldName were introduced for, and the same
// clang-tidy check — bugprone-easily-swappable-parameters — asks for it here.
struct Circumstances {
    // Whether the privileged setup access point is still up: the surface a browser can actually
    // reach. An expired surface is never pretended into existence.
    bool surface_live{false};

    // The station's own quarantine. It outranks every other answer here, because the typed
    // WifiUnavailable Device Fault is the sole owner of a driver that stopped answering.
    bool quarantined{false};

    // Whether this exact typed failure has already been published by the candidate's own settlement.
    // True only on the staging-disagreement path; the terminal then records without republishing.
    bool already_reported{false};
};

// ⛔ A DECISION, NOT AN ORDER OF STATEMENTS — the shape reentry::after_expiry() already establishes.
// It owns every publication ruling at once: what a terminal result is called, whether the browser may
// be told, that an expired surface is never pretended into existence, that a quarantined station
// yields no restore vocabulary at all, and that one failure is published exactly once.
//
// ⛔ THE TYPED FAILURE AND THE PUBLICATION ARE SEPARATE ANSWERS. `failure` is the terminal accounting
// trail and is set whenever one exists — including when the browser was told earlier, and including
// when there is no browser left to tell. `publication` is delivery, and only delivery depends on the
// surface and on whether this failure has already been delivered once.
[[nodiscard]] Report report_for(Supervisor::Result result, const Circumstances& circumstances);

} // namespace prd::app_core::wifi_restore
