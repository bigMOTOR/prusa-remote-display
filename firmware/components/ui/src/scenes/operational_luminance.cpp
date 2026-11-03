// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include "scenes/operational_luminance.hpp"

#include <algorithm>

#include "theme/tokens.hpp"

namespace prd::ui::scene {

std::int32_t body_level(const Energy energy) {
    // Clamped rather than asserted, matching form_geometry(). §11 makes
    // motion traverse the axis continuously, and an interpolator that ran a fraction past an
    // endpoint should produce the endpoint rather than an extrapolation — which at this end
    // would mean a composition dimmer than §4's floor allows.
    const Energy clamped = std::clamp(energy, kEnergyMin, kEnergyMax);

    // The same interpolation the geometry uses, so the shape and the body move together along the
    // axis by construction rather than by coincidence. §1's "one composition observed at six
    // energies" is only true if they share one parameter.
    //
    // ⬅️ THE ACCENT IS NO LONGER ONE OF THEM. §5.1, as ruled on 2026-09-01, gives the operational
    // form one accent at every energy, so the form is now under NO function of this axis except
    // its shape. This ramp's scope is unchanged: achromatic text and telemetry, never the form.
    return theme::kBodyLevelRest +
           (((theme::kBodyLevelFull - theme::kBodyLevelRest) * clamped) / kEnergyMax);
}

} // namespace prd::ui::scene
