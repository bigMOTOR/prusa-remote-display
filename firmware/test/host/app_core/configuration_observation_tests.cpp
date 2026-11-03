// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include "configuration_observation.hpp"

#include <cstdint>

#include <gtest/gtest.h>

#include "app_model/device_data.hpp"
#include "config/config.hpp"
#include "secrets/secrets.hpp"

// The composition root's one piece of policy.
//
// THE RULING THIS FILE PINS: `credential_generation` is the generation of the ACTIVE SECRET
// RECORD, never the configuration record's. The failure it prevents is specific — a user
// entering a new API key for the same printer at the same address changes no non-secret
// field, so a configuration-sourced generation would never move and §16.2's "retry only
// after configuration change" would never fire for the one case that most needs it.
//
// It also pins the boundary: no credential VALUE may reach the observation, and a secret
// record left behind by a previous profile is not credentials for the current one.

namespace {

using prd::app_core::compose_configuration;
using prd::app_model::AuthMethod;
using prd::app_model::ConfigurationStatus;

[[nodiscard]] prd::config::LoadReport configured(const std::uint32_t generation = 1,
                                                 const std::uint32_t profile = 5) {
    prd::config::ConfigV1 value = prd::config::defaults();
    value.wifi_profile_id = profile;
    value.printer_host.assign("printer.local");
    value.printer_port = 80;
    value.printer_alias.assign("Workshop");
    value.auth_method = AuthMethod::ApiKey;

    return prd::config::LoadReport{prd::config::LoadOutcome::Loaded, value, generation};
}

[[nodiscard]] prd::secrets::LoadReport with_credentials(const std::uint32_t generation,
                                                        const std::uint32_t profile = 5) {
    prd::secrets::SecretConfigV1 value = prd::secrets::defaults();
    value.profile_id = profile;
    value.wifi_ssid.assign("Workshop-5G");
    value.wifi_password.assign("battery-staple");
    value.printer_api_key.assign("abc123");

    return prd::secrets::LoadReport{prd::secrets::LoadOutcome::Loaded, value, generation};
}

[[nodiscard]] prd::secrets::LoadReport no_credentials() {
    return prd::secrets::LoadReport{};
}

// --- The ruling ----------------------------------------------------------------

TEST(ConfigurationObservationTests, CredentialGenerationComesFromTheSecretRecord) {
    const auto observation = compose_configuration(configured(2), with_credentials(7));

    EXPECT_EQ(observation.credential_generation, 7U) << "not the configuration's generation";
}

// The exact case the ruling exists for: a new API key for the same printer at the same
// address moves no non-secret field, so only the secret generation can make it observable.
TEST(ConfigurationObservationTests, ANewCredentialMovesTheGenerationWithNoConfigurationChange) {
    const auto before = compose_configuration(configured(3), with_credentials(1));
    const auto after = compose_configuration(configured(3), with_credentials(2));

    EXPECT_EQ(before.printer_address, after.printer_address);
    EXPECT_EQ(before.auth_method, after.auth_method);
    EXPECT_NE(before.credential_generation, after.credential_generation);
}

// And the converse: a configuration change with untouched credentials must not pretend the
// credential moved.
TEST(ConfigurationObservationTests, AConfigurationChangeDoesNotMoveTheCredentialGeneration) {
    const auto before = compose_configuration(configured(3), with_credentials(4));
    const auto after = compose_configuration(configured(4), with_credentials(4));

    EXPECT_EQ(before.credential_generation, after.credential_generation);
}

TEST(ConfigurationObservationTests, NoCredentialRecordMeansGenerationZero) {
    const auto observation = compose_configuration(configured(9), no_credentials());

    EXPECT_EQ(observation.credential_generation, 0U);
    EXPECT_FALSE(observation.has_wifi_credentials);
}

// --- The association -------------------------------------------------------------

TEST(ConfigurationObservationTests, CredentialsCountOnlyForTheConfiguredProfile) {
    const auto matched = compose_configuration(configured(1, 5), with_credentials(1, 5));
    EXPECT_TRUE(matched.has_wifi_credentials);

    // A record left behind by a previous provisioning.
    const auto stale = compose_configuration(configured(1, 6), with_credentials(1, 5));
    EXPECT_FALSE(stale.has_wifi_credentials)
        << "a reprovisioned device must not claim credentials it cannot use";
}

TEST(ConfigurationObservationTests, AConfigurationWithNoProfileHasNoWifiCredentials) {
    prd::config::LoadReport report = configured();
    report.value.wifi_profile_id = 0;

    EXPECT_FALSE(compose_configuration(report, with_credentials(1, 5)).has_wifi_credentials);
}

// --- Status resolution ------------------------------------------------------------

TEST(ConfigurationObservationTests, AnAbsentConfigurationBecomesMissing) {
    const auto observation = compose_configuration(prd::config::LoadReport{}, no_credentials());

    EXPECT_EQ(observation.status, ConfigurationStatus::Missing);
}

// A device that cannot read its credentials cannot be said to have a valid configuration,
// whatever the non-secret record says.
TEST(ConfigurationObservationTests, AnUnreadableSecretStoreOutranksAValidConfiguration) {
    prd::secrets::LoadReport failed;
    failed.outcome = prd::secrets::LoadOutcome::StorageFailure;

    EXPECT_EQ(compose_configuration(configured(), failed).status,
              ConfigurationStatus::StorageFailure);
}

TEST(ConfigurationObservationTests, EveryConfigurationOutcomeMapsToItsStatus) {
    const struct {
        prd::config::LoadOutcome outcome;
        ConfigurationStatus status;
    } cases[] = {
        {prd::config::LoadOutcome::Absent, ConfigurationStatus::Missing},
        {prd::config::LoadOutcome::Invalid, ConfigurationStatus::Invalid},
        {prd::config::LoadOutcome::MigrationRequired, ConfigurationStatus::MigrationRequired},
        {prd::config::LoadOutcome::StorageFailure, ConfigurationStatus::StorageFailure},
        {prd::config::LoadOutcome::Loaded, ConfigurationStatus::Valid},
        {prd::config::LoadOutcome::LoadedFromPreviousGeneration, ConfigurationStatus::Valid},
    };

    for (const auto& entry : cases) {
        prd::config::LoadReport report = configured();
        report.outcome = entry.outcome;

        EXPECT_EQ(compose_configuration(report, with_credentials(1)).status, entry.status)
            << "outcome " << static_cast<int>(entry.outcome);
    }
}

// A configuration that could not be used must not carry values either — the same "never
// accidental defaults" property the record layer holds, restated at the boundary.
TEST(ConfigurationObservationTests, AnUnusableConfigurationCarriesNothing) {
    prd::config::LoadReport report = configured();
    report.outcome = prd::config::LoadOutcome::Invalid;

    const auto observation = compose_configuration(report, with_credentials(3));

    EXPECT_TRUE(observation.printer_address.empty());
    EXPECT_FALSE(observation.has_printer_configuration);
    EXPECT_FALSE(observation.printer_configuration_pending);
    EXPECT_FALSE(observation.has_wifi_credentials);
    EXPECT_EQ(observation.credential_generation, 0U);
}

// --- R8's three-way distinction, unchanged by the secret half ---------------------

TEST(ConfigurationObservationTests, TheThreeWayPrinterDistinctionIsUnaffectedByCredentials) {
    for (const bool has_credentials : {false, true}) {
        const auto secret = has_credentials ? with_credentials(1) : no_credentials();

        prd::config::LoadReport none = configured();
        none.value.printer_host.clear();
        none.value.printer_port = 0;
        none.value.auth_method = AuthMethod::None;
        const auto absent = compose_configuration(none, secret);
        EXPECT_FALSE(absent.has_printer_configuration);
        EXPECT_FALSE(absent.printer_configuration_pending);

        const auto pending = compose_configuration(configured(), secret);
        EXPECT_FALSE(pending.has_printer_configuration);
        EXPECT_TRUE(pending.printer_configuration_pending);

        prd::config::LoadReport promoted = configured();
        promoted.value.printer_validated = true;
        const auto active = compose_configuration(promoted, secret);
        EXPECT_TRUE(active.has_printer_configuration);
        EXPECT_FALSE(active.printer_configuration_pending);
    }
}

// --- §24.3 / §13.5 at the boundary -------------------------------------------------

// The observation is the only thing that reaches AppState. §24.3 permits "has-credential
// flags and auth metadata" and nothing else — a type that cannot hold a credential is a
// stronger guarantee than a rule that says it must not, and this pins the fields that DO
// travel so a future addition has to be deliberate.
TEST(ConfigurationObservationTests, TheObservationCarriesFlagsAndMetadataOnly) {
    const auto observation = compose_configuration(configured(), with_credentials(2));

    EXPECT_TRUE(observation.has_wifi_credentials);
    EXPECT_EQ(observation.credential_generation, 2U);
    EXPECT_EQ(observation.auth_method, AuthMethod::ApiKey);

    // Non-secret by §5.9, and already in ConfigurationState for that reason.
    EXPECT_EQ(observation.printer_address.view(), "printer.local");
    EXPECT_EQ(observation.printer_alias.view(), "Workshop");

    EXPECT_EQ(observation.schema_version, prd::config::kSchemaVersion);
}

} // namespace
