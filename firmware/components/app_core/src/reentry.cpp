// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include "reentry.hpp"

namespace prd::app_core::reentry {

void PrivilegedSession::begin(const std::uint32_t now_ms) {
    active_ = true;
    transaction_running_ = false;
    last_activity_ms_ = now_ms;
}

void PrivilegedSession::note_activity(const std::uint32_t now_ms) {
    if (!active_) {
        return;
    }

    last_activity_ms_ = now_ms;
}

void PrivilegedSession::set_transaction_running(const bool running, const std::uint32_t now_ms) {
    if (!active_) {
        return;
    }

    // ⬅️ THE SETTLEMENT IS THE RESUMPTION POINT. Accounting restarts when the transaction ends,
    // not from wherever the clock had reached when it began — owner ruling 1: "once that
    // transaction settles, idle accounting resumes". A person who has just watched a validation
    // succeed or fail is mid-task, and the window they get afterwards is the whole window.
    //
    // This cannot be used to extend a session without a person: a transaction starts only from an
    // accepted submission, which already refreshed the deadline on its own account.
    if (transaction_running_ && !running) {
        last_activity_ms_ = now_ms;
    }

    transaction_running_ = running;
}

void PrivilegedSession::end() {
    active_ = false;
    transaction_running_ = false;
    last_activity_ms_ = 0;
}

bool PrivilegedSession::expired(const std::uint32_t now_ms) const {
    if (!active_) {
        return false;
    }

    // A network being joined or a configuration being committed must not have its access point
    // taken away underneath it.
    if (transaction_running_) {
        return false;
    }

    return idle_for(now_ms) >= kIdleLifetimeMs;
}

std::uint32_t PrivilegedSession::idle_for(const std::uint32_t now_ms) const {
    // Unsigned subtraction, so a wrapping millisecond counter yields the true elapsed interval
    // rather than an enormous one. The counter wraps after 49 days; a session that appeared to
    // have been idle for a month at that moment would be stopped mid-use.
    const std::uint32_t elapsed = now_ms - last_activity_ms_;

    // ⬅️ A SECOND LINE OF DEFENCE, AND DELIBERATELY NOT THE FIRST. A `now_ms` that precedes the
    // recorded activity is not an elapsed interval at all, and unsigned subtraction reports it as
    // very nearly the whole 32-bit range — which is how Milestone 3.11 T4 tore down an access
    // point 8 ms after raising it. The real fix is upstream: app_core takes ONE time observation
    // per tick and passes it down, so a backwards value cannot arise (see monotonic_ms()).
    //
    // ⛔ THIS DOES NOT WEAKEN THE WRAP HANDLING ABOVE, WHICH IS THE POINT OF READING IT SIGNED.
    // A genuine wrap leaves `elapsed` small and positive — the true interval — while a backwards
    // reading leaves it near 2^32, which as a signed value is negative. The two cases look
    // identical to an unsigned comparison and are trivially separable to a signed one.
    if (static_cast<std::int32_t>(elapsed) < 0) {
        return 0U;
    }

    return elapsed;
}

AfterExpiry after_expiry(const bool configuration_is_valid, const ProvisioningDecision decision) {
    // ⛔ THE FAULT OUTRANKS THE RECORD, AND CHECKING IT FIRST IS THE WHOLE RULE. A faulted secret
    // beside a perfectly valid configuration still reaches DeferredToRecovery — decide_provisioning
    // weighs both records — so testing `configuration_is_valid` first would send exactly that
    // device to Inactive and leave it presenting as configured while it cannot connect at all.
    if (decision == ProvisioningDecision::DeferredToRecovery) {
        return {app_model::ProvisioningPhase::Inactive, true};
    }

    return {configuration_is_valid ? app_model::ProvisioningPhase::Inactive
                                   : app_model::ProvisioningPhase::SetupRequired,
            false};
}

} // namespace prd::app_core::reentry
