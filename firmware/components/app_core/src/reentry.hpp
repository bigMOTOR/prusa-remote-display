// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstdint>

#include "app_model/state_layers.hpp"

#include "provisioning_decision.hpp"

// The privileged setup session's bounded idle lifetime — Milestone 3.10, owner rulings 2 and C.
//
// PURE, AND HOST-TESTED. No clock, no driver, no global: the caller supplies a monotonic
// millisecond count, so "an abandoned browser polled for an hour" is a test rather than an hour.
//
// ⬅️ WHY A LIFETIME EXISTS AT ALL. Owner ruling 2 of Milestone 3.9 makes the setup access point
// the ONLY surface on which sensitive configuration may be mutated, and a deliberate long press
// is the physical-presence act that brings it into existence. That makes the access point a
// privileged window, and a privileged window that never closes is not one. Architecture Spec
// §14.2 already wanted the access point stopped after setup, and §14.4 wanted a bounded idle
// period "when safe"; ADR-0007 scoped that rule to exactly this — temporary provisioning and
// privileged-authorization exposure, never the persistent read-only LAN server.
//
// ⛔ AUTOMATIC POLLING IS NOT ACTIVITY, AND THIS IS THE WHOLE POINT OF THE TYPE. The served page
// fetches /api/status once a second for as long as it is open. Counting that as activity would let
// a browser tab forgotten on a desk hold the privileged access point open indefinitely, which
// defeats the bound rather than implementing it. Only an accepted user-initiated mutation refreshes
// the deadline. Every GET — the page, the stylesheet, the script, the status poll — refreshes
// nothing.
//
// ⛔ AND A RUNNING TRANSACTION CANNOT EXPIRE. The access point must not vanish while a network is
// being joined or a configuration committed: the browser would lose the surface mid-flow, which is
// the failure owner ruling D-2's grace period exists to prevent at the other end of the same flow.
// Expiry is suspended while a transaction is in progress and accounting resumes when it settles.
//
// Note that nothing automatic can produce a settlement: a transaction begins only because a
// submission was accepted, and an accepted submission is itself activity. So the suspension cannot
// be used to extend the window without a person doing something.

namespace prd::app_core::reentry {

// How long a privileged setup session may sit idle before its access point is stopped.
//
// A WORKING BASELINE, NOT PRODUCT POLICY. No accepted source determines a value. Ten minutes is
// long enough to walk to another room, find a phone, read a password off the panel and type it,
// and short enough that a session nobody returned to does not outlive the visit.
inline constexpr std::uint32_t kIdleLifetimeMs = 10U * 60U * 1000U;

class PrivilegedSession {
  public:
    // A deliberate re-entry raised the access point. Starts the idle clock.
    void begin(std::uint32_t now_ms);

    // An accepted user-initiated mutation arrived — a submission the request guard allowed AND
    // the provisioning flow accepted. Refuses and rejections do not reach here: a request refused
    // by the window must not extend the window that refused it.
    void note_activity(std::uint32_t now_ms);

    // Whether a validation or commit is in progress. While true, expired() is false whatever the
    // clock says; the transition back to false restarts idle accounting from that moment, which is
    // owner ruling 1's "once that transaction settles, idle accounting resumes".
    void set_transaction_running(bool running, std::uint32_t now_ms);

    // The session is over — success, or the access point stopped for any other reason.
    void end();

    [[nodiscard]] bool active() const {
        return active_;
    }

    [[nodiscard]] bool transaction_running() const {
        return transaction_running_;
    }

    // Whether the access point should now be stopped. False when no session is active, and false
    // while a transaction is in progress.
    [[nodiscard]] bool expired(std::uint32_t now_ms) const;

    // Milliseconds since the last thing that counted as activity. Diagnostic only.
    [[nodiscard]] std::uint32_t idle_for(std::uint32_t now_ms) const;

  private:
    bool active_{false};
    bool transaction_running_{false};
    std::uint32_t last_activity_ms_{0};
};

// The product state a privileged session leaves behind when its window expires.
//
// ⛔ WHY THIS IS A DECISION AND NOT A DEFAULT. stop_setup_ap() clears the transient session, so the
// instant the access point stops the panel has no SSID, no password, no address and no QR to draw.
// At Milestone 3.11 T4 the phase was left where it was — SetupActive — and the panel went on
// showing "Join this network" over an empty composition, indefinitely, about a network that no
// longer existed. A panel asserting something untrue is worse than one saying nothing.
//
// ⬅️ AND WHY IT DEPENDS ON THE CONFIGURATION. A device that already holds a valid configuration
// has nothing left to say: it is configured, the setup surface is gone, and Inactive removes the
// setup composition altogether. A device that is still unprovisioned genuinely does need setting
// up, so it returns to asking — the state it was in before the window opened, and reachable again
// with a long press.
//
// ⬅️ AND WHY IT DEPENDS ON WHY THE SESSION WAS OPENED — MILESTONE 3.11 T18, OWNER RULING. A
// session opened out of Recovery Mode is the one case where "still unprovisioned" understates the
// truth. The stored record is not merely missing, it is FAULTED, and if the session ends without
// replacing it the fault is exactly as real as it was before the long press. SetupRequired would
// drop the reason the user was put there and ask them to set up a device that has already told
// them it cannot read its own configuration, so the honest resting state is the one it came from.
//
// ⛔ THE DECISION IS PASSED IN RATHER THAN A BOOL, so this unit owns which outcomes mean recovery
// instead of trusting a caller to re-derive it. A call site that computes "was this recovery" for
// itself is the shape Milestone 3.11 T16 already had to remove twice.
struct AfterExpiry {
    // The phase to publish once the access point has stopped.
    app_model::ProvisioningPhase phase;

    // Whether Recovery Mode must be re-entered. Its phase is always Inactive: recovery has no
    // session, so the two are mutually exclusive by construction rather than by convention.
    bool return_to_recovery;
};

// Pure, so every path is host-tested rather than reasoned about at a call site.
[[nodiscard]] AfterExpiry after_expiry(bool configuration_is_valid, ProvisioningDecision decision);

} // namespace prd::app_core::reentry
