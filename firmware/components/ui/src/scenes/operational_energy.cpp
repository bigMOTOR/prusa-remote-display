// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include "scenes/operational_energy.hpp"

namespace prd::ui::scene {

std::optional<Energy> energy_for(const app_model::SubjectState state) {
    switch (state) {
    case app_model::SubjectState::Printing:
        return kEnergyPrinting;

    case app_model::SubjectState::Paused:
        return kEnergyPaused;

    case app_model::SubjectState::Completed:
        return kEnergyCompleted;

    case app_model::SubjectState::Preparing:
        return kEnergyPreparing;

    case app_model::SubjectState::Stopped:
        return kEnergyStopped;

    case app_model::SubjectState::Ready:
        return kEnergyReady;

    case app_model::SubjectState::AcquiringState:
        // NOT A MEMBER OF THE FAMILY, and therefore not on the axis at all.
        //
        // UIDD 004's Scope covers six states and AcquiringState is not among them; it belongs
        // to the State Acquisition & Provisioning Family (UIDD 003), which represents the
        // companion becoming able to represent the printer rather than the printer itself.
        // Production UI §9 gives it its own screen — Connecting — and scene_selection.hpp does
        // not route it to the operational composition.
        //
        // Empty rather than the minimum. The minimum is Ready: a real point on the axis with
        // an accepted resting form. Returning it here would make "outside the family"
        // indistinguishable from "carrying nothing", and §14's review test prohibits exactly
        // that kind of conflation — a non-member would be participating in the axis, which is
        // expressing a state rather than an energy.
        //
        // Listed rather than defaulted so that adding a subject state fails to compile until
        // it has been placed on the axis or deliberately kept off it.
        return std::nullopt;
    }

    // Unreachable for a valid enumerator; the switch above is total. Present because the
    // parameter is an enum and a value outside its enumerators is representable, and because
    // returning empty is the only safe answer to "what energy does an unknown state carry?".
    return std::nullopt;
}

} // namespace prd::ui::scene
