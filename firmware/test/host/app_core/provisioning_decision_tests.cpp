// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include <vector>

#include <gtest/gtest.h>

#include "provisioning_decision.hpp"

// §14.1's entry conditions, in the narrow reading owner ruling D2 approved for M3.5:
// ONLY a missing configuration provisions automatically. Invalid, MigrationRequired and
// StorageFailure are M3.10's recovery, and until it exists they must do nothing.

namespace prd::app_core {
namespace {

using ConfigOutcome = config::LoadOutcome;
using SecretOutcome = secrets::LoadOutcome;

// Every outcome, so a new enumerator added later cannot quietly go untested: the
// exhaustiveness tests below iterate these lists.
const std::vector<ConfigOutcome> kConfigOutcomes{
    ConfigOutcome::Absent,
    ConfigOutcome::Loaded,
    ConfigOutcome::LoadedFromPreviousGeneration,
    ConfigOutcome::Invalid,
    ConfigOutcome::MigrationRequired,
    ConfigOutcome::StorageFailure,
};

const std::vector<SecretOutcome> kSecretOutcomes{
    SecretOutcome::Absent,
    SecretOutcome::Loaded,
    SecretOutcome::LoadedFromPreviousGeneration,
    SecretOutcome::Invalid,
    SecretOutcome::MigrationRequired,
    SecretOutcome::StorageFailure,
};

} // namespace

// The first boot the whole checkpoint exists for.
TEST(ProvisioningDecisionTests, AVirginDeviceStartsSetup) {
    EXPECT_EQ(decide_provisioning(ConfigOutcome::Absent, SecretOutcome::Absent, false),
              ProvisioningDecision::StartSetup);
}

// A configuration record can exist while the credential beside it was never written.
TEST(ProvisioningDecisionTests, AConfigurationWithNoCredentialStartsSetup) {
    EXPECT_EQ(decide_provisioning(ConfigOutcome::Loaded, SecretOutcome::Absent, false),
              ProvisioningDecision::StartSetup);
}

TEST(ProvisioningDecisionTests, AUsableCredentialDoesNotProvision) {
    EXPECT_EQ(decide_provisioning(ConfigOutcome::Loaded, SecretOutcome::Loaded, true),
              ProvisioningDecision::NotRequired);
}

// §13.3's staged write did exactly what it exists for. The device is running on a
// known-good record, which is a success, not a fault.
TEST(ProvisioningDecisionTests, APreviousGenerationIsNotAFault) {
    EXPECT_EQ(decide_provisioning(ConfigOutcome::LoadedFromPreviousGeneration,
                                  SecretOutcome::LoadedFromPreviousGeneration, true),
              ProvisioningDecision::NotRequired);

    EXPECT_EQ(decide_provisioning(ConfigOutcome::LoadedFromPreviousGeneration,
                                  SecretOutcome::Absent, false),
              ProvisioningDecision::StartSetup);
}

// ⬅️ D2, and the reason it is a ruling rather than a detail. A corrupt record must not put
// the device into provisioning: it would invite the user to re-enter credentials into a
// store that may fail the same way, and §14.1 forbids discarding configuration over a
// fault.
TEST(ProvisioningDecisionTests, AFaultedRecordDefersToRecoveryRatherThanProvisioning) {
    EXPECT_EQ(decide_provisioning(ConfigOutcome::Invalid, SecretOutcome::Absent, false),
              ProvisioningDecision::DeferredToRecovery);

    EXPECT_EQ(decide_provisioning(ConfigOutcome::MigrationRequired, SecretOutcome::Absent, false),
              ProvisioningDecision::DeferredToRecovery);

    EXPECT_EQ(decide_provisioning(ConfigOutcome::StorageFailure, SecretOutcome::Absent, false),
              ProvisioningDecision::DeferredToRecovery);
}

TEST(ProvisioningDecisionTests, AFaultedSecretAlsoDefersToRecovery) {
    EXPECT_EQ(decide_provisioning(ConfigOutcome::Loaded, SecretOutcome::Invalid, false),
              ProvisioningDecision::DeferredToRecovery);

    EXPECT_EQ(decide_provisioning(ConfigOutcome::Loaded, SecretOutcome::MigrationRequired, false),
              ProvisioningDecision::DeferredToRecovery);

    EXPECT_EQ(decide_provisioning(ConfigOutcome::Loaded, SecretOutcome::StorageFailure, false),
              ProvisioningDecision::DeferredToRecovery);
}

// A device that can reach its network is not diverted because some other record is
// unreadable. The working half of a partly broken store keeps working.
TEST(ProvisioningDecisionTests, AUsableCredentialWinsOverAFaultElsewhere) {
    for (const ConfigOutcome config_outcome : kConfigOutcomes) {
        for (const SecretOutcome secret_outcome : kSecretOutcomes) {
            EXPECT_EQ(decide_provisioning(config_outcome, secret_outcome, true),
                      ProvisioningDecision::NotRequired)
                << "config=" << config::log_label(config_outcome)
                << " secret=" << secrets::log_label(secret_outcome);
        }
    }
}

// The invariant that matters for M3.5's blast radius: with no usable credential, setup
// starts if and only if NOTHING is faulted.
TEST(ProvisioningDecisionTests, SetupStartsExactlyWhenNothingIsFaulted) {
    for (const ConfigOutcome config_outcome : kConfigOutcomes) {
        for (const SecretOutcome secret_outcome : kSecretOutcomes) {
            const bool faulted = is_faulted(config_outcome) || is_faulted(secret_outcome);
            const ProvisioningDecision decision =
                decide_provisioning(config_outcome, secret_outcome, false);

            EXPECT_EQ(decision, faulted ? ProvisioningDecision::DeferredToRecovery
                                        : ProvisioningDecision::StartSetup)
                << "config=" << config::log_label(config_outcome)
                << " secret=" << secrets::log_label(secret_outcome);
        }
    }
}

// Without a usable credential the answer is never "carry on normally": a device that
// cannot join anything must either provision or wait for recovery.
TEST(ProvisioningDecisionTests, NoCredentialNeverResolvesToNotRequired) {
    for (const ConfigOutcome config_outcome : kConfigOutcomes) {
        for (const SecretOutcome secret_outcome : kSecretOutcomes) {
            EXPECT_NE(decide_provisioning(config_outcome, secret_outcome, false),
                      ProvisioningDecision::NotRequired)
                << "config=" << config::log_label(config_outcome)
                << " secret=" << secrets::log_label(secret_outcome);
        }
    }
}

TEST(ProvisioningDecisionTests, AbsentIsNotAFaultOnEitherRecord) {
    EXPECT_FALSE(is_faulted(ConfigOutcome::Absent));
    EXPECT_FALSE(is_faulted(SecretOutcome::Absent));
}

TEST(ProvisioningDecisionTests, EveryDecisionHasADistinctLabel) {
    EXPECT_NE(log_label(ProvisioningDecision::NotRequired),
              log_label(ProvisioningDecision::StartSetup));
    EXPECT_NE(log_label(ProvisioningDecision::StartSetup),
              log_label(ProvisioningDecision::DeferredToRecovery));
    EXPECT_NE(log_label(ProvisioningDecision::NotRequired),
              log_label(ProvisioningDecision::DeferredToRecovery));
}

} // namespace prd::app_core
