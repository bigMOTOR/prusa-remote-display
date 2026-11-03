// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include "scenes/unavailable_motion.hpp"

#include <algorithm>

#include "scenes/operational_accent.hpp"
#include "scenes/operational_energy.hpp"

namespace prd::ui::scene {
namespace {

// Progress is carried in permille throughout, so the whole law is integer arithmetic and the
// endpoints are exact. Named rather than written as literals because std::clamp() deduces one type
// from all three arguments, and on the target std::int32_t is `long int` while a bare 0 is `int`.
constexpr std::int32_t kProgressMin = 0;
constexpr std::int32_t kProgressMax = 1000;

// LVGL's fully opaque alpha, as an integer to scale by.
constexpr std::int32_t kOpaque = 255;

// Smoothstep, in integer arithmetic. Matches the restrained easing the accepted storyboard was
// rendered with.
//
// The intermediate does not overflow: x²(3000 − 2x) rises monotonically over [0, 1000] and its
// maximum is at the top of the range, 1000² × 1000 = 1e9, well inside a 32-bit signed integer.
[[nodiscard]] std::int32_t ease(const std::int32_t progress) {
    const std::int32_t pos = std::clamp(progress, kProgressMin, kProgressMax);
    return (pos * pos * (3000 - (2 * pos))) / (kProgressMax * kProgressMax);
}

[[nodiscard]] std::int32_t progress_permille(const std::uint32_t elapsed_ms,
                                             const std::uint32_t from_ms,
                                             const std::uint32_t to_ms) {
    if (elapsed_ms <= from_ms) {
        return kProgressMin;
    }

    // ⬅️ AND EVERY LATER MOMENT LANDS HERE, WHICH IS WHY THE MULTIPLY BELOW CANNOT OVERFLOW. LVGL's
    // tick wraps at 2^32 ms, so an elapsed value can be enormous; past `to_ms` it is saturated
    // before any arithmetic, leaving the product bounded by the span itself.
    if (elapsed_ms >= to_ms) {
        return kProgressMax;
    }

    const std::uint32_t span = to_ms - from_ms;
    return static_cast<std::int32_t>(((elapsed_ms - from_ms) * 1000U) / span);
}

} // namespace

app_model::SubjectState subject_state_of(const app_model::LossOriginForm origin) {
    switch (origin) {
    case app_model::LossOriginForm::Ready:
        return app_model::SubjectState::Ready;
    case app_model::LossOriginForm::Stopped:
        return app_model::SubjectState::Stopped;
    case app_model::LossOriginForm::Preparing:
        return app_model::SubjectState::Preparing;
    case app_model::LossOriginForm::Completed:
        return app_model::SubjectState::Completed;
    case app_model::LossOriginForm::Paused:
        return app_model::SubjectState::Paused;
    case app_model::LossOriginForm::Printing:
        return app_model::SubjectState::Printing;

    // ⬅️ TRUTHFUL, AND IT IS WHAT KEEPS THE FALLBACK HONEST. This origin names a loss that carried
    // no operational form, so the state it reports is the one that has none — and energy_for()
    // deliberately keeps AcquiringState off the axis, so origin_energy() below resolves it through
    // its documented `value_or(kEnergyReady)` fallback rather than through a second table.
    case app_model::LossOriginForm::NoOperationalForm:
        return app_model::SubjectState::AcquiringState;
    }

    return app_model::SubjectState::AcquiringState;
}

Energy origin_energy(const app_model::LossOriginForm origin) {
    // ⛔ THROUGH THE PRODUCTION AXIS, NOT A TABLE OF ITS OWN. energy_for() is the law the operational
    // family is drawn by, so the shape this motion begins at IS the shape the panel was showing —
    // Preparing starts at Preparing's crown, Paused at Paused's, Completed at Completed's. Two
    // buckets made four of the six jump to an endpoint at the instant of loss.
    //
    // ⬅️ AND THE MODEL STILL CARRIES NO VALUE. The origin names a position; everything reconstructed
    // from it is structure. No progress, no temperature, no identity crosses this function.
    const std::optional<Energy> energy = energy_for(subject_state_of(origin));

    // ⬅️ THE FALLBACK IS REACHED, NOT DEAD. Six of the seven origins name a state that IS on the
    // axis, asserted for all six by the host tests. The seventh — NoOperationalForm — names one
    // that deliberately is not, so it lands here: the resting end is the truthful "no form was
    // carried" answer, and a render task must draw something. It is a fallback with a test, not an
    // unreachable branch.
    return energy.value_or(kEnergyReady);
}

bool outgoing_visible(const std::uint32_t elapsed_ms) {
    return elapsed_ms < kUnavailableOutgoingMs;
}

std::uint32_t copy_from_ms(const app_model::LossOriginForm origin) {
    // ⛔ WHETHER THE WORDS MUST WAIT IS A GEOMETRIC FACT, NOT A CATEGORY OF STATE. A descent that
    // begins at kEnergyPreparing or above passes straight THROUGH the copy's line box on its way
    // down, so those words wait for the shape to arrive.
    //
    // Measured against scene::crosses() over the whole axis: the lowest energy whose shape crosses
    // that box is 371, so Preparing (400) and every origin above it crosses, while Stopped (200) and
    // Ready (0) never do. The threshold is written as the family's own constant rather than as 371,
    // and the firing control checks all six origins rather than trusting either number.
    // ⬅️ B2, READY ONLY. Its words wait for the pivot, because its whole first half is the lift and
    // §9 permits no copy during it. Every other origin is untouched by this branch.
    if (origin == app_model::LossOriginForm::Ready) {
        return kReadyPivotMs;
    }

    if (origin_energy(origin) >= kEnergyPreparing) {
        return kUnavailableFormMs;
    }

    // A FORM THAT NEVER OCCUPIES THAT ROW need only wait long enough for the settlement to have
    // visibly begun — a quarter of it. Appearing at t = 0 would put the words on a panel that has not
    // yet changed in any other way, which reads as a screen rather than as a change.
    return kUnavailableSettleMs / 4U;
}

std::uint32_t total_ms(const app_model::LossOriginForm origin) {
    // ⬅️ B2, READY ONLY: its descent and its reveal are ONE progress and END TOGETHER, so the entry
    // is the settlement's own length. Stated explicitly rather than left to the max() below,
    // because the general formula would give 1800 and silently lengthen an accepted clock.
    if (origin == app_model::LossOriginForm::Ready) {
        return kUnavailableSettleMs;
    }

    return std::max(kUnavailableSettleMs, copy_from_ms(origin) + kUnavailableCopyMs);
}

std::int32_t form_top_y(const UnavailablePhase& phase) {
    return bounding_box(phase.form).y + phase.offset_y;
}

std::int32_t copy_top_y(const UnavailablePhase& phase) {
    return kUnavailableCopyBaseY + phase.offset_y;
}

UnavailablePhase phase_from_position(const app_model::LossOriginForm origin,
                                     const FormPosition start, const std::uint32_t elapsed_ms) {
    if (start.energy == origin_energy(origin) && start.offset_y == 0 && start.opacity == 255) {
        return phase_at(origin, elapsed_ms); // Includes every accepted genuine Ready-B2 frame.
    }
    // The form was between endpoints. Reuse the production travel and settlement clocks;
    // never jump to the semantic origin's endpoint or borrow Ready's lift mid-transition.
    UnavailablePhase phase;
    const auto travelled = ease(progress_permille(elapsed_ms, 0, kUnavailableFormMs));
    const auto settling = ease(progress_permille(elapsed_ms, 0, kUnavailableSettleMs));
    phase.energy = start.energy - ((start.energy * travelled) / kProgressMax);
    phase.form = form_geometry(phase.energy);
    phase.offset_y =
        start.offset_y + (((kUnavailableSettleY - start.offset_y) * settling) / kProgressMax);
    phase.form_opacity = static_cast<std::uint8_t>(
        start.opacity + (((kOpaque - start.opacity) * travelled) / kProgressMax));
    phase.treatment = form_treatment(true, phase.form.closed, false);
    phase.accent = accent();
    const auto resolved = ease(
        progress_permille(elapsed_ms, kUnavailableFormMs, kUnavailableFormMs + kUnavailableCopyMs));
    phase.copy_alpha = static_cast<std::uint8_t>(kOpaque * resolved / kProgressMax);
    phase.settled =
        elapsed_ms >= std::max(kUnavailableSettleMs, kUnavailableFormMs + kUnavailableCopyMs);
    // With a continuously variable origin, use the existing carrying-form copy window:
    // the form has reached the horizon before words resolve, so no origin-specific tuning.
    return phase;
}

UnavailablePhase phase_at(const app_model::LossOriginForm origin, const std::uint32_t elapsed_ms) {
    UnavailablePhase phase;

    const std::int32_t travelled = ease(progress_permille(elapsed_ms, 0, kUnavailableFormMs));

    // The form runs down the accepted energy axis to the resting crown. form_geometry() is the same
    // law the operational family uses — not a second one — so every intermediate shape here is a
    // shape the family already reaches.
    const Energy from = origin_energy(origin);
    phase.energy = std::clamp(from - (((from - kEnergyReady) * travelled) / kProgressMax),
                              kEnergyMin, kEnergyMax);
    phase.form = form_geometry(phase.energy);

    // ⛔ THE COLOUR DOES NOT TRAVEL WITH THE ENERGY, AND MUST NOT. §5.1, as ruled on 2026-09-01,
    // gives the form one accent at every point on the axis. The descent is expressed by the shape
    // alone — which is what `phase.form` above already carries. `phase.energy` deliberately does
    // not reach this line.
    phase.accent = accent();

    // And the same shape answers HOW it is painted, through the family's own rule. `drawn` is always
    // true — this composition only ever runs while it is on the panel — and `draws_record` is always
    // FALSE, because a lost observation has no reading to host. So a closed form resolves to
    // TrackOnly and an open one to FormAccent, and no sweep exists anywhere in this acknowledgement.
    phase.treatment = form_treatment(true, phase.form.closed, false);

    if (origin == app_model::LossOriginForm::Ready) {
        // ⬅️ B2 — TWO HALVES, AND THE SECOND SHARES ONE PROGRESS. See the header for why Ready is
        // the one origin needing its own phasing: its energy interpolation is a no-op, so the
        // general law leaves it an 8 px translation and nothing else.
        if (elapsed_ms <= kReadyPivotMs) {
            // The lift. ease() has zero slope at both ends, which is what stops the first frame
            // jumping and makes the reversal at the pivot smooth rather than a corner.
            const std::int32_t lifted = ease(progress_permille(elapsed_ms, 0, kReadyPivotMs));
            phase.offset_y = -((kReadyLiftY * lifted) / kProgressMax);

            // ⛔ EXACTLY ZERO, NOT MERELY SMALL. §9 permits no copy during the lift, and a rounded
            // fraction of an alpha is the ghosting the owner rejected.
            phase.copy_alpha = 0;
        } else {
            // ⛔ ONE PROGRESS, TWO OUTPUTS. The descent and the reveal are the SAME eased value, so
            // they cannot desynchronise and they arrive together by construction.
            const std::int32_t arriving =
                ease(progress_permille(elapsed_ms, kReadyPivotMs, kUnavailableSettleMs));

            phase.offset_y =
                -kReadyLiftY + (((kUnavailableSettleY + kReadyLiftY) * arriving) / kProgressMax);
            phase.copy_alpha = static_cast<std::uint8_t>((kOpaque * arriving) / kProgressMax);
        }
    } else {
        // The settlement runs over the whole arrival, as the accepted rasters do. It is only 8 px,
        // and an intermediate shape's deepest ink is far above the crown's, so nothing is pushed out
        // of §7's content box on the way — asserted for every frame, not assumed.
        const std::int32_t settling = ease(progress_permille(elapsed_ms, 0, kUnavailableSettleMs));
        phase.offset_y = (kUnavailableSettleY * settling) / kProgressMax;

        const std::uint32_t copy_from = copy_from_ms(origin);
        const std::int32_t resolved =
            ease(progress_permille(elapsed_ms, copy_from, copy_from + kUnavailableCopyMs));
        phase.copy_alpha = static_cast<std::uint8_t>((kOpaque * resolved) / kProgressMax);
    }

    phase.settled = elapsed_ms >= total_ms(origin);
    if (phase.settled) {
        // Pinned rather than computed, so the settled frame is exact whatever the tick landed on.
        phase.energy = kEnergyReady;
        phase.form = form_geometry(kEnergyReady);
        phase.accent = accent();
        phase.treatment = form_treatment(true, phase.form.closed, false);
        phase.offset_y = kUnavailableSettleY;
        phase.copy_alpha = static_cast<std::uint8_t>(kOpaque);
    }

    return phase;
}

} // namespace prd::ui::scene
