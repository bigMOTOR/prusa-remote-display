// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstdint>
#include <string_view>

#include "app_model/presentation.hpp"

// Scene selection — Architecture Spec §5.12 `ui_controller`, §17.2's scene model.
//
// The one decision this file makes: which composition a resolved Presentation is
// drawn as. It is a pure function of the projection's output, contains no LVGL
// header, and is compiled into the host test build — so the mapping is asserted over
// every layer and subject state rather than discovered on the panel.
//
// UIAD 001 §12's priority order is ALREADY RESOLVED before this runs. The projection
// reduced seven competing layers to one `Presentation::layer`; this only names the
// composition for it. Re-deriving priority here would create a second opinion about
// the same rule.
//
// ONE COMPOSITION FOR THE WHOLE OPERATIONAL FAMILY. Production UI §9 says Paused,
// Completed and Stopped have "visible components identical to Printing"; Ready and
// Preparing use the same frame with the progress geometry in its resting form; and the
// accepted continuity artwork is titled "one composition, one state flow". So the six
// operational subject states resolve to one scene whose content varies, and what varies is
// decided by scenes/operational_content.hpp — pure, and host-tested per state.
//
// Everything outside the operational family still resolves to `Unimplemented`, which draws
// the persistent root and nothing else. That is deliberately not a placeholder screen:
// Production UI §9 forbids simulated data three times, and a "coming soon" panel would be
// exactly that.

namespace prd::ui {

// Sized explicitly: it is copied into the controller's report and compared on every
// revision, and there is no reason for a two-value tag to occupy a machine word.
enum class Scene : std::uint8_t {
    // The persistent root only. No composition exists for this presentation yet.
    Unimplemented,

    // Production UI §9's operational family: Ready, Preparing, Printing, Paused, Completed
    // and Stopped, drawn as one composition.
    Operational,

    // The provisional provisioning composition — Milestone 3.6, owner ruling R3. Selected
    // for SetupRequired and SetupActive only, and from Milestone 3.10 for RecoveryMode.
    Provisioning,

    // Production UI §9's Factory Reset screen — Milestone 3.10. Both halves of the operation: the
    // confirmation §5.6 requires to be ON SCREEN, and the progress that follows it.
    //
    // ⬅️ ITS OWN SCENE BECAUSE §9 GIVES IT ITS OWN SCREEN, and because the provisioning composition
    // is frozen by owner rulings R3 and D11. Same pattern as the two above: one scene per §9 screen
    // family.
    FactoryReset,
    // Acquisition or a positively classified access failure, without job telemetry.
    Access,

    // Production UI §9's firmware-update family. It is deliberately separate from the
    // operational composition: update progress is not printer progress and never uses the ring.
    FirmwareUpdate,
};

[[nodiscard]] Scene select(const app_model::Presentation& presentation);

[[nodiscard]] std::string_view log_label(Scene scene);

} // namespace prd::ui
