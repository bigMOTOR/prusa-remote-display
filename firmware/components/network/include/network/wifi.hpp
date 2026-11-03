// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstdint>
#include <string_view>

#include "app_model/events.hpp"

// Wi-Fi station lifecycle — Architecture Spec §5.7, §8.1, §8.2, §10.1, §10.2, §15.1 steps 13
// and 18.
//
// NO ESP-IDF HEADER APPEARS HERE (Bootstrap Spec §11.3). The driver adapter is a separate
// translation unit; the intent machine, the event ring and the backoff ladder are pure and
// host-tested.
//
// ⛔ THE STATION HAS NO TASK OF ITS OWN, AND THAT IS THIS CHECKPOINT'S CHANGE. Until now this
// component created `prd_wifi` — an undocumented sixth task against §10.1's five, which also says
// "Do not create a task per module or endpoint" — and that task owned the retry deadline, the phase
// machine and the driver calls. §8.2 assigns effects to `app_core` ("effects are executed by
// `app_core`, then completion or failure is returned as another event") with `StartWifi` on the
// effect list, and §10.2 gives the application task "execute or dispatch effects" and "run
// lifecycle timers". The SDK's own documented station architecture is the same shape
// (`wifi.rst:404-416`).
//
// So the policy now runs on the application task, synchronously, inside the 100 ms tick it already
// has. service() below IS that step: it drains the driver's events, advances the intent machine,
// issues whatever driver operation the machine asks for, and RETURNS the operational truth and the
// candidate evidence directly to the caller. Nothing is posted back through the general event
// queue, and no observation sink exists any more.
//
// ⛔ IT STORES NOTHING BEYOND ONE PENDING STATION CONFIGURATION. Credentials arrive as parameters
// and are handed to the driver. The one exception is a submission that arrives while a driver
// completion is still owed: its configuration is held — in a single buffer that is wiped the
// instant esp_wifi_set_config() consumes it, and wiped again at every terminal boundary — because
// the pinned SDK forbids reconfiguring a station that is still connecting. Nothing else is
// retained, nothing is logged, and §13.5 and §24.3 do the rest.

namespace prd::network {

// What app_core hands over.
//
// Views, not owned strings: they are copied into the driver's configuration inside submit() and
// are not retained afterwards beyond the pending-configuration case above. That is deliberate — a
// credential this component kept would be a credential that could be logged, dumped or leaked from
// here.
struct Credentials {
    std::string_view ssid;
    std::string_view password;
};

namespace station {

// --- Request identity ------------------------------------------------------

// §8.1: "rejects stale responses using request generation or sequence IDs".
//
// ⛔ ONE MONOTONIC, NON-SECRET, FIXED-WIDTH IDENTITY, PUBLISHED AND CONSUMED WITHOUT TEARING. It is
// assigned before the driver operation it belongs to, and every completion that operation provokes
// is charged to it. That is what stops candidate B's evidence validating candidate C — the defect
// the previous design guarded against by comparing a phase against the phase the transaction
// started with, which is a heuristic rather than an identity.
//
// The conceptual precedent is prusalink::Transaction, which already does this for the printer. The
// two subsystems are deliberately NOT coupled: a Wi-Fi request and a printer request have nothing
// to say to each other.
using Tag = std::uint32_t;

// Never a live identity. Distinguishing "no request" from "request zero" is what the printer path
// learned to do with prusalink::kNoGeneration, and for the same reason.
inline constexpr Tag kNoTag = 0;

// The next identity after `current`.
//
// ⛔ WRAP IS HANDLED RATHER THAN ASSUMED AWAY. After 2^32 - 1 requests the counter returns to 0,
// which means "no request" — so it skips it. Reaching that needs four billion station requests on
// one device; it costs one comparison to make it harmless instead of a device whose evidence
// suddenly matches nothing. The same shape provisioning_flow::next_profile_id() uses.
[[nodiscard]] constexpr Tag next_tag(const Tag current) {
    const Tag next = current + 1;
    return next == kNoTag ? Tag{1} : next;
}

// --- Candidate evidence ----------------------------------------------------

// What the station proved about ONE request identity.
//
// ⛔ A SEPARATE CHANNEL FROM OPERATIONAL TRUTH, AND IT STAYS SEPARATE. A candidate result never
// colours NetworkState and NetworkState never settles a candidate. Update below carries both, and
// they are read by different owners for different questions.
enum class Outcome : std::uint8_t {
    // The station holds a lease on the submitted network. The dwell may run.
    Leased,

    // A lease or an association went away. NOT terminal: the manager retries under the same
    // identity, and the dwell restarts.
    LeaseInterrupted,

    // Terminal. The station stopped trying — §16.2's give-up after repeated credential rejection,
    // or §11.3's ladder saturated.
    Unreachable,

    // Terminal. A driver operation's documented completion never arrived, or a synchronous driver
    // call was refused in a state where the pinned SDK documents it to succeed. The owner's
    // DriverStalled ruling: the station is quarantined and recovery requires a user restart.
    DriverStalled,

    // Terminal. The event channel lost a record, so this transaction's evidence is incomplete.
    // Distinct from DriverStalled because the causes and the diagnostics differ, even though both
    // quarantine.
    EvidenceLost,
};

[[nodiscard]] std::string_view log_label(Outcome outcome);

// What one service() step produced.
//
// ⬅️ RETURNED RATHER THAN POSTED. §10.7's general AppEvent queue is bounded and lossy by design,
// and the owner ruled that Wi-Fi truth must not travel through it. The application task is the
// policy owner and the consumer, so it reads both answers here and dispatches the observation
// itself.
struct Update {
    // The operational truth changed — including when only the address changed and the phase name
    // did not. `observation` is complete and coherent: it was built from one instant.
    bool published{false};
    app_model::NetworkObservation observation{};

    // Candidate truth for `tag`. A consumer must ignore evidence whose tag is not the identity it
    // is waiting on.
    bool evidence{false};
    Tag tag{kNoTag};
    Outcome outcome{Outcome::Leased};
};

// Makes `credentials` the station's newest intent and returns the identity the transaction is
// charged to.
//
// Brings the driver up if it is not up (the shared stack, the event registration, the radio mode
// and esp_wifi_start(), in that order and each at most once), converts the credential into a
// station configuration, and hands the intent to the machine. If a driver completion is still owed
// the intent is RETAINED and nothing is issued until that completion arrives — the pinned SDK
// documents no completion for a disconnect issued at a station that is merely connecting, so
// cancelling one is not available.
//
// Returns kNoTag when the intent was refused: the driver could not be brought up, the credential
// does not fit the driver's fields, or the station is quarantined. A refused submission has changed
// nothing.
// `now_ms` is the caller's own tick reading, passed down so nothing here reads a clock twice.
[[nodiscard]] Tag submit(const Credentials& credentials, std::uint32_t now_ms);

// ⛔ BOOT ONLY: "there is no credential, so no station was started" — R6. The station reports
// Disabled and nothing is attempted, because bringing one up with no SSID would manufacture a
// failure the user cannot act on and §14.1 has already put that device into provisioning.
//
// ⛔ THIS IS NOT RUNTIME WITHDRAWAL. From any state other than a machine that has never been given
// an intent it is REFUSED, changing neither the published truth nor the physical station. Taking a
// live station down means a disconnect transaction with a driver completion of its own, plus
// restoring the configuration it replaced — that is 3e's restoration work, and this function
// deliberately does not pretend to do it. The name says what it is so no caller can assume more.
void station_absent(std::uint32_t now_ms);

// ⛔ RUNTIME WITHDRAWAL — CHECKPOINT 3e. Takes the station down from wherever it is and leaves it
// Disabled. Unlike station_absent() this is a real transaction: from an established or associated
// station it issues the one disconnect the pinned SDK documents a completion for and settles only
// once that completion has been consumed; requested while another operation's completion is owed it
// is RETAINED and issued when the SDK permits progress.
//
// ⬅️ WHAT IT IS FOR, AND WHAT IT IS NOT. A provisioning candidate that fails or is abandoned leaves
// the driver holding a credential nothing proved and nothing persisted. When a previous pair exists
// the honest repair is to REJOIN it, which is an ordinary submit() — this function is the answer for
// the case where there is no previous pair to rejoin, and PM ruling 1 of this checkpoint is that the
// station must not be left joined to the unproven candidate. It reads no store and erases nothing.
//
// The held station configuration is wiped before the machine is asked, so the abandoned candidate's
// passphrase does not outlive the request.
//
// Returns false when the withdrawal was refused and NOTHING was taken down: the station is
// quarantined — in which case the typed Device Fault path already owns that device and the caller
// must not add a restore-failure of its own (PM ruling 3) — or an exclusive operation holds the
// product state. A refusal is not counted as a diagnostic event; quarantined() is what says why.
[[nodiscard]] bool withdraw(std::uint32_t now_ms);

// ⛔ STAND DOWN FOR AN EXCLUSIVE OPERATION — R4. A confirmed factory reset owns the product state:
// the station issues no further driver operation until a new submission, any retained-but-unissued
// intent is dropped, and the adapter's held station configuration is WIPED — that buffer carries a
// passphrase the user submitted into the window the reset opened, and it is exactly the class of RAM
// credential the submission mailbox is already drained for.
//
// It does not disconnect an established association and does not model resumption after a FAILED
// erase: a successful reset restarts the device, and the failed-erase path is the open factory-reset
// item at the 3e boundary. An operation whose completion is still owed keeps owing it.
void quiesce(std::uint32_t now_ms);

// ONE step of the station machine, on the CALLER'S task. Must be called from the application task
// and from nowhere else: it is the single consumer of the driver's event ring, and that is what
// makes the machine's state safe without a lock.
//
// Never blocks, never waits and never delays. `now_ms` is the caller's own tick reading, passed
// down so nothing here reads a clock twice.
[[nodiscard]] Update service(std::uint32_t now_ms);

// Whether the station is quarantined by the DriverStalled ruling. Diagnostic, and the reason a
// caller must not offer the user another attempt that cannot be judged.
[[nodiscard]] bool quarantined();

// How many station events the ring refused, saturating. §10.7's diagnostic counter; the recovery
// policy that goes with it is the quarantine above.
[[nodiscard]] std::uint32_t evidence_refused();

} // namespace station

// Whether the station driver has been brought up. Read by the access point to decide whether it
// must share the radio.
[[nodiscard]] bool running();

} // namespace prd::network
