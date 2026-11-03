// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <array>
#include <cstdint>
#include <string_view>

#include "text/scene_text.hpp"
#include "text/text.hpp"
#include "theme/theme.hpp"

// The shared full-screen message composition — Production UI §8, owner-accepted 2026-09-12.
//
// INTERNAL HEADER of the ui component.
//
// WHAT IT IS. One symbol, one optional identity, one title and one explanation, at the geometry
// fixed in theme/layout.hpp. Every family that states a condition in words draws it: the two
// access failures, the four device-fault statements, the recovery statement and the factory-reset
// question. It is also the acquisition composition's own body — Connecting is this form with the
// printer outline in place of the warning symbol and §9's 28 px title, which is what lets the
// morph between them be one continuous object rather than two screens swapping.
//
// ⛔ WHY IT IS A COMPONENT AND NOT THREE SCENE BRANCHES. Before this, three scenes drew a
// title-over-a-message: the access scene with its own 24/20 pair and its own triangle, the
// provisioning scene's draw_statement() at StateTitle over Body, and the factory-reset scene with
// a third arrangement again. They had already drifted — two triangle constructions, two
// explanation roles, two overflow budgets — which is the drift Production UI §2 forbids ("One
// shared component library", "Screen-specific styling is prohibited") and the cross-screen audit
// of 2026-09-12 measured. One implementation is the repair; a fourth scene branch for the OTA
// failure would have been a fourth drift.
//
// ⬅️ IT INTRODUCES NO ARCHITECTURAL LAYER. It is a §8 shared component beside the progress ring,
// the status message and the telemetry column, built by a scene and painted by it. Scenes still
// own their clocks and their content decisions; this owns geometry, typography and painting.
//
// ⛔ THE SYMBOL IS ONE CONSTRUCTION. §8's icon rules are "share visual construction" and "do not
// introduce stylistic variants". The warning mark here is the same 40 × 32 triangle at the same
// emphasis stroke, carrying the same glyph at the same size, that the operational condition
// composition draws inside the progress ring. What differs is where it sits — which is what §7's
// "equivalent information occupies equivalent positions" actually asks for, since a statement has
// no ring to sit inside.
//
// ⛔ AND THE COLOUR IS SEMANTIC, NOT A SUBSYSTEM LABEL. Warning means something is waiting on the
// reader — an attention, or a question. Error means an operation completed and failed. That is an
// owner decision of 2026-09-12, recorded at text::MessageTone, and it is deliberately NOT "amber
// for the printer, red for the display". Neither tone carries the meaning on its own: the title
// and the explanation say it in words, so the composition reads correctly in monochrome (Product
// Brief, and Production UI §12's colour-independence rule).

namespace prd::ui::component {

class MessageForm {
  public:
    // Builds the composition under `parent`, hidden. Returns false if any LVGL allocation failed.
    // Render task only.
    [[nodiscard]] bool build(lv_obj_t* parent);

    // The statement composition: the accepted 24 px title, the 20 px explanation one region gap
    // below the title box it actually laid out, and the warning symbol in `tone`. `subject` names
    // the caller in the overflow diagnostic, so a report still says which text outgrew its budget.
    void set_statement(text::TextId title, text::TextId explanation, text::MessageTone tone,
                       std::string_view subject);

    // The acquisition composition: §9's 28 px title, the printer outline, no explanation. The
    // text is passed rather than looked up because the accepted dot loop rewrites it in place.
    void set_acquisition(std::string_view title);

    // Replaces the title text without disturbing its role — the dot loop's one permitted write.
    void set_title_text(std::string_view title);

    // The printer identity, shown only when known. Empty hides the row; nothing is invented.
    void set_identity(std::string_view name);

    // Where the symbol is between the two endpoints: 0 is the accepted Connecting printer
    // outline, 1 is the warning mark. Every property that differs between them is carried
    // continuously — the path, the origin, the stroke and the mark's opacity — so the morph is
    // reversible from any painted position rather than only from an endpoint.
    void set_symbol(double mix);

    [[nodiscard]] double symbol() const {
        return mix_;
    }

    // Paints the whole body at `level` (0...255) — UI Design Decision 004 §4's "the composition
    // recedes as one body". Idempotent: a repeated level with an unchanged symbol draws nothing.
    void paint(std::int32_t level);

    void show(bool visible);

    [[nodiscard]] bool built() const {
        return root_ != nullptr;
    }

    [[nodiscard]] lv_obj_t* object() const {
        return root_;
    }

  private:
    void place_symbol();

    lv_obj_t* root_{nullptr};
    lv_obj_t* identity_{nullptr};
    lv_obj_t* title_{nullptr};
    lv_obj_t* explanation_{nullptr};
    lv_obj_t* mark_{nullptr};

    // The printer outline's four strokes. outline_[0] is the one that becomes the triangle; the
    // other three fade out, because a triangle has no gantry, nozzle or bed.
    std::array<lv_obj_t*, 4> outline_{};

    // lv_line stores the caller's point array rather than copying it (lv_line.c: the points
    // pointer is retained and lv_line_get_points hands it back), so the morphed path has to
    // outlive every frame that draws it. It lives here for that reason.
    std::array<lv_point_precise_t, 5> path_{};

    // How many of them are live. The settled warning is the accepted four-point path; every
    // other position on the morph is the five-point one. See kSettled in the implementation.
    std::size_t points_{5};

    double mix_{0};
    double painted_mix_{-1};
    std::int32_t level_{-1};
    bool statement_{false};
};

} // namespace prd::ui::component
