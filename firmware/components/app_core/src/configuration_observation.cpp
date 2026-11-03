// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include "configuration_observation.hpp"

namespace prd::app_core {
namespace {

// Whether the secret record was read at all, whatever it contained.
[[nodiscard]] bool secret_is_readable(const secrets::LoadOutcome outcome) {
    switch (outcome) {
    case secrets::LoadOutcome::Loaded:
    case secrets::LoadOutcome::LoadedFromPreviousGeneration:
        return true;

    case secrets::LoadOutcome::Absent:
    case secrets::LoadOutcome::Invalid:
    case secrets::LoadOutcome::MigrationRequired:
    case secrets::LoadOutcome::StorageFailure:
        return false;
    }

    return false;
}

} // namespace

app_model::ConfigurationObservation compose_configuration(const config::LoadReport& configuration,
                                                          const secrets::LoadReport& secret) {
    app_model::ConfigurationObservation observation;

    // THE STORE COULD NOT BE READ AT ALL. A device that cannot read its credentials cannot
    // be said to have a valid configuration, whatever the non-secret record says — so this
    // outranks the configuration's own status rather than being folded into it.
    if (secret.outcome == secrets::LoadOutcome::StorageFailure) {
        observation.status = app_model::ConfigurationStatus::StorageFailure;
        return observation;
    }

    switch (configuration.outcome) {
    case config::LoadOutcome::Absent:
        // §14.1's entry condition — "no valid configuration exists". The reducer turns this
        // into ProvisioningPhase::SetupRequired.
        observation.status = app_model::ConfigurationStatus::Missing;
        return observation;

    case config::LoadOutcome::Invalid:
        observation.status = app_model::ConfigurationStatus::Invalid;
        return observation;

    case config::LoadOutcome::MigrationRequired:
        observation.status = app_model::ConfigurationStatus::MigrationRequired;
        return observation;

    case config::LoadOutcome::StorageFailure:
        observation.status = app_model::ConfigurationStatus::StorageFailure;
        return observation;

    case config::LoadOutcome::Loaded:
    case config::LoadOutcome::LoadedFromPreviousGeneration:
        break;
    }

    const config::ConfigV1& value = configuration.value;

    observation.status = app_model::ConfigurationStatus::Valid;
    observation.schema_version = config::kSchemaVersion;

    // ⬅️ THE RULING. The generation of the ACTIVE SECRET RECORD, never the configuration's.
    // 0 when no credential record was loaded, which is a real and distinguishable state.
    observation.credential_generation = secret.generation;

    // The credentials must belong to the CURRENT profile. A record left behind by a
    // previous provisioning carries the old handle, and counting it would make a
    // reprovisioned device claim credentials it cannot use.
    //
    // Only the presence of the SSID is read — never its value, and never the password's.
    const bool associated = secret_is_readable(secret.outcome) && value.wifi_profile_id != 0 &&
                            secret.value.profile_id == value.wifi_profile_id;

    observation.has_wifi_credentials = associated && secrets::presence_of(secret.value).wifi_ssid;

    // R8's three-way distinction, unchanged and still derived from the non-secret record
    // alone: whether a printer has been VALIDATED is a configuration fact, not a credential
    // one.
    const bool printer_configured = !value.printer_host.empty();
    observation.has_printer_configuration = printer_configured && value.printer_validated;
    observation.printer_configuration_pending = printer_configured && !value.printer_validated;

    observation.printer_address = value.printer_host;
    observation.printer_alias = value.printer_alias;
    observation.auth_method = value.auth_method;

    return observation;
}

} // namespace prd::app_core
