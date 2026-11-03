// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include "provisioning_decision.hpp"

namespace prd::app_core {

std::string_view log_label(const ProvisioningDecision decision) {
    switch (decision) {
    case ProvisioningDecision::NotRequired:
        return "not-required";

    case ProvisioningDecision::StartSetup:
        return "start-setup";

    case ProvisioningDecision::DeferredToRecovery:
        return "deferred-to-recovery";
    }

    return "not-required";
}

bool is_faulted(const config::LoadOutcome outcome) {
    switch (outcome) {
    case config::LoadOutcome::Absent:
    case config::LoadOutcome::Loaded:
    case config::LoadOutcome::LoadedFromPreviousGeneration:
        return false;

    case config::LoadOutcome::Invalid:
    case config::LoadOutcome::MigrationRequired:
    case config::LoadOutcome::StorageFailure:
        return true;
    }

    // Unreachable for a value of the enumeration. Treated as faulted rather than as an
    // invitation to provision, because the safe answer to "what is this record?" is not
    // "start advertising a setup network".
    return true;
}

bool is_faulted(const secrets::LoadOutcome outcome) {
    switch (outcome) {
    case secrets::LoadOutcome::Absent:
    case secrets::LoadOutcome::Loaded:
    case secrets::LoadOutcome::LoadedFromPreviousGeneration:
        return false;

    case secrets::LoadOutcome::Invalid:
    case secrets::LoadOutcome::MigrationRequired:
    case secrets::LoadOutcome::StorageFailure:
        return true;
    }

    return true;
}

ProvisioningDecision decide_provisioning(const config::LoadOutcome config_outcome,
                                         const secrets::LoadOutcome secret_outcome,
                                         const bool wifi_credentials_usable) {
    // A usable credential settles it, and it is checked FIRST on purpose. A device that can
    // reach its network must not be diverted into provisioning because some other record
    // beside it is unreadable — §14.1's "do not automatically erase configuration" applies
    // to the working half of a partly broken store too.
    if (wifi_credentials_usable) {
        return ProvisioningDecision::NotRequired;
    }

    if (is_faulted(config_outcome) || is_faulted(secret_outcome)) {
        return ProvisioningDecision::DeferredToRecovery;
    }

    // No usable credential, and nothing is broken: the credential was never written.
    return ProvisioningDecision::StartSetup;
}

} // namespace prd::app_core
