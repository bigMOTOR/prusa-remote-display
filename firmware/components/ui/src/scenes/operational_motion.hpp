// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

#include "scenes/operational_content.hpp"
#include "scenes/operational_form.hpp"
#include "scenes/unavailable_motion.hpp"
#include "theme/layout.hpp"

namespace prd::ui::scene {

// Renderer-owned interpolation only. No clock here changes a model fact or a value.
struct OperationalPose {
    enum Field : std::size_t {
        EnergyField,
        TitleY,
        TitleSize,
        ValueY,
        ValueSize,
        ThirdY,
        PauseAlpha,
        TelemetryAlpha,
        OffsetY,
        AppearanceAlpha,
        RecordAlpha,
        CycleTreatmentAlpha,
        // Owner repair 2026-09-08: Completed keeps Printing's body luminance.
        // Separate from geometry, but interpolated on the same clock and interruption seed.
        BodyEnergy,
        ConditionAlpha,
        ConditionContextY,
        ConditionIconY,
        Count
    };
    std::array<double, Count> values{};
};

inline OperationalPose operational_pose(const OperationalContent& content, const bool telemetry,
                                        const bool unknown_progress) {
    using P = OperationalPose;
    OperationalPose result{
        {static_cast<double>(content.form.value_or(kEnergyReady)), theme::kMotionTitleY,
         theme::kMotionTitleSize, theme::kMotionValueY, theme::kFontSizeHero, theme::kMotionThirdY,
         0.0, telemetry ? 255.0 : 0.0, 0.0, 255.0, content.show_progress ? 255.0 : 0.0,
         content.form == kEnergyPrinting ? 255.0 : 0.0,
         static_cast<double>(content.form == kEnergyCompleted
                                 ? kEnergyPrinting
                                 : content.form.value_or(kEnergyPrinting))}};
    if (content.arrangement == Arrangement::Resting) {
        result.values[P::TitleY] = theme::kMotionRestingTitleY;
    }
    result.values[P::ConditionContextY] = theme::kPrinterErrorContextY;
    result.values[P::ConditionIconY] = theme::kPrinterErrorIconTop;
    if (content.form == kEnergyPaused) {
        result.values[P::PauseAlpha] = 255.0;
    }
    if (content.form == kEnergyCompleted) {
        result.values[P::TitleY] = theme::kMotionCompletedTitleY;
        result.values[P::TitleSize] = theme::kFontSizeOutcome;
        result.values[P::ValueY] = theme::kMotionCompletedValueY;
        result.values[P::ValueSize] = theme::kMotionCompletedValueSize;
    } else if (unknown_progress) {
        result.values[P::TitleY] = theme::kMotionUnknownTitleY;
        result.values[P::TitleSize] = theme::kMotionUnknownTitleSize;
        result.values[P::ThirdY] = theme::kMotionUnknownThirdY;
    }
    return result;
}

// A newly arriving record is revealed only once the form reaches its ink row. The remaining
// geometric clearance, relative to the destination, completes the reveal without a timer or
// per-state threshold. Destinations that intentionally place a record above the form (Stopped)
// retain their normal appearance; their accepted composition does not require enclosure.
inline double record_enclosure(const OperationalPose& current, const OperationalPose& target,
                               const std::int32_t ink_top, const std::int32_t target_ink_top) {
    using P = OperationalPose;
    // An incoming record must not appear on the old side of a title/record exchange,
    // disappear at their crossing, then appear again. Existing records retain their own
    // crossing fade; this gate applies only to the incoming increment in reveal_record().
    const auto order = [](const OperationalPose& pose) {
        return pose.values[P::ValueY] - pose.values[P::TitleY];
    };
    if (order(current) * order(target) < 0.0)
        return 0.0;
    const auto top = [](const OperationalPose& pose) {
        return drawn_top_y(
                   form_geometry(static_cast<Energy>(std::lround(pose.values[P::EnergyField])))) +
               pose.values[P::OffsetY];
    };
    const double destination_clearance = target_ink_top - top(target);
    if (destination_clearance <= 0.0)
        return 1.0;
    return std::clamp((ink_top - top(current)) / destination_clearance, 0.0, 1.0);
}

class OperationalMotion {
  public:
    void resume_from_form(const FormPosition form) {
        // New truthful copy begins in the family's existing neutral centre stack, never
        // in a hidden previous state's outcome/unknown layout. The shared motion carries
        // it to the current state's reviewed endpoint. No second timing or font is added.
        current_ = operational_pose(OperationalContent{}, false, false);
        current_.values[OperationalPose::EnergyField] = form.energy;
        current_.values[OperationalPose::BodyEnergy] = form.energy;
        current_.values[OperationalPose::CycleTreatmentAlpha] =
            form_geometry(form.energy).closed ? 255.0 : 0.0;
        current_.values[OperationalPose::OffsetY] = form.offset_y;
        current_.values[OperationalPose::AppearanceAlpha] = form.opacity;
        from_ = to_ = current_;
        record_tween_ = current_.values[OperationalPose::RecordAlpha];
        initialised_ = true;
        running_ = armed_ = false;
    }

    void resume_from_blank(const OperationalPose& destination) {
        // There is no painted geometry to preserve. Reveal the current composition itself;
        // do not resurrect a stale form or manufacture a Ready-origin journey.
        current_ = from_ = to_ = destination;
        current_.values[OperationalPose::AppearanceAlpha] = 0;
        initialised_ = true;
        running_ = armed_ = false;
        from_ = to_ = current_;
        record_tween_ = current_.values[OperationalPose::RecordAlpha];
        target(destination);
    }

    void target(const OperationalPose& pose) {
        if (!initialised_) {
            current_ = from_ = to_ = pose;
            record_tween_ = current_.values[OperationalPose::RecordAlpha];
            initialised_ = true;
            return;
        }
        if (pose.values == to_.values) {
            return; // A new reading at the same layout never restarts a transition.
        }
        from_ = current_; // Interruptions start at the last painted pose, not an endpoint.
        record_tween_ = current_.values[OperationalPose::RecordAlpha];
        to_ = pose;
        running_ = true;
        armed_ = true;
    }

    bool step(const bool visible, const std::uint32_t now) {
        if (!running_) {
            return false;
        }
        if (!visible) {
            armed_ = true;
            from_ = current_;
            record_tween_ = current_.values[OperationalPose::RecordAlpha];
            return false; // Retain motion, but do not request fast ticks on an Off panel.
        }
        if (armed_) {
            start_ = now;
            armed_ = false;
        }
        const std::uint32_t elapsed = now - start_; // Unsigned wrap is intentional.
        if (elapsed >= theme::kOperationalMotionMs) {
            current_ = to_;
            record_tween_ = current_.values[OperationalPose::RecordAlpha];
            running_ = false;
            return false;
        }
        const double p = static_cast<double>(elapsed) / theme::kOperationalMotionMs;
        const double eased = p * p * p * (p * (p * 6.0 - 15.0) + 10.0);
        for (std::size_t i = 0; i < OperationalPose::Count; ++i) {
            current_.values[i] = from_.values[i] + (to_.values[i] - from_.values[i]) * eased;
        }
        record_tween_ = current_.values[OperationalPose::RecordAlpha];
        return true;
    }

    [[nodiscard]] const OperationalPose& current() const {
        return current_;
    }
    [[nodiscard]] const OperationalPose& destination() const {
        return to_;
    }
    double reveal_record(const double enclosure) {
        using P = OperationalPose;
        const double start = from_.values[P::RecordAlpha];
        auto& alpha = current_.values[P::RecordAlpha];
        alpha = record_tween_;
        // Existing records are not withdrawn as the form changes. For incoming records,
        // retain the last painted opacity on interruption, rather than the ungated tween.
        if (alpha > start)
            alpha = start + (alpha - start) * enclosure;
        return alpha;
    }
    [[nodiscard]] bool running() const {
        return running_;
    }
    void finish() {
        current_ = to_;
        record_tween_ = current_.values[OperationalPose::RecordAlpha];
        running_ = armed_ = false;
    }

  private:
    OperationalPose current_{}, from_{}, to_{};
    double record_tween_{0.0};
    std::uint32_t start_{0};
    bool initialised_{false}, running_{false}, armed_{false};
};

} // namespace prd::ui::scene
