// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstdint>
#include <string_view>

#include "app_model/bounded_string.hpp"
#include "app_model/state_layers.hpp"
#include "config/config.hpp"
#include "network/wifi.hpp"
#include "secrets/secrets.hpp"
#include "storage/key_value.hpp"

#include "candidate.hpp"

// The provisioning transaction — Architecture Spec §14.3, owner rulings D-1 to D-6 of
// 2026-08-16.
//
// PURE, AND HOST-TESTED. No ESP-IDF header, no driver, no clock, no global state. Every
// decision the flow makes lives here: when a submitted network counts as validated, what
// the two persisted records contain, and in which order they are committed. The adapter in
// app_core.cpp translates driver events into calls and obeys the answers; it decides
// nothing.
//
// THAT SPLIT IS WHY THE POWER-LOSS TABLE IS TESTABLE. stage_pair() and activate_pair() take a
// storage::KeyValue, and the host suite's FakeKeyValue can fail any single write on demand — so "the
// device lost power between the two activations" is a test rather than a paragraph.

namespace prd::app_core::provisioning_flow {

// --- What the browser submitted --------------------------------------------
//
// ⛔ CARRIES CREDENTIALS. Nothing here reaches AppState, Presentation, a log or a GET
// response (§13.5, §24.3). It exists for the length of one transaction and is wiped.
// Which step of the browser flow produced this submission — owner ruling 1, 2026-08-16.
//
// The flow's own enumeration rather than the server's, so this module stays independent of
// the HTTP component: `provisioning` maps its wire value onto this at the boundary, which is
// the same direction every other translation in app_core runs.
enum class Stage : std::uint8_t {
    // SSID and password. Joined and validated for real; nothing persisted.
    Wifi,

    // Address, port and API key. Runs the persistence transaction.
    Printer,
};

[[nodiscard]] std::string_view log_label(Stage stage);

struct Submission {
    Stage stage{Stage::Wifi};

    app_model::BoundedString<32> wifi_ssid;
    app_model::BoundedString<64> wifi_password;
    app_model::BoundedString<64> printer_host;
    app_model::BoundedString<64> printer_api_key;
    std::uint16_t printer_port{0};
};

// --- Failure, in the only shape that may be shown --------------------------
//
// NON-SECRET AND ACTIONABLE, per owner ruling D-6. Each value names something the user can
// act on and echoes nothing they submitted.
enum class Failure : std::uint8_t {
    None,

    // The station reported Unavailable: the access point rejected the credentials, or it
    // could not be associated with at all. M3.4 reaches this after its rejected-credential
    // limit, which is roughly six seconds.
    WifiUnreachable,

    // The overall attempt ran out of time.
    //
    // ⬅️ A DHCP FAILURE STILL ARRIVES HERE, and that remains a property of the station rather
    // than a gap in this file: an associated station with no lease is honestly "still trying",
    // and checkpoint 3c bounds how long it may stay that way rather than inventing a new
    // user-facing distinction for it.
    WifiTimedOut,

    // ⛔ THE STATION DRIVER DID NOT ANSWER — the owner's DriverStalled ruling. A driver call was
    // accepted and its documented terminal completion never arrived within the bound, or a
    // synchronous call was refused in a state the pinned SDK documents it to succeed in. The
    // station is quarantined: it issues no further driver operation, the setup access point is
    // preserved, nothing was persisted, and recovery requires an explicit user restart. It is
    // deliberately NOT WifiUnreachable, which would send the user to retype a password that was
    // never tested.
    WifiDriverStalled,

    // ⛔ THE EVIDENCE FOR THIS TRANSACTION IS INCOMPLETE. The station's event channel refused a
    // record, so a verdict about this candidate cannot be honest — not Succeeded (§14.3 step 9)
    // and not CredentialsRejected, which would blame the user's password for our lost record.
    // Distinct from WifiDriverStalled because the cause and the diagnostic differ, even though
    // both quarantine the station.
    WifiEvidenceLost,

    // §13.3 step 1 refused the record. Nothing was written.
    StorageRejected,

    // The candidate could not be written or did not read back. THE PREVIOUS ACTIVE RECORD
    // IS UNTOUCHED — the candidate always goes to the inactive slot.
    StorageCandidateFailed,

    // The candidate was written and verified but the active-generation pointer did not
    // move. The device keeps running the previous record.
    StorageCommitFailed,

    // --- Printer validation, Milestone 4 (§14.3 steps 5-8, §13.3 step 5) ---------------
    //
    // The candidate is committed but was NOT marked active. Each of these names something
    // the user can act on, per owner ruling D-6, and echoes nothing they submitted.

    // Nothing answered at the address: wrong address, printer switched off, or a network
    // that will not carry the request.
    PrinterUnreachable,

    // The printer answered and refused the key. The one failure the user can fix by
    // retyping something.
    PrinterUnauthorized,

    // Something answered but does not speak the API this build needs — no such endpoint,
    // or a request it will not accept. A different device on that address, or firmware too
    // old.
    PrinterUnsupported,

    // The endpoint answered and authenticated, but the response could not be trusted: not
    // decodable, no required printer state, or larger than the bound.
    PrinterUnusableResponse,

    // Validation succeeded and the promotion write did not. THE CANDIDATE STAYS PENDING and
    // the previous active record is untouched, so the device is exactly where it was.
    PromotionFailed,

    // --- Restoring the radio after an abandoned candidate (checkpoint 3e) --------------

    // ⛔ AN ACCEPTED PREVIOUS-PAIR RESTORE ENDED IN ORDINARY station::Outcome::Unreachable. That is
    // the whole of it, and every word of the broader description this replaces was wrong.
    //
    // The sequence: a candidate failed or was abandoned, so §13.3 step 7's restoration submitted the
    // exact previously active pair; the station ACCEPTED that intent; §16.2 then gave up on it inside
    // the station. So the previous network is not coming back on its own, and the user's own network
    // is the subject of the statement — which is why this one, unlike its sibling, is a claim about
    // connectivity at all.
    //
    // ⛔ THE STATION IS Unavailable HERE, NOT Disabled, AND NOTHING WITHDRAWS IT. Outcome::Unreachable
    // is produced precisely when the station's published phase is already NetworkPhase::Unavailable
    // (see station_intent.cpp's disassociated()), which is §16.1's truthful account of a network that
    // cannot be reached. An earlier version of this comment claimed the station was "withdrawn to
    // Disabled either way"; it never was, and adding a withdrawal to make that sentence true would
    // publish Disabled about a radio that is genuinely still trying nothing of the sort.
    //
    // ⛔ WHAT DOES *NOT* REACH HERE, AND WHERE EACH ONE GOES INSTEAD:
    //
    //   • the acceptance of a submission — that is Supervisor::Result::Pending, and treating it as a
    //     result is the defect the supervisor exists to make unrepresentable;
    //   • an untrustworthy store, an exact record that will not load, or a synchronous refusal of the
    //     restore submission — WifiRollbackUnavailable, because no restoration was attempted and
    //     there is therefore nothing true to say about the network;
    //   • a driver stall, lost evidence or an unresponsive driver — the existing
    //     Fault::DriverUnresponsive → NetworkFault::DriverUnresponsive →
    //     DeviceFaultReason::WifiUnavailable Device Fault, which is the SOLE owner of that condition.
    //     When it wins, this outcome is deliberately not recorded at all (PM ruling 3).
    //
    // ⛔ IT IS ORDINARY CONNECTIVITY LOSS AND NEVER A DEVICE FAULT. A previous network that has gone
    // away is §16.1's Wi-Fi class, not its device class, and UIAD 001 §10 forbids Device Fault from
    // representing ordinary connectivity loss.
    //
    // ⛔ AND IT IS NOT PRODUCED BY WifiValidation. No candidate transaction can end this way — a
    // restoration runs only after one has already ended — so it never reaches a verdict.
    //
    // ⛔ APPENDED RATHER THAN GROUPED WITH THE OTHER Wifi* MEMBERS, DELIBERATELY. provisioning_tick()
    // writes the enumerator's NUMERIC value into a timeline record, and the checkpoint-3c evidence
    // archive contains such records; inserting a member above these would silently change what
    // every archived `validation-failed failure=<n>` line means.
    WifiRestoreFailed,

    // ⛔ THE ROLLBACK TARGET COULD NOT BE ESTABLISHED, SO NO RESTORATION WAS ATTEMPTED. Three ways in,
    // and they share one truthful sentence — "the display could not verify its saved network":
    //
    //   • BEFORE the radio was disturbed: the store gave no trustworthy answer about which pair was
    //     active, so the candidate is refused outright and esp_wifi is never touched. Nothing about
    //     the device changed, which is why this is not a connectivity failure.
    //   • At staging: the previous pair stage_pair() captured disagrees with the target this
    //     transaction retained before it disturbed the radio, so neither can be trusted.
    //   • AFTER the radio was disturbed: the exact retained generations no longer load, or the driver
    //     would not take the credential. The unexplained candidate is withdrawn to Disabled and this
    //     is reported instead of a connectivity verdict nobody measured.
    //
    // ⛔ IT IS DELIBERATELY DISTINCT FROM WifiRestoreFailed. That one means a restore was tried and
    // the network did not come back; this one means the device could not work out what to try. Only
    // the first is a statement about the user's network.
    //
    // ⛔ APPENDED, LIKE ITS PREDECESSOR, BECAUSE THE NUMERIC VALUES ARE EVIDENCE. provisioning_tick()
    // writes this enumerator's value into a timeline record and the checkpoint-3c archive contains
    // such records; a host test pins every existing value.
    WifiRollbackUnavailable,
};

[[nodiscard]] std::string_view log_label(Failure failure);

// --- Wi-Fi validation (owner ruling D-1) -----------------------------------

struct Policy {
    // How long a Connected station must stay connected before the join is believed.
    //
    // WORKING BASELINE, NOT PRODUCT POLICY — owner ruling D-1, 2026-08-16. It exists
    // because association and DHCP can both succeed moments before an access point drops a
    // station it has decided not to keep, and a configuration persisted on that instant is
    // one the device cannot use afterwards.
    std::uint32_t dwell_ms{2000};

    // The whole attempt, from the first join.
    //
    // WORKING BASELINE. Long enough for a slow DHCP lease, short enough that a user
    // watching a browser is not left guessing.
    std::uint32_t timeout_ms{30000};
};

enum class Verdict : std::uint8_t {
    // No attempt is running.
    Idle,

    Pending,
    Validated,
    Failed,
};

// The station request identity an attempt is judged by.
//
// ⛔ A DISTINCT TYPE BECAUSE THE MISTAKE IS SILENT. begin() takes a millisecond reading and a
// request identity; both are 32-bit unsigned, and only their order at the call site tells them
// apart. Swapping them would compile, would bind the attempt to a timestamp no station will ever
// report, and would then time out every submission — a failure that looks exactly like a network
// problem. Same remedy escaping.hpp's FieldName, backoff.hpp's Jitter and server_view.hpp's
// CsrfToken were introduced for, and clang-tidy's bugprone-easily-swappable-parameters is what
// asked for it here.
struct Candidate {
    network::station::Tag tag{network::station::kNoTag};
};

// The dwell-and-timeout machine over §7.4's NetworkPhase.
//
// ⛔ NOTHING ELSE IS PROBED. Owner ruling D-1: no internet check, no DNS lookup, no printer
// request. Association plus a DHCP lease plus a short dwell is what a local-first product
// can honestly assert, and every one of those is observable without leaving the LAN.
class WifiValidation {
  public:
    WifiValidation() = default;
    explicit WifiValidation(const Policy policy) : policy_{policy} {}

    // Starts an attempt against ONE station request identity.
    //
    // ⛔ THE IDENTITY IS WHAT MAKES THIS SAFE TO RUN TWICE, AND IT REPLACED A HEURISTIC. Until
    // checkpoint 3c this took the phase the store held when the attempt began and disregarded
    // reports equal to it — because the station published through a queue, so the first tick after
    // a restart still showed the PREVIOUS transaction's terminal phase. On 2026-08-18 that read
    // `Unavailable`, left over from a genuinely rejected password, and a corrected retry was failed
    // 6 ms after being accepted while the station went on to associate and lease an address
    // perfectly. Guarding by content cannot distinguish "the old attempt's answer" from "the new
    // attempt reached the same state"; an identity can. §8.1: "rejects stale responses using
    // request generation or sequence IDs".
    void begin(std::uint32_t now_ms, Candidate candidate);

    // Folds in one piece of station evidence. Evidence charged to any other identity advances time
    // and nothing else, so candidate B's answer can never validate candidate C. Also advances
    // time, so a caller that only ever observes does not need to tick as well.
    [[nodiscard]] Verdict observe(network::station::Tag tag, network::station::Outcome outcome,
                                  std::uint32_t now_ms);

    // Advances time with no new phase — how the dwell completes and how the timeout fires
    // for a station that has gone quiet.
    [[nodiscard]] Verdict tick(std::uint32_t now_ms);

    [[nodiscard]] Verdict verdict() const {
        return verdict_;
    }

    [[nodiscard]] Failure failure() const {
        return failure_;
    }

    // Whether the dwell is currently open. Read by the adapter so the dwell's beginning and end
    // appear in the timeline as their own interval — the owner asked for it to be visible
    // separately from association and DHCP, and a reader cannot separate them from the phase
    // alone. Exposed rather than re-derived, because two derivations of one fact is how they come
    // to disagree.
    [[nodiscard]] bool dwelling() const {
        return connected_;
    }

    void reset();

  private:
    [[nodiscard]] Verdict settle(Verdict verdict, Failure failure);

    Policy policy_{};
    Verdict verdict_{Verdict::Idle};
    Failure failure_{Failure::None};

    std::uint32_t started_ms_{0};
    std::uint32_t connected_since_ms_{0};
    bool connected_{false};

    // The ONE station request identity this attempt may be judged by.
    network::station::Tag candidate_{network::station::kNoTag};
};

// --- The two records -------------------------------------------------------

struct Records {
    config::ConfigV1 configuration;
    secrets::SecretConfigV1 secret;
};

// The next Wi-Fi profile handle.
//
// THE HANDLE IS WHAT MAKES THE PAIR ATOMIC. app_core only treats a device as provisioned
// when the configuration record and the credential record name the SAME profile id
// (configuration_observation.cpp), so a transaction interrupted between the two commits
// leaves ids that disagree and reads as un-provisioned. Moving the id on every transaction
// is what arms that check; reusing it would let a half-written pair look complete.
//
// Never returns 0, which means "no profile".
[[nodiscard]] std::uint32_t next_profile_id(std::uint32_t current);

// Builds the pair from a submission, carrying forward everything in `base` the browser did
// not supply — brightness, delays, update channel.
//
// ⛔ printer_validated IS ALWAYS false. Owner ruling R4 keeps every printer protocol in
// Milestone 4, so Milestone 3 stores an address and a key it has never contacted. There is
// deliberately no argument that could make this true.
[[nodiscard]] Records compose(const Submission& submission, const config::ConfigV1& base,
                              std::uint32_t profile_id);

// --- Staging and activation (Milestone 4) ----------------------------------
//
// ⬅️ WHAT REPLACED THE OLD commit(). That function wrote both records AND activated both, because
// through Milestone 3 there was no applicable connection test to put between the two (R4 kept every
// printer probe out). Milestone 4 makes §13.3 step 5 applicable, so the transaction becomes three
// steps with the probe in the middle:
//
//   stage_pair()  → both candidates written and read back; NOTHING active changes
//   ... the printer is probed ...
//   activate_pair() → recovery record, both activations, reread, record cleared
//
// A failed probe simply never calls the second, and the previous validated pair is still the active
// one — §13.3 step 7 obtained by not having moved rather than by restoring. commit() itself is gone:
// it had no production caller left, and keeping the rejected activate-before-validation design
// around with a full test suite made it read as live coverage of a path production cannot take.

struct StagedPair {
    bool staged{false};
    Failure failure{Failure::None};

    // The generations the two candidates were written at. They are independent numbers: the two
    // namespaces keep their own pointers, and the PAIR is identified by the profile id both records
    // carry, which is the project's existing pairing handle.
    std::uint32_t configuration_generation{config::kNoGeneration};
    std::uint32_t secret_generation{secrets::kNoGeneration};

    // The profile id both candidates name.
    std::uint32_t profile{0};

    // ⬅️ THE EXACT PAIR THAT WAS ACTIVE WHEN THIS ONE WAS STAGED, AND WHY IT IS CAPTURED HERE.
    // The recovery record must be able to name a restore target, and after the candidates are in
    // flash there is no way to tell an active generation from a staged one by arithmetic. Both are
    // kNoGeneration when there was no whole previous pair — a first provisioning, where nothing is
    // at risk and there is nothing to go back to.
    std::uint32_t previous_configuration_generation{config::kNoGeneration};
    std::uint32_t previous_secret_generation{secrets::kNoGeneration};

    config::SaveOutcome configuration{config::SaveOutcome::Committed};
    secrets::SaveOutcome secret{secrets::SaveOutcome::Committed};
    bool secret_attempted{false};
};

// §13.3 steps 1–4 for both records.
//
// ⛔ CONFIGURATION FIRST, CREDENTIAL LAST, AND THE ORDER IS LOAD-BEARING. Either order is safe for
// the boot decision, because the profile-id binding rejects a mismatched pair whichever half is
// missing. The order is chosen for what a FAILED transaction leaves behind: stopping before the
// credential means the user's Wi-Fi password never reached flash at all, where the mirror order
// would strand an orphaned copy of it until something happened to overwrite that slot.
//
// NOTHING HERE ERASES (R10), and a failed staged write cannot damage the active record — the
// candidate always goes to the inactive slot.
[[nodiscard]] StagedPair stage_pair(storage::KeyValue& store, const Records& records);

struct ActivatedPair {
    bool activated{false};
    Failure failure{Failure::None};

    // True when both pointers moved and a reread proved the pair, but clearing the recovery record
    // failed. Harmless — the next boot reads it, finds the target pair already active and clears it
    // then — and reported so the storage complaint is visible.
    bool marker_retained{false};
};

// §13.3 step 6 for both records, as one boundary.
//
// Writes the recovery record, activates the configuration, activates the credential, REREADS to
// prove a whole pair, and only then clears the record. A power loss anywhere inside is resolved at
// the next boot by candidate::reconcile(), which is what makes two pointer writes behave as one
// activation.
//
// ⛔ CALLED ONLY AFTER A SUCCESSFUL PROBE. That is what lets the recovery record be read as proof
// that a validation succeeded, which in turn is what makes rolling an interrupted activation forward
// safe rather than a guess.
[[nodiscard]] ActivatedPair activate_pair(storage::KeyValue& store, const StagedPair& staged);

// --- The phase the flow is in ----------------------------------------------

// --- Printer validation and promotion (Milestone 4) ------------------------
//
// ⬅️ THIS IS WHAT COMPLETES §14.3. Steps 1-4 and 9's *candidate* write already existed at
// Milestone 3.8: a submitted printer is committed with printer_validated false, which §13.3
// calls a written-and-committed candidate. What was missing is §13.3 step 5, "test connection
// when applicable", and step 6, "mark configuration active" — and §14.3 step 9's "save as
// active ONLY AFTER validation succeeds". Until the promotion below runs, the candidate must
// not be a source of operational data or presentation.

// Turns a failed validation attempt's connection phase into something showable.
//
// Total over ConnectionPhase, so a phase nobody anticipated becomes a conservative
// "unreachable" rather than a claim about the printer that no evidence supports.
[[nodiscard]] Failure failure_for_printer(app_model::ConnectionPhase phase);

// Maps a verdict onto UIAD 001 §5's provisioning phase.
//
// SetupFailed IS RECOVERABLE, NOT TERMINAL — owner ruling D-6. It persists until the user
// submits again rather than bouncing back to SetupActive, because a failure that erased
// itself would leave the user with no idea what happened.
[[nodiscard]] app_model::ProvisioningPhase phase_for(Verdict verdict);

} // namespace prd::app_core::provisioning_flow
