// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstdint>
#include <string_view>

#include "app_model/device_data.hpp"

#include "backoff.hpp"
#include "lifecycle.hpp"
#include "network/wifi.hpp"
#include "station_events.hpp"

// WHAT THE STATION IS ALLOWED TO BE ASKED TO DO, AND WHEN — Architecture Spec §8.1, §8.2, §10.2,
// §11.3, §16.1, §16.2.
//
// INTERNAL HEADER of the network component. Pure — no ESP-IDF, no FreeRTOS, no clock, no
// randomness of its own — so every rule below is host-tested against the same code the firmware
// runs. The adapter translates ESP-IDF events into station::Event, supplies the four driver calls
// behind function pointers, and obeys what comes back. It decides nothing.
//
// ⛔ THIS IS THE UNIT THAT HAD NO HOME BEFORE. Until this checkpoint the decisions below lived in a
// FreeRTOS task inside the driver adapter — an undocumented sixth task against §10.1's five — and
// therefore could not be tested at all. Three separate rewrites of that task were rejected in
// review because each substituted a timer, a counter or a probe budget for the one thing the
// pinned SDK actually guarantees. What is here is that guarantee, written down once.
//
// ⛔ THE TEN INVARIANTS. Every one of them is a host test.
//
//   1. At most ONE driver operation that owes a terminal completion is outstanding.
//   2. No configure, connect or disconnect may be issued while a completion is owed.
//   3. `completion_owed()` is cleared ONLY by consuming the matching documented terminal event —
//      never by a deadline.
//   4. A new intent arriving while attempting is RETAINED as the newest pending intent.
//      disconnect() is not called to cancel a connecting station: `wifi.rst:277-280` documents
//      three WIFI_EVENT_STA_DISCONNECTED scenarios and all three require an already-connected
//      station, a failed connect setup, or a disrupted connection. There is NO documented
//      completion barrier for "disconnect while merely connecting", so calling it would leave the
//      manager owing a completion that may never arrive.
//   5. If the old attempt eventually fails, the newest pending intent is acted on FROM THE
//      NON-ASSOCIATED STATE.
//   6. If the old attempt connects, it is retired through the documented connected-state
//      disconnect completion, and only then is the pending intent acted on.
//   7. Connected-state reconfiguration is `disconnect` → matching DISCONNECTED → `set_config` →
//      `connect`. esp_wifi_set_config() documents ESP_ERR_WIFI_STATE ("WiFi still connecting") and
//      esp_wifi_connect()'s @attention 2 requires an explicit disconnect when connected.
//   8. Every synchronous driver return code is consumed explicitly. A REFUSED CALL CANNOT CREATE
//      AN OWED COMPLETION.
//   9. Backoff is deadline-based and never blocks: the caller supplies `now_ms` from its own
//      bounded tick and this class compares.
//  10. A validation or product deadline cannot make the manager reusable while a completion is
//      still owed.

namespace prd::network::station {

// Where the station is with respect to the DRIVER, which is a different question from what the
// product says about the network (Truth::phase below is that).
//
// ⬅️ WHY BOTH EXIST. NetworkPhase::Connecting covers Attempting and Associated; Unavailable covers
// GaveUp, a saturated retry ladder and Stalled. Collapsing them would mean either publishing driver
// mechanics to the user or deciding driver questions from a product label — and this project has
// already recorded the rule: physical driver/link state and per-credential lifecycle state are
// different things.
enum class State : std::uint8_t {
    // No intent, or the station interface is down. Nothing is owed.
    Idle,

    // connect() was accepted; its terminal completion is owed.
    Attempting,

    // The link is up and there is no lease. Nothing is owed — DHCP is not an operation we issued —
    // and the no-lease deadline is running.
    Associated,

    // The link is up and a lease is held.
    Connected,

    // disconnect() was issued to retire an existing association; its completion is owed.
    Retiring,

    // §11.3's ladder is waiting out a delay before the next connect().
    Backoff,

    // §16.2's terminal state for this credential: the access point rejected it repeatedly and
    // nothing but a configuration change may re-arm it.
    GaveUp,

    // ⛔ THE OWNER'S DriverStalled RULING. A driver call was accepted and its promised terminal
    // completion never arrived, a synchronous call was refused in a state where the pinned SDK
    // documents it to succeed, or the event channel lost evidence. No further station driver
    // operation is issued, no reboot is attempted, no unproven esp_wifi_stop()/start() recovery is
    // tried, the setup access point in APSTA is left alone, and recovery requires an explicit user
    // restart. A late matching terminal event may clear the owed fact but MUST NOT resume, retry
    // or validate the failed transaction.
    Stalled,
};

[[nodiscard]] std::string_view log_label(State state);

// §16.1's Wi-Fi errors in the shape NetworkState::last_error carries.
//
// ⬅️ WHY THIS REPLACED error_text(DisconnectCause). DisconnectCause answers "why did the LINK go
// down", which has no honest value for a driver that never answered at all. A stall is not a
// disconnect and must not borrow a disconnect's words.
enum class Fault : std::uint8_t {
    None,
    LinkLost,
    CredentialsRejected,
    DriverUnresponsive,
};

// FACTS, NEVER INPUT. §13.5 forbids credentials in logs and §24.4 forbids them anywhere in
// diagnostics; the strings are fixed and no caller can substitute an SSID or a password into them.
//
// ⛔ FOR A READER, NOT FOR A DECISION. NetworkState::last_error carries this text, and nothing may
// take a product decision from its WORDS — see published_fault() below, which is the typed form the
// projection and the reducer read instead.
[[nodiscard]] std::string_view error_text(Fault fault);

// The same fact in the form app_model carries it, so the panel never matches a string — R7.
//
// ⬅️ TWO ENUMERATIONS RATHER THAN ONE SHARED ONE, DELIBERATELY. station::Fault is this component's
// internal vocabulary and app_model::NetworkFault is the product's; §4.2 keeps app_model free of
// anything a driver adapter defines, and this one-line translation at the boundary is what the rest
// of this component's observations already do for the phase.
[[nodiscard]] app_model::NetworkFault published_fault(Fault fault);

// The station's OPERATIONAL truth, as the product states it. §7.4.
//
// ⛔ NO SSID. The configured network name is credential material (§§7.3, 13.2, 13.5) and was
// removed from NetworkState on 2026-08-23. Nothing here can carry it.
struct Truth {
    app_model::NetworkPhase phase{app_model::NetworkPhase::Disabled};

    // The lease, host byte order. 0 means no lease — which is also why Connected can never be
    // published without one.
    std::uint32_t ipv4{0};

    Fault fault{Fault::None};
};

[[nodiscard]] bool operator==(const Truth& lhs, const Truth& rhs);
[[nodiscard]] bool operator!=(const Truth& lhs, const Truth& rhs);

// The four driver calls, as function pointers rather than a template or a virtual interface.
//
// ⬅️ WHY FUNCTION POINTERS. RTTI and exceptions are off and this project does not add vtables for
// one implementation; a template would force the whole machine into a header. The same shape
// network::ObservationSink and provisioning::SubmissionSink already use.
//
// ⛔ THE CONTRACT EACH IMPLEMENTATION MUST HONOUR:
//
//   - Publish `tag` where the event producer can read it BEFORE performing the driver call, so
//     every completion the call provokes is charged to the identity that was current when it was
//     issued. §8.1's "request generation or sequence IDs".
//   - Return true if and only if the driver ACCEPTED the request. A true return is what creates an
//     owed completion for connect() and disconnect(); configure() owes nothing, because
//     esp_wifi_set_config() is synchronous and posts no event.
//   - Never block, never log a credential, never allocate on the exhaustion path.
struct Driver {
    // esp_wifi_set_config(WIFI_IF_STA, …) for the credential the adapter is holding for the newest
    // intent. Synchronous: it completes or it is refused.
    bool (*configure)(void* context, Tag tag){nullptr};

    bool (*connect)(void* context, Tag tag){nullptr};
    bool (*disconnect)(void* context, Tag tag){nullptr};

    void* context{nullptr};
};

// The caller's tick, in one value.
//
// ⛔ ONE READING OF THE CLOCK PER TICK, PASSED DOWN. This project has already been bitten by two
// readings of a monotonic clock being subtracted from each other; whoever owns a tick's time
// observation passes it, and nothing below reads a clock at all.
struct Moment {
    std::uint32_t now_ms{0};

    // One random byte for §11.3's jitter. Supplied by the caller so this file stays pure and so a
    // test can choose it and assert an exact ladder rather than a range.
    Jitter jitter{};
};

// What one call to the manager produced.
struct Progress {
    // The operational truth differs from what it was before the call. The caller publishes
    // truth() — including when only the address changed and the phase name did not.
    bool truth_changed{false};

    // Candidate-validation truth, on its own channel. A candidate result never colours
    // NetworkState and vice versa.
    bool evidence{false};
    Tag tag{kNoTag};
    Outcome outcome{Outcome::Leased};
};

// Whether an outcome ends a candidate's transaction.
[[nodiscard]] bool terminal(Outcome outcome);

// Folds `next` into `into` for a caller that steps the manager several times per tick.
//
// ⛔ THE FIRST TERMINAL OUTCOME WINS, AND THAT IS THE WHOLE RULE. A burst can contain a terminal
// failure followed by an ordinary transition for the intent that replaced it; collapsing to "the
// last one" would let the replacement's progress erase the failure that a browser is waiting on.
// Among non-terminal outcomes the latest wins, because the dwell is a level rather than an edge.
void merge(Progress& into, const Progress& next);

class Manager {
  public:
    struct Policy {
        // ⚠️ A PM-ACCEPTED WORKING BASELINE, NOT A VALUE SOURCED FROM ANY ACCEPTED PROJECT SOURCE.
        // Accepted explicitly on 2026-08-25 and REQUIRING LATER PHYSICAL VALIDATION.
        //
        // ⛔ THE EARLIER ATTRIBUTION TO §11.3 WAS WRONG AND IS CORRECTED HERE. §11.3 sits under
        // "11. Polling Architecture"; its "2 s → 4 s → 8 s → 15 s → 30 s maximum" ladder bounds
        // PRINTER polling backoff and says "reset failure count after a confirmed successful status
        // response". It authorises no Wi-Fi driver-completion deadline, and no other Accepted
        // Project Source does either — §16.1 and §16.2 classify Wi-Fi errors and retry classes
        // without giving a number.
        //
        // How long a driver operation may owe its completion. The pinned SDK gives no time bound for
        // esp_wifi_connect() or esp_wifi_disconnect(): both are thin wrappers over libnet80211.a
        // blobs, so no latency can be proven by reading — which is exactly why the physical
        // measurement is a condition of final acceptance. Deliberately conservative: the
        // consequence of this deadline firing is a quarantine the user must power-cycle out of, so
        // it must never fire on a merely slow driver.
        std::uint32_t completion_ms{30000};

        // ⚠️ ALSO A PM-ACCEPTED WORKING BASELINE, accepted 2026-08-25 and requiring later physical
        // validation. Not from §11.3 either, for the reason above.
        //
        // ⛔ IT BOUNDS ONE CASE ONLY: AN INITIAL ASSOCIATION THAT HAS NOT OBTAINED ITS FIRST LEASE.
        // A station that associates and never leases is honestly "still trying", and this is how
        // long the product will tolerate that before retiring the association and starting over.
        //
        // ⛔ IT DOES NOT APPLY AFTER IP_EVENT_STA_LOST_IP — PM decision, 2026-08-25. That event only
        // fires after ESP-IDF's own ~120 s lost-IP interval, by which point lwIP has already been
        // rediscovering for the whole interval (dhcp.c:544-548 performs dhcp_release_and_stop();
        // dhcp_start(); itself). Waiting a further 30 s there would have made one outage roughly
        // 150 s long before the association layer was asked to help, so the retirement begins
        // immediately instead. See the LostIp handling in station_intent.cpp.
        std::uint32_t no_lease_ms{30000};
    };

    Manager() = default;
    explicit Manager(const Policy policy) : policy_{policy} {}

    // Makes `tag` the newest intent.
    //
    // The adapter must already hold the credential for it and must have brought the driver up. If a
    // completion is owed the intent is RETAINED and nothing is issued (invariant 4); otherwise the
    // existing association is retired first (invariants 6, 7) or the intent is adopted immediately.
    //
    // ⛔ REFUSED WHILE QUARANTINED. A stalled station issues no further driver operations, so the
    // adapter must check quarantined() and answer the caller with kNoTag rather than accepting a
    // transaction that can never be judged.
    [[nodiscard]] Progress submit(Tag tag, const Moment& moment, const Driver& driver);

    // ⛔ BOOT ONLY: "there is no credential, so no station was started" — R6. This is the
    // un-provisioned device's ONE transition, taken from `start_network()` before any intent exists;
    // §14.1 has already put that device into provisioning, and bringing a station up with no SSID
    // would manufacture a failure the user cannot act on.
    //
    // ⛔ IT IS NOT RUNTIME WITHDRAWAL, AND IT REFUSES RATHER THAN PRETENDING. From any state other
    // than a fresh Idle with no intent, this returns unchanged and counts the refusal: withdrawing a
    // LIVE station means disconnecting a real association and restoring a previous configuration,
    // which is a transaction with a driver completion of its own. That is 3e's restoration work
    // (`PreviousPair`), and the earlier contract — "withdraws any intent" — promised it here.
    // It did not deliver it: while a completion was owed it cleared only the retained intent and let
    // the active one carry on associating, and from Connected it published Disabled while leaving
    // the station physically joined. Neither branch was reachable from the single boot-time caller,
    // and neither is representable now.
    [[nodiscard]] Progress station_absent(const Moment& moment);

    // How many times station_absent() was refused because the machine was not in its one legal
    // starting state. Diagnostic; saturates.
    [[nodiscard]] std::uint32_t absent_refusals() const {
        return absent_refusals_;
    }

    // ⛔ RUNTIME WITHDRAWAL — CHECKPOINT 3e, AND THE TRANSACTION station_absent() REFUSED TO FAKE.
    // Take the station down from wherever it is and leave it Disabled. Erases nothing, persists
    // nothing, and reads no store: the credential half of restoration belongs to the caller, which
    // rejoins a previous pair by submit()ting it instead of asking for this.
    //
    // ⬅️ WHEN THE PRODUCT NEEDS IT. A provisioning candidate that fails or is abandoned leaves the
    // driver holding a credential nothing proved and nothing persisted. If there is a previous pair
    // the honest move is to rejoin it — an ordinary submit(), which invariants 6 and 7 already
    // retire the candidate for. When there is NO previous pair there is nothing to rejoin, and PM
    // ruling 1 of this checkpoint is that the station must not be left joined to that candidate:
    // it comes down instead.
    //
    // ⬅️ IT IS A TRANSACTION, WHICH IS THE WHOLE DIFFERENCE FROM station_absent(). From Connected or
    // Associated it issues the ONE disconnect the pinned SDK documents a completion for, owes that
    // completion like any other operation (invariants 1, 2, 3, 8), and only settles to Disabled once
    // the matching DISCONNECTED has been consumed. From a non-associated state there is no
    // association to lose and it settles immediately.
    //
    // ⛔ REQUESTED WHILE A COMPLETION IS OWED, IT IS RETAINED AND NOTHING IS ISSUED — invariant 4's
    // rule, applied to a withdrawal. A station that is merely connecting has no documented
    // disconnect completion (wifi.rst:277-280), so calling disconnect() there would leave a
    // completion owed that may never arrive. The withdrawal waits for the attempt's own terminal
    // event and is then acted on from whatever state that leaves — Idle if it failed, Connected if
    // it succeeded, and the disconnect is issued in that second case.
    //
    // ⛔ IT SUPERSEDES A RETAINED-BUT-UNISSUED INTENT, AND A LATER submit() SUPERSEDES IT. The
    // session that submitted that intent is over, so joining its network afterwards would be acting
    // on an instruction nobody is waiting for; and a user who submits again has asked for something
    // newer than the abandonment.
    //
    // ⛔ REFUSED WHILE QUARANTINED OR STOOD DOWN, WITHOUT COUNTING A REFUSAL. A stalled station
    // issues no further driver operation — the Device Fault path owns that device, and PM ruling 3
    // keeps the restore-failure vocabulary out of it — and an exclusive operation owns the product
    // state. Returns unchanged in both cases; the caller reads quarantined()/quiesced() to say why.
    [[nodiscard]] Progress withdraw(const Moment& moment, const Driver& driver);

    // Whether a withdrawal has been asked for and has not finished. True while its disconnect is in
    // flight and while it is retained behind another operation's completion.
    [[nodiscard]] bool withdrawing() const {
        return withdraw_;
    }

    // ⛔ STAND DOWN FOR AN EXCLUSIVE OPERATION — R4, and nothing more than that. A confirmed factory
    // reset owns the product state, so the station must issue no further driver operation while it
    // runs and must not be holding a candidate the user submitted into the window the reset opened.
    //
    // It drops a retained-but-unissued intent and refuses every subsequent operation until a new
    // submission. It deliberately does NOT disconnect an established association, restore a previous
    // pair, or model resumption after a FAILED erase: a successful reset restarts the device, and
    // the failed-erase path is the open factory-reset item (audit item 5) at the 3e boundary.
    //
    // An operation whose completion is still owed keeps owing it — invariant 3 — so a late terminal
    // event is still consumed and still cannot revive anything.
    [[nodiscard]] Progress quiesce(const Moment& moment);

    // Whether an exclusive operation currently forbids station driver operations.
    [[nodiscard]] bool quiesced() const {
        return quiesced_;
    }

    // Folds in one delivered event. `cause` is the adapter's classification of `event.reason` and
    // is meaningful for Disassociated only — the ESP-IDF WIFI_REASON_* macros live in the adapter,
    // so the classification is made there and the decision is made here.
    [[nodiscard]] Progress on(const Event& event, DisconnectCause cause, const Moment& moment,
                              const Driver& driver);

    // The event channel lost a record. Detected loss is a FAULT, not a case: the transaction's
    // evidence is incomplete, so no verdict about it can be honest.
    [[nodiscard]] Progress evidence_lost();

    // Advances time with no new event — how a backoff expires, how the no-lease escalation fires,
    // and how a missing completion becomes Stalled.
    [[nodiscard]] Progress tick(const Moment& moment, const Driver& driver);

    [[nodiscard]] State state() const {
        return state_;
    }

    [[nodiscard]] const Truth& truth() const {
        return truth_;
    }

    // Invariant 3: true from the instant a connect() or disconnect() is accepted until the matching
    // documented terminal event is consumed. A deadline moves the STATE; it never clears this.
    [[nodiscard]] bool completion_owed() const {
        return owed_;
    }

    [[nodiscard]] bool quarantined() const {
        return state_ == State::Stalled;
    }

    [[nodiscard]] Tag active() const {
        return active_;
    }

    [[nodiscard]] Tag retained() const {
        return pending_;
    }

    // Whether the adapter's held credential can still be asked for.
    //
    // ⛔ THIS IS THE ADAPTER'S WIPE SIGNAL, so it must go false on EVERY path that ends an intent,
    // not only the one where configure() consumed it. A quarantine and a station stop both forget
    // the intent inside this class; without them here, the passphrase the adapter built its held
    // configuration from would stay in that buffer for the life of the device.
    [[nodiscard]] bool credential_wanted() const {
        if (state_ == State::Stalled) {
            return false;
        }

        // ⛔ A RETAINED INTENT ALWAYS WANTS IT, AND `configured_` DOES NOT SAY SO. That flag
        // describes the ACTIVE intent — the credential the driver already holds — so a submission
        // that arrived while a completion was owed would look satisfied by its predecessor's
        // set_config and have its configuration wiped out from under it. The adopt that follows
        // would then ask for a configuration nobody was holding.
        if (pending_ != kNoTag) {
            return true;
        }

        return !configured_ && active_ != kNoTag;
    }

  private:
    // Decides and issues. The ONE place a driver operation is started, which is what makes
    // invariants 1, 2 and 7 properties of the code rather than of five call sites.
    void advance(Progress& progress, const Moment& moment, const Driver& driver);

    // The one disconnect the pinned SDK documents a completion for, shared by the three requests
    // that need an existing association retired first: a newer intent, a pending retirement and a
    // withdrawal. False when the driver refused, which has already quarantined. Called only from
    // advance(), so "one place issues a driver operation" stays true.
    [[nodiscard]] bool issue_retirement(Progress& progress, const Moment& moment,
                                        const Driver& driver);

    void disassociated(Progress& progress, const Event& event, DisconnectCause cause,
                       const Moment& moment);
    void stopped(Progress& progress, const Moment& moment);

    // ⛔ WHICH TRANSACTION A STATION-FATAL CONDITION MUST SETTLE — R2, and it is one rule in one
    // place. `pending_` if present, else `owed_tag_` if present, else `active_`.
    //
    // The newest candidate is the only one with a listener: app_core keeps a SINGLE WifiValidation,
    // and a new submission re-begins it for the new identity, so nothing is waiting on the attempt
    // that was superseded. Charging a stall to the older identity therefore sends the actionable
    // outcome to a transaction nobody is reading, and the browser waits out the generic budget and
    // is told the network timed out — when the truth is that the driver stopped answering.
    //
    // ⬅️ AND IT IS READ BEFORE quarantine() RUNS, ALWAYS. quarantine() clears `pending_`, so a
    // victim computed afterwards would silently be the wrong one.
    [[nodiscard]] Tag victim() const;

    // Whether a newer intent is waiting, which makes the ACTIVE attempt's ordinary progress and
    // ordinary failure internal rather than reportable — R2.
    //
    // ⛔ THERE IS ONE EVIDENCE RESULT PER TICK AND THE NEWEST CANDIDATE OWNS IT. A superseded
    // attempt's disconnect is not a verdict anybody can consume: it is the step that PERMITS its
    // successor to be adopted. Publishing it would occupy the single slot and, being terminal, would
    // survive merge() and suppress the successor's own outcome in the same tick.
    [[nodiscard]] bool superseded() const {
        return pending_ != kNoTag;
    }

    void adopt(const Moment& moment);

    // The end of a withdrawal: no intent, no held credential wanted, Disabled published. Reached
    // only from advance(), and only with no completion owed.
    void withdrawn(const Moment& moment);

    void apply(const network::Step& step, const Moment& moment);
    void refused(Progress& progress);
    void quarantine(Fault fault);

    // Takes the whole Moment rather than its millisecond reading, so the deadline's START and its
    // SPAN cannot be swapped: both are 32-bit unsigned, and only their order at the call site would
    // have told them apart. The same remedy backoff.hpp's Jitter and server_view.hpp's CsrfToken
    // were introduced for, and the same check — bugprone-easily-swappable-parameters — asked for it.
    void arm(const Moment& moment, std::uint32_t span_ms);
    void disarm();
    [[nodiscard]] bool due(std::uint32_t now_ms) const;

    Policy policy_{};
    Lifecycle lifecycle_;

    State state_{State::Idle};
    Truth truth_{};

    // The identity whose operations are in flight and whose credential the driver holds.
    Tag active_{kNoTag};

    // A newer intent, retained until it may legally be acted on. Invariant 4.
    Tag pending_{kNoTag};

    // Whether the driver already holds `active_`'s credential, so a reconnect needs connect()
    // alone. §11.3's retries reuse the configuration the driver already has.
    bool configured_{false};

    // An associated station must be retired before the next attempt. Set when the bounded no-lease
    // interval expires and when a lease is lost at an established connection; a pending intent
    // implies the same requirement and is handled beside it in advance().
    bool retire_{false};

    // An exclusive operation owns the product state — R4. No driver operation is issued while this
    // holds, and only a new submission clears it.
    bool quiesced_{false};

    // A withdrawal has been asked for — checkpoint 3e. Like `retire_` it is a REQUEST rather than a
    // state, so advance() remains the one place a driver operation is issued; unlike `retire_` it
    // has no successor intent, so what follows the retirement is Disabled rather than a connect.
    bool withdraw_{false};

    std::uint32_t absent_refusals_{0};

    // Invariant 3's fact, and the tag the completion is charged to.
    bool owed_{false};
    Tag owed_tag_{kNoTag};

    // ONE deadline, because the states that use it are mutually exclusive: a completion deadline
    // while Attempting or Retiring, the no-lease interval while Associated, §11.3's delay while in
    // Backoff. Stored as start-plus-span so the comparison is unsigned and survives the clock's
    // own wrap, the same arithmetic provisioning_flow uses.
    bool armed_{false};
    std::uint32_t armed_at_ms_{0};
    std::uint32_t span_ms_{0};
};

} // namespace prd::network::station
