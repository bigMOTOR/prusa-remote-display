// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include "lifecycle.hpp"

#include "backoff.hpp"

namespace prd::network {

using app_model::NetworkPhase;

Step Lifecycle::settle(const NetworkPhase phase, const NextAction action,
                       const std::uint32_t retry_attempt) {
    Step step;
    step.phase_changed = phase != phase_;
    phase_ = phase;
    step.phase = phase;
    step.action = action;
    step.retry_attempt = retry_attempt;
    return step;
}

NetworkPhase Lifecycle::attempting_phase() const {
    // §7.4 lists both, and the distinction is the product's: a device that has never
    // reached the network is connecting, one that had it and lost it is reconnecting.
    return has_connected_ ? NetworkPhase::Reconnecting : NetworkPhase::Connecting;
}

void Lifecycle::reset() {
    phase_ = app_model::NetworkPhase::Disabled;
    transient_failures_ = 0;
    rejected_attempts_ = 0;
    has_connected_ = false;
}

Step Lifecycle::on(const Trigger trigger, const DisconnectCause cause) {
    switch (trigger) {
    case Trigger::CredentialsAbsent:
        // The un-provisioned device. The station is not started at all: bringing one up
        // with no SSID would manufacture a failure the user cannot act on, and §14.1
        // already has them in provisioning.
        transient_failures_ = 0;
        rejected_attempts_ = 0;
        has_connected_ = false;
        return settle(NetworkPhase::Disabled, NextAction::None, 0);

    case Trigger::Started:
        return settle(NetworkPhase::Starting, NextAction::Attempt, 0);

    case Trigger::Associating:
        return settle(attempting_phase(), NextAction::None, 0);

    case Trigger::Associated:
        // ASSOCIATION IS NOT CONNECTION. §7.4 carries an ipAddress, and a station with a
        // link but no lease cannot reach anything — calling that Connected would make every
        // later milestone's first request fail for a reason the state denies. The phase
        // stays where it is until GotIp.
        return settle(phase_, NextAction::None, 0);

    case Trigger::GotIp:
        // §11.3: "Reset failure count after a confirmed successful status response." This
        // is the network's equivalent, and it is also what re-arms the whole ladder.
        transient_failures_ = 0;
        rejected_attempts_ = 0;
        has_connected_ = true;
        return settle(NetworkPhase::Connected, NextAction::None, 0);

    case Trigger::LeaseLost:
        // ⛔ NOT AN ASSOCIATION FAILURE, AND THIS IS THIS CHECKPOINT'S CORRECTION. It used to fall
        // into the failure path below, which incremented §11.3's ladder for a DHCP problem and
        // told the adapter to reconnect a station that was still associated. §16.1 lists "DHCP
        // failure" among Wi-Fi errors but §16.2 retries by ASSOCIATION, and there is nothing to
        // re-associate: the link is up. The phase becomes attempting again — truthfully, because
        // a station with a link and no lease can reach nothing — and the counters are untouched,
        // so one physical outage is counted once.
        return settle(attempting_phase(), NextAction::None, 0);

    case Trigger::Disconnected:
        break;

    case Trigger::Stopped:
        return settle(NetworkPhase::Disabled, NextAction::None, 0);
    }

    // --- The failure paths ---------------------------------------------------

    if (trigger == Trigger::Disconnected && cause == DisconnectCause::CredentialsRejected) {
        ++rejected_attempts_;

        if (rejected_attempts_ >= policy_.rejected_attempt_limit) {
            // §16.2: "do not continuously retry at high frequency: ... invalid
            // configuration". Nothing but a configuration change can fix a rejected
            // password, so the station stops rather than burning the radio and hiding
            // provisioning behind a retry sequence.
            return settle(NetworkPhase::Unavailable, NextAction::GiveUp, 0);
        }

        return settle(attempting_phase(), NextAction::WaitBackoff, rejected_attempts_);
    }

    // Transient: association timeout, DHCP failure, beacon loss, the access point going
    // away. §16.2 retries these automatically and §11.3 bounds how fast.
    ++transient_failures_;

    // Once the ladder has reached its maximum step the device has been failing for the
    // whole schedule, and saying "connecting" would stop being true. Retries continue at
    // the capped delay — this is a change of what the product reports, not of what it does.
    const NetworkPhase phase =
        schedule_saturated(transient_failures_) ? NetworkPhase::Unavailable : attempting_phase();

    return settle(phase, NextAction::WaitBackoff, transient_failures_);
}

} // namespace prd::network
