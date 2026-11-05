// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

// sdkconfig.h first and explicitly: this translation unit is gated on a CONFIG_PRD_UI_*
// macro below, and a #if on an undefined macro is silently false.
#include "sdkconfig.h"

#include "fonts/motion_fonts.hpp"
#include "scenes/operational_scene.hpp"
#include <cmath>

#include "diagnostics/log.hpp"
#include "diagnostics/tags.hpp"
#include <cstdlib>

#include "fonts/fonts.hpp"
#include "formatters.hpp"
#include "text/scene_text.hpp"
#include "text/text.hpp"

namespace prd::ui::scene {
namespace {

constexpr std::string_view kTag = diagnostics::tag::kUi;

constexpr std::array<lv_point_precise_t, 4> kErrorOutline{
    {{theme::kPrinterErrorIconHalfWidth, 0},
     {2 * theme::kPrinterErrorIconHalfWidth, theme::kPrinterErrorIconHeight},
     {0, theme::kPrinterErrorIconHeight},
     {theme::kPrinterErrorIconHalfWidth, 0}}};

// THIS SCENE OWNS NO USER-VISIBLE STRING. The state title and the percent unit are
// requested from the text resource boundary by id (text/scene_text.hpp); every other
// piece of text on screen is formatter output. A literal here would have to be edited to
// add a language, which would make a language change a change to composition code.
// firmware/tools/check-tokens.sh rejects one.

// Progress is a percentage, so three digit columns hold every value it can take.
// Fixed rather than fitted: §4 requires that "numeric values shall remain
// horizontally stable during updates", and a field that resized at 9 → 10 and
// 99 → 100 would move every digit on screen at exactly those moments. Tabular
// figures exist to make this field possible; fitting the width to the current value
// would waste them.
constexpr int kPercentDigitColumns = 3;

// No gap between the value and its unit. A percent sign is conventionally set tight against
// its number, and the `space.1` that used to sit here bought nothing visually while costing
// 4 px of the chord clearance the three-digit case at 100 % has least of.
constexpr std::int32_t kValueToUnitGap = theme::kSpace0;

// Single-line catalogue/formatter labels. For non-ASCII printer names, use the full
// line box conservatively rather than interpreting UTF-8 bytes as separate glyphs.
[[nodiscard]] fonts::InkExtent label_ink(lv_obj_t* label) {
    const auto* font = lv_obj_get_style_text_font(label, LV_PART_MAIN);
    fonts::InkExtent result;
    for (const unsigned char c : std::string_view{lv_label_get_text(label)}) {
        if (c >= 128) {
            return {true, 0, font->line_height, font->line_height};
        }
        const auto glyph = fonts::ink_extent(font, c);
        if (!glyph.known || glyph.height == 0) {
            continue;
        }
        if (!result.known) {
            result = glyph;
        } else {
            result.top = std::min(result.top, glyph.top);
            result.bottom = std::max(result.bottom, glyph.bottom);
        }
    }
    result.height = result.bottom - result.top;
    return result;
}

// The three lines of UI Design Decision 001's centre stack, each at its own line
// height, filling the region exactly. The region is sized as their sum, so there is no
// spare to distribute and no arbitrary padding — the numeral's row is centred on the ring
// and the other two follow it.
constexpr std::int32_t kPercentRowTop = 0;
constexpr std::int32_t kTitleRowTop = kPercentRowTop + theme::kLineHero;
constexpr std::int32_t kThirdRowTop = kTitleRowTop + theme::kLineSection;

static_assert(kThirdRowTop + theme::kLineBody == theme::kRegionOperational.height,
              "the three-line stack must fill region.operational exactly");

// Sets a label from the text resource boundary. The only route by which a user-visible
// string reaches LVGL in this file, so there is exactly one place to look when asking
// where a word came from.
void set_label_text(lv_obj_t* const label, const text::TextId id) {
    if (label == nullptr) {
        return;
    }

    const std::string_view value = text::lookup(id);
    lv_label_set_text_fmt(label, "%.*s", static_cast<int>(value.size()), value.data());
}

// Total advance width of a catalogue entry in a given face, in pixels. Used to size the
// fixed field the unit sits after, so the geometry follows the string rather than
// assuming a single character.
[[nodiscard]] std::int32_t text_advance(const lv_font_t* const font, const text::TextId id) {
    std::int32_t total = 0;

    // ASCII only, which the catalogue is guaranteed to be within except for the degree
    // sign — and no entry drawn through this path carries one. A multi-byte codepoint
    // would need UTF-8 decoding here; text::is_renderable() bounds what can arrive.
    for (const char character : text::lookup(id)) {
        total += static_cast<std::int32_t>(
            lv_font_get_glyph_width(font, static_cast<std::uint32_t>(character), 0));
    }

    return total;
}

// Centres a content-sized label horizontally on the ring, at a given absolute y.
//
// Aligned rather than positioned, so LVGL recentres it whenever its text changes width — a
// duration crossing from 9:59 to 10:00 stays centred without the scene recomputing anything.
// The offset is relative to the canvas mid-point because the label's parent is the canvas,
// and the ring's centre is not the canvas's.
void centre_on_ring(lv_obj_t* const label, const std::int32_t absolute_y) {
    if (label == nullptr) {
        return;
    }

    lv_obj_set_width(label, LV_SIZE_CONTENT);
    lv_obj_align(label, LV_ALIGN_TOP_MID, theme::kRingCentreX - (theme::kDisplayWidth / 2),
                 absolute_y);
}

// Distance from a face's top to its baseline. LVGL stores the descent as base_line,
// so the ascent is what remains of the line height.
[[nodiscard]] std::int32_t ascent_of(const lv_font_t* const font) {
    if (font == nullptr) {
        return 0;
    }

    return static_cast<std::int32_t>(font->line_height) -
           static_cast<std::int32_t>(font->base_line);
}

#if defined(CONFIG_PRD_UI_RENDER_REPORT)

// A colour in the form a reviewer can compare against theme/tokens.hpp. std::to_string gives
// decimal, which no one can read as a colour and which would make the accent the one reported
// value that cannot be checked at a glance.
//
// The digits are computed rather than held in a lookup table on purpose. A digit table is a
// constant string of letters, which is exactly the shape a scene caching a title would have and
// exactly what tools/check-tokens.sh rejects under scenes/ and components/. The guard is right
// about the shape even though this is not prose, so the code changed rather than the rule.
[[nodiscard]] std::string hex_colour(const std::uint32_t rgb) {
    constexpr std::int32_t kBitsPerDigit = 4;
    constexpr std::int32_t kHighestDigitShift = 20;
    constexpr std::uint32_t kNibbleMask = 0xFU;
    constexpr std::uint32_t kDecimalDigits = 10U;

    std::string text = "#";
    for (std::int32_t shift = kHighestDigitShift; shift >= 0; shift -= kBitsPerDigit) {
        const std::uint32_t nibble = (rgb >> static_cast<std::uint32_t>(shift)) & kNibbleMask;
        const std::uint32_t base = nibble < kDecimalDigits
                                       ? static_cast<std::uint32_t>('0')
                                       : static_cast<std::uint32_t>('A') - kDecimalDigits;

        text += static_cast<char>(base + nibble);
    }

    return text;
}

// Reports the composition's MEASURED geometry, and whether the form crosses each row.
//
// Everything else about this scene is derived arithmetic, and arithmetic can be wrong in
// ways a clean build cannot see — the ring drew nothing for a whole flash cycle, and the
// hero line box was 3 px short of its face. This reads back what LVGL actually laid out and
// measures it against the form that is actually on the panel, so the composition can be
// judged from the log instead of from a photograph.
//
// MEASURED AGAINST THE CURRENT FORM, NOT AGAINST THE RING. Before Phase E the form was one of
// two fixed shapes and the gauge could assume the ring's own circle. It is now a function of
// energy, so a gauge holding kRingDiameter and kRingCentreY would report a circle the
// composition no longer draws — and this project has already learned what a lying diagnostic
// costs: a box-based telemetry gauge once reported a collision that did not exist.
//
// TWO QUESTIONS, because they are genuinely different. "Does the form's stroke pass through
// this row?" is the structural one and applies at every energy. "Does this row sit inside the
// form's interior?" only means anything while the form is a closed cycle — it is what D8's
// accepted region overlap rests on, and it is where the 100 % three-digit pressure shows up.
void log_row(const std::string_view name, lv_obj_t* const object,
             const std::optional<scene::FormGeometry>& form) {
    if (object == nullptr) {
        return;
    }

    const std::int32_t x = lv_obj_get_x(object);
    const std::int32_t y = lv_obj_get_y(object);
    const std::int32_t w = lv_obj_get_width(object);
    const std::int32_t h = lv_obj_get_height(object);

    const theme::Rect box{x, y, w, h};

    std::string verdict = " form=absent";

    if (form.has_value()) {
        const bool struck = scene::crosses(*form, theme::kRingStroke, box);

        verdict = " crosses=" + std::string{struck ? "YES" : "no"};

        if (form->closed) {
            // Half-height of the row measured from the form's centre, taking the worse end.
            const std::int32_t top_offset = std::abs(y - form->centre_y);
            const std::int32_t bottom_offset = std::abs((y + h) - form->centre_y);
            const std::int32_t offset = top_offset > bottom_offset ? top_offset : bottom_offset;

            const std::int32_t inner_radius = form->radius - theme::kRingStroke;
            const std::int32_t span = (inner_radius * inner_radius) - (offset * offset);

            std::int32_t half_chord = 0;
            while (span > 0 && (half_chord + 1) * (half_chord + 1) <= span) {
                ++half_chord;
            }

            // The FORM's centre, not a global one: the settled horizon is centred on the
            // canvas while every operational state is centred beside the telemetry column, so a
            // fixed centre here would measure clearance against a circle that is not on screen.
            const std::int32_t left_clear = x - (form->centre_x - half_chord);
            const std::int32_t right_clear = (form->centre_x + half_chord) - (x + w);
            const bool inside = span > 0 && left_clear >= 0 && right_clear >= 0;

            verdict += " half_chord=" + std::to_string(half_chord) +
                       " clear_l=" + std::to_string(left_clear) +
                       " clear_r=" + std::to_string(right_clear) +
                       " inside_form=" + (inside ? "yes" : "NO");
        }
    }

    const bool problem = form.has_value() && scene::crosses(*form, theme::kRingStroke, box);

    diagnostics::log(problem ? diagnostics::LogLevel::Warning : diagnostics::LogLevel::Info, kTag,
                     "layout " + std::string{name} + " x=" + std::to_string(x) +
                         " y=" + std::to_string(y) + " w=" + std::to_string(w) +
                         " h=" + std::to_string(h) + verdict);
}

#endif // CONFIG_PRD_UI_RENDER_REPORT

} // namespace

bool OperationalScene::build(lv_obj_t* const parent) {
    root_ = theme::bare_object(parent);
    if (root_ == nullptr) {
        diagnostics::log(diagnostics::LogLevel::Error, kTag, "printing scene root failed");
        return false;
    }

    // The scene fills the canvas and positions its regions absolutely. It is not a
    // layout container: §7 requires that "layout does not reflow", and a flex or grid
    // parent is precisely a thing that reflows when a child appears.
    theme::place(root_, theme::kCanvas);

    // No region argument: the ring's geometry is set by its form, which the first applied
    // presentation chooses. A ring built at a fixed region would have to be moved again the
    // moment a resting state arrived.
    if (!ring_.build(root_)) {
        diagnostics::log(diagnostics::LogLevel::Error, kTag, "progress ring failed");
        return false;
    }

    if (!telemetry_.build(root_, theme::kRegionTelemetry)) {
        diagnostics::log(diagnostics::LogLevel::Error, kTag, "telemetry column failed");
        return false;
    }

    if (!update_available_.build(root_, theme::kUpdateReadyMark)) {
        diagnostics::log(diagnostics::LogLevel::Error, kTag, "update availability mark failed");
        return false;
    }
    update_available_.show(false);
    update_available_.paint(theme::opacity(theme::Opacity::Full));

    // --- The centre stack, inside the ring -----------------------------------

    const lv_font_t* const value_font = fonts::for_role(theme::TextRole::PrimaryValue);
    const lv_font_t* const unit_font = fonts::for_role(theme::TextRole::PrimaryUnit);

    // Measured from the generated faces rather than assumed. The digit advance is the
    // figure fonts::verify() reports at start-up, so the field width and the tabular
    // claim rest on the same measurement.
    const std::int32_t digit_advance =
        static_cast<std::int32_t>(lv_font_get_glyph_width(value_font, '0', 0));
    // Measured from the catalogue entry rather than from a literal, so a unit whose
    // spelling differs in a future language still sizes its own field.
    const std::int32_t unit_advance = text_advance(unit_font, text::kPrintingPercentUnit);

    digit_advance_ = digit_advance;
    unit_advance_ = unit_advance;

    // The widest the pair can ever be: three digits at 100 %. Checked against the ring's
    // chord here rather than discovered on the panel, because the leading digit grows
    // leftward and is the first thing that would touch the arc.
    const std::int32_t widest_block =
        (kPercentDigitColumns * digit_advance) + kValueToUnitGap + unit_advance;

    percent_value_ = theme::bare_label(root_);
    if (percent_value_ == nullptr) {
        diagnostics::log(diagnostics::LogLevel::Error, kTag, "percent value label failed");
        return false;
    }

    theme::apply_text_style(percent_value_, theme::TextRole::PrimaryValue, theme::Colour::Primary);

    // Sized to its content, not to a fixed three-digit field.
    //
    // A fixed field has to pad the digit it is not using, and right-aligning inside it put a
    // two-digit value 20 px right of the ring's centre — measured on the panel, and visible.
    // Since a print spends nearly its whole life at two digits, that is the composition
    // almost all the time, and the accepted artwork shows the numeral optically centred.
    //
    // §4's stability rule is not weakened by this. Tabular figures make every digit one
    // width, so 62 → 63 moves nothing at all; the pair recentres only at 9 → 10 and
    // 99 → 100, twice in a print, which is a change in the value's magnitude rather than
    // the update jitter §17.7 is aimed at.
    lv_obj_set_style_text_align(percent_value_, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN);
    lv_label_set_text(percent_value_, "");

    percent_unit_ = theme::bare_label(root_);
    if (percent_unit_ == nullptr) {
        diagnostics::log(diagnostics::LogLevel::Error, kTag, "percent unit label failed");
        return false;
    }

    // §4: "Units shall not alter numeric alignment." The unit sits at a fixed
    // position after the field, never after the digits, so its own width can never
    // move a numeral.
    theme::apply_text_style(percent_unit_, theme::TextRole::PrimaryUnit, theme::Colour::Primary,
                            theme::Opacity::High);

    // Baseline-aligned with the numerals rather than top-aligned, which is what UI
    // Design Decision 001's asset shows: the smaller unit sits on the same baseline
    // as the large value.
    // OPTICALLY CENTRED, NOT BASELINE ALIGNED.
    //
    // Baseline alignment is the typographic default and it measured wrong on the panel: the
    // unit's ink centre came out 11 px below the numeral's, so the `%` hugged the baseline
    // while the numeral towered over it. The accepted UIDD 001 artwork sets the unit
    // optically centred on the numeral, which is what the eye reads as level.
    //
    // Derived from the two faces' ink extents rather than from a measured constant, so it
    // stays correct if either face is regenerated at a different size.
    const auto value_ink = fonts::ink_extent(value_font, '0');
    const auto unit_ink_metrics = fonts::ink_extent(unit_font, '%');

    if (value_ink.known && unit_ink_metrics.known) {
        baseline_offset_ = ((value_ink.top + value_ink.bottom) / 2) -
                           ((unit_ink_metrics.top + unit_ink_metrics.bottom) / 2);
    } else {
        // Falls back to the baseline, which is wrong optically but never wrong structurally.
        baseline_offset_ = ascent_of(value_font) - ascent_of(unit_font);
    }
    set_label_text(percent_unit_, text::kPrintingPercentUnit);

    // Placed for the widest case at build time so the objects have somewhere to be; apply()
    // recentres the pair for the digit count actually being shown.
    reposition_value(kPercentDigitColumns);

    state_title_ = theme::bare_label(root_);
    if (state_title_ == nullptr) {
        diagnostics::log(diagnostics::LogLevel::Error, kTag, "state title label failed");
        return false;
    }

    // Owner-reviewed 2026-09-06: native whole-pixel fonts interpolate from 20 to 28 px.
    // Titles remain achromatic except the existing Completed success treatment.
    theme::apply_text_style(state_title_, theme::TextRole::OutcomeTitle, theme::Colour::Primary);
    title_raster_.attach(state_title_);
    set_label_text(state_title_, text::kPrintingStateTitle);
    centre_on_ring(state_title_, theme::kRegionOperational.y + kTitleRowTop);

    third_line_ = theme::bare_label(root_);
    if (third_line_ == nullptr) {
        diagnostics::log(diagnostics::LogLevel::Error, kTag, "third line label failed");
        return false;
    }

    // The third line carries whichever of two things the state has: a remaining duration
    // (Printing) or a subtitle (Preparing). One label rather than two, because they never
    // coexist and §7 requires equivalent information to occupy equivalent positions — two
    // labels in the same slot would be two positions that happen to coincide.
    //
    // Centred rather than given a fixed field like the percentage. §4's stability rule
    // is aimed at values that update, and no one-minute change alters a duration's
    // width; only crossing ten hours does, once, downward. §17.7's actual concern is
    // "no rapid text jitter from small estimate changes", which centring does not cause.
    theme::apply_text_style(third_line_, theme::TextRole::Body, theme::Colour::Secondary,
                            theme::Opacity::High);
    lv_label_set_text(third_line_, "");
    centre_on_ring(third_line_, theme::kRegionOperational.y + kThirdRowTop);

    for (std::size_t i = 0; i < 2; ++i) {
        pause_bars_[i] = theme::bare_object(root_);
        if (pause_bars_[i] == nullptr) {
            return false;
        }
        theme::place(pause_bars_[i],
                     {theme::kPauseLeft + static_cast<std::int32_t>(i) * theme::kPauseBarSpacing,
                      theme::kPauseTop, theme::kPauseBarWidth, theme::kPauseBarHeight});
        lv_obj_set_style_bg_color(pause_bars_[i], theme::colour(theme::Colour::Primary),
                                  LV_PART_MAIN);
        lv_obj_set_style_bg_opa(pause_bars_[i], theme::opacity(theme::Opacity::Transparent),
                                LV_PART_MAIN);
    }

    error_outline_ = lv_line_create(root_);
    error_mark_ = theme::bare_label(root_);
    if (error_outline_ == nullptr || error_mark_ == nullptr) {
        return false;
    }
    lv_obj_remove_style_all(error_outline_);
    lv_line_set_points(error_outline_, kErrorOutline.data(), kErrorOutline.size());
    lv_obj_set_pos(error_outline_, theme::kRingCentreX - theme::kPrinterErrorIconHalfWidth,
                   theme::kPrinterErrorIconTop);
    lv_obj_set_style_line_width(error_outline_, theme::kSpace1, LV_PART_MAIN);
    lv_obj_set_style_line_rounded(error_outline_, true, LV_PART_MAIN);
    lv_obj_set_style_line_color(error_outline_, theme::colour(theme::Colour::Error), LV_PART_MAIN);
    theme::apply_text_style(error_mark_, theme::TextRole::Body, theme::Colour::Error);
    lv_obj_set_style_text_font(error_mark_, fonts::motion_title(theme::kPrinterErrorTitleSize),
                               LV_PART_MAIN);
    set_label_text(error_mark_, text::TextId::AccessWarningMark);
    centre_on_ring(error_mark_, theme::kPrinterErrorIconTop + theme::kSpace2);
    lv_obj_add_flag(error_outline_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(error_mark_, LV_OBJ_FLAG_HIDDEN);

    // --- The degraded-confidence indicator (§11.5) ----------------------------

    stale_indicator_ = theme::bare_label(root_);
    if (stale_indicator_ == nullptr) {
        diagnostics::log(diagnostics::LogLevel::Error, kTag, "stale indicator failed");
        return false;
    }

    // §11.5 permits stale values to be shown only behind an EXPLICIT indicator, and UI
    // Behaviour Decision 001 §7 is blunter: "The product must never present stale information
    // as current."
    //
    // NOT AN ATTENTION INDICATOR. §8's Attention Indicator "uses semantic warning or error
    // colours only", and reduced confidence is neither — UIBD 001 §8 Level 3 requires it to
    // communicate uncertainty "without creating unnecessary urgency". It states the fact
    // in readable secondary text rather than colouring the composition red.
    // Owner's 2026-09-07 age-indicator preview: readable Body 14 confidence copy,
    // independent of operational energy, without the Ready printer alias.
    theme::apply_text_style(stale_indicator_, theme::TextRole::Body, theme::Colour::Secondary,
                            theme::Opacity::Full);
    lv_label_set_text(stale_indicator_, "");
    stale_motion_.target(OperationalPose{});
    lv_obj_add_flag(stale_indicator_, LV_OBJ_FLAG_HIDDEN);

#if defined(CONFIG_PRD_UI_RENDER_REPORT)
    // Forces LVGL to resolve sizes now, so the figures below are what will be drawn rather
    // than what was requested. A content-sized label reports 0 until this runs.
    lv_obj_update_layout(root_);

    // Ink, not boxes. This is what the eye aligns, and it is the only way to tell whether
    // the unit is optically level with the numeral or merely sitting on the same baseline.
    // Build-time, because it is a property of the faces rather than of any one state.
    const auto digit_ink = fonts::ink_extent(value_font, '0');
    const auto unit_ink = fonts::ink_extent(unit_font, '%');

    if (digit_ink.known && unit_ink.known) {
        const std::int32_t digit_top = lv_obj_get_y(percent_value_) + digit_ink.top;
        const std::int32_t digit_bottom = lv_obj_get_y(percent_value_) + digit_ink.bottom;
        const std::int32_t unit_top = lv_obj_get_y(percent_unit_) + unit_ink.top;
        const std::int32_t unit_bottom = lv_obj_get_y(percent_unit_) + unit_ink.bottom;

        diagnostics::log(diagnostics::LogLevel::Info, kTag,
                         "ink digit=" + std::to_string(digit_top) + ".." +
                             std::to_string(digit_bottom) + " unit=" + std::to_string(unit_top) +
                             ".." + std::to_string(unit_bottom) +
                             " digit_mid=" + std::to_string((digit_top + digit_bottom) / 2) +
                             " unit_mid=" + std::to_string((unit_top + unit_bottom) / 2) +
                             " baseline_delta=" + std::to_string(unit_bottom - digit_bottom));
    }

    // The per-row layout report is NOT emitted here any more. Before Phase E the form was
    // fixed, so build time was as good a moment as any; the form is now a function of the
    // state's energy and no state has been applied yet, so a report here would measure the
    // rows against a form that does not exist. It moves to apply(), where it reports the
    // composition that is actually on the panel — once per state, which is also what the
    // operational walk wants.
#endif

    diagnostics::log(diagnostics::LogLevel::Info, kTag,
                     "operational scene built: hero=" + std::to_string(theme::kFontSizeHero) +
                         " digit_adv=" + std::to_string(digit_advance) +
                         " widest_block=" + std::to_string(widest_block) +
                         " baseline_offset=" + std::to_string(baseline_offset_));
    return true;
}

#if defined(CONFIG_PRD_UI_RENDER_REPORT)

void OperationalScene::log_composition() {
    // Resolve sizes first: a content-sized label reports 0 until this runs, and the labels
    // were only just given this state's text.
    lv_obj_update_layout(root_);

    const std::optional<scene::FormGeometry>& form = ring_.form();

    // Formatted from the SAME published report the validation flows read back, rather than from
    // a second walk over the geometry. Two accounts of one form are two things that can
    // disagree, and this project has already been burned by exactly that: a clearance gauge
    // that measured a box while the ink sat elsewhere reported a collision that did not exist.
    const OperationalFormReport report = form_report();

    if (report.drawn) {
        diagnostics::log(
            diagnostics::LogLevel::Info, kTag,
            "form e=" + std::to_string(report.energy) + " r=" + std::to_string(report.radius) +
                " cy=" + std::to_string(report.centre_y) + " half=" +
                std::to_string(report.half_angle) + " extent=" + std::to_string(report.extent) +
                " rot=" + std::to_string(report.rotation) +
                " mid=" + std::to_string(report.midpoint_y) +
                " top=" + std::to_string(report.top_y) + " low=" + std::to_string(report.bottom_y) +
                " sag=" + std::to_string(report.sagitta) + " cx=" +
                std::to_string(report.centre_x) + " hw=" + std::to_string(report.half_width) +
                " reach=" + std::to_string(report.half_extent) + " nominal=" +
                std::to_string(report.nominal_left) + ".." + std::to_string(report.nominal_right) +
                " paint=" + std::to_string(report.paint_left) + ".." +
                std::to_string(report.paint_right) + " accent=" + hex_colour(report.accent) +
                " closed=" + (report.closed ? "yes" : "no") +
                " record=" + (report.draws_record ? "yes" : "no") +
                " cycle_alpha=" + std::to_string(report.cycle_treatment_alpha));
    } else {
        diagnostics::log(diagnostics::LogLevel::Info, kTag, "form absent");
    }

    // Report the body's painted coordinate, not the form's geometry energy: Completed's
    // body now stays at Printing level under the September 8 owner repair.
    const std::int32_t level = report.drawn ? painted_body_level() : theme::kBodyLevelFull;
    diagnostics::log(diagnostics::LogLevel::Info, kTag,
                     "body level=" + std::to_string(level) + "/" +
                         std::to_string(theme::kBodyLevelFull) + " stale-indicator=excluded");

    log_row("percent", percent_value_, form);
    log_row("unit", percent_unit_, form);
    log_row("title", state_title_, form);
    log_row("third_line", third_line_, form);
    log_row("stale", stale_indicator_, form);
    telemetry_.log_geometry(form);
}

#endif // CONFIG_PRD_UI_RENDER_REPORT

void OperationalScene::reposition_value(const std::size_t digits) {
    if (percent_value_ == nullptr || percent_unit_ == nullptr) {
        return;
    }

    // Exact arithmetic, not a measurement: tabular figures mean every digit is one advance
    // wide, so the value's width is its digit count times that advance. That is the whole
    // point of §4's tabular requirement — it makes the layout computable.
    // Retained so that a stack rearrangement can put the pair on its new row without waiting for
    // the value to change. The width belongs to the value; the row belongs to the stack.
    percent_digits_ = digits;

    const std::int32_t value_width = static_cast<std::int32_t>(digits) * digit_advance_;
    const std::int32_t block_width = value_width + kValueToUnitGap + unit_advance_;
    const std::int32_t left = theme::kRingCentreX - (block_width / 2);
    const std::int32_t row_top = theme::kRegionOperational.y + percent_row_top_;

    lv_obj_set_width(percent_value_, value_width);
    lv_obj_set_pos(percent_value_, left, row_top);
    lv_obj_set_pos(percent_unit_, left + value_width + kValueToUnitGap, row_top + baseline_offset_);
}

void OperationalScene::apply(const app_model::Presentation& presentation,
                             const PresentationDelta& delta) {
    if (root_ == nullptr) {
        return;
    }

    // WHAT THIS STATE SHOWS is resolved once, purely, and host-tested. The scene applies it.
    const OperationalContent content = resolve(presentation);
    content_ = content;
    printer_condition_ =
        (presentation.subject_condition == app_model::SubjectCondition::Error ||
         presentation.subject_condition == app_model::SubjectCondition::Attention) &&
        presentation.confidence != app_model::Confidence::Unavailable;

    if (delta.full || delta.update || delta.content || delta.stale) {
        const bool show_update = presentation.ota_phase == app_model::OtaPhase::Available &&
                                 presentation.subject == app_model::SubjectState::Ready &&
                                 presentation.at_visual_rest;
        update_available_.show(show_update);
    }
    // Remember the last current validity layout: ageing must not manufacture the distinct
    // no-reading layout. The confidence row may reserve space without changing that validity.
    if (presentation.confidence == app_model::Confidence::Current) {
        unknown_progress_ = presentation.subject == app_model::SubjectState::Printing &&
                            !presentation.progress.known;
    } else if (presentation.subject != app_model::SubjectState::Printing) {
        unknown_progress_ = false;
    }
    auto destination = operational_pose(content, presentation.show_telemetry, unknown_progress_);
    if (presentation.stale_indicator && content.arrangement == Arrangement::Operational &&
        !unknown_progress_) {
        // Reserve a real gap above the confidence row, not merely disjoint glyph pixels.
        // Move the existing title/record group together on the same motion clock; current
        // layouts, record validity, type sizes and the form itself remain unchanged.
        double lift = theme::kSpace6;
        if (destination.values[OperationalPose::PauseAlpha] > 0.0) {
            // The retained record must also clear the existing pause mark. Constrain the
            // whole stack by that mark's geometry, without changing fonts or the mark itself.
            lift = std::min(lift, destination.values[OperationalPose::ValueY] -
                                      (theme::kPauseTop + theme::kPauseBarHeight + theme::kSpace1));
        }
        destination.values[OperationalPose::TitleY] -= lift;
        destination.values[OperationalPose::ValueY] -= lift;
    }
    if (presentation.stale_indicator && content.form == kEnergyCompleted) {
        destination.values[OperationalPose::ValueY] -= theme::kSpace4;
    }
    if (presentation.stale_indicator && unknown_progress_) {
        destination.values[OperationalPose::ThirdY] -= theme::kSpace1;
    }
    if (printer_condition_) {
        destination.values[OperationalPose::TitleY] = theme::kPrinterErrorTitleY;
        destination.values[OperationalPose::TitleSize] = theme::kPrinterErrorTitleSize;
        destination.values[OperationalPose::ThirdY] = theme::kPrinterErrorContextY;
        destination.values[OperationalPose::RecordAlpha] = 0.0;
        destination.values[OperationalPose::ConditionAlpha] = 255.0;
        if (presentation.stale_indicator) {
            destination.values[OperationalPose::TitleY] -= theme::kSpace4;
            destination.values[OperationalPose::ConditionContextY] -= theme::kSpace4;
            destination.values[OperationalPose::ConditionIconY] -= theme::kSpace2;
        }
    }
    if (blank_entry_) {
        motion_.resume_from_blank(destination);
        blank_entry_ = false;
    } else {
        motion_.target(destination);
    }

    // Placement is renderer-owned and interpolated; semantic readings below update immediately.

    // Each block below is guarded by its own flag, so a revision that moved only the bed
    // temperature invalidates one label's area and nothing else (§11 Redraw, §17.4).

    if (delta.progress || delta.content) {
        if (content.show_progress) {
            const auto percent = format::percent(presentation.progress);
            lv_label_set_text(percent_value_, percent.c_str());

            // Recentred on the digit count, so 8 %, 62 % and 100 % are each optically centred
            // on the ring rather than one of them being centred and the others padded.
            if (!percent.empty()) {
                reposition_value(percent.size());
            }

            // The unit follows the value's presence, not its own: a bare "%" with no
            // number would be a placeholder for a value that is absent.
            const bool has_value = !percent.empty();
            lv_obj_remove_flag(percent_value_, LV_OBJ_FLAG_HIDDEN);
            if (has_value) {
                lv_obj_remove_flag(percent_unit_, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(percent_unit_, LV_OBJ_FLAG_HIDDEN);
            }
        } else {
            // The resting states carry no progress. Both halves of the pair are hidden — not
            // blanked — so nothing occupies the row they would have used.
            lv_obj_add_flag(percent_value_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(percent_unit_, LV_OBJ_FLAG_HIDDEN);
        }

        ring_.set_progress(presentation.progress);
    }

    if (delta.content || delta.printer_name || delta.stale) {
        apply_title(presentation, content);
    }

    if (delta.remaining || delta.content || delta.progress || delta.stale) {
        apply_third_line(presentation, content);
    }

    if (delta.nozzle) {
        telemetry_.set_value(component::TelemetrySlot::Nozzle,
                             format::temperature(presentation.nozzle.current).view());
    }

    if (delta.bed) {
        telemetry_.set_value(component::TelemetrySlot::Bed,
                             format::temperature(presentation.bed.current).view());
    }

    if (delta.layers) {
        telemetry_.set_value(component::TelemetrySlot::Layer,
                             format::layers(presentation.layers).view());
    }

    paint_motion();

#if defined(CONFIG_PRD_UI_RENDER_REPORT)
    // Reported per applied composition rather than once at build time: the form is a function
    // of the state's energy, so "what is on the panel" is only knowable here.
    if (delta.full || delta.content) {
        log_composition();
    }
#endif

    if (stale_target_ != presentation.stale_indicator) {
        const double previous = stale_motion_.current().values[OperationalPose::AppearanceAlpha];
        const double painted = stale_target_ ? previous : std::max(0.0, 2.0 * previous - 255.0);
        // The recovery's two row phases use a different alpha mapping. Rebase from actual
        // painted opacity when reversing, not from that mapping's internal coordinate.
        OperationalPose seed;
        seed.values[OperationalPose::AppearanceAlpha] =
            presentation.stale_indicator ? painted : (painted + 255.0) / 2.0;
        stale_motion_ = OperationalMotion{};
        stale_motion_.target(seed);
    }
    stale_target_ = presentation.stale_indicator;
    if (stale_target_) {
        stale_observed_at_ = presentation.stale_observed_at;
    }
    OperationalPose indicator;
    indicator.values[OperationalPose::AppearanceAlpha] = stale_target_ ? 255.0 : 0.0;
    stale_motion_.target(indicator); // Same target (including age updates) never restarts motion.
    paint_stale(lv_tick_get());
    paint_condition();
}

bool OperationalScene::step(const bool panel_visible, const std::uint32_t now) {
    const bool moving = motion_.running();
    const bool running = motion_.step(panel_visible, now);
    const bool stale_running = stale_motion_.step(panel_visible, now);
    if (panel_visible) {
        if (moving) {
            paint_motion(); // Includes the final frame.
        }
        paint_stale(now);
        paint_condition();
    }
    return running || stale_running;
}

void OperationalScene::paint_stale(const std::uint32_t now) {
    const double alpha = stale_motion_.current().values[OperationalPose::AppearanceAlpha];
    const auto text = format::observation_age(stale_observed_at_, now);
    if (text != stale_text_) {
        stale_text_ = text;
        lv_label_set_text(stale_indicator_, stale_text_.c_str());
    }
    // Confidence occupies its own row; the Ready reference is centred on the whole canvas.
    const bool ready = content_.form == kEnergyReady && !printer_condition_;
    const auto* font = fonts::for_role(ready ? theme::TextRole::Body : theme::TextRole::Label);
    if (lv_obj_get_style_text_font(stale_indicator_, LV_PART_MAIN) != font) {
        lv_obj_set_style_text_font(stale_indicator_, font, LV_PART_MAIN);
    }
    const auto centre = ready ? theme::kDisplayWidth / 2 : theme::kRingCentreX;
    lv_obj_update_layout(stale_indicator_);
    const bool open_outcome =
        content_.form == kEnergyPreparing || content_.form == kEnergyCompleted;
    const auto age_y = printer_condition_ ? theme::kPrinterErrorAgeY
                       : ready            ? theme::kStaleReadyY
                       : open_outcome     ? theme::kStalePreparingY
                                          : theme::kStaleIndicatorY;
    lv_obj_set_pos(stale_indicator_, centre - lv_obj_get_width(stale_indicator_) / 2, age_y);
    // On recovery the restored estimate can share this row. Retire the old confidence text
    // before revealing that estimate; never overprint two different statements.
    if (!stale_target_ && !printer_condition_) {
        const auto level = painted_body_level();
        theme::apply_text_level(
            third_line_, theme::Opacity::High,
            static_cast<std::int32_t>(level * std::max(0.0, 255.0 - 2.0 * alpha) / 255.0));
    }
    if (!stale_target_ && alpha == 0.0) {
        stale_observed_at_.reset();
    }
    double painted = stale_target_ ? alpha : std::max(0.0, 2.0 * alpha - 255.0);
    // The age row stays fixed while the title moves. Gate its reveal on actual
    // glyph clearance, not elapsed-time guesses: reserve space.1 before any ink appears,
    // then gradually reveal up to space.2. This uses the existing motion, with no timer,
    // changed endpoint or per-state phase constant. Eight pixels is the settled contract;
    // intermediate smaller gaps carry proportionally less age ink, never an overprint.
    if (painted > 0.0 && !lv_obj_has_flag(state_title_, LV_OBJ_FLAG_HIDDEN)) {
        const auto age_ink = label_ink(stale_indicator_);
        const auto ink = label_ink(state_title_);
        if (ink.known) {
            const auto y = lv_obj_get_y(state_title_);
            const auto top = title_raster_.active() ? std::min(y + ink.top, title_raster_.ink_top())
                                                    : y + ink.top;
            const auto bottom = title_raster_.active()
                                    ? std::max(y + ink.bottom, title_raster_.ink_bottom())
                                    : y + ink.bottom;
            const auto gap = std::max(age_y + age_ink.top - bottom, top - age_y - age_ink.bottom);
            painted *= std::clamp(static_cast<double>(gap - theme::kSpace1) /
                                      (theme::kSpace2 - theme::kSpace1),
                                  0.0, 1.0);
        }
    }
    if (printer_condition_ && painted > 0.0) {
        lv_obj_update_layout(third_line_);
        const auto context_ink = label_ink(third_line_);
        const auto age_ink = label_ink(stale_indicator_);
        const auto gap = age_y + age_ink.top - lv_obj_get_y(third_line_) - context_ink.bottom;
        painted *= std::clamp(static_cast<double>(gap - theme::kSpace1) / theme::kSpace1, 0.0, 1.0);
    }
    const auto opacity = static_cast<lv_opa_t>(std::lround(painted));
    if (lv_obj_get_style_text_opa(stale_indicator_, LV_PART_MAIN) != opacity) {
        lv_obj_set_style_text_opa(stale_indicator_, opacity, LV_PART_MAIN);
    }
    if (painted > 0.0 && !stale_text_.empty()) {
        lv_obj_remove_flag(stale_indicator_, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(stale_indicator_, LV_OBJ_FLAG_HIDDEN);
    }
}

std::optional<FormPosition> OperationalScene::painted_form() const {
    if (!content_.form.has_value()) {
        return std::nullopt;
    }
    const auto& pose = motion_.current().values;
    return FormPosition{
        static_cast<Energy>(std::lround(pose[OperationalPose::EnergyField])),
        static_cast<std::int32_t>(std::lround(pose[OperationalPose::OffsetY])),
        static_cast<std::uint8_t>(std::lround(pose[OperationalPose::AppearanceAlpha]))};
}

void OperationalScene::paint_motion() {
    using P = OperationalPose;
    const auto& pose = motion_.current().values;
    // Ordinary inherited opacity, not layered opacity or a transform allocation. This
    // appearance channel is independent of energy and remains full outside blank entry.
    lv_obj_set_style_opa(root_, static_cast<lv_opa_t>(std::lround(pose[P::AppearanceAlpha])),
                         LV_PART_MAIN);
    const auto energy = static_cast<Energy>(std::lround(pose[P::EnergyField]));
    ring_.set_form(content_.form.has_value() ? std::optional<Energy>{energy} : std::nullopt,
                   content_.show_progress, static_cast<std::int32_t>(std::lround(pose[P::OffsetY])),
                   static_cast<std::uint8_t>(std::lround(pose[P::CycleTreatmentAlpha])));
    OperationalContent painted = content_;
    painted.form = energy;
    apply_luminance(painted);

    // Whole-pixel font sizes share the form's progress. No transform layer is allocated.
    // The large ARGB layer required by scaled labels exceeded the fixed 64 KiB pool.
    const auto value_size = static_cast<std::int32_t>(std::lround(pose[P::ValueSize]));
    const auto unit_size = static_cast<std::int32_t>(
        std::lround(pose[P::ValueSize] * theme::kFontSizeDisplay / theme::kFontSizeHero));
    const auto* value_font = fonts::motion_value(value_size);
    const auto* unit_font = fonts::motion_unit(unit_size);
    const auto* title_font =
        fonts::motion_title(static_cast<std::int32_t>(std::lround(pose[P::TitleSize])));
    lv_obj_set_style_text_font(percent_value_, value_font, LV_PART_MAIN);
    lv_obj_set_style_text_font(percent_unit_, unit_font, LV_PART_MAIN);
    lv_obj_set_style_text_font(state_title_, title_font, LV_PART_MAIN);
    const auto digit_width = lv_font_get_glyph_width(value_font, '0', 0);
    const auto unit_width = lv_font_get_glyph_width(unit_font, '%', 0);
    const auto value_width = static_cast<std::int32_t>(percent_digits_) * digit_width;
    const auto block_width = value_width + kValueToUnitGap + unit_width;
    const auto left = theme::kRingCentreX - block_width / 2;
    const auto value_ink = fonts::ink_extent(value_font, '0');
    const auto unit_ink = fonts::ink_extent(unit_font, '%');
    const auto offset = ((value_ink.top + value_ink.bottom) - (unit_ink.top + unit_ink.bottom)) / 2;
    const auto value_y = static_cast<std::int32_t>(std::lround(pose[P::ValueY]));
    lv_obj_set_width(percent_value_, value_width);
    lv_obj_set_pos(percent_value_, left, value_y);
    lv_obj_set_pos(percent_unit_, left + value_width + kValueToUnitGap, value_y + offset);
    // A face switch changes ascent discretely. Anchor the baseline to the continuously
    // interpolated ascent, otherwise a growing title can step down while travelling up.
    // At integer sizes the correction is exactly zero, preserving every settled layout.
    const auto title_size = pose[P::TitleSize];
    const auto lower_size = static_cast<std::int32_t>(std::floor(title_size));
    const auto* lower_font = fonts::motion_title(lower_size);
    const auto* upper_font = fonts::motion_title(lower_size + 1);
    const double fraction = title_size - lower_size;
    const double baseline = pose[P::TitleY] + ascent_of(lower_font) +
                            fraction * (ascent_of(upper_font) - ascent_of(lower_font));
    const auto title_y = static_cast<std::int32_t>(std::lround(baseline)) - ascent_of(title_font);
    centre_on_ring(state_title_, title_y);
    title_raster_.update(state_title_, {title_size, pose[P::TitleY]}, motion_.running());
    const auto& destination = motion_.destination().values;
    const auto* target_value_font =
        fonts::motion_value(static_cast<std::int32_t>(std::lround(destination[P::ValueSize])));
    const auto target_ink = fonts::ink_extent(target_value_font, '0');
    const auto ink_top = std::min(value_y + value_ink.top, value_y + offset + unit_ink.top);
    const auto target_top =
        static_cast<std::int32_t>(std::lround(destination[P::ValueY])) + target_ink.top;
    const auto record_alpha = motion_.reveal_record(
        record_enclosure(motion_.current(), motion_.destination(), ink_top, target_top));
    // A reflow can exchange title and record rows. Keep the current state word readable;
    // fade the subordinate record only while their line boxes cross. The four-pixel
    // approach is the existing space.1 token, not an extra timing or a state-specific path.
    if (!lv_obj_has_flag(state_title_, LV_OBJ_FLAG_HIDDEN) && content_.show_progress) {
        const auto record_bottom =
            std::max(value_y + value_font->line_height, value_y + offset + unit_font->line_height);
        const auto title_top =
            title_raster_.active() ? std::min(title_y, title_raster_.ink_top()) : title_y;
        const auto title_bottom =
            title_raster_.active()
                ? std::max(title_y + title_font->line_height, title_raster_.ink_bottom())
                : title_y + title_font->line_height;
        const auto clearance = std::max(title_top - record_bottom, value_y - title_bottom);
        const auto alpha = std::clamp<std::int32_t>(
            clearance * theme::kBodyLevelFull / theme::kSpace1, 0, theme::kBodyLevelFull);
        const auto level = static_cast<std::int32_t>(
            std::lround(painted_body_level() * alpha / theme::kBodyLevelFull * record_alpha /
                        theme::kBodyLevelFull));
        theme::apply_text_level(percent_value_, theme::Opacity::Full, level);
        theme::apply_text_level(percent_unit_, theme::Opacity::High, level);
    }
    centre_on_ring(third_line_,
                   printer_condition_
                       ? static_cast<std::int32_t>(std::lround(pose[P::ConditionContextY]))
                       : static_cast<std::int32_t>(std::lround(pose[P::ThirdY])));
    for (lv_obj_t* bar : pause_bars_) {
        lv_obj_set_style_bg_opa(bar, static_cast<lv_opa_t>(std::lround(pose[P::PauseAlpha])),
                                LV_PART_MAIN);
    }
    telemetry_.set_visible(pose[P::TelemetryAlpha] > 0.0);
    telemetry_.apply_level(static_cast<std::int32_t>(
        std::lround(painted_body_level() * pose[P::TelemetryAlpha] / theme::kBodyLevelFull)));
}

void OperationalScene::paint_condition() {
    if (!printer_condition_) {
        lv_obj_add_flag(error_outline_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(error_mark_, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    // The ordinary record still drives the sweep, but is qualified in the context row.
    // Never superimpose a large percentage and the warning symbol during entry.
    lv_obj_add_flag(percent_value_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(percent_unit_, LV_OBJ_FLAG_HIDDEN);
    theme::apply_text_level(state_title_, theme::Opacity::Full, theme::kBodyLevelFull);
    theme::apply_text_level(third_line_, theme::Opacity::High, theme::kBodyLevelFull);
    lv_obj_update_layout(state_title_);
    const auto ink = label_ink(state_title_);
    const auto top =
        title_raster_.active() ? title_raster_.ink_top() : lv_obj_get_y(state_title_) + ink.top;
    const auto bottom = title_raster_.active() ? title_raster_.ink_bottom()
                                               : lv_obj_get_y(state_title_) + ink.bottom;
    lv_obj_update_layout(third_line_);
    const auto context_ink = label_ink(third_line_);
    const auto context_gap = lv_obj_get_y(third_line_) + context_ink.top - bottom;
    const double context_gate =
        std::clamp(static_cast<double>(context_gap - theme::kSpace1) / theme::kSpace1, 0.0, 1.0);
    theme::apply_text_level(third_line_, theme::Opacity::High,
                            static_cast<std::int32_t>(theme::kBodyLevelFull * context_gate));
    const auto clearance = top - (static_cast<std::int32_t>(std::lround(
                                      motion_.current().values[OperationalPose::ConditionIconY])) +
                                  theme::kPrinterErrorIconHeight + theme::kSpace1 / 2);
    const auto icon_y = static_cast<std::int32_t>(
        std::lround(motion_.current().values[OperationalPose::ConditionIconY]));
    lv_obj_set_y(error_outline_, icon_y);
    if (lv_obj_get_y(error_mark_) != icon_y + theme::kSpace2) {
        lv_obj_set_y(error_mark_, icon_y + theme::kSpace2);
    }
    const double gate = std::clamp(static_cast<double>(clearance) / theme::kSpace1, 0.0, 1.0);
    const auto alpha = static_cast<lv_opa_t>(
        std::lround(gate * motion_.current().values[OperationalPose::ConditionAlpha]));
    if (lv_obj_get_style_line_opa(error_outline_, LV_PART_MAIN) != alpha) {
        lv_obj_set_style_line_opa(error_outline_, alpha, LV_PART_MAIN);
    }
    if (lv_obj_get_style_text_opa(error_mark_, LV_PART_MAIN) != alpha) {
        lv_obj_set_style_text_opa(error_mark_, alpha, LV_PART_MAIN);
    }
    lv_obj_remove_flag(error_outline_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(error_mark_, LV_OBJ_FLAG_HIDDEN);
}

std::int32_t OperationalScene::painted_body_level() const {
    return body_level(
        static_cast<Energy>(std::lround(motion_.current().values[OperationalPose::BodyEnergy])));
}

void OperationalScene::apply_luminance(const OperationalContent& content) {
    // Body luminance follows its own coordinate on the shared motion clock. The owner's
    // September 8 repair keeps Completed at Printing's body level while its form opens;
    // all other settled states retain their existing energy-to-luminance mapping.
    //
    // The fallback is the TOP of the scale, not the floor. A composition with no energy is not
    // a composition at rest, and a state outside the family never reaches this scene at all —
    // so nothing may recede by accident on a path that should not exist.
    const std::int32_t level =
        content.form.has_value() ? painted_body_level() : theme::kBodyLevelFull;

    // Each element scaled from the §5 step it was built with. §13.1 fixes text roles across all
    // six states, so these tokens are constants of the composition and not per-state choices.
    theme::apply_text_level(percent_value_, theme::Opacity::Full, level);
    theme::apply_text_level(percent_unit_, theme::Opacity::High, level);
    theme::apply_text_level(state_title_, theme::Opacity::Full, level);
    theme::apply_text_level(third_line_, theme::Opacity::High, level);

    telemetry_.apply_level(level);

    // THE STALE INDICATOR IS DELIBERATELY ABSENT. Owner ruling, 2026-08-08, on §10: confidence
    // is orthogonal to energy and "states itself explicitly". A marker whose whole job is to say
    // the machine cannot currently be observed, dimmed because the machine happens to be
    // carrying no job, would be energy speaking about observability — which is the one thing §10
    // keeps it away from. It holds the luminance build() gave it.
}

void OperationalScene::apply_title(const app_model::Presentation& presentation,
                                   const OperationalContent& content) {
    if (state_title_ == nullptr) {
        return;
    }

    if (printer_condition_) {
        lv_obj_set_style_text_color(state_title_, theme::colour(theme::Colour::Primary),
                                    LV_PART_MAIN);
        const bool attention =
            presentation.subject_condition == app_model::SubjectCondition::Attention;
        const auto colour =
            theme::colour(attention ? theme::Colour::Warning : theme::Colour::Error);
        lv_obj_set_style_line_color(error_outline_, colour, LV_PART_MAIN);
        lv_obj_set_style_text_color(error_mark_, colour, LV_PART_MAIN);
        set_label_text(state_title_,
                       attention ? text::TextId::PrinterAttention : text::TextId::PrinterError);
        lv_obj_remove_flag(state_title_, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    lv_obj_set_style_text_color(state_title_, theme::colour(content.title_colour), LV_PART_MAIN);

    switch (content.title) {
    case TitleSource::Catalogue:
        set_label_text(state_title_, content.title_id);
        lv_obj_remove_flag(state_title_, LV_OBJ_FLAG_HIDDEN);
        break;

    case TitleSource::PrinterName: {
        // Ready names the machine rather than its condition — §9 lists "Printer identity"
        // among its visible components and lists no state title. The name is data, so it does
        // not come from the catalogue; a printer called "Printing" would be its own name.
        const auto name = presentation.printer_name.view();
        lv_label_set_text_fmt(state_title_, "%.*s", static_cast<int>(name.size()), name.data());
        lv_obj_remove_flag(state_title_, LV_OBJ_FLAG_HIDDEN);
        break;
    }

    case TitleSource::None:
        // A resting device whose name was never learned. Nothing is invented to fill the
        // line — UI Design Decision 003: information that does not yet exist stays absent.
        lv_obj_add_flag(state_title_, LV_OBJ_FLAG_HIDDEN);
        break;
    }
}

void OperationalScene::apply_third_line(const app_model::Presentation& presentation,
                                        const OperationalContent& content) {
    if (third_line_ == nullptr) {
        return;
    }

    if (printer_condition_) {
        const auto context = format::error_context(presentation);
        lv_label_set_text(third_line_, context.c_str());
        // One fixed context role, including the longest retained terminal record.
        // Never change a font because a number or state name became wider.
        lv_obj_set_style_text_font(third_line_, fonts::for_role(theme::TextRole::Label),
                                   LV_PART_MAIN);
        lv_obj_remove_flag(third_line_, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_set_style_text_font(third_line_, fonts::for_role(theme::TextRole::Body), LV_PART_MAIN);

    // THE SLOT'S ONE RESOLUTION POINT — UIDD 004 §8. The third line is "a single slot holding
    // whichever machine fact is currently true", and where two facts compete, "activity
    // outranks estimate": what the machine is doing takes precedence over what it predicts.
    //
    // THAT PRECEDENCE IS RECORDED HERE AND NOT ENCODED, because the family has exactly one
    // candidate for this slot — Printing's remaining duration, an estimate. Preparing's
    // "Heating up" was the only activity the slot ever held and it was invented rather than
    // reported (API Research Report §16.4: "the API may not identify the exact preparation
    // phase"), so it went in Phase G.4. Writing an ordering now would mean inventing the
    // activity that wins it. When a real machine-supplied activity arrives, it is tested above
    // the estimate, and this comment is the specification for that change.
    if (content.show_remaining) {
        const auto remaining = format::remaining_duration(presentation.remaining,
                                                          presentation.remaining_is_approximate);
        lv_label_set_text(third_line_, remaining.c_str());
        lv_obj_remove_flag(third_line_, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    // The reviewed absence copy fills an otherwise empty unknown-progress presentation.
    // It is not a machine activity and must never displace a supplied remaining estimate.
    if (unknown_progress_) {
        set_label_text(third_line_, text::TextId::ProgressUnavailable);
        lv_obj_remove_flag(third_line_, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    // Ready, Preparing, Paused, Completed and Stopped carry no third line. Hidden rather than
    // blanked, so an empty label cannot occupy the slot — and hidden in place, so the label
    // stays exactly where build() put it and the rows around it do not move.
    lv_obj_add_flag(third_line_, LV_OBJ_FLAG_HIDDEN);
}

void OperationalScene::set_visible(const bool visible) {
    if (root_ == nullptr) {
        return;
    }

    if (visible) {
        lv_obj_remove_flag(root_, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(root_, LV_OBJ_FLAG_HIDDEN);
        stale_motion_ = OperationalMotion{};
        stale_motion_.target(OperationalPose{});
        stale_observed_at_.reset();
        stale_text_.clear();
        stale_target_ = false;
        lv_obj_add_flag(stale_indicator_, LV_OBJ_FLAG_HIDDEN);
    }
}

} // namespace prd::ui::scene
