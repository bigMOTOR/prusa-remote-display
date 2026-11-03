// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstdint>

#include "components/status_message.hpp"
#include "scenes/unavailable_motion.hpp"
#include "theme/theme.hpp"

// The accepted Candidate-D acknowledgement — Production UI §9, UIAD 001 §11.1.
//
// INTERNAL HEADER of the ui component.
//
// ⛔ A TRANSIENT COMPOSITION, NEVER A Scene. Architecture Spec §17.2 removes printer unavailable
// from the scene list and records that its "only temporary visible representation is the transient
// acknowledgement of §17.8", which "is not an independently addressable scene, is not a persistent
// scene, and is not a node of this list". So this is not a Scene-enum member and never becomes one:
// it lives in §17.3's `transient overlay` slot, above whichever scene is selected.
//
// ⛔ IT OWNS NO CLOCK AND DECIDES NOTHING. Whether an acknowledgement exists is
// AppState::loss_episode; when its motion restarts is the model's presentation generation; how far
// through the motion this frame is comes from scene::phase_at(). This class applies a frame.
//
// ⛔ AND IT INVENTS NO PRIMITIVE. One operational form, on the family's own circle at the energy
// the law supplies, plus §8's Status Message component unmodified. No glyph, no icon, no second
// arc, no card, no ornament.
//
// The temporary resting interval uses bounded lv_line storage and the continuous stroke anchor
// described by resting_stroke(). Ready and every operational endpoint retain the arc renderer.

namespace prd::ui::scene {

class UnavailableAcknowledgement {
  public:
    [[nodiscard]] bool build(lv_obj_t* parent);

    // Applies one motion frame. Shows the composition if it was hidden.
    void apply(const UnavailablePhase& phase);

    // Hides everything and clears the characters LVGL holds.
    void hide();

    [[nodiscard]] bool built() const {
        return arc_ != nullptr && segment_ != nullptr && copy_.built();
    }

    [[nodiscard]] bool visible() const {
        return visible_;
    }

    // Renderer-owned transition handoff, not a reconstructed diagnostic report.
    [[nodiscard]] const UnavailablePhase& painted_phase() const {
        return painted_phase_;
    }

    // Which primitive drew the last applied frame. Diagnostics only; the composition is one form.
    enum class Rasteriser : std::uint8_t { None, Arc, Segment };

    [[nodiscard]] Rasteriser rasteriser() const {
        return rasteriser_;
    }

    [[nodiscard]] std::uint32_t segment_points() const {
        return point_count_;
    }

    // Horizontal sampling resolution. The resting interval never exceeds a semicircle;
    // the segment chord error is below one pixel before final coordinate quantisation.
    static constexpr std::int32_t kPixelsPerSample = 6;

    static constexpr std::uint32_t kMinSegmentPoints = 8;
    static constexpr std::uint32_t kMaxSegmentPoints = 64;

  private:
    UnavailablePhase painted_phase_{};
    void apply_arc(const UnavailablePhase& phase);
    void apply_segment(const UnavailablePhase& phase);
    void apply_copy(const UnavailablePhase& phase);

    lv_obj_t* arc_{nullptr};
    lv_obj_t* segment_{nullptr};
    component::StatusMessage copy_{};

    // ⛔ MEMBER STORAGE, BECAUSE lv_line_set_points() KEEPS THE POINTER. LVGL does not copy the
    // array, so it must outlive every frame that references it. Fixed capacity, so the bounded
    // representation allocates nothing at all: 64 * sizeof(lv_point_precise_t).
    lv_point_precise_t points_[kMaxSegmentPoints]{};
    std::uint32_t point_count_{0};

    Rasteriser rasteriser_{Rasteriser::None};
    bool visible_{false};
};

} // namespace prd::ui::scene
