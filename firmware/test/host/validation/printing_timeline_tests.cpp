// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "app_model/presentation.hpp"
#include "app_model/reducer.hpp"
#include "app_store/store.hpp"

#include "exclusive_operation.hpp"
#include "fake_source/scenarios.hpp"
#include "gesture_policy.hpp"
#include "scenes/operational_content.hpp"

#include "decode.hpp"
#include "dto.hpp"
#include "normalize.hpp"
#include "status_timeline.hpp"

// The Printing telemetry timeline, end to end.
//
// ⛔ WHAT THIS IS FOR, AND WHY IT IS NOT A DECODER TEST. decode_tests.cpp proves the bytes become
// a DTO and normalize_tests.cpp proves the DTO becomes an observation. Neither proves the figure
// reaches the PANEL, and that gap is exactly where this checkpoint's defect lived: the domain
// model, the projection and the composition were all complete and correct for months, and no
// progress value ever arrived because nothing between the response and the store carried one.
// A pipeline assembled from individually-correct stages is the failure mode, so this walks the
// whole of it:
//
//   the same JSON bytes the device replays
//     → prusalink::decode_status()
//     → prusalink::status_from()
//     → app_model::Event::printer_status   one event, one mutation
//     → app_model::reduce()
//     → app_model::project()
//     → ui::scene::resolve()          the production UI's own visibility and content decision
//
// ⛔ AND THE BODIES ARE THE DEVICE'S OWN. They come from validation/status_timeline.cpp, the same
// translation unit the gated flow reads, so this suite and the bench instrument cannot disagree
// about what was sent. They are SYNTHETIC, shaped to the Prusa-Link-Web OpenAPI contract and to
// Prusa-Firmware-Buddy v6.8.1's status renderer. ⛔ THEY ARE NOT CAPTURED FROM A PRINTER, and
// nothing proved here is evidence about what a CORE One sends.

namespace prd::validation {
namespace {

// The synthetic worker this replay runs as. Any non-zero generation would do; what matters is that
// every step of one run shares it, exactly as one worker's poll cycles do.
constexpr std::uint32_t kGeneration = 1;

// Replays the timeline through the PRODUCTION SEAM, into a real store.
//
// ⛔ IT REBUILDS NO WIRING. Every body goes through validation::apply_status_body(), which is the
// same function the gated bench flow calls and which performs exactly what
// app_core::apply_poll_report() performs with a real report: decode_status(), status_from(), and
// ONE Event::printer_status. An earlier version of this class decoded and dispatched for itself,
// which meant the suite could pass while the product's own wiring was wrong — and it was: the two
// events it mirrored were not atomic and did not share one watermark.
class Replay {
  public:
    // Applies the step at `index` and returns the projection that resulted.
    app_model::Presentation apply(const std::size_t index) {
        const StatusStep step = status_timeline_step_at(index);

        const StatusApplication applied = apply_status_body(
            store_, step.body, at_ms(kBootMs + step.offset_ms), {kGeneration, sequence_});
        ++sequence_;

        // Every body this project ships must decode. A fixture that did not would make the whole
        // replay silently vacuous — the panel would show nothing new and every assertion about
        // absence would pass.
        EXPECT_EQ(applied.decoded, prusalink::DecodeError::None) << step.label;
        EXPECT_TRUE(applied.dispatched) << step.label;

        job_reported_ = applied.job_reported;
        return app_model::project(store_.state());
    }

    // The device is booted, configured and reachable before any status body arrives.
    //
    // ⬅️ BORROWED, NOT REWRITTEN. It is fake_source's own ColdBootToReady scenario, dispatched
    // through the same step_at() the gated flow uses for its prelude. A hand-assembled version
    // here would be a second opinion about what a booted device looks like, and the two would
    // drift — the flow would be reviewing one precondition and this suite asserting against
    // another.
    Replay() {
        const std::size_t count = fake_source::step_count(fake_source::Scenario::ColdBootToReady);

        for (std::size_t index = 0; index < count; ++index) {
            const fake_source::Step step =
                fake_source::step_at(fake_source::Scenario::ColdBootToReady, index, at_ms(0), 0);
            (void)store_.dispatch(step.event);
        }

        // ⛔ THE PRELUDE'S OWN OBSERVATIONS RUN AT GENERATION 0, and the replay below runs at
        // kGeneration. snapshot_is_current() accepts a different generation's first observation
        // outright, so the replay is not judged against the prelude's watermark — which is exactly
        // the property that makes a replacement worker's first reading usable on a real device.
        static_assert(kGeneration != 0, "the replay must not share the prelude's generation");
    }

    [[nodiscard]] const app_model::AppState& state() const {
        return store_.state();
    }

    [[nodiscard]] bool job_was_present() const {
        return job_reported_;
    }

    [[nodiscard]] app_store::Store& store() {
        return store_;
    }

    [[nodiscard]] std::uint32_t next_sequence() {
        return sequence_++;
    }

    [[nodiscard]] static app_model::Timestamp at_ms(const std::int64_t ms) {
        return std::chrono::duration_cast<app_model::Timestamp>(std::chrono::milliseconds{ms});
    }

    // Far enough past the prelude that no status body can land behind it and be clamped forward.
    static constexpr std::int64_t kBootMs = 1000;

  private:
    app_store::Store store_{};
    std::uint32_t sequence_{0};
    bool job_reported_{false};
};

// The timeline's step indices, named. A magic index in an assertion is unreadable and, worse,
// silently wrong if a step is ever inserted.
enum Step : std::size_t {
    kReady = 0,
    kHeatingEarly = 1,
    kHeatingMid = 2,
    kHeatingReady = 3,
    kPrintingStart = 4,
    kPrintingEarly = 5,
    kPrintingDecimal = 6,
    kPrintingNoEstimate = 7,
    kPrintingAdditive = 8,
    kPaused = 9,
    kResumed = 10,
    kProgressWrongType = 11,
    kProgressNonFinite = 12,
    kPrintingLate = 13,
    kFinished = 14,
    kJobRetired = 15,
    kBackToReady = 16,
};

TEST(PrintingTimelineTests, TheTimelineCoversTheWholeCycleTheCheckpointAsksFor) {
    ASSERT_EQ(status_timeline_step_count(), 17U);

    // Offsets strictly increase: the reducer clamps an event that arrives behind the last one it
    // accepted (§8.1), so a timeline that went backwards would collapse onto one instant.
    std::int64_t previous = -1;

    for (std::size_t index = 0; index < status_timeline_step_count(); ++index) {
        const StatusStep step = status_timeline_step_at(index);

        EXPECT_GT(step.offset_ms, previous) << "step " << index;
        previous = step.offset_ms;

        EXPECT_FALSE(step.label.empty()) << "step " << index;
        EXPECT_FALSE(step.body.empty()) << "step " << index;
    }

    // Out of range is reported by an empty step, which is the only way it is reported.
    const StatusStep past_the_end = status_timeline_step_at(status_timeline_step_count());
    EXPECT_TRUE(past_the_end.label.empty());
    EXPECT_TRUE(past_the_end.body.empty());
}

TEST(PrintingTimelineTests, TheProvenanceSaysItIsNotACapture) {
    // ⛔ THE ONE CLAIM THIS INSTRUMENT MUST NEVER MAKE. The string is logged at the start of every
    // bench run, so a capture of that run records what it was driven by. If the wording ever
    // softens into implying a real payload, this fails.
    const std::string_view provenance = kStatusTimelineProvenance;

    EXPECT_NE(provenance.find("synthetic"), std::string_view::npos);
    EXPECT_NE(provenance.find("NOT a capture"), std::string_view::npos);
    EXPECT_NE(provenance.find("v6.8.1"), std::string_view::npos);
}

TEST(PrintingTimelineTests, DecodedProgressAndTimeReachThePresentationAndTheProductionUi) {
    // ⛔ THE END-TO-END PROOF THIS CHECKPOINT EXISTS FOR. Before it, every stage below the second
    // arrow was correct and the panel still showed no percentage, because nothing carried one.
    Replay replay;

    const app_model::Presentation printing = replay.apply(kPrintingEarly);

    ASSERT_EQ(printing.subject, app_model::SubjectState::Printing);
    ASSERT_TRUE(printing.show_telemetry);

    // Reached the projection.
    ASSERT_TRUE(printing.progress.known);
    EXPECT_EQ(printing.progress.percent, 7U);
    ASSERT_TRUE(printing.remaining.known);
    EXPECT_EQ(printing.remaining.seconds, 3720U);
    ASSERT_TRUE(printing.elapsed.known);
    EXPECT_EQ(printing.elapsed.seconds, 300U);
    EXPECT_TRUE(printing.job_record_supplied);

    // And reached the production UI's own decision, which is the last thing before pixels.
    const ui::scene::OperationalContent content = ui::scene::resolve(printing);

    EXPECT_TRUE(content.show_progress) << "the percentage the reviewer is looking for";
    EXPECT_EQ(content.arrangement, ui::scene::Arrangement::Operational)
        << "a supplied record leads the centre stack";

    // The temperatures of the same response, which were the only thing that used to arrive.
    ASSERT_TRUE(printing.nozzle.current.known);
    EXPECT_NEAR(printing.nozzle.current.celsius, 215.4F, 0.05F);
    ASSERT_TRUE(printing.bed.current.known);
    EXPECT_NEAR(printing.bed.current.celsius, 59.8F, 0.05F);
}

TEST(PrintingTimelineTests, ProgressClimbsAcrossTheRunAndTheClampIsOnlyForDisplay) {
    Replay replay;

    struct Expected {
        std::size_t step;
        std::uint8_t percent;
    };

    const Expected climb[] = {
        {kPrintingStart, 0},       // 0 is a reading, not an absence
        {kPrintingEarly, 7},       //
        {kPrintingDecimal, 42},    // 41.6 rounds up
        {kPrintingNoEstimate, 63}, //
        {kPrintingAdditive, 78},   // with unknown members at three levels
        {kPaused, 78},             // suspended: the figure holds
        {kResumed, 79},            // and resumes from where it was
    };

    for (const Expected& one : climb) {
        const app_model::Presentation view = replay.apply(one.step);

        ASSERT_TRUE(view.progress.known) << "step " << one.step;
        EXPECT_EQ(view.progress.percent, one.percent) << "step " << one.step;
        EXPECT_TRUE(ui::scene::resolve(view).show_progress) << "step " << one.step;
    }

    // The last Printing reading is deliberately NOT at 100 % — see
    // TheTimelineItselfFinishesDirectlyWithoutAHundredPercentPrintingStep for why. "Zero seconds
    // remaining is a reading" is proved where such a value exists, in
    // PrusaLinkJobNormalizeTests.ANegativeOrUnrepresentableDurationIsRefused; fabricating a body
    // for it here would put a response in the instrument that no printer had to send.
    const app_model::Presentation late = replay.apply(kPrintingLate);
    ASSERT_TRUE(late.progress.known);
    EXPECT_EQ(late.progress.percent, 92U);
    ASSERT_TRUE(late.remaining.known);
    EXPECT_EQ(late.remaining.seconds, 330U);
    EXPECT_TRUE(ui::scene::resolve(late).show_progress);
}

TEST(PrintingTimelineTests, PausedAndResumedKeepTheSameJobRatherThanStartingANewOne) {
    // `apply_job()` treats a changed id as a different job and carries nothing over, so an id that
    // failed to decode would silently reset elapsed time on every poll. The pause is where that
    // would show first.
    Replay replay;

    (void)replay.apply(kPrintingAdditive);
    const app_model::PrintJob before = replay.state().current_job;

    const app_model::Presentation paused = replay.apply(kPaused);
    EXPECT_EQ(paused.subject, app_model::SubjectState::Paused);

    const app_model::PrintJob during = replay.state().current_job;
    EXPECT_EQ(during.id.view(), before.id.view());
    EXPECT_EQ(during.id.view(), "173");
    EXPECT_EQ(during.locally_inferred_started_at, before.locally_inferred_started_at)
        << "the same job, so its own start is not re-inferred";

    const app_model::Presentation resumed = replay.apply(kResumed);
    EXPECT_EQ(resumed.subject, app_model::SubjectState::Printing);
    EXPECT_EQ(replay.state().current_job.id.view(), "173");
}

TEST(PrintingTimelineTests, PreparingChangesTemperaturesAndSuppliesNoRecord) {
    Replay replay;

    const app_model::Presentation ready = replay.apply(kReady);
    EXPECT_EQ(ready.subject, app_model::SubjectState::Ready);
    EXPECT_FALSE(ready.progress.known);
    EXPECT_FALSE(replay.job_was_present()) << "no job object at rest";

    float previous_nozzle = ready.nozzle.current.celsius;

    for (const std::size_t step :
         {std::size_t{kHeatingEarly}, std::size_t{kHeatingMid}, std::size_t{kHeatingReady}}) {
        const app_model::Presentation view = replay.apply(step);

        EXPECT_EQ(view.subject, app_model::SubjectState::Preparing) << "step " << step;
        ASSERT_TRUE(view.nozzle.current.known) << "step " << step;
        EXPECT_GT(view.nozzle.current.celsius, previous_nozzle) << "step " << step;
        previous_nozzle = view.nozzle.current.celsius;

        ASSERT_TRUE(view.nozzle.target.known) << "step " << step;
        EXPECT_NEAR(view.nozzle.target.celsius, 215.0F, 0.05F) << "step " << step;

        // ⛔ NO RECORD, AND THE COMPOSITION SAYS SO WITHOUT INVENTING ONE. Production UI §9 gives
        // Preparing no progress, and the machine has supplied no job object to draw from.
        EXPECT_FALSE(view.progress.known) << "step " << step;
        EXPECT_FALSE(ui::scene::resolve(view).show_progress) << "step " << step;
    }
}

TEST(PrintingTimelineTests, AnAbsentEstimateLeavesTheOtherFiguresStanding) {
    Replay replay;

    (void)replay.apply(kPrintingDecimal);
    const app_model::Presentation view = replay.apply(kPrintingNoEstimate);

    ASSERT_TRUE(view.progress.known);
    EXPECT_EQ(view.progress.percent, 63U);
    ASSERT_TRUE(view.elapsed.known);
    EXPECT_EQ(view.elapsed.seconds, 2580U);

    // ⛔ NOT ZERO, AND NOT THE PREVIOUS RESPONSE'S ESTIMATE. v6.8.1 omits `time_remaining` when it
    // has none, and the panel must say nothing rather than carry a stale number forward.
    EXPECT_FALSE(view.remaining.known);
    EXPECT_EQ(view.remaining.seconds, 0U);
    EXPECT_FALSE(view.remaining_is_approximate)
        << "the endpoint states no estimate quality, so nothing may claim the figure is rough";

    EXPECT_TRUE(ui::scene::resolve(view).show_progress) << "the percentage is unaffected";
}

TEST(PrintingTimelineTests, AnUnusableFieldCostsOnlyItselfAllTheWayToTheUi) {
    Replay replay;

    (void)replay.apply(kResumed);

    for (const std::size_t step :
         {std::size_t{kProgressWrongType}, std::size_t{kProgressNonFinite}}) {
        const app_model::Presentation view = replay.apply(step);

        EXPECT_EQ(view.subject, app_model::SubjectState::Printing) << "step " << step;

        // ⛔ THE FIGURE IS WITHDRAWN, NOT FROZEN AND NOT ZEROED. §10's decay is the only thing
        // allowed to make a supplied value absent, and this is the other way a value goes away:
        // the printer sent something this build cannot read as a percentage.
        EXPECT_FALSE(view.progress.known) << "step " << step;
        EXPECT_EQ(view.progress.percent, 0U) << "step " << step;
        EXPECT_FALSE(ui::scene::resolve(view).show_progress) << "step " << step;

        // The record still exists — the id and the elapsed time decoded — so the composition does
        // not rearrange itself. UIDD 004 §10: confidence changes no layout, and neither does this.
        EXPECT_TRUE(view.elapsed.known) << "step " << step;
        EXPECT_EQ(replay.state().current_job.id.view(), "173") << "step " << step;
    }

    // The negative `time_remaining` in the second of those two is refused rather than wrapped.
    EXPECT_FALSE(replay.state().current_job.remaining.known);
}

TEST(PrintingTimelineTests, ARetiredJobObjectDoesNotImmediatelyEraseTheJob) {
    Replay replay;

    (void)replay.apply(kPrintingLate);
    const app_model::Presentation finished = replay.apply(kFinished);

    EXPECT_EQ(finished.subject, app_model::SubjectState::Completed);
    ASSERT_TRUE(finished.progress.known);
    EXPECT_EQ(finished.progress.percent, 100U);

    const app_model::Presentation retired = replay.apply(kJobRetired);

    EXPECT_FALSE(replay.job_was_present()) << "the printer stopped sending the object";

    // §7.8: "missing job data does not immediately erase a recently active job." The absence is
    // recorded so expiry can act on it later; it does not clear the record on arrival.
    EXPECT_TRUE(replay.state().current_job.present);
    EXPECT_TRUE(replay.state().current_job.reported_missing);
    EXPECT_EQ(replay.state().current_job.id.view(), "173");
    EXPECT_EQ(retired.subject, app_model::SubjectState::Completed);
}

TEST(PrintingTimelineTests, TheRunEndsBackAtReadyShowingNoFigureFromThePrintBefore) {
    Replay replay;

    for (std::size_t index = 0; index < status_timeline_step_count(); ++index) {
        (void)replay.apply(index);
    }

    const app_model::Presentation view = app_model::project(replay.state());

    EXPECT_EQ(view.subject, app_model::SubjectState::Ready);

    // ⛔ THE PANEL IS WHERE THIS IS DECIDED, AND IT SHOWS NOTHING. Production UI §9 lists no
    // progress for Ready, and the composition — not the projection — is what enforces it:
    // operational_content.cpp never sets `show_progress` for Ready or Preparing, whatever value
    // the projection is carrying.
    EXPECT_FALSE(ui::scene::resolve(view).show_progress);
    EXPECT_FALSE(ui::scene::resolve(view).show_remaining);
    EXPECT_EQ(ui::scene::resolve(view).arrangement, ui::scene::Arrangement::Resting);

    // ⬅️ AND THE PROJECTION STILL CARRIES THE FINISHED PRINT'S 100 %, WHICH IS CORRECT AND IS
    // WORTH STATING. §7.8 forbids missing job data from immediately erasing a recently active
    // job, so the two `job_unavailable` observations at the end of the run mark the record missing
    // and start expiry rather than clearing it — and the timeline is 96 seconds long, far short of
    // that window. So `progress.known` is deliberately still true here.
    //
    // It is asserted rather than left implicit because the obvious expectation is the opposite,
    // and a future reader finding the value present would otherwise reasonably think it a leak.
    // The guarantee this checkpoint owes is that the FIGURE DOES NOT REACH THE PANEL, and that is
    // the assertion above.
    EXPECT_TRUE(view.progress.known);
    EXPECT_TRUE(replay.state().current_job.present);
    EXPECT_TRUE(replay.state().current_job.reported_missing);
}

TEST(PrintingTimelineTests, EveryOutOfRangeProgressIsCountedRatherThanCorrected) {
    // The whole run must produce no out-of-range counts, because none of the bodies carries one.
    // A count here would mean a conversion had gone wrong somewhere between percent and byte —
    // the class of defect that a fraction-versus-percent mix-up produces.
    Replay replay;

    for (std::size_t index = 0; index < status_timeline_step_count(); ++index) {
        (void)replay.apply(index);
    }

    EXPECT_EQ(replay.state().diagnostics.out_of_range_progress_seen, 0U);

    // And nothing was rejected as stale: one generation, strictly increasing sequences, both
    // halves of every response ordered together.
    EXPECT_EQ(replay.state().diagnostics.events_rejected_stale, 0U);
}

TEST(PrintingTimelineTests, AReplacementWorkerIsNotRejectedAsStale) {
    // ⛔ THE REGRESSION THE JOB GENERATION EXISTS FOR. A new worker's sequence restarts at zero, so
    // a bare-sequence watermark would discard every job update until it climbed past the previous
    // worker's — silently, for as many cycles as that worker had run. Now one status response
    // carries one order for both halves, so there is one watermark and one answer.
    Replay replay;
    for (std::size_t index = 0; index <= kResumed; ++index) {
        (void)replay.apply(index);
    }

    ASSERT_TRUE(replay.state().current_job.present);

    // ⬅️ THE READING'S WATERMARK IS THE PRINTER ONE, and `last_job_sequence` stays 0 throughout —
    // it belongs to the standalone general job event, which only fixtures publish. That is the
    // point of the compact payload: one reading, one ordering identity, no second number to keep
    // in step.
    ASSERT_GT(replay.state().last_printer_sequence, 1U)
        << "the watermark has to be somewhere for this to be a test";
    ASSERT_EQ(replay.state().last_job_sequence, 0U)
        << "the status path does not touch the general job watermark";
    const std::uint32_t before = replay.state().diagnostics.events_rejected_stale;

    // A replacement worker, starting its own sequence at 0 again — through the same seam.
    const StatusApplication fresh =
        apply_status_body(replay.store(), status_timeline_step_at(kPrintingEarly).body,
                          Replay::at_ms(500000), {kGeneration + 1, 0});

    ASSERT_TRUE(fresh.dispatched);
    EXPECT_EQ(replay.state().diagnostics.events_rejected_stale, before)
        << "a new generation's first reading is accepted whatever the old watermark reached";

    // ⛔ ONE WATERMARK, AND ONLY ONE. A status reading is ordered by the printer half's
    // `{generation, sequence}` — the compact job payload deliberately carries no second copy — so
    // there is no pair of numbers that can diverge.
    EXPECT_EQ(replay.state().last_printer_generation, kGeneration + 1);
    EXPECT_EQ(replay.state().last_printer_sequence, 0U);
    EXPECT_TRUE(replay.state().current_job.present)
        << "and the job half of that first reading was applied, not discarded";
    EXPECT_EQ(replay.state().current_job.progress.percent, 7U);
}

// --- The direct finish: PRINTING 79% → FINISHED 100%, with nothing in between ---

TEST(PrintingTimelineTests, ADirectFinishRecordsTheSameResponsesProgressAndIdentity) {
    // ⛔ THE PRODUCT DEFECT THIS SEAM WAS BUILT TO FIX, AS ITS OWN REGRESSION.
    //
    // persist_completion() reads `state.current_job`, so while the printer half and the job half of
    // one response were two separate store mutations, a FINISHED response's completion snapshot was
    // taken BEFORE that response's own 100 % had been applied — and recorded the progress of the
    // reading before it. 79 %, on a print that finished.
    //
    // ⛔ NO INTERVENING PRINTING 100% RESPONSE. That is the whole point: an earlier timeline sent
    // one, which made the wrong answer and the right answer the same number and hid this entirely.
    // A printer does not promise to report every percentage.
    Replay replay;

    constexpr std::string_view kPrinting79 =
        R"({"printer":{"state":"PRINTING","temp_nozzle":215.0,"target_nozzle":215,)"
        R"("temp_bed":60.0,"target_bed":60},)"
        R"("job":{"id":4021,"progress":79,"time_remaining":860,"time_printing":3180}})";

    constexpr std::string_view kFinished100 =
        R"({"printer":{"state":"FINISHED","temp_nozzle":198.4,"target_nozzle":0,)"
        R"("temp_bed":58.2,"target_bed":0},)"
        R"("job":{"id":4021,"progress":100,"time_printing":4020}})";

    const StatusApplication printing = apply_status_body(
        replay.store(), kPrinting79, Replay::at_ms(Replay::kBootMs + 1000), {kGeneration, 0});
    ASSERT_TRUE(printing.dispatched);

    ASSERT_EQ(app_model::project(replay.state()).subject, app_model::SubjectState::Printing);
    ASSERT_EQ(replay.state().current_job.progress.percent, 79U);
    ASSERT_FALSE(replay.state().last_completed_job.present)
        << "nothing has finished yet, so nothing may be recorded as completed";

    const StatusApplication finished = apply_status_body(
        replay.store(), kFinished100, Replay::at_ms(Replay::kBootMs + 2000), {kGeneration, 1});
    ASSERT_TRUE(finished.dispatched);

    const app_model::CompletedJobSnapshot& completed = replay.state().last_completed_job;

    ASSERT_TRUE(completed.present);
    EXPECT_EQ(completed.terminal_state, app_model::PrinterState::Finished);

    // ⛔ 100, NOT 79. The completion is recorded from the response that reported it.
    ASSERT_TRUE(completed.progress.known);
    EXPECT_EQ(completed.progress.percent, 100U)
        << "the completion snapshot must come from the FINISHED response, not the one before it";

    // ⛔ AND THE SAME RESPONSE'S JOB IDENTITY.
    EXPECT_EQ(completed.id.view(), "4021");
    EXPECT_EQ(completed.id.view(), replay.state().current_job.id.view());

    // And the panel agrees.
    const app_model::Presentation view = app_model::project(replay.state());
    EXPECT_EQ(view.subject, app_model::SubjectState::Completed);
    ASSERT_TRUE(view.progress.known);
    EXPECT_EQ(view.progress.percent, 100U);
    EXPECT_TRUE(ui::scene::resolve(view).show_progress);
}

TEST(PrintingTimelineTests, TheTimelineItselfFinishesDirectlyWithoutAHundredPercentPrintingStep) {
    // ⛔ THE INSTRUMENT MUST NOT MASK THE DEFECT EITHER. If a later edit reinserts a PRINTING 100%
    // response before FINISHED, a bench run would look correct however the seam behaved. Asserted
    // on the fixture bodies, so the masking cannot come back quietly.
    Replay replay;

    for (std::size_t index = 0; index <= kFinished; ++index) {
        (void)replay.apply(index);
    }

    const app_model::CompletedJobSnapshot& completed = replay.state().last_completed_job;
    ASSERT_TRUE(completed.present);
    EXPECT_EQ(completed.progress.percent, 100U);
    EXPECT_EQ(completed.id.view(), "173");

    // The response immediately before FINISHED reported 92 %, not 100 %.
    prusalink::StatusDto before;
    ASSERT_EQ(prusalink::decode_status(status_timeline_step_at(kPrintingLate).body, before),
              prusalink::DecodeError::None);
    ASSERT_TRUE(before.job_present);
    ASSERT_TRUE(before.progress_known);
    EXPECT_LT(before.progress_percent, 100.0)
        << "the step before the finish must not already be at 100 %, or this proves nothing";
    EXPECT_EQ(before.printer_state.view(), "PRINTING");
}

TEST(PrintingTimelineTests, OneResponseProducesOneMutationAndNoIntermediatePresentation) {
    // ⛔ TWO EVENTS WERE TWO PROJECTIONS. A printer event followed by a job event let the panel
    // show a composition no response described — the new state word beside the previous response's
    // figures. One event means the store's revision moves exactly once per reading, so no such
    // frame exists to be drawn.
    Replay replay;
    (void)replay.apply(kPrintingEarly);

    const std::uint32_t before = replay.store().revision();

    const StatusApplication applied =
        apply_status_body(replay.store(), status_timeline_step_at(kPaused).body,
                          Replay::at_ms(900000), {kGeneration, 900});

    ASSERT_TRUE(applied.dispatched);
    ASSERT_TRUE(applied.mutated);
    EXPECT_EQ(replay.store().revision(), before + 1)
        << "one status response must move the store's revision exactly once";
}

// --- Optional job identity: absence is unknown, not an empty changed ID --------
//
// ⛔ §6.1 MAKES EVERY MEMBER OF THE `job` OBJECT OPTIONAL, `id` INCLUDED. Comparing an absent id
// against the stored one as though it were empty made every ID-less poll look like a NEW job: the
// record was reset each cycle, the locally inferred start and elapsed counter were lost, any
// `/api/v1/job`-owned metadata was wiped, and a terminal response persisted a completion with no
// identity. `StatusJobTelemetry::id_known` is the fact that tells the three cases apart.

// A status body with whichever job members are wanted. Bodies rather than hand-built DTOs, so
// these regressions run through the production decoder like everything else here.
[[nodiscard]] std::string printing_body(const std::string& job_members) {
    return R"({"printer":{"state":"PRINTING","temp_nozzle":215.0,"target_nozzle":215,)"
           R"("temp_bed":60.0,"target_bed":60},"job":{)" +
           job_members + R"(}})";
}

[[nodiscard]] std::string finished_body(const std::string& job_members) {
    return R"({"printer":{"state":"FINISHED","temp_nozzle":198.0,"target_nozzle":0,)"
           R"("temp_bed":58.0,"target_bed":0},"job":{)" +
           job_members + R"(}})";
}

TEST(PrintingTimelineTests, AKnownMatchingIdKeepsTheJobAndUpdatesTheReportedFields) {
    Replay replay;

    ASSERT_TRUE(apply_status_body(replay.store(),
                                  printing_body(R"("id":501,"progress":20,)"
                                                R"("time_printing":100)"),
                                  Replay::at_ms(2000), {kGeneration, 0})
                    .dispatched);
    const app_model::Timestamp started = replay.state().current_job.locally_inferred_started_at;

    ASSERT_TRUE(apply_status_body(replay.store(),
                                  printing_body(R"("id":501,"progress":45,)"
                                                R"("time_printing":400)"),
                                  Replay::at_ms(3000), {kGeneration, 1})
                    .dispatched);

    EXPECT_EQ(replay.state().current_job.id.view(), "501");
    EXPECT_EQ(replay.state().current_job.progress.percent, 45U);
    EXPECT_EQ(replay.state().current_job.elapsed.seconds, 400U);
    EXPECT_EQ(replay.state().current_job.locally_inferred_started_at, started)
        << "the same job, so its own start is not re-inferred";
}

TEST(PrintingTimelineTests, AKnownDifferentIdResetsTheRecordBeforeApplyingTheNewJob) {
    Replay replay;

    ASSERT_TRUE(apply_status_body(replay.store(),
                                  printing_body(R"("id":501,"progress":90,)"
                                                R"("time_remaining":60,)"
                                                R"("time_printing":3000)"),
                                  Replay::at_ms(2000), {kGeneration, 0})
                    .dispatched);
    const app_model::Timestamp started = replay.state().current_job.locally_inferred_started_at;

    // A different print. Nothing from the previous one may appear against it.
    ASSERT_TRUE(apply_status_body(replay.store(), printing_body(R"("id":502,"progress":3)"),
                                  Replay::at_ms(9000), {kGeneration, 1})
                    .dispatched);

    EXPECT_EQ(replay.state().current_job.id.view(), "502");
    EXPECT_EQ(replay.state().current_job.progress.percent, 3U);
    EXPECT_FALSE(replay.state().current_job.remaining.known)
        << "the previous job's remaining estimate must not carry over";
    EXPECT_FALSE(replay.state().current_job.elapsed.known) << "nor its elapsed counter";
    EXPECT_NE(replay.state().current_job.locally_inferred_started_at, started)
        << "a different job's start is inferred afresh";
}

TEST(PrintingTimelineTests, AnAbsentIdPreservesTheExistingIdentityAndItsMetadata) {
    // ⛔ THE DEFECT'S OWN REGRESSION. The record must survive an ID-less poll intact, including the
    // fields only `/api/v1/job` can supply — this endpoint says nothing about them, so it may not
    // clear them.
    Replay replay;

    ASSERT_TRUE(apply_status_body(replay.store(),
                                  printing_body(R"("id":501,"progress":20,)"
                                                R"("time_printing":100)"),
                                  Replay::at_ms(2000), {kGeneration, 0})
                    .dispatched);

    // Metadata a general job record would have brought, which the status path must not touch.
    app_model::AppState seeded = replay.state();
    (void)seeded.current_job.display_name.assign("bracket.gcode");
    (void)seeded.current_job.source_path.assign("/usb/bracket.gcode");
    seeded.current_job.layers.known = true;
    seeded.current_job.layers.current = 12;
    seeded.current_job.layers.total = 240;
    const app_model::Timestamp started = seeded.current_job.locally_inferred_started_at;

    // The same printer, still printing, now reporting no id at all.
    prusalink::StatusDto dto;
    ASSERT_EQ(prusalink::decode_status(printing_body(R"("progress":31,"time_printing":600)"), dto),
              prusalink::DecodeError::None);
    ASSERT_FALSE(dto.job_id_known) << "the body under test must really omit the id";

    const app_model::AppState after = app_model::reduce(
        seeded, app_model::Event::printer_status(Replay::at_ms(4000),
                                                 prusalink::status_from(dto, {kGeneration, 1})));

    EXPECT_TRUE(after.current_job.present);
    EXPECT_EQ(after.current_job.id.view(), "501") << "absence is unknown, not a change of identity";
    EXPECT_EQ(after.current_job.locally_inferred_started_at, started)
        << "the record was not reset, so its start survived";

    // What the response DID report is applied.
    EXPECT_EQ(after.current_job.progress.percent, 31U);
    EXPECT_EQ(after.current_job.elapsed.seconds, 600U);

    // What only /api/v1/job supplies is untouched.
    EXPECT_EQ(after.current_job.display_name.view(), "bracket.gcode");
    EXPECT_EQ(after.current_job.source_path.view(), "/usb/bracket.gcode");
    EXPECT_TRUE(after.current_job.layers.known);
    EXPECT_EQ(after.current_job.layers.current, 12U);
}

TEST(PrintingTimelineTests, AnAbsentIdWithNoCurrentJobCreatesAnAnonymousOne) {
    // Truthful: this printer reported a job and did not name it. The values it did report are
    // applied, and the identity stays empty rather than being invented.
    Replay replay;

    ASSERT_TRUE(apply_status_body(replay.store(),
                                  printing_body(R"("progress":8,"time_remaining":1200,)"
                                                R"("time_printing":90)"),
                                  Replay::at_ms(2000), {kGeneration, 0})
                    .dispatched);

    EXPECT_TRUE(replay.state().current_job.present);
    EXPECT_TRUE(replay.state().current_job.id.view().empty());
    EXPECT_EQ(replay.state().current_job.progress.percent, 8U);
    EXPECT_EQ(replay.state().current_job.remaining.seconds, 1200U);
    EXPECT_EQ(replay.state().current_job.elapsed.seconds, 90U);

    const app_model::Presentation view = app_model::project(replay.state());
    EXPECT_EQ(view.subject, app_model::SubjectState::Printing);
    EXPECT_TRUE(ui::scene::resolve(view).show_progress)
        << "an anonymous job still has a figure to draw";
}

TEST(PrintingTimelineTests, ADirectFinishWithNoIdKeepsTheKnownIdentityAndTakesItsProgress) {
    // ⛔ THE TWO DEFECTS TOGETHER, WHICH IS THE CASE THAT WOULD HAVE REACHED HARDWARE. A terminal
    // response that omits the id must still record THIS response's progress — the atomic-ordering
    // fix — against the identity the device already knew — the identity fix. Getting either wrong
    // produces a completed job that is wrong about what finished, or about how far it got.
    Replay replay;

    ASSERT_TRUE(apply_status_body(replay.store(),
                                  printing_body(R"("id":773,"progress":92,)"
                                                R"("time_printing":3690)"),
                                  Replay::at_ms(2000), {kGeneration, 0})
                    .dispatched);
    ASSERT_EQ(replay.state().current_job.id.view(), "773");
    ASSERT_FALSE(replay.state().last_completed_job.present);

    // FINISHED, 100 %, and no id at all.
    ASSERT_TRUE(apply_status_body(replay.store(),
                                  finished_body(R"("progress":100,"time_printing":4020)"),
                                  Replay::at_ms(3000), {kGeneration, 1})
                    .dispatched);

    const app_model::CompletedJobSnapshot& completed = replay.state().last_completed_job;

    ASSERT_TRUE(completed.present);
    EXPECT_EQ(completed.terminal_state, app_model::PrinterState::Finished);
    ASSERT_TRUE(completed.progress.known);
    EXPECT_EQ(completed.progress.percent, 100U) << "this response's progress, not the one before";
    EXPECT_EQ(completed.id.view(), "773")
        << "and the identity the device knew, not an empty one the response never asserted";

    const app_model::Presentation view = app_model::project(replay.state());
    EXPECT_EQ(view.subject, app_model::SubjectState::Completed);
    EXPECT_EQ(view.progress.percent, 100U);
}

TEST(PrintingTimelineTests, TheDecoderReportsWhetherAnIdWasThereAtAll) {
    // The fact the reducer depends on, at its source.
    struct Case {
        const char* members;
        bool known;
        const char* id;
    };

    const Case cases[] = {
        {R"("id":501,"progress":5)", true, "501"},
        {R"("id":"abc","progress":5)", true, "abc"},
        {R"("progress":5)", false, ""},
        {R"("id":null,"progress":5)", false, ""},
        {R"("id":17.5,"progress":5)", false, ""},
    };

    for (const Case& one : cases) {
        prusalink::StatusDto dto;
        ASSERT_EQ(prusalink::decode_status(printing_body(one.members), dto),
                  prusalink::DecodeError::None)
            << one.members;

        EXPECT_EQ(dto.job_id_known, one.known) << one.members;

        const app_model::StatusObservation reading = prusalink::status_from(dto, {kGeneration, 0});
        EXPECT_EQ(reading.job.id_known, one.known) << one.members;

        if (one.known) {
            EXPECT_EQ(reading.job.id.view(), one.id) << one.members;
        }
    }
}

// --- Startup-heap budgets ----------------------------------------------------

TEST(PrintingTimelineIsolationTests, ThePrintingTelemetryPayloadCostsTheEventQueueNothing) {
    // ⛔ THE COST THIS CHECKPOINT ALMOST SHIPPED. The first version embedded the general 348-byte
    // JobObservation in the status payload — ~224 bytes of name, file name and source path this
    // endpoint never fills, plus a duplicate of the printer half's ordering and state. It took
    // `sizeof(Event)` from 360 to 464 and spent 1248 bytes of STARTUP HEAP across the twelve-slot
    // application event queue — xQueueCreate() allocates through pvPortMalloc, so the cost is
    // FreeRTOS heap taken at startup and not `.bss`, out of the very pool the device's measured
    // internal-heap low-water of ~916 bytes describes. It compiled, it passed, and the loose
    // 512-byte budget did not notice.
    //
    // ⬅️ PRINTING TELEMETRY IS PRODUCT FUNCTIONALITY and is present in debug and release. What
    // this asserts is that it arrived at NO cost to the queue: the endpoint-specific payload fits
    // inside the union's existing high-water mark, which the general JobObservation sets.
    constexpr std::size_t kEventQueueSlots = 12; // app_core::kEventQueueLength
    constexpr std::size_t kPreCheckpointEvent = 360;

    EXPECT_LE(sizeof(app_model::Event), kPreCheckpointEvent)
        << "Event grew, so the event queue's startup heap allocation grew by "
        << (sizeof(app_model::Event) - kPreCheckpointEvent) * kEventQueueSlots << " bytes";

    // The general record still sets the union's size, which is why the compact one is free.
    EXPECT_GE(sizeof(app_model::JobObservation), sizeof(app_model::StatusObservation))
        << "the status payload has become the union's largest member, so it now costs queue slots";

    // And it is compact for the stated reason, not by accident.
    EXPECT_LT(sizeof(app_model::StatusJobTelemetry), std::size_t{64});
    EXPECT_LE(sizeof(app_model::StatusObservation), std::size_t{160});
}

TEST(PrintingTimelineIsolationTests, TheCompactPayloadCarriesOnlyWhatTheEndpointSupplies) {
    // A structural inventory: three values, an identity and TWO FLAGS — `reported`, which says
    // whether a job object arrived at all, and `id_known`, which says whether it named the job.
    // Asserted by size rather than by listing members, because a member added later would pass a
    // list that was written before it.
    //
    // ⬅️ THE SECOND FLAG WAS MISSING FROM THIS SUM AND THE `+ 8` SLACK HID IT. A tolerance wide
    // enough to absorb a member nobody counted is a tolerance that fails only after two are added,
    // so `id_known` is now summed explicitly and the slack is what it was meant to be: an
    // allowance for alignment padding, not for forgotten fields.
    constexpr std::size_t kThreeValuesAnIdentityAndTwoFlags =
        sizeof(app_model::DurationValue) * 2 + sizeof(app_model::ProgressValue) +
        sizeof(app_model::BoundedString<24>) + sizeof(bool) * 2;

    EXPECT_LE(sizeof(app_model::StatusJobTelemetry), kThreeValuesAnIdentityAndTwoFlags + 8)
        << "something beyond remaining, elapsed, progress, id, reported and id_known has been "
           "added to the status payload";

    // The reading's ordering and state are on the printer half and are not duplicated.
    EXPECT_EQ(sizeof(app_model::StatusObservation),
              sizeof(app_model::PrinterSnapshotObservation) + sizeof(app_model::StatusJobTelemetry))
        << "the compound observation is exactly its two halves, with no padding between them and "
           "nothing else in it";
}

// --- The real printer worker is structurally excluded --------------------------

// Everything a polling worker needs, so the only thing under test is the diagnostic-source term.
[[nodiscard]] app_core::exclusive::Conditions fully_eligible() {
    app_core::exclusive::Conditions conditions;
    conditions.provisioning = app_model::ProvisioningPhase::Inactive;
    conditions.network = app_model::NetworkPhase::Connected;
    conditions.has_printer_configuration = true;
    return conditions;
}

TEST(PrintingTimelineIsolationTests, ARealWorkerCannotLaunchWhileASyntheticSourceOwnsThePrinter) {
    // ⛔ THE HAZARD THIS CLOSES. The instrument's prelude makes the network and the printer
    // configuration eligible — it has to, or no operational composition is reachable — and the
    // ordinary application loop then reaches printer_launch() and would start a REAL worker against
    // the board's persisted printer pair. Real readings would race the synthetic ones.
    //
    // ⛔ AND IT MUST NOT REST ON THE BENCH. Not on the printer being switched off, not on DNS
    // failing, not on a development configuration being empty: each of those is a property of one
    // afternoon, and each has already cost this project a discarded validation run.
    app_core::exclusive::Conditions conditions = fully_eligible();

    ASSERT_TRUE(app_core::exclusive::printer_worker_permitted(conditions))
        << "the control: with no diagnostic source, this configuration is exactly the one that "
           "launches a worker — otherwise the assertion below proves nothing";
    ASSERT_TRUE(app_core::exclusive::printer_reports_permitted(conditions));
    ASSERT_TRUE(app_core::exclusive::real_printer_source_permitted(conditions));

    conditions.synthetic_status_source = true;

    EXPECT_FALSE(app_core::exclusive::real_printer_source_permitted(conditions));
    EXPECT_FALSE(app_core::exclusive::printer_worker_permitted(conditions))
        << "no worker may be launched while a diagnostic status source drives the store";

    // ⛔ AND INGRESS TOO, so a worker that somehow already existed still contributes nothing.
    EXPECT_FALSE(app_core::exclusive::printer_reports_permitted(conditions))
        << "a report from a worker that predates the instrument must not reach the store";
}

TEST(PrintingTimelineIsolationTests, TheBarrierHoldsWhateverElseIsTrue) {
    // Swept over the other terms, because a barrier that only holds in the quiet case is not a
    // barrier. Every combination below is one a real device can be in.
    for (const bool provisioning : {false, true}) {
        for (const bool network : {false, true}) {
            for (const bool configured : {false, true}) {
                app_core::exclusive::Conditions conditions;
                conditions.provisioning = provisioning ? app_model::ProvisioningPhase::SetupActive
                                                       : app_model::ProvisioningPhase::Inactive;
                conditions.network = network ? app_model::NetworkPhase::Connected
                                             : app_model::NetworkPhase::Disabled;
                conditions.has_printer_configuration = configured;
                conditions.synthetic_status_source = true;

                EXPECT_FALSE(app_core::exclusive::printer_worker_permitted(conditions));
                EXPECT_FALSE(app_core::exclusive::printer_reports_permitted(conditions));
            }
        }
    }
}

// --- Diagnostic controls have no product meaning -------------------------------

TEST(PrintingTimelineIsolationTests, EveryTimelineGestureIsConsumedAndNoneReachesTheStore) {
    constexpr app_model::ButtonGesture kAll[] = {
        app_model::ButtonGesture::ShortPress,
        app_model::ButtonGesture::LongPress,
        app_model::ButtonGesture::VeryLongPress,
    };

    for (const app_model::ButtonGesture gesture : kAll) {
        // With the instrument compiled in: consumed, so it is never posted as a product fact.
        EXPECT_EQ(disposition_of(gesture, true), GestureDisposition::DiagnosticOnly)
            << "gesture " << static_cast<int>(gesture);

        // Without it: unchanged. The scenario sweep and the operational walk depend on this —
        // their Kconfig help states that a press keeps its full product meaning there.
        EXPECT_EQ(disposition_of(gesture, false), GestureDisposition::ProductFact)
            << "gesture " << static_cast<int>(gesture);
    }
}

TEST(PrintingTimelineIsolationTests, ThoseGesturesWouldOtherwiseHaveRealProductEffects) {
    // ⛔ THE CONTROL THAT MAKES THE TEST ABOVE MEAN SOMETHING. If a gesture had no product effect,
    // consuming it would prove nothing — so this shows that each one does, by letting it reach the
    // reducer exactly as app_core would if the policy said ProductFact.
    Replay replay;
    (void)replay.apply(kPrintingEarly);

    app_model::AppState lit = replay.state();
    lit.display.power = app_model::DisplayPower::Dimmed;

    const app_model::AppState after =
        app_model::reduce(lit, app_model::Event::button_gesture(
                                   Replay::at_ms(200000), app_model::ButtonGesture::ShortPress));

    EXPECT_NE(after.display.last_interaction_at, lit.display.last_interaction_at)
        << "a gesture that reaches the store is an interaction and moves display state";
    EXPECT_NE(after.display.power, app_model::DisplayPower::Dimmed)
        << "so a stray press during a brightness review would not have been harmless";
}

TEST(PrintingTimelineIsolationTests, NoTimelineGestureCanSleepThePanelOpenSetupOrArmAReset) {
    // The end-to-end statement: drive the whole timeline and, at every step, offer all three
    // gestures the way app_core does — posting only what the policy does NOT consume. Nothing is
    // posted, so no product state moves.
    //
    // app_core's own branch is the single `if (!validation::gesture_is_consumed(...))` around its
    // post_event(); the policy it asks is the one called here.
    Replay replay;

    const app_model::DisplayPower power_before = replay.state().display.power;
    const app_model::ModalSurface surface_before = replay.state().modal_surface;

    for (std::size_t index = 0; index < status_timeline_step_count(); ++index) {
        (void)replay.apply(index);

        for (const app_model::ButtonGesture gesture :
             {app_model::ButtonGesture::ShortPress, app_model::ButtonGesture::LongPress,
              app_model::ButtonGesture::VeryLongPress}) {
            if (disposition_of(gesture, true) == GestureDisposition::ProductFact) {
                (void)replay.store().dispatch(app_model::Event::button_gesture(
                    Replay::at_ms(Replay::kBootMs + 300000 + static_cast<std::int64_t>(index)),
                    gesture));
            }
        }
    }

    const app_model::AppState& state = replay.state();

    // ⛔ NOTHING DESTRUCTIVE WAS EVEN APPROACHED.
    EXPECT_FALSE(state.factory_resetting);
    EXPECT_FALSE(state.factory_reset_failed);
    EXPECT_NE(state.modal_surface, app_model::ModalSurface::FactoryResetConfirmation)
        << "a very-long bench gesture must never reach the arming path";

    // No setup surface, no reconfiguration.
    EXPECT_EQ(state.provisioning, app_model::ProvisioningPhase::Inactive);
    EXPECT_EQ(state.modal_surface, surface_before);

    // And the display policy was untouched by the controls: whatever power the ordinary policy
    // chose for a live print, no press changed it.
    EXPECT_EQ(state.display.power,
              power_before == app_model::DisplayPower::Off ? state.display.power : power_before)
        << "no bench press may sleep or wake the panel";
    EXPECT_EQ(state.display.last_interaction_at, app_model::Timestamp::zero())
        << "no gesture ever reached the store, so nothing was recorded as an interaction";
}

// --- Isolation from ordinary product builds ---------------------------------

[[nodiscard]] std::string read_file(const std::string& path) {
    std::ifstream file{path};
    std::ostringstream text;
    text << file.rdbuf();
    return text.str();
}

TEST(PrintingTimelineIsolationTests, NoProductProfileEnablesTheSimulator) {
    // ⛔ THE ONE PROPERTY THAT CANNOT BE CHECKED AT RUNTIME. The flow compiles to empty stubs
    // unless its Kconfig symbol is set, so nothing in a product image can reach the fixtures — but
    // that guarantee is only as good as the profile defaults, and a single `=y` added to the wrong
    // file would ship a synthetic print in a release. This is that guard.
    const std::string firmware{PRD_FIRMWARE_DIR};

    const char* const profiles[] = {
        "/sdkconfig.defaults",
        "/sdkconfig.defaults.debug",
        "/sdkconfig.defaults.release",
        "/sdkconfig.defaults.waveshare_esp32_s3_lcd_2_8",
    };

    for (const char* const profile : profiles) {
        const std::string text = read_file(firmware + profile);

        ASSERT_FALSE(text.empty()) << profile << " could not be read";
        EXPECT_EQ(text.find("CONFIG_PRD_VALIDATION_PRINTING_TIMELINE=y"), std::string::npos)
            << profile << " enables the simulator in a product image";
    }
}

TEST(PrintingTimelineIsolationTests, TheKconfigSymbolDefaultsOff) {
    // A profile that names no value gets the Kconfig default, so the default is the real gate.
    const std::string kconfig =
        read_file(std::string{PRD_FIRMWARE_DIR} + "/components/validation/Kconfig");

    ASSERT_FALSE(kconfig.empty());

    const std::size_t symbol = kconfig.find("config PRD_VALIDATION_PRINTING_TIMELINE");
    ASSERT_NE(symbol, std::string::npos);

    // `default n` must appear before the next config block starts.
    const std::size_t next = kconfig.find("    config ", symbol + 1);
    const std::string block =
        kconfig.substr(symbol, next == std::string::npos ? std::string::npos : next - symbol);

    EXPECT_NE(block.find("default n"), std::string::npos);
    EXPECT_NE(block.find("hardware-validation"), std::string::npos)
        << "the help must say where it is allowed to be enabled";
}

} // namespace
} // namespace prd::validation
