// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include "fonts/fonts.hpp"

#include <array>
#include <string>

#include "diagnostics/log.hpp"
#include "diagnostics/tags.hpp"

// The generated faces. Declared here rather than in a generated header because
// lv_font_conv emits only the definition, and a hand-written extern beside the role
// mapping keeps the two in one place — a face added to the generator that is not
// added here is a link error rather than a face nothing draws.
extern "C" {
extern const lv_font_t prd_font_hero_68;
extern const lv_font_t prd_font_display_36;
extern const lv_font_t prd_font_title_20;
extern const lv_font_t prd_font_outcome_28;
extern const lv_font_t prd_font_section_16;
extern const lv_font_t prd_font_body_14;
extern const lv_font_t prd_font_caption_12;
extern const lv_font_t prd_font_micro_10;
}

namespace prd::ui::fonts {
namespace {

constexpr std::string_view kTag = diagnostics::tag::kUi;

// The ten digits, as LVGL letter codepoints.
constexpr std::array<std::uint32_t, 10> kDigits{'0', '1', '2', '3', '4', '5', '6', '7', '8', '9'};

struct FaceEntry {
    theme::TextRole role;
    const lv_font_t* font;
    std::string_view label;
};

// One entry per §4 text role. The label is the generator's role name, so a log line
// can be matched against tools/generate-fonts.py without a lookup.
const std::array<FaceEntry, theme::kTextRoleCount> kFaces{{
    {theme::TextRole::OutcomeTitle, &prd_font_outcome_28, "outcome_28"},
    {theme::TextRole::PrimaryValue, &prd_font_hero_68, "hero_68"},
    {theme::TextRole::PrimaryUnit, &prd_font_display_36, "display_36"},
    {theme::TextRole::StateTitle, &prd_font_title_20, "title_20"},
    {theme::TextRole::Section, &prd_font_section_16, "section_16"},
    {theme::TextRole::Body, &prd_font_body_14, "body_14"},
    {theme::TextRole::Label, &prd_font_caption_12, "caption_12"},
    {theme::TextRole::Metadata, &prd_font_micro_10, "micro_10"},
}};

} // namespace

const lv_font_t* for_role(const theme::TextRole role) {
    for (const auto& entry : kFaces) {
        if (entry.role == role) {
            return entry.font;
        }
    }

    // Unreachable while kFaces covers the enumeration, which a static_assert on
    // kTextRoleCount and the loop above together guarantee. Returns a real face
    // rather than nullptr because LVGL dereferences the font inside its draw path.
    return &prd_font_body_14;
}

DigitMetrics digit_metrics(const lv_font_t* const font) {
    DigitMetrics metrics;

    if (font == nullptr) {
        return metrics;
    }

    // letter_next = 0 asks for the plain advance with no kerning pair applied, which
    // is the property tabular figures are about: the space one digit occupies,
    // independent of what follows it.
    metrics.minimum = lv_font_get_glyph_width(font, kDigits[0], 0);
    metrics.maximum = metrics.minimum;

    for (const std::uint32_t digit : kDigits) {
        const std::uint16_t width = lv_font_get_glyph_width(font, digit, 0);
        metrics.minimum = width < metrics.minimum ? width : metrics.minimum;
        metrics.maximum = width > metrics.maximum ? width : metrics.maximum;
    }

    metrics.advance = metrics.maximum;
    metrics.uniform = metrics.minimum == metrics.maximum;
    return metrics;
}

InkExtent ink_extent(const lv_font_t* const font, const std::uint32_t letter) {
    InkExtent extent;

    if (font == nullptr) {
        return extent;
    }

    lv_font_glyph_dsc_t descriptor{};
    if (!lv_font_get_glyph_dsc(font, &descriptor, letter, 0)) {
        return extent;
    }

    // LVGL measures ofs_y upward from the baseline to the bitmap's bottom edge, and the
    // baseline sits (line_height - base_line) below the box's top.
    const auto baseline =
        static_cast<std::int32_t>(font->line_height) - static_cast<std::int32_t>(font->base_line);

    extent.height = static_cast<std::int32_t>(descriptor.box_h);
    extent.bottom = baseline - static_cast<std::int32_t>(descriptor.ofs_y);
    extent.top = extent.bottom - extent.height;
    extent.known = true;
    return extent;
}

bool verify() {
    bool all_uniform = true;
    bool all_fit = true;

    for (const auto& entry : kFaces) {
        const DigitMetrics metrics = digit_metrics(entry.font);

        if (!metrics.uniform) {
            all_uniform = false;
        }

        // Does the face fit the row its §4 token reserves?
        //
        // A face whose own line_height exceeds its token overflows into the row below, and
        // nothing else would say so — the composition would simply look subtly wrong. This
        // check exists because it happened: the hero token was first set from a predicted
        // cap-height ratio and came out 3 px short of the rasteriser's actual figure.
        // Predicting a font metric is guessing; this reads it.
        const auto reserved = theme::line_height(entry.role);
        const auto actual = static_cast<std::int32_t>(entry.font->line_height);
        const bool fits = actual <= reserved;

        if (!fits) {
            all_fit = false;
        }

        diagnostics::log(
            (metrics.uniform && fits) ? diagnostics::LogLevel::Info : diagnostics::LogLevel::Error,
            kTag,
            "font " + std::string{entry.label} + " line=" + std::to_string(actual) + "/" +
                std::to_string(reserved) + " digit_adv=" + std::to_string(metrics.advance) +
                " digit_min=" + std::to_string(metrics.minimum) + " digit_max=" +
                std::to_string(metrics.maximum) + " tabular=" + (metrics.uniform ? "yes" : "NO") +
                " fits_row=" + (fits ? "yes" : "NO"));
    }

    if (!all_fit) {
        diagnostics::log(diagnostics::LogLevel::Error, kTag,
                         "a generated face is taller than the line height its text role "
                         "reserves; it will overflow into the row below");
    }

    if (!all_uniform) {
        diagnostics::log(diagnostics::LogLevel::Error, kTag,
                         "tabular figures absent: Production UI Specification §4 requires "
                         "tabular figures; regenerate with tools/generate-fonts.py");
    }

    return all_uniform && all_fit;
}

} // namespace prd::ui::fonts
