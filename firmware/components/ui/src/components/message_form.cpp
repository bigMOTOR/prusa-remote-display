// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include "components/message_form.hpp"

#include <cmath>
#include <string>

#include "diagnostics/log.hpp"
#include "diagnostics/tags.hpp"
#include "fonts/motion_fonts.hpp"

namespace prd::ui::component {
namespace {

constexpr std::string_view kTag = diagnostics::tag::kUi;

// Icon path coordinates are asset geometry in their own box, not layout offsets. The four
// acquisition strokes are the accepted Connecting outline, unchanged; kWarning is the same
// 40 × 32 triangle the operational condition composition draws.
//
// ⛔ THE TWO PATHS SHARE A POINT COUNT AND A CORRESPONDENCE, WHICH IS WHAT MAKES THE MORPH ONE
// OBJECT RATHER THAN A DISSOLVE. Point i of the outline travels to point i of the triangle:
// bottom-left stays bottom-left, the top-left corner rises to the apex, the top-right corner
// falls to the bottom-right, and the two closing repeats fold onto the start. Changing either
// array without the other — or changing one's length — silently turns the transition into a
// shape that was never designed.
constexpr std::array<lv_point_precise_t, 5> kFrame{{{0, 26}, {0, 0}, {30, 0}, {30, 26}, {0, 26}}};
constexpr std::array<lv_point_precise_t, 2> kGantry{{{0, 8}, {30, 8}}};
constexpr std::array<lv_point_precise_t, 5> kNozzle{
    {{12, 8}, {12, 13}, {15, 16}, {18, 13}, {18, 8}}};
constexpr std::array<lv_point_precise_t, 2> kBed{{{5, 22}, {25, 22}}};

constexpr std::array<lv_point_precise_t, 5> kWarning{{
    {0, theme::kMessageSymbolHeight},
    {theme::kPrinterErrorIconHalfWidth, 0},
    {theme::kMessageSymbolWidth, theme::kMessageSymbolHeight},
    {0, theme::kMessageSymbolHeight},
    {0, theme::kMessageSymbolHeight},
}};

static_assert(kFrame.size() == kWarning.size(),
              "the morph is point-wise; the two endpoints must have the same number of points");

// ⛔ THE SETTLED WARNING IS THE ACCEPTED OPERATIONAL PATH, POINT FOR POINT — root finding R1,
// 2026-09-12. kWarning above describes the same triangle, and it still rasterises differently:
// it is five points beginning at the lower left with a degenerate closing repeat, while the
// accepted symbol is four points beginning at the apex. LVGL rounds the start of the first
// segment only, so the corner overlap depends on the ordering even where the geometry agrees —
// measured at ten differing outline pixels, which is a real difference and not a rounding
// artefact of the comparison.
//
// The morph therefore travels along kWarning and ARRIVES on this. The swap happens at mix
// exactly 1, where the two paths are geometrically identical and only their rasterisation
// differs, so nothing moves: the last frame of the transition is the accepted symbol itself.
//
// ⬅️ AND THE ACCEPTED CORPUS IS NOT TOUCHED TO MEET IT. The values below are read from the same
// layout tokens the operational composition builds its own array from, and the render test
// compares the two painted crops rather than trusting that they agree.
constexpr std::array<lv_point_precise_t, 4> kSettled{{
    {theme::kPrinterErrorIconHalfWidth, 0},
    {theme::kMessageSymbolWidth, theme::kMessageSymbolHeight},
    {0, theme::kMessageSymbolHeight},
    {theme::kPrinterErrorIconHalfWidth, 0},
}};

// The cap is the one property of the two endpoints that LVGL expresses as a boolean, so it cannot
// be carried continuously the way the path, the origin, the stroke and the mark's opacity are. It
// changes at the halfway point, where the stroke is 3 px and the shape is furthest from both
// endpoints — the least visible moment available, and stated here rather than left to be found.
constexpr double kCapSwitch = 0.5;

template <std::size_t N>
lv_obj_t* line(lv_obj_t* const parent, const std::array<lv_point_precise_t, N>& points,
               const theme::Colour token) {
    auto* const object = lv_line_create(parent);
    if (object == nullptr) {
        return nullptr;
    }

    lv_obj_remove_style_all(object);
    lv_line_set_points(object, points.data(), N);
    lv_obj_set_pos(object, theme::kMessageSymbolX, theme::kAcquisitionSymbolY);
    lv_obj_set_style_line_width(object, theme::kStrokeStandard, LV_PART_MAIN);
    lv_obj_set_style_line_color(object, theme::colour(token), LV_PART_MAIN);
    return object;
}

void set_label(lv_obj_t* const label, const std::string_view text) {
    // ⛔ AN EMPTY VIEW MAY CARRY A NULL POINTER, AND THE FORMATTER DEREFERENCES IT. An absent
    // printer identity arrives here as a default-constructed string_view whose data() is null;
    // "%.*s" with a precision of zero still loads from it, and LVGL's builtin vsnprintf faults.
    // Caught by UBSan in the renderer suite, not by inspection.
    if (text.empty()) {
        lv_label_set_text(label, "");
        return;
    }

    lv_label_set_text_fmt(label, "%.*s", static_cast<int>(text.size()), text.data());
}

void show_object(lv_obj_t* const object, const bool visible) {
    if (object == nullptr) {
        return;
    }

    if (visible) {
        lv_obj_remove_flag(object, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(object, LV_OBJ_FLAG_HIDDEN);
    }
}

[[nodiscard]] std::int32_t mixed(const std::int32_t from, const std::int32_t to, const double mix) {
    return static_cast<std::int32_t>(std::lround(static_cast<double>(from) + ((to - from) * mix)));
}

[[nodiscard]] lv_opa_t scaled(const std::int32_t level, const double factor) {
    const auto value = static_cast<std::int32_t>(std::lround(level * factor));
    return static_cast<lv_opa_t>(value < 0 ? 0 : value);
}

} // namespace

bool MessageForm::build(lv_obj_t* const parent) {
    root_ = theme::bare_object(parent);
    if (root_ == nullptr) {
        diagnostics::log(diagnostics::LogLevel::Error, kTag, "message form root failed");
        return false;
    }

    theme::place(root_, theme::kCanvas);

    // The outline first, so the text draws over it rather than under — Production UI §11's
    // layering order puts icons beneath typography.
    outline_ = {
        line(root_, kFrame, theme::Colour::Accent),
        line(root_, kGantry, theme::Colour::Accent),
        line(root_, kNozzle, theme::Colour::Accent),
        line(root_, kBed, theme::Colour::Accent),
    };

    identity_ = theme::bare_label(root_);
    title_ = theme::bare_label(root_);
    explanation_ = theme::bare_label(root_);
    mark_ = theme::bare_label(root_);

    if (identity_ == nullptr || title_ == nullptr || explanation_ == nullptr || mark_ == nullptr) {
        diagnostics::log(diagnostics::LogLevel::Error, kTag, "message form label failed");
        return false;
    }

    for (auto* const object : outline_) {
        if (object == nullptr) {
            diagnostics::log(diagnostics::LogLevel::Error, kTag, "message form outline failed");
            return false;
        }
    }

    // §9's access refinement, now the whole family's: the existing Outcome 28 title and Section 16
    // identity for acquisition, and the existing 24/20 semibold pair for a statement. No new font
    // asset is introduced by either — set_statement() overrides the face on the same objects, and
    // both overrides land on a role whose line-height gap is already zero, so the explanation
    // keeps its natural 24 px pitch with no extra inter-line spacing.
    theme::apply_text_style(title_, theme::TextRole::OutcomeTitle, theme::Colour::Primary);
    theme::apply_text_style(identity_, theme::TextRole::Section, theme::Colour::Secondary);
    theme::apply_text_style(explanation_, theme::TextRole::Section, theme::Colour::Primary);
    theme::apply_text_style(mark_, theme::TextRole::Body, theme::Colour::Warning);

    lv_obj_set_style_text_font(explanation_, fonts::motion_title(theme::kMessageExplanationSize),
                               LV_PART_MAIN);
    lv_obj_set_style_text_font(mark_, fonts::motion_title(theme::kMessageMarkSize), LV_PART_MAIN);

    lv_obj_set_width(title_, theme::kMessageWidth);
    lv_obj_set_width(identity_, theme::kMessageWidth);
    lv_obj_set_width(explanation_, theme::kMessageWidth);
    lv_label_set_long_mode(explanation_, LV_LABEL_LONG_MODE_WRAP);

    // The identity is the one value here that is not ours to shorten: a printer name long enough
    // to wrap would push into the title, so it is clipped to one line with an ellipsis instead.
    lv_label_set_long_mode(identity_, LV_LABEL_LONG_MODE_DOTS);

    // ⛔ CONTENT-SIZED AND ALIGNED, NOT A CENTRE-ALIGNED BOX — root finding R1. The accepted
    // operational mark is LV_SIZE_CONTENT aligned TOP_MID against the canvas, and a fixed-width
    // label centring its own text is not the same computation: the two round an odd glyph advance
    // at different points. Reproducing the construction is the only way to reproduce the pixels,
    // and the render test measures that rather than assuming it.
#ifdef PRD_OLD_MESSAGE_SYMBOL
    lv_obj_set_width(mark_, theme::kMessageSymbolWidth);
    lv_obj_set_style_text_align(mark_, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
#else
    lv_obj_set_width(mark_, LV_SIZE_CONTENT);
#endif
    set_label(mark_, text::lookup(text::kMessageMark));

    lv_obj_set_pos(title_, theme::kMessageX, theme::kMessageTitleY);
    lv_obj_set_pos(identity_, theme::kMessageX, theme::kMessageIdentityY);
    lv_obj_set_pos(explanation_, theme::kMessageX, theme::kMessageExplanationY);

    lv_label_set_text(title_, "");
    lv_label_set_text(identity_, "");
    lv_label_set_text(explanation_, "");

    show_object(identity_, false);
    show_object(explanation_, false);

    path_ = kFrame;
    place_symbol();

    show(false);
    return true;
}

void MessageForm::set_statement(const text::TextId title, const text::TextId explanation,
                                const text::MessageTone tone, const std::string_view subject) {
    if (title_ == nullptr) {
        return;
    }

    statement_ = true;

    // ⛔ THE FACE IS SET BEFORE THE TEXT AND THE WIDTH IS ALREADY FIXED, so the label wraps against
    // the measure it will actually be drawn at. Setting the text first and the face after measures
    // the wrap against the wrong advance, which is how a title that fits becomes a title that
    // wrapped only on the panel.
    lv_obj_set_style_text_font(title_, fonts::motion_title(theme::kMessageTitleSize), LV_PART_MAIN);
    set_label(title_, text::lookup(title));
    set_label(explanation_, text::lookup(explanation));
    show_object(explanation_, true);

    const auto token =
        tone == text::MessageTone::Warning ? theme::Colour::Warning : theme::Colour::Error;
    lv_obj_set_style_text_color(mark_, theme::colour(token), LV_PART_MAIN);
    lv_obj_set_style_line_color(outline_[0], theme::colour(token), LV_PART_MAIN);

    // "space.6 below the actual laid-out title box" — measured, not predicted. A title that wrapped
    // to two lines moves the explanation down with it rather than being written over.
    lv_obj_update_layout(title_);
    lv_obj_update_layout(explanation_);

    const std::int32_t title_height = lv_obj_get_height(title_);
    const std::int32_t explanation_y =
        theme::kMessageTitleY + title_height + theme::kMessageExplanationGap;
    lv_obj_set_y(explanation_, explanation_y);

    // The same discipline the provisioning rows follow: measure the consequence rather than
    // predict it, against the bounds this composition actually has. The ceiling is §7's content
    // area — "all content shall remain inside canvas.inset" — and not the 240 px panel, so a
    // statement that would touch the display edge is reported even though it would still draw.
    const std::int32_t bottom = explanation_y + lv_obj_get_height(explanation_);
    if (bottom > theme::kCanvasContent.bottom()) {
        diagnostics::log(diagnostics::LogLevel::Warning, kTag,
                         std::string{subject} + " is taller than the content area (" +
                             std::to_string(bottom) + " px against " +
                             std::to_string(theme::kCanvasContent.bottom()) +
                             "); it needs re-measuring against the committed faces");
    }
}

void MessageForm::set_acquisition(const std::string_view title) {
    if (title_ == nullptr) {
        return;
    }

    statement_ = false;

    // §9: "Returning to Connecting restores its 28 px title and 16 px secondary identity
    // treatment." Restored through the role rather than by naming a face, so the acquisition
    // endpoint cannot drift from the token table.
    theme::apply_text_style(title_, theme::TextRole::OutcomeTitle, theme::Colour::Primary);
    set_label(title_, title);

    set_label(explanation_, "");
    show_object(explanation_, false);

    lv_obj_set_style_line_color(outline_[0], theme::colour(theme::Colour::Accent), LV_PART_MAIN);
}

void MessageForm::set_title_text(const std::string_view title) {
    if (title_ == nullptr) {
        return;
    }

    set_label(title_, title);
}

void MessageForm::set_identity(const std::string_view name) {
    if (identity_ == nullptr) {
        return;
    }

    if (name != lv_label_get_text(identity_)) {
        set_label(identity_, name);
    }

    show_object(identity_, !name.empty());
}

void MessageForm::set_symbol(const double mix) {
    mix_ = mix < 0 ? 0 : (mix > 1 ? 1 : mix);
}

void MessageForm::place_symbol() {
    if (mix_ >= 1.0
#ifdef PRD_OLD_MESSAGE_SYMBOL
        && false
#endif
    ) {
        // The accepted construction, not an interpolation that happens to land on it.
        std::size_t point = 0;
        for (const auto& settled : kSettled) {
            path_[point++] = settled;
        }
        points_ = kSettled.size();
    } else {
        for (std::size_t i = 0; i < kFrame.size(); ++i) {
            path_[i].x = static_cast<lv_value_precise_t>(kFrame[i].x +
                                                         ((kWarning[i].x - kFrame[i].x) * mix_));
            path_[i].y = static_cast<lv_value_precise_t>(kFrame[i].y +
                                                         ((kWarning[i].y - kFrame[i].y) * mix_));
        }
        points_ = kFrame.size();
    }

    lv_line_set_points(outline_[0], path_.data(), points_);

    // Position, stroke and cap travel with the path, so the symbol arrives at the accepted warning
    // bounds — 44 × 36 painted, beginning at (14, 24) — from the accepted Connecting outline
    // without either endpoint moving. Both are endpoints of one continuous object.
    const std::int32_t origin = mixed(theme::kAcquisitionSymbolY, theme::kMessageSymbolY, mix_);
    const std::int32_t stroke = mixed(theme::kStrokeStandard, theme::kStrokeEmphasis, mix_);

    for (auto* const object : outline_) {
        lv_obj_set_y(object, origin);
        lv_obj_set_style_line_width(object, stroke, LV_PART_MAIN);
        lv_obj_set_style_line_rounded(object, mix_ >= kCapSwitch, LV_PART_MAIN);
    }

    // The same alignment the accepted symbol uses: content-sized, TOP_MID against the canvas,
    // offset to the triangle's centre. The offset is relative to the canvas mid-point because the
    // label's parent is the canvas, and the symbol's centre is not the canvas's.
#ifdef PRD_OLD_MESSAGE_SYMBOL
    lv_obj_set_pos(mark_, theme::kMessageSymbolX, origin + theme::kMessageMarkOffsetY);
#else
    lv_obj_align(mark_, LV_ALIGN_TOP_MID,
                 theme::kMessageSymbolX + theme::kPrinterErrorIconHalfWidth -
                     (theme::kDisplayWidth / 2),
                 origin + theme::kMessageMarkOffsetY);
#endif
}

void MessageForm::paint(const std::int32_t level) {
    if (root_ == nullptr) {
        return;
    }

    if (level_ == level && painted_mix_ == mix_) {
        return;
    }

    if (painted_mix_ != mix_) {
        place_symbol();
    }

    level_ = level;
    painted_mix_ = mix_;

    for (auto* const label : {identity_, title_, explanation_}) {
        theme::apply_text_level(label, theme::Opacity::Full, level);
    }

    // The mark belongs to the warning endpoint alone, and the three secondary strokes to the
    // acquisition one. Each is scaled by its own end of the morph, so neither is ever painted
    // over the other and the whole symbol still recedes with the body.
    theme::apply_text_level(mark_, theme::Opacity::Full,
                            static_cast<std::int32_t>(std::lround(level * mix_)));

    lv_obj_set_style_line_opa(outline_[0], scaled(level, 1.0), LV_PART_MAIN);
    for (std::size_t i = 1; i < outline_.size(); ++i) {
        lv_obj_set_style_line_opa(outline_[i], scaled(level, 1.0 - mix_), LV_PART_MAIN);
    }
}

void MessageForm::show(const bool visible) {
    show_object(root_, visible);

    if (!visible) {
        // Forget the painted level, not the content: the next entry must repaint from whatever
        // level it is given rather than skipping it as redundant.
        level_ = -1;
    }
}

} // namespace prd::ui::component
