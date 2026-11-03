// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstdint>

#include "app_model/device_data.hpp"

// The station phase machine — Architecture Spec §7.4, §16.1, §16.2.
//
// INTERNAL HEADER of the network component. Pure — no ESP-IDF, no driver, no clock — so
// every transition below is host-tested against the same code the firmware runs. The
// adapter's job is to translate driver events into Triggers and to obey the returned
// action; it decides nothing.

namespace prd::network {

// What the driver reported, in this component's terms rather than ESP-IDF's.
enum class Trigger : std::uint8_t {
    // There is no credential to connect with. Not a failure — it is the un-provisioned
    // device, which ConfigurationStatus::Missing has already put into SetupRequired.
    CredentialsAbsent,

    // The driver is up and the station is configured.
    Started,

    // An association attempt has begun.
    Associating,

    // The link is up. NOT YET CONNECTED — see NetworkPhase::Connected below.
    Associated,

    GotIp,

    // ⛔ THE LEASE WENT AND THE LINK DID NOT — IP_EVENT_STA_LOST_IP with no accompanying
    // DISCONNECTED. NOT an association failure: it must not advance §11.3's ladder, and the
    // station must not be reconnected while it is still associated. It was `LostIp` and fell
    // through to the failure path below until this checkpoint, which counted one physical outage
    // twice and answered a DHCP problem with esp_wifi_connect().
    LeaseLost,

    Disconnected,
    Stopped,
};

// §16.1's Wi-Fi errors, reduced to the distinction that changes behaviour.
//
// §16.2 draws the line and it is the only one that matters here: "retry automatically:
// transient Wi-Fi loss"; "do not continuously retry at high frequency: ... invalid
// configuration". Association timeouts, DHCP failures and beacon loss are the first;
// credentials the access point rejected are the second.
enum class DisconnectCause : std::uint8_t {
    Transient,
    CredentialsRejected,
};

enum class NextAction : std::uint8_t {
    // Nothing to do; the driver is where it should be.
    None,

    // Attempt a connection now.
    Attempt,

    // Attempt again after backoff::delay_for(retry_attempt, …).
    WaitBackoff,

    // STOP TRYING. §16.2's "do not continuously retry ... invalid configuration": the
    // credentials were rejected and nothing but a configuration change can fix that. The
    // adapter must not schedule another attempt; `credential_generation` moving is what
    // makes a real change observable.
    GiveUp,
};

struct Step {
    app_model::NetworkPhase phase{app_model::NetworkPhase::Disabled};
    NextAction action{NextAction::None};

    // 1-based, meaningful when `action` is WaitBackoff.
    std::uint32_t retry_attempt{0};

    // Whether the phase differs from the one before this trigger.
    //
    // THE ONLY THING THE ADAPTER MAY PUBLISH ON. UI Behaviour Decision 001 §4 is binding:
    // "Routine polling, retries and small telemetry changes shall not become visible." A
    // retry that changes nothing must not wake the store, so a Step that did not change the
    // phase produces no observation.
    bool phase_changed{false};
};

class Lifecycle {
  public:
    struct Policy {
        // How many rejected-credential attempts before the station gives up.
        //
        // A WORKING BASELINE, NOT ACCEPTED PRODUCT POLICY — owner ruling, 2026-08-15, which
        // also required that the sequence stay responsive so provisioning and recovery are
        // not hidden behind a long retry. Three attempts run 2 s + 4 s of backoff, so the
        // device reports Unavailable within roughly six seconds of a wrong password rather
        // than climbing §11.3's whole ladder first.
        std::uint32_t rejected_attempt_limit{3};
    };

    Lifecycle() = default;
    explicit Lifecycle(Policy policy) : policy_{policy} {}

    [[nodiscard]] Step on(Trigger trigger, DisconnectCause cause = DisconnectCause::Transient);

    // Returns the machine to its un-started state, for a NEW credential set.
    //
    // ⬅️ WHY THIS EXISTS. §16.2's GiveUp is terminal ON PURPOSE: rejected credentials must
    // not be retried, and only "a configuration change" may re-arm them. A second Milestone
    // 3.8 Wi-Fi submission IS that configuration change, and without a way to say so the
    // machine stays in Unavailable/GiveUp with its rejected-attempt count at the limit and
    // never attempts the corrected password. That is exactly what failed hardware
    // acceptance.
    //
    // It clears the counters, the phase and has_connected_ — nothing carries over, because
    // none of it describes the credential now being tried.
    void reset();

    [[nodiscard]] app_model::NetworkPhase phase() const {
        return phase_;
    }

    // Transient failures since the last successful IP acquisition. Drives the backoff
    // ladder; distinct from NetworkState::consecutive_failures, which the reducer maintains
    // and which counts product-visible transitions into Unavailable.
    [[nodiscard]] std::uint32_t transient_failures() const {
        return transient_failures_;
    }

    [[nodiscard]] std::uint32_t rejected_attempts() const {
        return rejected_attempts_;
    }

    // Whether an IP has ever been acquired for this credential set. It is what separates
    // NetworkPhase::Connecting from Reconnecting — §7.4 lists both, and a device that has
    // never reached the network is in a different product situation from one that has lost
    // a network it had.
    [[nodiscard]] bool has_connected() const {
        return has_connected_;
    }

  private:
    [[nodiscard]] Step settle(app_model::NetworkPhase phase, NextAction action,
                              std::uint32_t retry_attempt);

    [[nodiscard]] app_model::NetworkPhase attempting_phase() const;

    Policy policy_{};

    app_model::NetworkPhase phase_{app_model::NetworkPhase::Disabled};
    std::uint32_t transient_failures_{0};
    std::uint32_t rejected_attempts_{0};
    bool has_connected_{false};
};

} // namespace prd::network
