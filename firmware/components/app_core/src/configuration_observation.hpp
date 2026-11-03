// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "app_model/events.hpp"
#include "config/config.hpp"
#include "secrets/secrets.hpp"

// The composition root's one piece of policy, kept pure.
//
// INTERNAL HEADER of app_core, on the same footing as button_translation.hpp: it is
// compiled by the host suite so a decision made here is checkable, rather than hiding
// inside the boot sequence where nothing can reach it.
//
// WHY THIS EXISTS AT ALL. Owner ruling, 2026-08-15: `credential_generation` must represent
// the generation of the active SECRET record, not the non-secret ConfigV1 record — because
// a credential can change while nothing non-secret does, and sourcing the generation from
// the configuration record would leave that change invisible to §16.2's "retry only after
// configuration change". The same ruling fixes the ownership:
//
//   - `config` knows nothing about secrets or secret generations;
//   - `secrets` knows nothing about non-secret configuration;
//   - `app_core` combines the two independent reports.
//
// So neither component may assemble the observation, and this is the only place that sees
// both. `config::observe()` is gone; what it did lives here with the secret half added.
//
// ⛔ NO CREDENTIAL VALUE CROSSES THIS BOUNDARY. §24.3 and §13.5 permit the application
// state only has-credential flags and auth metadata, and the output type is a
// ConfigurationObservation, which has nowhere to put one. The secret report's values are
// read only to answer "is this present", never copied.

namespace prd::app_core {

// Combines the two reports into the observation dispatched to the store.
//
// The association is checked here for the same reason: only this function can see both the
// configuration's `wifi_profile_id` and the secret record's opaque `profile_id`. A secret
// record left behind by a previous profile does not count as credentials for the current
// one — otherwise a reprovisioned device would report Wi-Fi credentials it can no longer
// use.
[[nodiscard]] app_model::ConfigurationObservation
compose_configuration(const config::LoadReport& configuration, const secrets::LoadReport& secret);

} // namespace prd::app_core
