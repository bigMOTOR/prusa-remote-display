// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include <gtest/gtest.h>

#include <array>
#include <limits>
#include <string_view>

#include "app_model/presentation.hpp"
#include "app_model/reducer.hpp"
#include "components/telemetry_slots.hpp"
#include "fake_source/fixtures.hpp"
#include "formatters.hpp"
#include "presentation_diff.hpp"
#include "support/state_builders.hpp"
#include "text/scene_text.hpp"

// The Telemetry Column's §7 presence rule — UI Design Decision 004, divergence OF-8, delivered
// by Milestone 2 Phase G.3.
//
// THE RULE. "A component is present if and only if: 1. the machine supplies its value; and
// 2. that value is true of the machine now… No component shall be retained empty to imply
// anything." Until G.3 an absent value cleared the numeral and left the label standing, which
// is a component retained empty: it implies the machine has a value that merely went
// unreported.
//
// WHAT THIS SUITE CAN AND CANNOT PROVE. The widget is LVGL and cannot be compiled here, so what
// is tested is the whole of the decision it makes — row_is_present(), applied to the output of
// the real formatters, over the real fixtures. The widget's part is to carry that one boolean
// to both halves of the row, which is enforced by construction rather than by a test: set_value
// computes it once and sets the value and the label from it, so there is no path that hides one
// without the other. The pairing is asserted here as the rule it is; the render report's
// `present=` field is where it is confirmed on the panel.

using prd::app_model::Presentation;
using prd::app_model::project;
using prd::fake_source::Fixture;
using prd::ui::component::kTelemetrySlotCount;
using prd::ui::component::row_is_present;
using prd::ui::component::TelemetrySlot;

namespace {

constexpr std::array<TelemetrySlot, kTelemetrySlotCount> kSlots{
    TelemetrySlot::Nozzle,
    TelemetrySlot::Bed,
    TelemetrySlot::Layer,
};

// The value the column would be given for a slot, formatted exactly as the scene formats it.
[[nodiscard]] prd::ui::format::ValueText value_for(const TelemetrySlot slot,
                                                   const Presentation& view) {
    switch (slot) {
    case TelemetrySlot::Nozzle:
        return prd::ui::format::temperature(view.nozzle.current);

    case TelemetrySlot::Bed:
        return prd::ui::format::temperature(view.bed.current);

    case TelemetrySlot::Layer:
        return prd::ui::format::layers(view.layers);
    }

    return {};
}

[[nodiscard]] bool row_present_for(const TelemetrySlot slot, const Presentation& view) {
    return row_is_present(value_for(slot, view).view());
}

[[nodiscard]] const char* slot_name(const TelemetrySlot slot) {
    switch (slot) {
    case TelemetrySlot::Nozzle:
        return "nozzle";
    case TelemetrySlot::Bed:
        return "bed";
    case TelemetrySlot::Layer:
        return "layer";
    }

    return "unknown";
}

} // namespace

// --- The rule ---------------------------------------------------------------

TEST(TelemetryPresenceTests, AnAbsentValueMakesTheRowAbsent) {
    EXPECT_FALSE(row_is_present(""));
    EXPECT_FALSE(row_is_present(std::string_view{}));
}

TEST(TelemetryPresenceTests, ASuppliedValueMakesTheRowPresent) {
    EXPECT_TRUE(row_is_present("215°"));
    EXPECT_TRUE(row_is_present("156/250"));

    // Nothing about the content is inspected — a row is present because the machine supplied
    // something, not because the something looks like a number.
    EXPECT_TRUE(row_is_present("0"));
}

TEST(TelemetryPresenceTests, ALabelIsNeverPresentWithoutItsValue) {
    // The invariant, stated as the rule that produces it: presence is a function of the value
    // alone, and the label has no rule of its own to disagree with. There is deliberately no
    // label-specific predicate for a future change to introduce quietly — if one is ever added,
    // this test is where the two would have to be reconciled.
    //
    // Every slot has a label at every moment (text/scene_text.hpp binds one per slot, checked
    // by static_assert there), so a label is available whenever a row is present and is
    // withheld only with it.
    for (const auto slot : kSlots) {
        EXPECT_FALSE(row_is_present(prd::ui::format::ValueText{}.view()))
            << slot_name(slot) << ": an unset value can never justify a label";

        const auto label = prd::ui::text::lookup(prd::ui::text::label_for(slot));
        EXPECT_FALSE(label.empty()) << slot_name(slot) << " must have a label to withhold";
    }
}

// --- The chain from an absent reading to an absent row ----------------------

TEST(TelemetryPresenceTests, AnUnknownTemperatureProducesNoRow) {
    // The end-to-end property, through the real formatter: absence stays absence all the way to
    // the presence decision, with no placeholder in between.
    Presentation view;
    ASSERT_FALSE(view.nozzle.current.known);

    EXPECT_FALSE(row_present_for(TelemetrySlot::Nozzle, view));
    EXPECT_FALSE(row_present_for(TelemetrySlot::Bed, view));
}

TEST(TelemetryPresenceTests, AJobWithNoTotalProducesNoLayerRow) {
    // formatters.cpp withholds "87/0" because it would be false and "87" alone would read as a
    // total. The row rule inherits that judgement rather than second-guessing it.
    Presentation view;
    view.layers = prd::app_model::LayerProgress{true, 87, 0};

    EXPECT_FALSE(row_present_for(TelemetrySlot::Layer, view));
}

TEST(TelemetryPresenceTests, ANonFiniteTemperatureProducesNoRow) {
    Presentation view;
    view.nozzle.current =
        prd::app_model::TemperatureValue{true, std::numeric_limits<float>::quiet_NaN()};

    EXPECT_FALSE(row_present_for(TelemetrySlot::Nozzle, view));
}

// --- Which rows each operational state draws --------------------------------

TEST(TelemetryPresenceTests, TheRestingStatesDrawTwoRowsAndNotAThird) {
    // THE VISIBLE CHANGE G.3 MAKES, pinned per state. Ready and Preparing carry no job, so the
    // Layer row has no value and is absent entirely — where before it stood as a bare label.
    //
    // The two temperature rows are unaffected: the machine supplies them whether or not it is
    // printing, so §7 keeps them.
    const Fixture resting[] = {Fixture::Ready, Fixture::Preparing};

    for (const auto fixture : resting) {
        const auto view = project(prd::fake_source::state_for(fixture));
        const auto label = prd::fake_source::name(fixture);

        // ⛔ NARROWED, NOT DELETED — owner ruling 2026-08-29. The row membership this test exists
        // to pin is unchanged: the machine supplies two temperatures and no layer count, so §7
        // keeps two rows and drops the third, and that is asserted below for both states.
        //
        // What changed is only whether the column is DRAWN. Ready at Level 0 rest displays
        // nothing (UI Behaviour Decision 001 §2), while Preparing is an activity and still shows
        // its telemetry. The rows stay resolved either way, which is the point: rest withholds a
        // true value rather than deleting it.
        if (fixture == Fixture::Ready) {
            EXPECT_TRUE(view.at_visual_rest) << label;
            EXPECT_FALSE(view.show_telemetry) << label << ": rest draws the horizon alone";
        } else {
            EXPECT_TRUE(view.show_telemetry) << label << ": the column is membership, not content";
        }

        EXPECT_TRUE(row_present_for(TelemetrySlot::Nozzle, view)) << label;
        EXPECT_TRUE(row_present_for(TelemetrySlot::Bed, view)) << label;
        EXPECT_FALSE(row_present_for(TelemetrySlot::Layer, view))
            << label << ": no job, so no layer count, so no row";
    }
}

TEST(TelemetryPresenceTests, TheStatesCarryingAJobDrawAllThreeRows) {
    const Fixture with_a_job[] = {Fixture::Printing, Fixture::Paused, Fixture::Completed};

    for (const auto fixture : with_a_job) {
        const auto view = project(prd::fake_source::state_for(fixture));
        const auto label = prd::fake_source::name(fixture);

        for (const auto slot : kSlots) {
            EXPECT_TRUE(row_present_for(slot, view)) << label << " " << slot_name(slot);
        }
    }
}

TEST(TelemetryPresenceTests, ReducedConfidenceEmptiesTheColumnThroughTheSamePresenceRule) {
    // OF-9 AND OF-8 MEETING, which is why G.3 was sequenced before G.5. §10 makes the values
    // absent; §7 — the rule G.3 delivered — is what turns an absent value into an absent row.
    // Without G.3 this composition would be three labels standing over nothing, the exact thing
    // §7 prohibits.
    //
    // The column itself stays: `show_telemetry` is membership of the family, and §10 changes no
    // component's presence-as-membership. What empties is its contents, row by row.
    const Fixture degraded[] = {Fixture::PrintingDegraded, Fixture::ReadyDegraded};

    for (const auto fixture : degraded) {
        const auto view = project(prd::fake_source::state_for(fixture));
        const auto label = prd::fake_source::name(fixture);

        ASSERT_EQ(view.confidence, prd::app_model::Confidence::Degraded) << label;
        EXPECT_TRUE(view.show_telemetry) << label << ": the column is membership, not content";
        EXPECT_TRUE(view.stale_indicator) << label << ": and confidence states itself";

        for (const auto slot : kSlots) {
            EXPECT_FALSE(row_present_for(slot, view)) << label << " " << slot_name(slot);
        }
    }
}

TEST(TelemetryPresenceTests, ATerminalJobKeepsItsLayerRowAtReducedConfidence) {
    // §7's worked example reaching the panel: "a terminal job's layer count is still true — it
    // is the count the machine reached". The row survives while the two temperature rows, which
    // are claims about the machine now, do not.
    auto state = prd::fake_source::state_for(Fixture::Completed);
    state = prd::app_model::reduce(state,
                                   prd::app_model::Event::confidence_aged(prd::test::at_ms(30000)));

    const auto view = project(state);
    ASSERT_EQ(view.confidence, prd::app_model::Confidence::Degraded);

    EXPECT_TRUE(row_present_for(TelemetrySlot::Layer, view));
    EXPECT_FALSE(row_present_for(TelemetrySlot::Nozzle, view));
    EXPECT_FALSE(row_present_for(TelemetrySlot::Bed, view));
}

TEST(TelemetryPresenceTests, StoppedKeepsTheRowsItsMachineStillSupplies) {
    // §7's worked example, at the state that shows it: "a terminal job's layer count is still
    // true — it is the count the machine reached". The bed is still reported; the nozzle target
    // is released while the nozzle itself is still measured, and it is the measurement the row
    // draws.
    const auto view = project(prd::fake_source::state_for(Fixture::Stopped));

    EXPECT_TRUE(row_present_for(TelemetrySlot::Layer, view));
    EXPECT_TRUE(row_present_for(TelemetrySlot::Nozzle, view));
    EXPECT_TRUE(row_present_for(TelemetrySlot::Bed, view));
}

TEST(TelemetryPresenceTests, PrintingWithoutARecordDrawsNoLayerRow) {
    // The composition E.6 added a fixture for and G.6 will see: the machine is printing, the
    // form is at full energy, and the job record has not arrived. §3.2 keeps the form; §7
    // withholds the row.
    const auto view = project(prd::fake_source::state_for(Fixture::PrintingUnknownProgress));

    EXPECT_TRUE(view.show_operational_form);
    EXPECT_TRUE(row_present_for(TelemetrySlot::Nozzle, view));
    EXPECT_FALSE(row_present_for(TelemetrySlot::Layer, view));
}

// --- The row must be told when it changes -----------------------------------

TEST(TelemetryPresenceTests, AValueBecomingAbsentIsADifferenceTheDiffReports) {
    // Presence is applied by set_value, and set_value is called only when the diff says the
    // value changed. A known -> unknown transition that the diff missed would leave the row
    // drawn over a value the machine no longer supplies — which is worse than the defect G.3
    // fixes, because it would be stale rather than merely empty.
    Presentation printing = project(prd::fake_source::state_for(Fixture::Printing));
    ASSERT_TRUE(printing.layers.known);
    ASSERT_TRUE(printing.nozzle.current.known);

    Presentation lost = printing;
    lost.layers = prd::app_model::LayerProgress{};
    lost.nozzle.current = prd::app_model::TemperatureValue{};

    const auto delta = prd::ui::diff(printing, lost);

    EXPECT_TRUE(delta.layers) << "a layer count that disappeared must reach the column";
    EXPECT_TRUE(delta.nozzle) << "a temperature that disappeared must reach the column";
}
