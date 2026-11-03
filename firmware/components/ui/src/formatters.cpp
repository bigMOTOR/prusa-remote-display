// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include "formatters.hpp"
#include "text/text.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <string_view>

namespace prd::ui::format {
namespace {

// U+00B0 as UTF-8. Written as an escape rather than a literal degree sign so the
// file's own encoding cannot change what gets rasterised, and so it is visible that
// this is two bytes rather than one. It is in the generated faces' subset
// (tools/generate-fonts.py) — the only non-ASCII codepoint that is.
constexpr const char* kDegreeSign = "\xC2\xB0";

// Three ASCII periods, deliberately not U+2026. The generated faces cover printable
// ASCII plus the degree sign, so a real ellipsis character would resolve to a
// missing glyph and draw nothing — a truncation marker that vanishes is worse than
// no truncation marker.
constexpr const char* kEllipsis = "...";
constexpr std::size_t kEllipsisLength = 3;

// Middle truncation split for filename(). The head keeps more than the tail because
// a print's distinguishing text is usually at the front, while the tail is often a
// shared suffix such as a plate or material name.
constexpr std::size_t kFilenameHead = 20;
constexpr std::size_t kFilenameTail = NameText::capacity() - kFilenameHead - kEllipsisLength;

static_assert(kFilenameHead + kEllipsisLength + kFilenameTail == NameText::capacity(),
              "the truncated form must fill the capacity exactly");
static_assert(kFilenameTail > 0, "the tail must survive the split");

// Every formatter writes into one of these and hands the result to assign(). One
// byte over capacity so a value that exactly fills the token still terminates.
using Buffer = std::array<char, ValueText::capacity() + 1>;
using NameBuffer = std::array<char, NameText::capacity() + 1>;
using UpdateVersionBuffer = std::array<char, UpdateVersionText::capacity() + 1>;

// Accepts what snprintf wrote, or nothing at all. A truncated number is a *wrong*
// reading rather than an absent one — "12" from "123" would be believed — so a
// result that did not fit is discarded entirely.
[[nodiscard]] std::string_view accept(const char* const buffer, const std::size_t capacity,
                                      const int written) {
    if (written <= 0 || static_cast<std::size_t>(written) >= capacity) {
        return {};
    }

    return {buffer, static_cast<std::size_t>(written)};
}

NameText substitute_context(text::TextId pattern_id, std::array<std::string_view, 2> arguments) {
    const auto pattern = text::lookup(pattern_id);
    NameText result;
    NameBuffer buffer{};
    std::size_t used = 0;
    std::size_t argument = 0;
    for (std::size_t i = 0; i < pattern.size(); ++i) {
        auto part = pattern.substr(i, 1);
        if (pattern[i] == '%' && i + 1 < pattern.size()) {
            const char specifier = pattern[++i];
            if (specifier == 's') {
                if (argument == arguments.size()) {
                    return {};
                }
                part = arguments[argument++];
            } else if (specifier == '%') {
                part = pattern.substr(i, 1);
            } else {
                return {};
            }
        }
        if (used + part.size() > NameText::capacity()) {
            return {};
        }
        std::copy(part.begin(), part.end(), buffer.begin() + static_cast<std::ptrdiff_t>(used));
        used += part.size();
    }
    result.assign({buffer.data(), used});
    return result;
}

} // namespace

NameText error_context(const app_model::Presentation& presentation) {
    using S = app_model::SubjectState;
    using T = text::TextId;
    NameText result;
    if (presentation.subject == S::AcquiringState ||
        presentation.confidence == app_model::Confidence::Unavailable) {
        result.assign(text::lookup(T::CheckPrinterDisplay));
        return result;
    }
    T state = T::StateReady;
    switch (presentation.subject) {
    case S::Ready:
        state = T::StateReady;
        break;
    case S::Preparing:
        state = T::StatePreparing;
        break;
    case S::Printing:
        state = T::StatePrinting;
        break;
    case S::Paused:
        state = T::StatePaused;
        break;
    case S::Completed:
        state = T::StateCompleted;
        break;
    case S::Stopped:
        state = T::StateStopped;
        break;
    case S::AcquiringState:
        break;
    }
    const auto name = text::lookup(state);
    const auto progress = percent(presentation.progress);
    const bool attention = presentation.subject_condition == app_model::SubjectCondition::Attention;
    if (attention && presentation.confidence == app_model::Confidence::Current) {
        return substitute_context(progress.empty() ? T::CurrentSubject : T::CurrentSubjectProgress,
                                  {name, progress.view()});
    }
    return substitute_context(progress.empty() ? T::LastSubject : T::LastSubjectProgress,
                              {name, progress.view()});
}

NameText observation_age(const std::optional<app_model::Timestamp> observed,
                         const std::uint32_t now_ms) {
    NameText text;
    if (!observed.has_value()) {
        return text;
    }
    const auto observed_ms = static_cast<std::uint32_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(*observed).count());
    const std::uint32_t elapsed = now_ms - observed_ms;
    // A future timestamp is not billions of seconds old. The presentable freshness
    // window is far below the half-range needed for unsigned monotonic ordering.
    const auto seconds = elapsed <= UINT32_MAX / 2 ? elapsed / 1000 : 0;
    const auto pattern = text::lookup(text::TextId::StaleIndicator);
    const auto marker = pattern.find("%lu");
    if (marker == std::string_view::npos) {
        return text;
    }
    const auto suffix = pattern.substr(marker + 3);
    NameBuffer buffer{};
    // Each %.*s is bounded: marker is within pattern, and the suffix uses its own size.
    // NOLINTBEGIN(bugprone-suspicious-stringview-data-usage)
    const int written = std::snprintf(
        buffer.data(), buffer.size(), "%.*s%lu%.*s", static_cast<int>(marker), pattern.data(),
        static_cast<unsigned long>(seconds), static_cast<int>(suffix.size()), suffix.data());
    // NOLINTEND(bugprone-suspicious-stringview-data-usage)
    text.assign(accept(buffer.data(), buffer.size(), written));
    return text;
}

ValueText temperature(const app_model::TemperatureValue& value) {
    ValueText text;

    if (!value.known) {
        return text;
    }

    // NaN and infinity are guarded because this float arrives from whatever the
    // printer reported. std::lround of a NaN is undefined, and a temperature that is
    // not a number is not a temperature — it is absent, and absence stays absent.
    if (!std::isfinite(value.celsius)) {
        return text;
    }

    // Rounds rather than truncates: 59.8 °C means 60, and §17.7's whole-degree rule
    // is about what the number says, not about discarding the fraction.
    const long degrees = std::lround(value.celsius);

    Buffer buffer{};
    const int written = std::snprintf(buffer.data(), buffer.size(), "%ld%s", degrees, kDegreeSign);
    text.assign(accept(buffer.data(), buffer.size(), written));
    return text;
}

ValueText percent(const app_model::ProgressValue& value) {
    ValueText text;

    if (!value.known) {
        return text;
    }

    // Not clamped here. app_model's projection clamps to 0...100 and its header says
    // it is the only place that does; clamping again would create a second owner of
    // the rule and hide a projection defect behind a plausible number. A host test
    // pins this behaviour deliberately.
    Buffer buffer{};
    const int written =
        std::snprintf(buffer.data(), buffer.size(), "%u", static_cast<unsigned>(value.percent));
    text.assign(accept(buffer.data(), buffer.size(), written));
    return text;
}

UpdateVersionText firmware_version(const app_model::BoundedString<32>& version) {
    UpdateVersionText result;
    if (version.empty()) {
        return result;
    }

    const auto label = text::lookup(text::TextId::UpdateFirmwareLabel);
    UpdateVersionBuffer buffer{};
    const int written =
        std::snprintf(buffer.data(), buffer.size(), "%.*s %.*s", static_cast<int>(label.size()),
                      label.data(), static_cast<int>(version.size()), version.c_str());
    result.assign(accept(buffer.data(), buffer.size(), written));
    return result;
}

ValueText duration(const app_model::DurationValue& value) {
    ValueText text;

    if (!value.known) {
        return text;
    }

    // Truncates toward zero. Rounding up would let a job show a minute it does not
    // have; rounding to nearest would make the last minute tick backwards.
    const unsigned total_minutes = value.seconds / 60U;
    const unsigned hours = total_minutes / 60U;
    const unsigned minutes = total_minutes % 60U;

    Buffer buffer{};
    const int written = std::snprintf(buffer.data(), buffer.size(), "%u:%02u", hours, minutes);
    text.assign(accept(buffer.data(), buffer.size(), written));
    return text;
}

ValueText approximate_duration(const app_model::DurationValue& value, const bool is_approximate) {
    const ValueText base = duration(value);

    if (!is_approximate || base.empty()) {
        return base;
    }

    Buffer buffer{};
    const int written = std::snprintf(buffer.data(), buffer.size(), "~%s", base.c_str());

    const std::string_view marked = accept(buffer.data(), buffer.size(), written);
    if (marked.empty()) {
        // The marker did not fit. The duration is the fact and the marker is a
        // qualifier, so the fact is kept rather than both being lost.
        return base;
    }

    ValueText text;
    text.assign(marked);
    return text;
}

ValueText layers(const app_model::LayerProgress& value) {
    ValueText text;

    // "87/0" would be false and "87" alone would read as a total. The projection
    // already withholds layer data before a job is established; this guards the
    // residual case rather than duplicating that decision.
    if (!value.known || value.total == 0) {
        return text;
    }

    Buffer buffer{};
    const int written = std::snprintf(buffer.data(), buffer.size(), "%lu/%lu",
                                      static_cast<unsigned long>(value.current),
                                      static_cast<unsigned long>(value.total));
    text.assign(accept(buffer.data(), buffer.size(), written));
    return text;
}

NameText filename(const app_model::BoundedString<64>& name) {
    NameText text;

    const std::string_view raw = name.view();
    if (raw.empty()) {
        return text;
    }

    // Extension removal is presentation-only (§7.8). The dot must not be the first
    // character: a name that is entirely an extension is still a name, and stripping
    // it would leave nothing to show.
    std::string_view stem = raw;
    const std::size_t dot = stem.find_last_of('.');
    if (dot != std::string_view::npos && dot > 0) {
        stem = stem.substr(0, dot);
    }

    if (stem.size() <= NameText::capacity()) {
        text.assign(stem);
        return text;
    }

    // Middle truncation, so the same name always truncates identically regardless of
    // what is drawn around it (§17.7's "stable filename truncation").
    NameBuffer buffer{};
    const int written = std::snprintf(
        buffer.data(), buffer.size(), "%.*s%s%.*s", static_cast<int>(kFilenameHead), stem.data(),
        kEllipsis, static_cast<int>(kFilenameTail), stem.data() + (stem.size() - kFilenameTail));
    text.assign(accept(buffer.data(), buffer.size(), written));
    return text;
}

} // namespace prd::ui::format
