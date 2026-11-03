// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

// Fixture replay into one accumulating store — what the operational walk does on the device.
//
// WHY THIS SUITE EXISTS. Every other fixture test uses state_for(), which folds a fixture into
// a FRESH AppState. The walk cannot: it drives app_core's real store, because the whole point
// is to exercise the product path. Replayed into a store that already holds another fixture,
// a fixture's observations carry sequence numbers that do not exceed the previous one's, and
// the reducer discards them as stale — correctly, since that is exactly the protection §8.1
// asks for. The state is then a mixture of two fixtures and resolves to no scene.
//
// On the panel that is a black screen, and it is what the first hardware advance produced.
// The fix is that the walk enters BootPhase::Starting first, which is the model's own
// representation of the reboot a fixture assumes.
//
// WIDENED IN PHASE G.1, ON BOTH AXES, BECAUSE THE ORIGINAL GUARD MISSED TWO DEVICE FAILURES.
// It compared subject, confidence and form presence only — never the job record — and it
// replayed with ascending synthetic bases and no real-clock event interleaved. Both fixtures
// added at E.6 passed here and failed on the board:
//
//   * `printing-degraded` reported confidence=current. The reducer clamps an out-of-order event
//     forward, and a button gesture carries real board uptime, so once uptime passed a fixture's
//     synthetic base its whole script collapsed onto one instant and a 30-second ageing gap
//     became zero. Guarded now by driving the real WalkTimeBase and by interleaving a gesture
//     stamped with the board's clock, which is what a press actually dispatches.
//   * `printing-unknown-progress` reported the previous fixture's job. Guarded now by comparing
//     the job record — see the pinned expectation at the foot of this file, which is the half
//     of that defect G.1 cannot fix.
#include "app_model/presentation.hpp"
#include "app_model/reducer.hpp"
#include "app_store/store.hpp"
#include "fake_source/fixtures.hpp"
#include "support/state_builders.hpp"
#include "walk_time_base.hpp"
#include <gtest/gtest.h>

using namespace prd::app_model;
using prd::fake_source::Fixture;
using prd::validation::WalkTimeBase;

namespace {

// The walk's own list, and it must stay the walk's own list: the defect this suite guards
// against is a fixture reaching the panel without a boot prelude, so a fixture the device walks
// but this array omits would be exactly the one nothing checks.
constexpr Fixture kWalk[] = {Fixture::Ready,
                             Fixture::Preparing,
                             Fixture::Printing,
                             Fixture::Paused,
                             Fixture::Completed,
                             Fixture::Stopped,
                             Fixture::PrintingDegraded,
                             Fixture::ReadyDegraded,
                             Fixture::PrintingUnknownProgress};

constexpr std::size_t kWalkLength = sizeof(kWalk) / sizeof(kWalk[0]);

// Board uptime late in a review session. The synthetic bases the walk used before G.1 ran to
// 900 s for a nine-fixture list, so anything past that reproduces the condition under which
// every one of them collapsed. The F.6 session ran far longer than this.
constexpr std::int64_t kLateUptimeMs = 2700000;

// A review that leaves each state on the panel for longer than the 60 s expiry window, and one
// that does not. The distinction is not decoration: it decides whether the previous fixture's
// observation is still fresh when the next fixture's job arrives.
constexpr std::int64_t kUnhurriedPaceMs = 90000;
constexpr std::int64_t kQuickPaceMs = 1000;

// The walk's own replay protocol, and it must be the walk's own protocol.
//
// TWO STEPS, AND A TEST THAT PERFORMS ONLY THE FIRST PROVES THE OPPOSITE OF WHAT IT CLAIMS. The
// flow takes its base from the time base and then feeds every emitted event back into it, so the
// next base clears this script's tail. A replay that computed a base and never observed anything
// would run on the clock alone — which is the collapse that fails a quick walk, and is what an
// earlier draft of this suite was accidentally asserting.
struct Replay {
    prd::app_store::Store store;
    WalkTimeBase clock;

    void load(const Fixture fixture, const Timestamp now) {
        prd::fake_source::emit_script(fixture, clock.base_for(now), observe_and_dispatch, this);
    }

    [[nodiscard]] Presentation view() const {
        return project(store.state());
    }

    static void observe_and_dispatch(const Event& event, void* const context) {
        auto* const replay = static_cast<Replay*>(context);

        replay->clock.observe(event.observed_at);
        (void)replay->store.dispatch(event);
    }
};

// The two fixtures that observe their job before their own snapshot. See
// TheTerminalFixturesLiveCountersFollowTheReviewPace.
[[nodiscard]] bool is_terminal(const Fixture fixture) {
    return fixture == Fixture::Completed || fixture == Fixture::Stopped;
}

// A copy of `live` with the job record taken from `fresh`.
//
// Written as a subtraction from the whole presentation rather than as a list of fields to
// check, so a field added to Presentation later is compared by default instead of being
// silently omitted. The subtracted fields are not skipped — they are compared one by one
// below, where a failure can name the value that drifted.
[[nodiscard]] Presentation with_job_record_of(Presentation live, const Presentation& fresh) {
    live.job_name_known = fresh.job_name_known;
    live.job_name = fresh.job_name;
    live.progress = fresh.progress;
    live.remaining = fresh.remaining;
    live.remaining_is_approximate = fresh.remaining_is_approximate;
    live.elapsed = fresh.elapsed;
    live.layers = fresh.layers;
    // Absolute clock origin shifts on replay; relative age is independently checked below.
    live.stale_observed_at = fresh.stale_observed_at;
    return live;
}

// Everything the replay must get right, the job record included.
//
// UNCONDITIONAL SINCE G.2. Until the reducer cleared the job on BootPhase::Starting, the
// fixtures that observe no job of their own inherited the previous one, and this comparison had
// to exempt them. It does not any more: a fixture that observes no job now reproduces the empty
// record exactly, the same as any other field.
//
// The job record is compared field by field rather than through Presentation's operator==,
// because a failure has to name the value that drifted. The whole-object comparison reports two
// 412-byte hex dumps, which is how a job-record defect stayed invisible in this suite in the
// first place.
void expect_replay_matches_fresh(const prd::app_model::AppState& live_state, const Fixture fixture,
                                 const char* const context) {
    const auto live = project(live_state);
    const auto fresh_state = prd::fake_source::state_for(fixture);
    const auto fresh = project(fresh_state);
    const auto label = prd::fake_source::name(fixture);

    ASSERT_EQ(live.stale_observed_at.has_value(), fresh.stale_observed_at.has_value());
    if (live.stale_observed_at) {
        EXPECT_EQ(*live.stale_observed_at, live_state.printer.observed_at);
        EXPECT_EQ(live_state.last_event_at - *live.stale_observed_at,
                  fresh_state.last_event_at - *fresh.stale_observed_at);
    }

    EXPECT_EQ(with_job_record_of(live, fresh), fresh) << context << ": " << label;

    // The durable half of the record: what the machine reached, and what it was printing.
    EXPECT_EQ(live.job_name_known, fresh.job_name_known) << context << ": " << label << " job name";
    EXPECT_EQ(live.job_name.view(), fresh.job_name.view()) << context << ": " << label;
    EXPECT_EQ(live.progress.known, fresh.progress.known) << context << ": " << label << " progress";
    EXPECT_EQ(live.progress.percent, fresh.progress.percent) << context << ": " << label;
    EXPECT_EQ(live.layers.known, fresh.layers.known) << context << ": " << label << " layers";
    EXPECT_EQ(live.layers.current, fresh.layers.current) << context << ": " << label;
    EXPECT_EQ(live.layers.total, fresh.layers.total) << context << ": " << label;

    if (is_terminal(fixture)) {
        return;
    }

    // The two counters whose meaning is "as of now", which the terminal fixtures cannot
    // reproduce at every review pace.
    EXPECT_EQ(live.remaining.known, fresh.remaining.known)
        << context << ": " << label << " remaining";
    EXPECT_EQ(live.remaining.seconds, fresh.remaining.seconds) << context << ": " << label;
    EXPECT_EQ(live.remaining_is_approximate, fresh.remaining_is_approximate)
        << context << ": " << label;
    EXPECT_EQ(live.elapsed.known, fresh.elapsed.known) << context << ": " << label << " elapsed";
    EXPECT_EQ(live.elapsed.seconds, fresh.elapsed.seconds) << context << ": " << label;
}

} // namespace

// --- The time base ----------------------------------------------------------

TEST(WalkTimeBaseTests, TheBaseIsNeverBehindTheBoardsClock) {
    // The failure the board showed. A base behind anything the store has already stamped is
    // clamped forward event by event, which does not reorder the script — it flattens it.
    WalkTimeBase clock;

    EXPECT_EQ(clock.base_for(prd::test::at_ms(kLateUptimeMs)), prd::test::at_ms(kLateUptimeMs));

    clock.observe(prd::test::at_ms(500));
    EXPECT_EQ(clock.base_for(prd::test::at_ms(kLateUptimeMs)), prd::test::at_ms(kLateUptimeMs))
        << "an old emitted event must not hold the base back";
}

TEST(WalkTimeBaseTests, TheBaseIsNeverBehindTheLastEmittedEvent) {
    // The other floor, and the reason the clock alone is not enough. Real time barely moves
    // between two quick presses, but the previous script's own offsets did: its last event can
    // sit a full script span ahead of the clock.
    WalkTimeBase clock;

    const auto now = prd::test::at_ms(kLateUptimeMs);
    clock.observe(now + prd::test::at_ms(30000));

    EXPECT_GT(clock.base_for(now), now + prd::test::at_ms(30000))
        << "a base at or behind the last emitted event replays the next script into its tail";
}

TEST(WalkTimeBaseTests, TheBaseOnlyEverAdvances) {
    WalkTimeBase clock;

    auto previous = clock.base_for(prd::test::at_ms(0));
    for (std::int64_t step = 0; step < 10; ++step) {
        // Alternating: the clock wins on the slow steps, the emitted floor on the quick ones.
        const auto now = prd::test::at_ms(step % 2 == 0 ? step * 1000 : 0);
        const auto base = clock.base_for(now);

        EXPECT_GE(base, previous);
        EXPECT_GE(base, now);

        clock.observe(base + prd::test::at_ms(30000));
        previous = base;
    }
}

// --- Replay -----------------------------------------------------------------

TEST(WalkReplayTests, EachFixtureReachesItsOwnStateWhenReplayedIntoOneStore) {
    // A review at a human pace: the reviewer looks at each state for a minute and a half, so
    // real time is always further on than the previous script reached and the clock is the
    // floor that binds.
    Replay replay;

    for (std::size_t i = 0; i < kWalkLength; ++i) {
        replay.load(kWalk[i], prd::test::at_ms(kLateUptimeMs +
                                               static_cast<std::int64_t>(i) * kUnhurriedPaceMs));

        expect_replay_matches_fresh(replay.store.state(), kWalk[i], "unhurried walk");
    }

    EXPECT_EQ(replay.store.state().diagnostics.events_out_of_order, 0U)
        << "no event the walk emits may be clamped forward";
}

TEST(WalkReplayTests, AQuickWalkStillGivesEachFixtureItsOwnTimeline) {
    // The same walk with the button pressed faster than the scripts are long. Real time advances
    // one second per load while a script spans thirty, so a base taken from the clock alone
    // would land inside the previous script's tail and be clamped forward — the synthetic
    // scheme's failure, arrived at from the opposite direction.
    Replay replay;

    for (std::size_t i = 0; i < kWalkLength; ++i) {
        replay.load(kWalk[i],
                    prd::test::at_ms(kLateUptimeMs + static_cast<std::int64_t>(i) * kQuickPaceMs));

        expect_replay_matches_fresh(replay.store.state(), kWalk[i], "quick walk");
    }

    EXPECT_EQ(replay.store.state().diagnostics.events_out_of_order, 0U)
        << "no event the walk emits may be clamped forward";
}

TEST(WalkReplayTests, EveryFixtureReachesItsOwnStateAfterEveryOther) {
    // The exhaustive version, and the real guard. Any pair, in either order — 72 transitions —
    // must land on exactly the presentation the fixture produces from a fresh device.
    //
    // This is what protects the walk from the defect returning through a different door: a
    // future fixture written without a boot prelude would not reset the watermarks, its
    // observations would be discarded, and the panel would go black again with nothing else
    // reporting it.
    for (const auto from : kWalk) {
        for (const auto to : kWalk) {
            if (from == to) {
                continue;
            }

            Replay replay;
            const auto now = prd::test::at_ms(kLateUptimeMs);

            replay.load(from, now);
            replay.load(to, now + prd::test::at_ms(kQuickPaceMs));

            expect_replay_matches_fresh(replay.store.state(), to,
                                        prd::fake_source::name(from).data());
            EXPECT_EQ(replay.store.state().diagnostics.events_out_of_order, 0U)
                << prd::fake_source::name(from) << " -> " << prd::fake_source::name(to);
        }
    }
}

// --- The board's own clock, interleaved -------------------------------------

TEST(WalkReplayTests, ARealClockEventBetweenLoadsDoesNotCollapseTheNextFixture) {
    // THE SECOND AXIS THE OLD GUARD WAS MISSING. On the device every press dispatches
    // Event::button_gesture(platform::monotonic_time(), …) into the same store the walk feeds,
    // so real board uptime enters the timeline between one fixture and the next. The old suite
    // replayed with nothing but its own synthetic bases and could not see what that did.
    for (const auto fixture : kWalk) {
        Replay replay;

        const auto first = prd::test::at_ms(kLateUptimeMs);
        replay.load(Fixture::Printing, first);

        // The press that asks for the next state, stamped the way app_core stamps it.
        const auto press = first + prd::test::at_ms(kUnhurriedPaceMs);
        (void)replay.store.dispatch(Event::button_gesture(press, ButtonGesture::ShortPress));

        replay.load(fixture, press);

        expect_replay_matches_fresh(replay.store.state(), fixture, "after a real-clock press");
    }
}

TEST(WalkReplayTests, TheDegradedFixturesStayDegradedLateInASession) {
    // The E.6 device failure, named. Both confidence fixtures reported confidence=current on the
    // board and Degraded here, because the host replayed them at a base no real clock had passed.
    const Fixture degraded[] = {Fixture::PrintingDegraded, Fixture::ReadyDegraded};

    for (const auto fixture : degraded) {
        Replay replay;

        const auto first = prd::test::at_ms(kLateUptimeMs);
        replay.load(Fixture::Printing, first);

        const auto press = first + prd::test::at_ms(kUnhurriedPaceMs);
        (void)replay.store.dispatch(Event::button_gesture(press, ButtonGesture::ShortPress));
        replay.load(fixture, press);

        const auto view = replay.view();

        EXPECT_EQ(view.confidence, Confidence::Degraded) << prd::fake_source::name(fixture);
        EXPECT_TRUE(view.stale_indicator) << prd::fake_source::name(fixture);

        // §10: confidence changes no energy. The form is present at both ends of the axis.
        EXPECT_TRUE(view.show_operational_form) << prd::fake_source::name(fixture);
    }
}

TEST(WalkReplayTests, ASyntheticBaseIsWhatCollapsedTheDegradedFixture) {
    // Not a guard on today's code — the demonstration that the two tests above have teeth.
    //
    // This replays PrintingDegraded exactly as the walk did before G.1: at `(index + 1) × 100 s`
    // with the board already past it. Every event of the script is clamped forward to the
    // store's watermark, the fixture's own thirty-second ageing gap becomes zero, and the
    // presentation comes back Current with no indicator. That is what the owner saw at E.6, and
    // what a reviewer would see again if the base ever stopped following the clock.
    Replay replay;

    (void)replay.store.dispatch(
        Event::button_gesture(prd::test::at_ms(kLateUptimeMs), ButtonGesture::ShortPress));

    constexpr std::int64_t kOldWalkIndex = 6;
    prd::fake_source::emit_script(Fixture::PrintingDegraded,
                                  prd::test::at_ms((kOldWalkIndex + 1) * 100000),
                                  Replay::observe_and_dispatch, &replay);

    const auto view = replay.view();

    EXPECT_EQ(view.confidence, Confidence::Current)
        << "if this now reports Degraded, the reducer's clamping changed and this "
           "demonstration needs rewriting";
    EXPECT_FALSE(view.stale_indicator);
    EXPECT_GT(replay.store.state().diagnostics.events_out_of_order, 0U);
}

// --- The mechanism, isolated ------------------------------------------------

TEST(WalkReplayTests, EnteringStartingIsWhatResetsTheWatermarks) {
    // The mechanism itself, isolated from the fixtures. Without this the suite above would
    // still pass if the fixtures happened to use ascending sequences, and would break the
    // moment one did not.
    prd::app_store::Store store;

    PrinterSnapshotObservation first;
    first.sequence = 9;
    first.normalized = PrinterState::Printing;
    (void)store.dispatch(Event::printer_snapshot(prd::test::at_ms(1000), first));
    ASSERT_EQ(store.state().last_printer_sequence, 9U);

    (void)store.dispatch(Event::boot_phase(prd::test::at_ms(2000), BootPhase::Starting));
    EXPECT_EQ(store.state().last_printer_sequence, 0U);
    EXPECT_EQ(store.state().last_job_sequence, 0U);

    // And a lower sequence is now accepted, which is the whole point.
    PrinterSnapshotObservation second;
    second.sequence = 1;
    second.normalized = PrinterState::Ready;
    (void)store.dispatch(Event::printer_snapshot(prd::test::at_ms(3000), second));
    EXPECT_EQ(store.state().last_printer_sequence, 1U);
}

TEST(WalkReplayTests, AStaleSequenceIsStillRejectedWithoutABoot) {
    // §8.1's protection must survive the change. Resetting on Starting must not have turned
    // the sequence check off generally — a late or duplicated response between boots is still
    // discarded.
    prd::app_store::Store store;

    PrinterSnapshotObservation first;
    first.sequence = 9;
    first.normalized = PrinterState::Printing;
    (void)store.dispatch(Event::printer_snapshot(prd::test::at_ms(1000), first));

    PrinterSnapshotObservation late;
    late.sequence = 4;
    late.normalized = PrinterState::Ready;
    (void)store.dispatch(Event::printer_snapshot(prd::test::at_ms(2000), late));

    EXPECT_EQ(store.state().last_printer_sequence, 9U) << "a late response must be discarded";
}

// --- The G.2 regression, in the place the defect was seen -------------------

TEST(WalkReplayTests, AFixtureThatObservesNoJobInheritsNothing) {
    // A PINNED DEFECT UNTIL G.2, AND NOW THE RULE. Before the reducer cleared the job on
    // BootPhase::Starting, these four inherited whatever the store still held: it is why
    // `printing-unknown-progress` showed Printing's job at E.6, and why Ready and Preparing
    // showed `156/250 Layer` in the F.6 photographs.
    //
    // Asserted here on top of the now-unconditional comparison in expect_replay_matches_fresh(),
    // because "equal to fresh" would also be satisfied by a fresh state that had itself gone
    // wrong. This states what must be true of the panel: no job at all.
    const Fixture without_a_job[] = {Fixture::Ready, Fixture::Preparing, Fixture::ReadyDegraded,
                                     Fixture::PrintingUnknownProgress};

    for (const auto fixture : without_a_job) {
        Replay replay;
        const auto now = prd::test::at_ms(kLateUptimeMs);

        // Printing first, so a full job record — 62 %, layer 156 of 250, a name, a remaining
        // time and an elapsed time — is there to be inherited if anything still can.
        replay.load(Fixture::Printing, now);
        ASSERT_TRUE(replay.view().layers.known) << "the job to be inherited must exist first";

        replay.load(fixture, now + prd::test::at_ms(kQuickPaceMs));

        const auto live = replay.view();
        const auto label = prd::fake_source::name(fixture);

        EXPECT_FALSE(live.layers.known) << label << " must not inherit Printing's layer count";
        EXPECT_FALSE(live.progress.known) << label << " must not inherit Printing's progress";
        EXPECT_FALSE(live.remaining.known) << label << " must not inherit Printing's remaining";
        EXPECT_FALSE(live.elapsed.known) << label << " must not inherit Printing's elapsed";
        EXPECT_FALSE(live.job_name_known) << label << " must not inherit Printing's job name";

        // Identity is not job data and is unaffected, which is what keeps this a change to the
        // job's lifetime rather than to a presence rule.
        EXPECT_TRUE(live.printer_name_known) << label;

        // The machine's own readings survive too — except where §10 has taken them, which is a
        // different rule at a different layer and is asserted in projection_tests.cpp. Stated as
        // a conditional rather than dropped, so the boot rule cannot start clearing telemetry
        // without this test noticing.
        EXPECT_EQ(live.nozzle.current.known, live.confidence == Confidence::Current) << label;
    }
}

TEST(WalkReplayTests, TheTerminalFixturesLiveCountersFollowTheReviewPace) {
    // PINNED, PRE-EXISTING, AND WITHOUT A COMPOSITIONAL CONSEQUENCE — but newly visible, which
    // is what widening the guard was for.
    //
    // Completed and Stopped observe their job BEFORE their own snapshot, deliberately: §8.1's
    // completion snapshot must have a job to persist. So the job arrives while the newest printer
    // observation is still the PREVIOUS fixture's. If the reviewer left that state on the panel
    // for longer than the 60 s expiry window, the observation is Expired by then, and §11.5's
    // rule removes the two values whose meaning is "as of now" — remaining and elapsed —
    // immediately after the job observation set them. The fixture's own snapshot lands 10 ms
    // later and restores freshness, but nothing restores the two counters.
    //
    // NOT INTRODUCED BY G.1. The synthetic scheme strided its bases 100 s apart, which is past
    // the same window, so every terminal fixture on the old walk did this too at every pace. The
    // old guard compared neither counter, so it was never visible.
    //
    // No Phase G checkpoint owns it and the panel cannot show it: remaining is drawn only as
    // Printing's third line, and elapsed is drawn nowhere in the family. Recorded so it stays a
    // known quantity rather than a surprise for whoever widens this suite next.
    const Fixture terminal[] = {Fixture::Completed, Fixture::Stopped};

    for (const auto fixture : terminal) {
        const auto fresh = project(prd::fake_source::state_for(fixture));
        const auto label = prd::fake_source::name(fixture);

        ASSERT_TRUE(fresh.remaining.known) << label << " observes its own remaining time";

        Replay quick;
        const auto now = prd::test::at_ms(kLateUptimeMs);
        quick.load(Fixture::Printing, now);
        quick.load(fixture, now + prd::test::at_ms(kQuickPaceMs));

        EXPECT_TRUE(quick.view().remaining.known)
            << label << ": inside the expiry window the record is reproduced exactly";
        EXPECT_EQ(quick.view().elapsed.seconds, fresh.elapsed.seconds) << label;

        Replay unhurried;
        unhurried.load(Fixture::Printing, now);
        unhurried.load(fixture, now + prd::test::at_ms(kUnhurriedPaceMs));

        EXPECT_FALSE(unhurried.view().remaining.known)
            << label << ": past the expiry window §11.5 removes the live counters";
        EXPECT_FALSE(unhurried.view().elapsed.known) << label;

        // The durable half is reproduced either way, which is what keeps this a counter question
        // rather than a lost record. §7: "a terminal job's layer count is still true".
        EXPECT_EQ(unhurried.view().layers.current, fresh.layers.current) << label;
        EXPECT_EQ(unhurried.view().progress.percent, fresh.progress.percent) << label;
    }
}
