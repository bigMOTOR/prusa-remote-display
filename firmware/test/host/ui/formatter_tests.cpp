// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <limits>

#include "formatters.hpp"

// The formatter suite — Architecture Spec §17.7's "deterministic and testable".
//
// Two properties are being protected across every function here. Absence never becomes
// a placeholder: an unknown value formats to nothing, and there is no dash anywhere in
// the output space. And a value that is shown reads as what it means: whole degrees,
// no seconds, no invented totals.

using namespace prd::ui::format;
using prd::app_model::BoundedString;
using prd::app_model::DurationValue;
using prd::app_model::LayerProgress;
using prd::app_model::ProgressValue;
using prd::app_model::TemperatureValue;

namespace {

TemperatureValue celsius(const float value) {
    return TemperatureValue{true, value};
}

DurationValue seconds(const std::uint32_t value) {
    return DurationValue{true, value};
}

} // namespace

TEST(PrinterErrorContextTests, QualifiesKnownSubjectAndProgressWithoutInventingMissingProgress) {
    prd::app_model::Presentation view;
    view.confidence = prd::app_model::Confidence::Current;
    view.subject = prd::app_model::SubjectState::Printing;
    view.progress = {true, 78};
    EXPECT_EQ(error_context(view).view(), "Last: Printing / 78%");
    view.progress = {};
    EXPECT_EQ(error_context(view).view(), "Last: Printing");
    view.subject = prd::app_model::SubjectState::Ready;
    EXPECT_EQ(error_context(view).view(), "Last: Ready");
}

TEST(PrinterErrorContextTests, DoesNotDiscloseAJobForUnknownSubjectOrUnavailableObservation) {
    prd::app_model::Presentation view;
    view.confidence = prd::app_model::Confidence::Current;
    view.subject = prd::app_model::SubjectState::AcquiringState;
    view.progress = {true, 78};
    EXPECT_EQ(error_context(view).view(), "Check printer display");
    view.subject = prd::app_model::SubjectState::Printing;
    view.confidence = prd::app_model::Confidence::Unavailable;
    EXPECT_EQ(error_context(view).view(), "Check printer display");
}

// --- Absence (UI Design Decision 003, Production UI §9) --------------------
TEST(ObservationAgeTests, UsesLastStatusRatherThanSceneEntryAndWholeSeconds) {
    EXPECT_EQ(observation_age(std::chrono::milliseconds{1234}, 21234).view(),
              "Reconnecting: 20 s old");
    EXPECT_EQ(observation_age(std::chrono::milliseconds{1234}, 22233).view(),
              "Reconnecting: 20 s old");
    EXPECT_EQ(observation_age(std::chrono::milliseconds{1234}, 22234).view(),
              "Reconnecting: 21 s old");
}

TEST(ObservationAgeTests, ClockWrapFutureAndAbsenceAreSafe) {
    EXPECT_EQ(observation_age(std::chrono::milliseconds{UINT32_MAX - 999}, 1000).view(),
              "Reconnecting: 2 s old");
    EXPECT_EQ(observation_age(std::chrono::milliseconds{1001}, 1000).view(),
              "Reconnecting: 0 s old");
    EXPECT_TRUE(observation_age(std::nullopt, 10000).empty());
}

//
// One test per formatter, because the rule has to hold for every one of them. A single
// function that emitted "--" would put a placeholder on screen that the projection went
// to some trouble to make impossible.

TEST(FormatterAbsenceTests, AnUnknownTemperatureFormatsToNothing) {
    EXPECT_TRUE(temperature(TemperatureValue{}).empty());
}

TEST(FormatterAbsenceTests, AnUnknownProgressFormatsToNothing) {
    EXPECT_TRUE(percent(ProgressValue{}).empty());
}

TEST(FormatterAbsenceTests, AnUnknownDurationFormatsToNothing) {
    EXPECT_TRUE(duration(DurationValue{}).empty());
}

TEST(FormatterAbsenceTests, AnUnknownLayerCountFormatsToNothing) {
    EXPECT_TRUE(layers(LayerProgress{}).empty());
}

TEST(FormatterAbsenceTests, AnEmptyFilenameFormatsToNothing) {
    EXPECT_TRUE(filename(BoundedString<64>{}).empty());
}

TEST(FormatterAbsenceTests, NoFormatterEverEmitsADash) {
    // The placeholder the accepted artwork shows and every written source forbids.
    // Asserted over the values most likely to tempt one: absent, zero, and the
    // boundary between them.
    //
    // The results are kept as VALUES, not as views. Each formatter returns a
    // fixed-capacity string by value, so a std::string_view taken from the temporary
    // and stored here would dangle the moment the initialiser finished — which an
    // earlier version of this test did, and AddressSanitizer caught as a
    // use-after-scope. Callers may pass `.view()` straight into another call, where the
    // temporary lives to the end of the full expression; they may not outlive it.
    const std::array<ValueText, 6> value_outputs{
        temperature(TemperatureValue{}), temperature(celsius(0.0F)), percent(ProgressValue{}),
        percent(ProgressValue{true, 0}), duration(DurationValue{}),  duration(seconds(0)),
    };

    const std::array<ValueText, 2> layer_outputs{
        layers(LayerProgress{}),
        layers(LayerProgress{true, 0, 0}),
    };

    for (const auto& output : value_outputs) {
        EXPECT_EQ(output.view().find('-'), std::string_view::npos)
            << "output was: " << output.view();
    }

    for (const auto& output : layer_outputs) {
        EXPECT_EQ(output.view().find('-'), std::string_view::npos)
            << "output was: " << output.view();
    }
}

// --- Temperature (§17.7 whole degrees) -------------------------------------

TEST(TemperatureFormatterTests, WholeDegreesCarryTheDegreeSign) {
    EXPECT_EQ(temperature(celsius(215.0F)).view(), "215\xC2\xB0");
}

TEST(TemperatureFormatterTests, RoundsToNearestRatherThanTruncating) {
    // 59.8 °C means 60. Truncation would report 59, which is a different reading of
    // the same measurement rather than a coarser one.
    EXPECT_EQ(temperature(celsius(59.8F)).view(), "60\xC2\xB0");
    EXPECT_EQ(temperature(celsius(59.4F)).view(), "59\xC2\xB0");
}

TEST(TemperatureFormatterTests, ZeroIsAValueAndNotAnAbsence) {
    // §9.3's rule that absent must be distinguishable from zero, at the presentation
    // boundary. A bed at 0 °C reports; a bed that did not report does not.
    EXPECT_EQ(temperature(celsius(0.0F)).view(), "0\xC2\xB0");
    EXPECT_TRUE(temperature(TemperatureValue{}).empty());
}

TEST(TemperatureFormatterTests, NegativeTemperaturesAreFormattedRatherThanDropped) {
    EXPECT_EQ(temperature(celsius(-5.0F)).view(), "-5\xC2\xB0");
}

TEST(TemperatureFormatterTests, NonFiniteValuesAreTreatedAsAbsent) {
    // These arrive from whatever the printer reported. std::lround of a NaN is
    // undefined, and a temperature that is not a number is not a temperature.
    EXPECT_TRUE(temperature(celsius(std::numeric_limits<float>::quiet_NaN())).empty());
    EXPECT_TRUE(temperature(celsius(std::numeric_limits<float>::infinity())).empty());
    EXPECT_TRUE(temperature(celsius(-std::numeric_limits<float>::infinity())).empty());
}

// --- Progress --------------------------------------------------------------

TEST(PercentFormatterTests, FormatsWithoutAUnit) {
    // UIDD 001 draws "64" large and "%" small, so joining them here would force one
    // font on both.
    EXPECT_EQ(percent(ProgressValue{true, 64}).view(), "64");
    EXPECT_EQ(percent(ProgressValue{true, 0}).view(), "0");
    EXPECT_EQ(percent(ProgressValue{true, 100}).view(), "100");
}

TEST(PercentFormatterTests, DoesNotClampBecauseTheProjectionDoes) {
    // Deliberate, and pinned. app_model's projection clamps to 0...100 and its header
    // says it is the only place that does. Clamping again here would create a second
    // owner of the rule and would hide a projection defect behind a plausible number.
    EXPECT_EQ(percent(ProgressValue{true, 101}).view(), "101");
}

TEST(UpdateVersionFormatterTests, PrefixesTheFullBoundedVersionWithoutTruncation) {
    const BoundedString<32> version{"0123456789012345678901234567890"};
    const auto formatted = firmware_version(version);

    EXPECT_EQ(formatted.view(), "Firmware 0123456789012345678901234567890");
    EXPECT_FALSE(formatted.truncated());
}

TEST(UpdateVersionFormatterTests, AnEmptyVersionRemainsAbsent) {
    EXPECT_TRUE(firmware_version(BoundedString<32>{}).empty());
}

// --- Duration (owner decision D9) ------------------------------------------

TEST(DurationFormatterTests, FormatsAsHoursAndMinutes) {
    EXPECT_EQ(duration(seconds((1 * 3600) + (42 * 60))).view(), "1:42");
}

TEST(DurationFormatterTests, MinutesAreAlwaysTwoDigits) {
    // A single-digit minute field would change the string's width every ten minutes.
    EXPECT_EQ(duration(seconds((2 * 3600) + (5 * 60))).view(), "2:05");
}

TEST(DurationFormatterTests, NeverShowsSeconds) {
    // §17.7: "no rapid text jitter from small estimate changes". A seconds field on a
    // value the printer revises every two seconds is exactly that jitter.
    EXPECT_EQ(duration(seconds(59)).view(), "0:00");
    EXPECT_EQ(duration(seconds(61)).view(), "0:01");
}

TEST(DurationFormatterTests, TruncatesTowardZero) {
    // Rounding up would let a job show a minute it does not have; rounding to nearest
    // would make the last minute tick backwards.
    EXPECT_EQ(duration(seconds((3 * 60) + 59)).view(), "0:03");
}

TEST(DurationFormatterTests, HoursAreNotClamped) {
    EXPECT_EQ(duration(seconds(100 * 3600)).view(), "100:00");
}

TEST(DurationFormatterTests, ZeroIsAValueAndNotAnAbsence) {
    EXPECT_EQ(duration(seconds(0)).view(), "0:00");
    EXPECT_TRUE(duration(DurationValue{}).empty());
}

TEST(DurationFormatterTests, TheApproximateMarkerPrefixesTheValue) {
    const auto value = seconds((1 * 3600) + (42 * 60));
    EXPECT_EQ(approximate_duration(value, true).view(), "~1:42");
    EXPECT_EQ(approximate_duration(value, false).view(), "1:42");
}

TEST(DurationFormatterTests, TheApproximateMarkerDoesNotConjureAValue) {
    // An unknown duration that happens to be flagged approximate is still unknown.
    EXPECT_TRUE(approximate_duration(DurationValue{}, true).empty());
}

// --- Layers ----------------------------------------------------------------

TEST(LayerFormatterTests, FormatsCurrentOverTotal) {
    EXPECT_EQ(layers(LayerProgress{true, 87, 240}).view(), "87/240");
}

TEST(LayerFormatterTests, AZeroTotalIsTreatedAsAbsent) {
    // "87/0" would be false, and "87" alone would read as a total.
    EXPECT_TRUE(layers(LayerProgress{true, 87, 0}).empty());
}

TEST(LayerFormatterTests, TheFirstLayerIsAValue) {
    EXPECT_EQ(layers(LayerProgress{true, 0, 240}).view(), "0/240");
}

// --- Filename (§7.8 presentation-only, §17.7 stable truncation) ------------

TEST(FilenameFormatterTests, RemovesTheExtension) {
    EXPECT_EQ(filename(BoundedString<64>{"CORE One.gcode"}).view(), "CORE One");
}

TEST(FilenameFormatterTests, RemovesOnlyTheLastExtension) {
    EXPECT_EQ(filename(BoundedString<64>{"part.v2.bgcode"}).view(), "part.v2");
}

TEST(FilenameFormatterTests, ANameThatIsEntirelyAnExtensionSurvives) {
    // Stripping it would leave nothing to show, and a name is a name.
    EXPECT_EQ(filename(BoundedString<64>{".gcode"}).view(), ".gcode");
}

TEST(FilenameFormatterTests, ANameWithNoExtensionIsUnchanged) {
    EXPECT_EQ(filename(BoundedString<64>{"benchy"}).view(), "benchy");
}

TEST(FilenameFormatterTests, TruncatesInTheMiddleWithAnAsciiEllipsis) {
    // Not U+2026: the generated faces cover printable ASCII plus the degree sign, so a
    // real ellipsis would resolve to a missing glyph and the marker would vanish.
    const auto formatted =
        filename(BoundedString<64>{"a-very-long-print-job-name-that-will-not-fit.gcode"});

    EXPECT_EQ(formatted.size(), decltype(formatted)::capacity());
    EXPECT_NE(formatted.view().find("..."), std::string_view::npos);
    EXPECT_EQ(formatted.view().substr(0, 8), "a-very-l");
}

TEST(FilenameFormatterTests, TruncationIsStable) {
    // §17.7's "stable filename truncation": the same name always truncates identically,
    // regardless of what is drawn around it.
    const BoundedString<64> name{"a-very-long-print-job-name-that-will-not-fit.gcode"};
    EXPECT_EQ(filename(name).view(), filename(name).view());
}

TEST(FilenameFormatterTests, ANameExactlyAtCapacityIsNotTruncated) {
    const BoundedString<64> name{"0123456789012345678901234567890a.gcode"};
    const auto formatted = filename(name);

    EXPECT_EQ(formatted.view(), "0123456789012345678901234567890a");
    EXPECT_EQ(formatted.view().find("..."), std::string_view::npos);
}

TEST(PrinterAttentionContextTests, CurrentContextIsNotMisrepresentedAsAStoppedHistoricalJob) {
    prd::app_model::Presentation view{};
    view.subject = prd::app_model::SubjectState::Printing;
    view.subject_condition = prd::app_model::SubjectCondition::Attention;
    view.confidence = prd::app_model::Confidence::Current;
    view.progress = {true, 78};
    EXPECT_EQ(error_context(view).view(), "Printing / 78%");
    view.confidence = prd::app_model::Confidence::Degraded;
    EXPECT_EQ(error_context(view).view(), "Last: Printing / 78%");
    view.confidence = prd::app_model::Confidence::Unavailable;
    EXPECT_EQ(error_context(view).view(), "Check printer display");
}

TEST(RemainingDurationTests, UnknownEstimateStaysAbsentWithEitherQualifier) {
    EXPECT_TRUE(remaining_duration(DurationValue{}, false).empty());
    EXPECT_TRUE(remaining_duration(DurationValue{}, true).empty());
}

TEST(RemainingDurationTests, UnitsAndDirectionKeepFloorSemanticsAcrossBoundaries) {
    struct Case {
        std::uint32_t seconds;
        std::string_view expected;
    };
    const Case cases[] = {
        {0, "0m left"},         {59, "0m left"},
        {60, "1m left"},        {3599, "59m left"},
        {3600, "1h00m left"},   {9720, "2h42m left"},
        {97500, "27h05m left"}, {std::numeric_limits<std::uint32_t>::max(), "1193046h28m left"},
    };
    for (const auto& item : cases) {
        const auto result = remaining_duration(seconds(item.seconds), false);
        EXPECT_EQ(result.view(), item.expected) << item.seconds;
        EXPECT_FALSE(result.truncated());
    }
    // The old colon format is deliberately retained for elapsed-time consumers.
    EXPECT_EQ(duration(seconds(9720)).view(), "2:42");
}

TEST(RemainingDurationTests, ApproximateMarkerSurvivesIncludingMaximumApiValue) {
    EXPECT_EQ(remaining_duration(seconds(9720), true).view(), "~2h42m left");
    EXPECT_EQ(remaining_duration(seconds(2520), true).view(), "~42m left");
    const auto maximum =
        remaining_duration(seconds(std::numeric_limits<std::uint32_t>::max()), true);
    EXPECT_EQ(maximum.view(), "~1193046h28m left");
    EXPECT_FALSE(maximum.truncated());
}
