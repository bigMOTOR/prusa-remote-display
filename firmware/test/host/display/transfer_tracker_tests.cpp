// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include "transfer_tracker.hpp"

#include <gtest/gtest.h>

// Ownership invariants for in-flight pixel transfers. These are the properties a
// caller's buffer safety rests on: if pending() can underflow, wait_for_transfers
// returns early and DMA reads memory the caller has already reused; if a spurious
// completion is absorbed instead of counted, the same thing happens silently.

namespace {

using prd::display::TransferTracker;

TEST(TransferTracker, StartsIdle) {
    const TransferTracker tracker;

    EXPECT_TRUE(tracker.idle());
    EXPECT_EQ(tracker.pending(), 0U);
    EXPECT_EQ(tracker.peak_pending(), 0U);
    EXPECT_EQ(tracker.spurious_completions(), 0U);
}

TEST(TransferTracker, SubmitMakesItBusyAndCompleteMakesItIdle) {
    TransferTracker tracker;

    tracker.on_submit();
    EXPECT_FALSE(tracker.idle());
    EXPECT_EQ(tracker.pending(), 1U);

    EXPECT_TRUE(tracker.on_complete());
    EXPECT_TRUE(tracker.idle());
    EXPECT_EQ(tracker.pending(), 0U);
}

TEST(TransferTracker, TracksSeveralTransfersInFlight) {
    TransferTracker tracker;

    for (int i = 0; i < 10; ++i) {
        tracker.on_submit();
    }
    EXPECT_EQ(tracker.pending(), 10U);
    EXPECT_EQ(tracker.peak_pending(), 10U);

    for (int i = 0; i < 10; ++i) {
        EXPECT_TRUE(tracker.on_complete());
    }
    EXPECT_TRUE(tracker.idle());

    // The peak is a watermark, so it survives going idle.
    EXPECT_EQ(tracker.peak_pending(), 10U);
}

TEST(TransferTracker, PendingNeverUnderflows) {
    // The property that matters most: an unsigned underflow here would report
    // billions of transfers in flight and hang wait_for_transfers forever.
    TransferTracker tracker;

    EXPECT_FALSE(tracker.on_complete());
    EXPECT_EQ(tracker.pending(), 0U);
    EXPECT_TRUE(tracker.idle());

    EXPECT_FALSE(tracker.on_complete());
    EXPECT_EQ(tracker.pending(), 0U);
}

TEST(TransferTracker, SpuriousCompletionsAreCountedNotAbsorbed) {
    TransferTracker tracker;

    tracker.on_submit();
    EXPECT_TRUE(tracker.on_complete());
    EXPECT_EQ(tracker.spurious_completions(), 0U);

    EXPECT_FALSE(tracker.on_complete());
    EXPECT_FALSE(tracker.on_complete());
    EXPECT_EQ(tracker.spurious_completions(), 2U);
}

TEST(TransferTracker, FailedSubmitRollsTheCountBack) {
    // A submit that is optimistically counted and then fails must not strand the
    // count above zero: no completion will ever arrive for it, so wait_for_transfers
    // would block until its timeout on every subsequent call.
    TransferTracker tracker;

    tracker.on_submit();
    tracker.on_submit_failed();

    EXPECT_TRUE(tracker.idle());
    EXPECT_EQ(tracker.pending(), 0U);
    EXPECT_EQ(tracker.spurious_completions(), 0U);
}

TEST(TransferTracker, FailedSubmitDoesNotUnderflowWhenNothingIsPending) {
    TransferTracker tracker;

    tracker.on_submit_failed();

    EXPECT_EQ(tracker.pending(), 0U);
    EXPECT_TRUE(tracker.idle());
}

TEST(TransferTracker, FailedSubmitOnlyCancelsItsOwnTransfer) {
    TransferTracker tracker;

    tracker.on_submit();
    tracker.on_submit();
    tracker.on_submit_failed();

    EXPECT_EQ(tracker.pending(), 1U);
    EXPECT_FALSE(tracker.idle());
}

TEST(TransferTracker, ResetForgetsOutstandingTransfers) {
    // Teardown only: the panel is being destroyed, so no completion can arrive for
    // the transfers still counted.
    TransferTracker tracker;

    tracker.on_submit();
    tracker.on_submit();
    tracker.reset();

    EXPECT_TRUE(tracker.idle());
    EXPECT_EQ(tracker.pending(), 0U);
}

TEST(TransferTracker, ResetKeepsDiagnosticCounters) {
    // Diagnostics must survive a teardown, or a fault that happened before deinit
    // disappears from the record.
    TransferTracker tracker;

    tracker.on_submit();
    tracker.on_submit();
    (void)tracker.on_complete();
    (void)tracker.on_complete();
    EXPECT_FALSE(tracker.on_complete());

    tracker.reset();

    EXPECT_EQ(tracker.spurious_completions(), 1U);
    EXPECT_EQ(tracker.peak_pending(), 2U);
}

TEST(TransferTracker, InterleavedSubmitAndCompleteStaysConsistent) {
    // What the real driver does: completions arrive from an interrupt while the
    // task keeps submitting, so the two are not phase-separated.
    TransferTracker tracker;

    tracker.on_submit();
    tracker.on_submit();
    EXPECT_TRUE(tracker.on_complete());
    tracker.on_submit();
    EXPECT_EQ(tracker.pending(), 2U);
    EXPECT_TRUE(tracker.on_complete());
    EXPECT_TRUE(tracker.on_complete());

    EXPECT_TRUE(tracker.idle());
    EXPECT_EQ(tracker.peak_pending(), 2U);
    EXPECT_EQ(tracker.spurious_completions(), 0U);
}

TEST(TransferTracker, SixBandFrameSettlesIdle) {
    // A full 320x240 frame is six 40-row bands. After the last completion the
    // caller must own every buffer again.
    TransferTracker tracker;

    for (int band = 0; band < 6; ++band) {
        tracker.on_submit();
        EXPECT_FALSE(tracker.idle());
        EXPECT_TRUE(tracker.on_complete());
        EXPECT_TRUE(tracker.idle());
    }

    EXPECT_EQ(tracker.peak_pending(), 1U);
    EXPECT_EQ(tracker.spurious_completions(), 0U);
}

} // namespace
