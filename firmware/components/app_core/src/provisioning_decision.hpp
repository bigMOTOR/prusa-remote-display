// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstdint>
#include <string_view>

#include "config/config.hpp"
#include "secrets/secrets.hpp"

// Whether a boot enters provisioning — Architecture Spec §14.1, owner rulings R1 and D2.
//
// PURE, AND HOST-TESTED. It reads two load outcomes and a flag and returns a decision; it
// starts nothing, logs nothing and touches no driver. That is the point: §14.1's entry
// conditions are product rules, and the alternative — an `if` in the middle of the boot
// sequence — is a product rule that can only be checked by booting a board.
//
// ⬅️ D2, OWNER RULING 2026-08-15: ONLY A MISSING CONFIGURATION ENTERS PROVISIONING
// AUTOMATICALLY. §14.1 lists four entry conditions and Milestone 3.5 implements exactly the
// first, in its narrowest reading. A record that is present but Invalid, one that needs a
// migration this firmware cannot perform, and a store that failed outright are all
// §14.1's *fourth* condition — "storage migration requires user intervention" — and belong
// to M3.10 with the rest of recovery. Until then they are reported and left alone.
//
// WHY THAT DISTINCTION IS WORTH A TYPE. Automatically provisioning on a corrupt record
// means a device that cannot read its configuration starts advertising an open-ish setup
// network and invites the user to re-enter credentials it may then also fail to store.
// §14.1's own rule — "Do not automatically erase configuration after temporary connection
// failures" — is the same instinct: a fault is not a reason to discard what is there.

namespace prd::app_core {

enum class ProvisioningDecision : std::uint8_t {
    // A usable Wi-Fi credential exists. The station starts; no access point.
    NotRequired,

    // Nothing has ever been provisioned. §14.1's first entry condition, and the only one
    // Milestone 3.5 acts on.
    StartSetup,

    // A stored record is faulted — Invalid, MigrationRequired or StorageFailure.
    //
    // ⬅️ MILESTONE 3.10 ACTS ON THIS: the device enters RecoveryMode, automatically, and stops
    // there. Owner Decision 3 reconciles §13.4's "unrecoverable non-secret corruption enters
    // provisioning/recovery mode" with the physical-presence boundary D2 and R1 were protecting:
    // the recovery STATE is automatic, the privileged setup surface is not. A deliberate long
    // press is still required before an access point is raised or anything becomes mutable.
    //
    // ⛔ AND NOTHING IS ERASED HERE. Least of all on StorageFailure: a store that has just failed
    // must never invite the firmware to rewrite it. §13.4's "migration failure must not erase
    // secrets automatically" and R10's erase policy both apply, and the factory-reset executor is
    // unreachable from this path by construction — it runs only from a confirmed reset.
    DeferredToRecovery,
};

[[nodiscard]] std::string_view log_label(ProvisioningDecision decision);

// True when an outcome represents a fault rather than an answer.
//
// Absent is not a fault: it is the first-boot case and the whole reason provisioning
// exists. LoadedFromPreviousGeneration is not a fault either — §13.3's staged write did
// exactly what it is for, and the device is running on a known-good record.
[[nodiscard]] bool is_faulted(config::LoadOutcome outcome);
[[nodiscard]] bool is_faulted(secrets::LoadOutcome outcome);

// `wifi_credentials_usable` is app_core's own resolution of whether the loaded secret
// actually carries a credential for the configured profile — the association rule the
// composition root already applies for ConfigurationObservation.
[[nodiscard]] ProvisioningDecision decide_provisioning(config::LoadOutcome config_outcome,
                                                       secrets::LoadOutcome secret_outcome,
                                                       bool wifi_credentials_usable);

} // namespace prd::app_core
