// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include <cstdint>
#include <limits>

#include <gtest/gtest.h>

#include "app_model/reducer.hpp"
#include "support/state_builders.hpp"

// Snapshot ordering across worker restarts — the audit's B2.
//
// ⛔ THE DEFECT THIS CLOSES. §8.1 rejects a snapshot whose sequence does not exceed the last
// accepted one, which is right for duplicates and late arrivals from ONE producer. But each worker
// counted from 1, and the watermark persisted for the whole boot — so after any reconnect the
// replacement worker's snapshots lost to the previous worker's high-water mark and were silently
// discarded into `events_rejected_stale`. An hour of polling at five seconds leaves a watermark of
// about 720, so the panel would show nothing new for roughly another hour.
//
// ⬅️ THE FIX IS A GENERATION, AND THE DIVISION OF LABOUR MATTERS. Ordering within a producer stays
// §8.1's rule. Across producers the ordering RESETS, because a different generation is a different
// counter and comparing the two is meaningless. Whether the producing worker is still the live one is
// a separate question that the reducer cannot answer and does not try to: the application task
// compares each report against the session it armed and translates nothing from a report that fails
// (app_core/src/printer_session.hpp). Equality upstream, change-detection here.

namespace prd::app_model {
namespace {

TEST(SnapshotOrderingTests, WithinOneGenerationTheSequenceMustStrictlyIncrease) {
    // §8.1, unchanged. A duplicate and a late arrival are both harmless.
    EXPECT_TRUE(snapshot_is_current({5, 2}, {5, 1}));
    EXPECT_FALSE(snapshot_is_current({5, 1}, {5, 1}));
    EXPECT_FALSE(snapshot_is_current({5, 1}, {5, 9}));

    // Zero remains the escape hatch for a producer that does not sequence at all.
    EXPECT_TRUE(snapshot_is_current({5, 0}, {5, 9}));
}

TEST(SnapshotOrderingTests, AREPLACEMENT_WORKERS_FIRST_SNAPSHOT_IS_ACCEPTED_IMMEDIATELY) {
    // ⬅️ THE REGRESSION, STATED AT THE SIZE IT ACTUALLY OCCURS. A worker that ran for an hour leaves a
    // watermark near 720. Its replacement starts at 1 and must be believed at once.
    EXPECT_TRUE(snapshot_is_current({6, 1}, {5, 720}));
    EXPECT_TRUE(snapshot_is_current({6, 1}, {5, std::numeric_limits<std::uint32_t>::max() - 1}));

    // NEGATIVE CONTROL: the same sequence in the SAME generation is still rejected, so the acceptance
    // above is the generation change and not a rule that lets everything through.
    EXPECT_FALSE(snapshot_is_current({5, 1}, {5, 720}));
}

TEST(SnapshotOrderingTests, GenerationsAreCOMPARED_FOR_CHANGE_AND_NEVER_FOR_MAGNITUDE) {
    // Deliberate: magnitude would reintroduce the watermark problem the moment a generation wrapped.
    // A lower generation is therefore "different" and accepted HERE — and rejected upstream, where
    // the live generation is known by equality. Encoding wrap-safe magnitude in the reducer would put
    // liveness knowledge in a pure function that has no business holding it.
    EXPECT_TRUE(snapshot_is_current({1, 1}, {4096, 500}));
    EXPECT_TRUE(snapshot_is_current({1, 1}, {std::numeric_limits<std::uint32_t>::max(), 500}));
}

TEST(SnapshotOrderingTests, TheStoreAcceptsAReplacementWorkerAfterAHighWatermark) {
    // End to end through the real reducer, which is where the defect actually bit.
    AppState state = test::booted_device(test::at_ms(0));

    // Generation 5 runs for a long time.
    for (std::uint32_t sequence = 1; sequence <= 40; ++sequence) {
        PrinterSnapshotObservation observation =
            test::printer_snapshot(PrinterState::Printing, sequence);
        observation.generation = 5;
        state = reduce(state, Event::printer_snapshot(test::at_ms(1000 + sequence), observation));
    }

    ASSERT_EQ(state.last_printer_generation, 5U);
    ASSERT_EQ(state.last_printer_sequence, 40U);

    const std::uint32_t rejected_before = state.diagnostics.events_rejected_stale;

    // The network drops and a replacement worker starts, counting from one again.
    PrinterSnapshotObservation replacement = test::printer_snapshot(PrinterState::Idle, 1);
    replacement.generation = 6;
    state = reduce(state, Event::printer_snapshot(test::at_ms(9000), replacement));

    // Accepted at once, and nothing was counted as stale.
    EXPECT_EQ(state.printer.normalized, PrinterState::Idle);
    EXPECT_EQ(state.last_printer_generation, 6U);
    EXPECT_EQ(state.last_printer_sequence, 1U);
    EXPECT_EQ(state.diagnostics.events_rejected_stale, rejected_before);
}

TEST(SnapshotOrderingTests, ADelayedSnapshotFromTheSAME_GENERATION_IS_STILL_REJECTED) {
    AppState state = test::booted_device(test::at_ms(0));

    PrinterSnapshotObservation third = test::printer_snapshot(PrinterState::Printing, 3);
    third.generation = 6;
    state = reduce(state, Event::printer_snapshot(test::at_ms(1000), third));
    ASSERT_EQ(state.printer.normalized, PrinterState::Printing);

    const std::uint32_t rejected_before = state.diagnostics.events_rejected_stale;

    // A response that took longer than the two after it, from the same worker.
    PrinterSnapshotObservation second = test::printer_snapshot(PrinterState::Idle, 2);
    second.generation = 6;
    state = reduce(state, Event::printer_snapshot(test::at_ms(2000), second));

    EXPECT_EQ(state.printer.normalized, PrinterState::Printing);
    EXPECT_EQ(state.diagnostics.events_rejected_stale, rejected_before + 1);
}

TEST(SnapshotOrderingTests, TheWatermarkAndItsGenerationBothResetOnBoot) {
    // Both must clear together. A generation surviving a reset with a cleared sequence — or the
    // reverse — would make the first snapshot of the next run ambiguous.
    AppState state = test::booted_device(test::at_ms(0));

    PrinterSnapshotObservation observation = test::printer_snapshot(PrinterState::Printing, 9);
    observation.generation = 3;
    state = reduce(state, Event::printer_snapshot(test::at_ms(1000), observation));

    ASSERT_NE(state.last_printer_generation, 0U);
    ASSERT_NE(state.last_printer_sequence, 0U);

    state = reduce(state, Event::boot_phase(test::at_ms(2000), BootPhase::Starting));

    EXPECT_EQ(state.last_printer_generation, 0U);
    EXPECT_EQ(state.last_printer_sequence, 0U);
}

} // namespace
} // namespace prd::app_model
