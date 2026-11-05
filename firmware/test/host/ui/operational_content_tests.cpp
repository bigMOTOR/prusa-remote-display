// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include <gtest/gtest.h>

#include <array>
#include <set>

#include "app_model/presentation.hpp"
#include "app_model/reducer.hpp"
#include "fake_source/fixtures.hpp"
#include "scenes/operational_content.hpp"
#include "scenes/operational_energy.hpp"
#include "support/state_builders.hpp"
#include "text/text.hpp"

// The per-state content suite — the whole operational family's Phase D scene decisions.
//
// This is where those decisions are checkable at all. The scene builder is LVGL and cannot be
// compiled here; the content table is pure precisely so that "what does Paused show?" has an
// answer a test can read.
//
// Every case is driven through a real fixture, so what is asserted is what the reducer and
// projection actually produce for that state — not a hand-assembled presentation that might
// describe something the product cannot reach.

using namespace prd::ui::scene;
using prd::app_model::project;
using prd::app_model::SubjectState;
using prd::fake_source::Fixture;
using prd::ui::text::kTextIdCount;
using prd::ui::text::TextId;

namespace {

OperationalContent content_for(const Fixture fixture) {
    return resolve(project(prd::fake_source::state_for(fixture)));
}

// §5's palette carries meaning by hue in exactly four tokens; the rest are steps on the neutral
// ramp. Written as the complement of the chromatic set rather than as a list of permitted
// neutrals, so that a token added to §5 later is chromatic-by-omission and fails loudly here
// instead of passing by default.
[[nodiscard]] bool is_achromatic(const prd::ui::theme::Colour colour) {
    using prd::ui::theme::Colour;

    return colour != Colour::Accent && colour != Colour::Success && colour != Colour::Warning &&
           colour != Colour::Error;
}

struct FamilyMember {
    Fixture fixture;
    SubjectState subject;
    const char* name;
};

// The six states the one composition covers, in the order the artwork's continuity board
// walks them.
constexpr std::array<FamilyMember, 6> kFamily{{
    {Fixture::Ready, SubjectState::Ready, "Ready"},
    {Fixture::Preparing, SubjectState::Preparing, "Preparing"},
    {Fixture::Printing, SubjectState::Printing, "Printing"},
    {Fixture::Paused, SubjectState::Paused, "Paused"},
    {Fixture::Completed, SubjectState::Completed, "Completed"},
    {Fixture::Stopped, SubjectState::Stopped, "Stopped"},
}};

} // namespace

// --- Properties that hold across the whole family ---------------------------

TEST(OperationalContentTests, EveryFamilyMemberReachesItsOwnSubjectState) {
    // Guards the rest of the suite: if a fixture stopped producing the state it is named for,
    // every assertion below would be testing the wrong thing and still passing.
    for (const auto& member : kFamily) {
        EXPECT_EQ(project(prd::fake_source::state_for(member.fixture)).subject, member.subject)
            << member.name;
    }
}

TEST(OperationalContentTests, EveryFamilyMemberDrawsTheOperationalForm) {
    // §6 and §8: the ring and the horizon are one primitive, and every operational state
    // carries it. A state with none would be a frame with a hole where its most characteristic
    // element belongs.
    for (const auto& member : kFamily) {
        EXPECT_TRUE(content_for(member.fixture).form.has_value()) << member.name;
    }
}

TEST(OperationalContentTests, TheFormsShapeFollowsTheEnergyOrderingAndNothingElse) {
    // UIDD 004 §3.1: the form's shape IS energy. Replaces the resting/operational split this
    // test used to assert, which was the binary OF-2 records — two shapes for six states.
    //
    // The whole family is checked as one ordering rather than state by state, because the
    // property is about the family: every state's form is more open than that of the state
    // below it on the axis. Stopped now sits BELOW Preparing, which is §2.2's order and the
    // correction of OF-3 — under the old binary Stopped drew the most closed shape available
    // while Preparing drew the most open one.
    const std::array<Fixture, 6> by_energy{
        Fixture::Ready,     Fixture::Stopped, Fixture::Preparing,
        Fixture::Completed, Fixture::Paused,  Fixture::Printing,
    };

    for (std::size_t index = 1; index < by_energy.size(); ++index) {
        const auto lower = content_for(by_energy[index - 1]);
        const auto higher = content_for(by_energy[index]);

        ASSERT_TRUE(lower.form.has_value());
        ASSERT_TRUE(higher.form.has_value());
        EXPECT_LT(*lower.form, *higher.form) << "at index " << index;
    }
}

TEST(OperationalContentTests, TheFormIsNotDecidedByTheRecord) {
    // §3.2, in bold in the decision itself: "The record shall never determine the form's
    // shape." The two resting states carry no record and the four operational ones do, so if
    // the record were still deciding the shape the six forms would fall into two groups. They
    // do not: all six are distinct.
    std::set<Energy> shapes;
    for (const auto& member : kFamily) {
        const auto content = content_for(member.fixture);
        ASSERT_TRUE(content.form.has_value()) << member.name;
        shapes.insert(*content.form);
    }

    EXPECT_EQ(shapes.size(), kFamily.size());
}

TEST(OperationalContentTests, TheThirdLineHasExactlyOneSource) {
    // UIDD 004 §8 makes the third line "a single slot". Until Phase G.4 two flags could reach
    // it and the test here was that they never both fired; there is now one flag, and what is
    // asserted is that the slot's only source is a value the machine supplied.
    //
    // A second source returning is exactly how an invented line comes back, so this is written
    // against the presentation rather than against the flag: a state may show the third line
    // only if the machine reported a remaining time for it.
    for (const auto& member : kFamily) {
        const auto view = project(prd::fake_source::state_for(member.fixture));
        const auto content = resolve(view);

        if (content.show_remaining) {
            EXPECT_TRUE(view.remaining.known)
                << member.name << ": the third line drew a duration the machine never sent";
        }
    }
}

TEST(OperationalContentTests, EveryCatalogueTitleResolves) {
    for (const auto& member : kFamily) {
        const auto content = content_for(member.fixture);

        if (content.title == TitleSource::Catalogue) {
            EXPECT_FALSE(prd::ui::text::lookup(content.title_id).empty()) << member.name;
            EXPECT_TRUE(prd::ui::text::is_renderable(prd::ui::text::lookup(content.title_id)))
                << member.name;
        }
    }
}

TEST(OperationalContentTests, EveryStateTitleIsDistinct) {
    // Two states reading the same would make the family indistinguishable at a glance, which
    // is the one thing the composition exists to prevent.
    std::set<TextId> titles;
    for (const auto& member : kFamily) {
        const auto content = content_for(member.fixture);
        if (content.title == TitleSource::Catalogue) {
            titles.insert(content.title_id);
        }
    }

    // Five catalogue titles — Ready uses the printer's name instead.
    EXPECT_EQ(titles.size(), 5U);
}

// --- §10: confidence changes what is drawn and nothing else (OF-9, Phase G.5) ----

namespace {

// The same state, observed and then unobservable. Everything about the machine is identical;
// only the display's confidence in it has fallen.
[[nodiscard]] prd::app_model::Presentation degraded(prd::app_model::Presentation view) {
    view.confidence = prd::app_model::Confidence::Degraded;
    view.stale_indicator = true;

    // What §10 removes, applied here so the test states the rule it is checking rather than
    // depending on a fixture that happens to have aged. The projection's own decay table is
    // asserted in projection_tests.cpp; this suite is about what the composition does with it.
    view.nozzle = {};
    view.bed = {};
    view.chamber = {};
    view.remaining = {};
    view.remaining_is_approximate = false;
    view.elapsed = {};

    if (view.subject != SubjectState::Completed && view.subject != SubjectState::Stopped) {
        view.progress = {};
        view.layers = {};
    }

    return view;
}

} // namespace

TEST(OperationalContentTests, ReducedConfidenceMovesNothingAndRestylesNothing) {
    // THE PHASE G INVARIANT, AND §10 IN ONE ASSERTION. "Confidence shall change no geometry,
    // luminance, typography, layout or energy."
    //
    // Everything that decides where a thing sits or how it looks is compared field by field
    // across the two confidences. Only `show_progress` and `show_remaining` may differ, and
    // both answer "is this drawn", which is the one thing §10 does change.
    for (const auto& member : kFamily) {
        const auto current = project(prd::fake_source::state_for(member.fixture));
        const auto before = resolve(current);
        const auto after = resolve(degraded(current));

        // Energy — §10 opens by making confidence orthogonal to it, and closes by insisting a
        // degraded Printing "remains Printing at maximum energy".
        EXPECT_EQ(before.form.has_value(), after.form.has_value()) << member.name;
        if (before.form.has_value() && after.form.has_value()) {
            EXPECT_EQ(*before.form, *after.form) << member.name << ": energy followed confidence";
        }

        // Layout. This is the one the first Phase G plan would have got wrong: deriving the
        // arrangement from the record's presence moves a degraded Printing's state word from
        // offset 52 to 26 and its third line from 72 to 46.
        EXPECT_EQ(before.arrangement, after.arrangement)
            << member.name << ": the stack rearranged itself as confidence fell";
        EXPECT_EQ(before.record_row, after.record_row) << member.name;

        // Typography and colour.
        EXPECT_EQ(before.title, after.title) << member.name;
        EXPECT_EQ(before.title_id, after.title_id) << member.name;
        EXPECT_EQ(before.title_colour, after.title_colour) << member.name;
    }
}

TEST(OperationalContentTests, ADegradedPrintingHoldsItsThreeLineStackWithTheRecordRowEmpty) {
    // The trap this checkpoint exists to avoid, named. The record's row stays where it is and
    // simply draws nothing; leaving that void is the accepted state of affairs until §6's
    // re-centring question is settled at the UI Freeze Review.
    const auto current = project(prd::fake_source::state_for(Fixture::Printing));
    const auto after = resolve(degraded(current));

    EXPECT_EQ(after.arrangement, Arrangement::Operational);
    EXPECT_TRUE(after.record_row) << "the record still exists";
    EXPECT_FALSE(after.show_progress) << "and its values do not";
    EXPECT_FALSE(after.show_remaining);
}

TEST(OperationalContentTests, ARecordTheMachineNeverSuppliedStillRestsAtEitherConfidence) {
    // THE OTHER HALF, AND WHY A SHORTCUT WOULD NOT DO. A composition that treated "degraded"
    // as "keep the operational stack" would move THIS state's word instead — it has no record
    // at either confidence, so it rests at both, and the two causes of an absent record have to
    // be genuinely distinguishable rather than inferred from the confidence.
    const auto current = project(prd::fake_source::state_for(Fixture::PrintingUnknownProgress));

    ASSERT_FALSE(current.job_record_supplied);
    EXPECT_EQ(resolve(current).arrangement, Arrangement::Resting);
    EXPECT_EQ(resolve(degraded(current)).arrangement, Arrangement::Resting);
}

TEST(OperationalContentTests, ATerminalStateKeepsItsOutcomeArrangementAndItsRecord) {
    // §7's worked example at the composition layer: Completed's record is still true, so the
    // outcome-first stack keeps both its rows and its order.
    const auto current = project(prd::fake_source::state_for(Fixture::Completed));
    const auto after = resolve(degraded(current));

    EXPECT_EQ(after.arrangement, Arrangement::Outcome);
    EXPECT_TRUE(after.show_progress) << "the count the machine reached is still true";
    EXPECT_EQ(after.title_colour, prd::ui::theme::Colour::Success)
        << "§5.2's exception is not confidence";
}

// --- Ready ------------------------------------------------------------------

TEST(OperationalContentTests, ReadyNamesThePrinterRatherThanItsStateWhenItIsNotAtRest) {
    // ⛔ NARROWED, NOT DELETED — owner ruling 2026-08-29. What this test protected and still
    // protects is that Ready never shows a STATE WORD: where the operational panels put "Printing"
    // it puts identity or nothing. What changed is when identity is drawn at all. Stable healthy
    // Ready is Level 0 rest and carries no text (see StableReadyIsTheHorizonAndNothingElse); a
    // Ready that has lost confidence or gained a condition is Level 3 or 4, owes the user a
    // statement, and keeps its identity.
    auto view = project(prd::fake_source::state_for(Fixture::Ready));
    ASSERT_TRUE(view.at_visual_rest) << "the fixture must start at rest for this to mean anything";

    view.at_visual_rest = false;
    const auto content = resolve(view);

    EXPECT_EQ(content.title, TitleSource::PrinterName);
    EXPECT_NE(content.title, TitleSource::Catalogue) << "Ready never shows a state word";
    EXPECT_FALSE(content.show_progress);
    EXPECT_FALSE(content.show_remaining);
}

TEST(OperationalContentTests, StableReadyIsTheHorizonAndNothingElse) {
    // The accepted F1 resting expression: docs/project-sources/assets/Ready Resting F1.jpeg is a
    // single horizon arc on an otherwise empty panel. UI Behaviour Decision 001 §2 — "information
    // shall never be shown merely because it exists" — and §9's ban on telemetry dashboards;
    // UI Design Decision 002's minimum-energy resting composition.
    const auto view = project(prd::fake_source::state_for(Fixture::Ready));
    ASSERT_TRUE(view.at_visual_rest);

    // The form is the whole composition, and it is still drawn.
    EXPECT_TRUE(view.show_operational_form) << "the horizon is what rest looks like";

    // Everything textual or numeric is gone.
    EXPECT_FALSE(view.show_telemetry) << "no temperatures at rest";

    const auto content = resolve(view);
    EXPECT_EQ(content.title, TitleSource::None) << "no name, no address, no state word";
    EXPECT_FALSE(content.show_progress);
    EXPECT_FALSE(content.show_remaining);
    EXPECT_FALSE(content.record_row) << "no third line";

    // ⬅️ THE NAME IS STILL KNOWN. Rest is a decision not to display a true thing, not a claim
    // that the device forgot its printer — which is what lets Levels 3 and 4 restore it with no
    // re-resolution.
    EXPECT_TRUE(view.printer_name_known);
}

TEST(OperationalContentTests, RestEndsWhenConfidenceFallsOrAConditionAppears) {
    // ⛔ THE GUARD ON THE REPAIR. Silencing Ready must not silence the levels that owe the user a
    // statement: UI Behaviour Decision 001 §8 Level 3 (reduced confidence) and Level 4 (action
    // required). Each case is the same fixture with exactly one thing changed.
    const auto& ready = prd::fake_source::state_for(Fixture::Ready);

    {
        auto degraded = ready;
        degraded.confidence = prd::app_model::Confidence::Degraded;
        const auto view = project(degraded);

        EXPECT_FALSE(view.at_visual_rest) << "Level 3 is not rest";
        EXPECT_TRUE(view.show_telemetry);
        // 2026-09-07 owner: explicit age replaces both Not current and the Ready alias.
        EXPECT_EQ(resolve(view).title, TitleSource::None);
        EXPECT_TRUE(view.stale_indicator);
    }

    {
        auto attention = ready;
        attention.subject_condition = prd::app_model::SubjectCondition::Attention;
        const auto view = project(attention);

        EXPECT_FALSE(view.at_visual_rest) << "Level 4 is not rest";
        EXPECT_TRUE(view.show_telemetry);
        EXPECT_EQ(resolve(view).title, TitleSource::PrinterName);
    }

    {
        auto unauthorized = ready;
        unauthorized.access_condition = prd::app_model::AccessCondition::AuthenticationRequired;
        const auto view = project(unauthorized);

        EXPECT_FALSE(view.at_visual_rest) << "an access condition is not rest";
    }
}

TEST(OperationalContentTests, OnlyReadyEverReachesVisualRest) {
    // Rest is the absence of anything to say. An operational activity always has something.
    const Fixture others[] = {Fixture::Preparing, Fixture::Printing, Fixture::Paused,
                              Fixture::Completed, Fixture::Stopped};

    for (const auto fixture : others) {
        const auto view = project(prd::fake_source::state_for(fixture));
        EXPECT_FALSE(view.at_visual_rest) << prd::fake_source::name(fixture);
        EXPECT_TRUE(view.show_telemetry) << prd::fake_source::name(fixture);
    }
}

TEST(OperationalContentTests, ReadyWithoutAKnownNameShowsNoTitleRatherThanAPlaceholder) {
    // UI Design Decision 003: information that does not yet exist stays absent. A resting
    // device whose name was never learned draws no title at all.
    auto view = project(prd::fake_source::state_for(Fixture::Ready));
    view.printer_name_known = false;

    EXPECT_EQ(resolve(view).title, TitleSource::None);
}

TEST(OperationalContentTests, ReadyHidesProgressAndRemainingTime) {
    // §9 Ready lists these as explicitly hidden.
    EXPECT_FALSE(content_for(Fixture::Ready).show_progress);
    EXPECT_FALSE(content_for(Fixture::Ready).show_remaining);
}

// --- Preparing --------------------------------------------------------------

TEST(OperationalContentTests, PreparingCarriesItsStateWordAndNothingElse) {
    // PHASE G.4 REMOVED THIS STATE'S THIRD LINE, and the removal is the assertion.
    //
    // "Heating up" was drawn here until then. API Research Report §16.4 — Accepted — says the
    // phase may be "heating; homing; mesh probing; pre-print activity; another busy operation"
    // and that "the API may not identify the exact preparation phase", so the word was the
    // display's guess and was wrong in four of the five cases it named. UIDD 004 §7 requires
    // the machine to supply a component's value and §8 requires the third line to hold a
    // machine fact; it failed both.
    const auto content = content_for(Fixture::Preparing);

    EXPECT_EQ(content.title, TitleSource::Catalogue);
    EXPECT_EQ(content.title_id, TextId::StatePreparing);
    EXPECT_FALSE(content.show_remaining) << "Preparing draws no third line at all";

    // §9 lists "Heating progress" for Preparing, and the model carries no such value —
    // `progress` is job completion, which a print that has not started does not have.
    // Drawing it would invent a value, which §9 forbids in the same breath.
    EXPECT_FALSE(content.show_progress);
}

TEST(OperationalContentTests, PreparingsSubtitleCannotReturnWithoutAMachineSuppliedActivity) {
    // THE BAR, ASSERTED RATHER THAN LEFT TO REVIEW.
    //
    // The line can only come back through a real activity value, so this pins the two things
    // that would have to change first — and both are outside the ui component, which is the
    // point: neither can be done inside a scene by someone who thinks the composition looks
    // bare.
    //
    // 1. The catalogue holds no subtitle. `SubtitleHeating` is gone; re-adding a word here is a
    //    change to the text resource boundary, visible in a diff of text.hpp, and it would have
    //    to justify itself against §16.4 rather than against how Preparing looks.
    // 2. The model carries no activity. Presentation has no field a scene could read for one,
    //    so there is nothing to draw even if a word existed. §16.4 is why: the API does not
    //    report it.
    //
    // The scene's own side is structural — OperationalContent has one third-line flag and it
    // is fed from presentation.remaining — so there is no path from a catalogue word to the
    // panel that does not pass through both of the above.
    // The tripwire has fired twice. At Milestone 3.6 it caught five entries — two state
    // titles and three field captions — and at 3.8 two more: the titles for
    // ValidatingConfiguration and SetupFailed. All seven are the provisioning scene's, none
    // is an operational subtitle, and none is reachable from this composition, which is
    // selected from a different presentation layer entirely. API Research Report §16.4 is
    // untouched and the bar below is unchanged.
    // Milestone 3.10 adds two: the recovery title and the one instruction beneath it, both drawn
    // by the provisioning/recovery composition and neither reachable from this one. The tripwire
    // is doing its job — it forced this comment rather than letting the number drift.
    // Checkpoint 3c adds two more: "Wi-Fi unavailable" and "Restart the display.", the title and
    // Status Message of the Wi-Fi Device Fault the owner's DriverStalled ruling requires. Neither
    // is an operational subtitle and neither is reachable from THIS composition — a device fault
    // resolves at UIAD 001 §12.1, above the subject presentation entirely, so the operational scene
    // is not even selected when it is live. API Research Report §16.4 is untouched: nothing here
    // invents a Preparing activity the machine did not supply.
    // Audit item 5 adds four: the titles and Status Messages of the two device faults an incomplete
    // factory reset and an unanswerable store require. All four are drawn by the provisioning/
    // recovery composition and none is reachable from THIS one — a device fault resolves at UIAD 001
    // §12.1, above the subject presentation entirely, so the operational scene is not selected while
    // either is live. API Research Report §16.4 is untouched: nothing here invents a Preparing
    // activity the machine did not supply, and none of the four is a subtitle of any kind.
    // The lost-observation checkpoint adds one: Production UI §9's Status Message for Printer
    // Unavailable, drawn as the transient acknowledgement of UIAD 001 §11.1. It is not a subtitle
    // and it is not reachable from THIS composition — at Unavailable no subject may be presented as
    // current (UIAD 001 §7), so the operational scene carries nothing and the acknowledgement is
    // drawn on §17.3's transient overlay instead. API Research Report §16.4 is untouched: nothing
    // here invents a Preparing activity the machine did not supply.
    // Owner-reviewed unknown-progress Printing adds a truthful absence label, not an activity.
    // Six access-scene entries, none reachable as a Preparing subtitle.
    // Five central Error entries qualify a retained subject; none invents an activity.
    // Three owner-approved Attention entries name the condition and actual subject, not activity.
    // The message family adds two: "Update failed" and its own "Restart the display.", the title
    // and instruction of the pending-image validation fault. Both are drawn by the shared message
    // composition at UIAD 001 §12.1 — above the subject presentation entirely, so the operational
    // scene is not selected while the fault is live — and neither is a subtitle of any kind. API
    // Research Report §16.4 is untouched: nothing here invents a Preparing activity the machine
    // did not supply.
    // The accepted live OTA family adds six more entries. They are consumed only by the dedicated
    // FirmwareUpdate scene selected from an exclusive operation (or its terminal failure), never
    // by OperationalContent; Installing deliberately has no numeric-progress label of its own.
    // The first-safe-Ready notice adds two more: its title and the physical-settings instruction.
    // They are consumed by the shared provisioning MessageForm only while the notice projection is
    // active; neither is reachable as Preparing's subtitle.
    // The owner-selected remaining-time templates add two entries for Printing estimates;
    // they cannot invent a Preparing activity or make its third line visible.
    EXPECT_EQ(kTextIdCount, 56U) << "a catalogue entry was added; if it is a Preparing subtitle, "
                                    "API Research Report §16.4 has to have been superseded first";

    for (std::size_t index = 0; index < kTextIdCount; ++index) {
        const auto word = prd::ui::text::lookup(prd::ui::text::text_id_at(index));
        EXPECT_NE(word, "Heating up") << "the removed subtitle is back in the catalogue";
    }

    // Preparing's presentation carries no remaining time, so the one live source for the third
    // line cannot supply this state either — the slot is empty because nothing fills it, not
    // because a flag happens to be false.
    const auto view = project(prd::fake_source::state_for(Fixture::Preparing));
    EXPECT_FALSE(view.remaining.known);
    EXPECT_FALSE(resolve(view).show_remaining);
}

// --- Printing ---------------------------------------------------------------

TEST(OperationalContentTests, PrintingShowsProgressAndItsRemainingDuration) {
    const auto content = content_for(Fixture::Printing);

    EXPECT_EQ(content.title, TitleSource::Catalogue);
    EXPECT_EQ(content.title_id, TextId::StatePrinting);
    EXPECT_TRUE(content.show_progress);
    EXPECT_TRUE(content.show_remaining);
}

TEST(OperationalContentTests, PrintingWithoutAKnownProgressStillDrawsItsForm) {
    // THE E.4 SEMANTIC CHANGE, seen from the scene layer.
    //
    // The projection used to delete the form when the printer had not reported progress, so a
    // Printing state with no figure drew nothing at all where its most characteristic element
    // belongs. UIDD 004 §3.2 forbids the record determining the form's shape and §13.1 keeps
    // the inventory constant across the family, so the form stays — at Printing's own maximum
    // energy, because energy is a property of the state and not of the report.
    //
    // Nothing is invented to fill the gap: the record remains absent, so the scene draws no
    // numeral, and the sweep has no value to show.
    auto view = project(prd::fake_source::state_for(Fixture::Printing));
    view.progress = {};

    const auto content = resolve(view);

    ASSERT_TRUE(content.form.has_value());
    EXPECT_EQ(*content.form, kEnergyPrinting);
    EXPECT_FALSE(content.show_progress);
}

TEST(OperationalContentTests, PrintingWithoutAKnownDurationShowsNoThirdLine) {
    auto view = project(prd::fake_source::state_for(Fixture::Printing));
    view.remaining.known = false;

    EXPECT_FALSE(resolve(view).show_remaining);
    EXPECT_TRUE(resolve(view).show_progress);
}

// --- Paused -----------------------------------------------------------------

TEST(OperationalContentTests, PausedKeepsItsProgressAndWithholdsTheDuration) {
    // §9: components identical to Printing, progress stopped and its value unchanged.
    //
    // The duration is deliberately withheld — the estimate is against printing time and the
    // printer is not printing, so showing it would present a stale figure as current, which
    // UI Behaviour Decision 001 §7 forbids outright.
    const auto content = content_for(Fixture::Paused);

    EXPECT_EQ(content.title_id, TextId::StatePaused);
    EXPECT_TRUE(content.show_progress);
    EXPECT_FALSE(content.show_remaining);
}

TEST(OperationalContentTests, PausedCarriesNoActionSubtitle) {
    // The artwork's "Resume print" names an action a read-only MVP cannot perform
    // (Architecture Spec §2.2 and invariant 18), so it is not in the catalogue and not here.
    //
    // Paused draws no third line at all: it withholds the remaining duration (above) and there
    // is no other source for the slot since Phase G.4.
    EXPECT_FALSE(content_for(Fixture::Paused).show_remaining);

    for (std::size_t index = 0; index < kTextIdCount; ++index) {
        EXPECT_NE(prd::ui::text::lookup(prd::ui::text::text_id_at(index)), "Resume print");
    }
}

// --- Completed --------------------------------------------------------------

TEST(OperationalContentTests, CompletedLeadsWithItsOutcome) {
    // The adopted outcome-first hierarchy. §9 makes the acknowledgement what this state exists
    // to say; leading with the record put the job's historical figure above the outcome.
    const auto content = content_for(Fixture::Completed);

    EXPECT_EQ(content.arrangement, Arrangement::Outcome);

    // THE RECORD IS MOVED, NOT REMOVED. §9 keeps Completed's components identical to Printing's,
    // and §3.2 requires the record to survive as the numeric value where the form cannot host
    // it — which at Completed's energy it cannot, because the shape is open.
    EXPECT_TRUE(content.show_progress);
}

TEST(OperationalContentTests, CompletedIsTheOnlyStateThatLeadsWithItsOutcome) {
    // §1: the family is one composition, so an arrangement is a property of what a state has —
    // a record and an outcome — and not a decoration any state may adopt.
    for (const auto& member : kFamily) {
        const auto content = content_for(member.fixture);

        if (member.subject == SubjectState::Completed) {
            EXPECT_EQ(content.arrangement, Arrangement::Outcome) << member.name;
            continue;
        }

        EXPECT_NE(content.arrangement, Arrangement::Outcome) << member.name;
    }
}

TEST(OperationalContentTests, TheArrangementFollowsTheRecordForEveryOtherState) {
    // Unchanged by Phase F, and asserted so that it stays that way: a state with a record builds
    // the three-row operational stack, and a state without one centres its two lines.
    for (const auto& member : kFamily) {
        if (member.subject == SubjectState::Completed) {
            continue;
        }

        const auto content = content_for(member.fixture);
        const auto expected =
            content.show_progress ? Arrangement::Operational : Arrangement::Resting;

        EXPECT_EQ(content.arrangement, expected) << member.name;
    }
}

TEST(OperationalContentTests, ACompletedJobWithNoRecordCannotLeadWithOne) {
    // The one combination no fixture reaches, and the reason the rule lives in one place rather
    // than inside the Completed case. A machine that reports a finished print without ever
    // having supplied its progress leaves nothing to put in the second row, so the stack is the
    // resting one — not an outcome stack with a hole where the record should be.
    //
    // Built through the real reducer and projection, like every other case here: a terminal
    // printer state with no job ever observed.
    const auto now = prd::test::at_ms(1000);
    const auto state = prd::test::with_printer_state(
        prd::test::with_connection(prd::test::booted_device(),
                                   prd::app_model::ConnectionPhase::Connected, now),
        prd::app_model::PrinterState::Finished, now, 1);
    const auto content = resolve(project(state));

    ASSERT_EQ(project(state).subject, SubjectState::Completed);
    EXPECT_FALSE(content.show_progress);
    EXPECT_EQ(content.arrangement, Arrangement::Resting);
}

TEST(OperationalContentTests, CompletedAcknowledgesInTheSuccessColour) {
    // §9: "Completion acknowledgement uses `color.success`."
    const auto content = content_for(Fixture::Completed);

    EXPECT_EQ(content.title_id, TextId::StateCompleted);
    EXPECT_EQ(content.title_colour, prd::ui::theme::Colour::Success);
    EXPECT_TRUE(content.show_progress);
}

TEST(OperationalContentTests, CompletedIsTheOnlyStateThatChangesTheTitleColour) {
    // The other five share one role. §13.1 fixes text roles across all six states, so a state
    // quietly acquiring a colour of its own — chromatic or not — would break the family.
    for (const auto& member : kFamily) {
        const auto content = content_for(member.fixture);
        const auto expected = member.subject == SubjectState::Completed
                                  ? prd::ui::theme::Colour::Success
                                  : prd::ui::theme::Colour::Primary;

        EXPECT_EQ(content.title_colour, expected) << member.name;
    }
}

TEST(OperationalContentTests, NoFamilyTextCarriesTheAccent) {
    // UIDD 004 §5.1, and the acceptance criterion that states it as a test: "the accent has
    // exactly one referent". That referent is the operational form. The state word carried the
    // accent in five states until Phase F, and Ready's printer name in the sixth.
    //
    // ASSERTED AS A LAW RATHER THAN AS A VALUE. The test above pins which achromatic role the
    // five states take, and that role is a composition decision that a later phase may revisit.
    // This one pins §5.1 itself, and it fails for any chromatic token — including a warning or
    // error hue, which §5.3 reserves for subject condition and forbids following from state.
    for (const auto& member : kFamily) {
        const auto content = content_for(member.fixture);

        if (member.subject == SubjectState::Completed) {
            // §5.2's single exception: the state word only, at Completed only.
            EXPECT_EQ(content.title_colour, prd::ui::theme::Colour::Success) << member.name;
            continue;
        }

        EXPECT_TRUE(is_achromatic(content.title_colour)) << member.name;
    }
}

// --- Stopped ----------------------------------------------------------------

TEST(OperationalContentTests, StoppedKeepsItsRecordWhileItsFormRecedes) {
    // THE CARRIED CONFLICT CLOSES HERE. §9 Print Stopped requires components identical to
    // Printing and progress to remain visible; accepted artwork board 2 panel 6 shows a
    // resting form and no ring. Both readings were implementable and the implementation
    // followed the written text, which is what this test used to pin.
    //
    // UIDD 004 §12 resolves it without amending §9: "'progress' is the value, not the arc. The
    // record therefore remains visible while the form recedes. The component inventory is
    // unchanged; only energy differs."
    //
    // So Stopped keeps its record — the numeral — and its form sits low on the axis, below
    // Preparing's, which is §2.2's ordering. The inventory is unchanged: it still draws the
    // one operational form, as every family member does.
    const auto content = content_for(Fixture::Stopped);

    EXPECT_EQ(content.title_id, TextId::StateStopped);
    EXPECT_TRUE(content.show_progress);

    ASSERT_TRUE(content.form.has_value());

    const auto preparing = content_for(Fixture::Preparing);
    ASSERT_TRUE(preparing.form.has_value());
    EXPECT_LT(*content.form, *preparing.form)
        << "Stopped's form must have receded below Preparing's (UIDD 004 §2.2)";

    const auto printing = content_for(Fixture::Printing);
    ASSERT_TRUE(printing.form.has_value());
    EXPECT_LT(*content.form, *printing.form);
}

// --- Degraded confidence ----------------------------------------------------

TEST(OperationalContentTests, DegradedConfidenceDoesNotChangeWhatAStateShows) {
    // §11.5 permits stale values to be shown behind an explicit indicator — the indicator is
    // added, the content is not withheld. A state that quietly dropped its title or its ring
    // when confidence fell would be a different composition, not the same one marked.
    for (const auto& member : kFamily) {
        const auto current = content_for(member.fixture);

        auto degraded_view = project(prd::fake_source::state_for(member.fixture));
        degraded_view.stale_indicator = true;
        const auto degraded = resolve(degraded_view);

        EXPECT_EQ(degraded.form, current.form) << member.name;
        EXPECT_EQ(degraded.title, current.title) << member.name;
        EXPECT_EQ(degraded.show_progress, current.show_progress) << member.name;
        EXPECT_EQ(degraded.show_remaining, current.show_remaining) << member.name;
    }
}
