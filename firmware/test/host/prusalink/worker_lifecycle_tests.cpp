// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include <cstdint>
#include <limits>

#include <gtest/gtest.h>

#include "worker_lifecycle.hpp"

// Worker ownership — Milestone 4's first vertical slice.
//
// ⛔ WHAT THESE PROTECT. stop() must not block: the application task is the store's only
// dispatcher and the thing that services button gestures, and a request can sit in a socket for
// its whole timeout. A non-blocking stop is only safe if the outgoing worker cannot do damage
// on its way out, and there are exactly three ways it could — publishing a result for a
// configuration that has been replaced, reading a target that has been swapped underneath it,
// or overlapping with a new worker's requests.
//
// ⬅️ WHY THE DECISION LOGIC IS A PURE UNIT. The worker itself is a FreeRTOS task and cannot run
// in this build, so the interleavings that matter would otherwise be untestable — and they are
// precisely the ones that never reproduce on a bench. Extracting the lease arithmetic is the
// same move this project made for the boot decision, the reset sequencing and the recovery
// re-entry, each time for the same reason.

namespace prd::prusalink::lifecycle {
namespace {

TEST(WorkerLifecycleTests, TheFirstLeaseIsNotTheNoOneLease) {
    // kNoLease means "nobody holds the worker", so a real lease must never equal it — otherwise
    // the very first worker would read as superseded and exit before doing anything.
    const std::uint32_t first = next_generation(kNoLease);

    EXPECT_NE(first, kNoLease);
    EXPECT_TRUE(may_publish(first, first));
    EXPECT_FALSE(should_exit(first, first));
}

TEST(WorkerLifecycleTests, ACurrentLeaseMayPublishAndAStaleOneMayNot) {
    const std::uint32_t mine = next_generation(kNoLease);

    EXPECT_TRUE(may_publish(mine, mine));

    // stop(), or a start() for a new configuration.
    const std::uint32_t current = next_generation(mine);

    EXPECT_FALSE(may_publish(mine, current));
    EXPECT_TRUE(should_exit(mine, current));

    // POSITIVE CONTROL: the new holder may publish, so the refusal above is about being stale
    // rather than about publishing never being allowed.
    EXPECT_TRUE(may_publish(current, current));
}

TEST(WorkerLifecycleTests, HAZARD_AStaleResultIsDroppedAfterReconfiguration) {
    // The scenario: a worker is mid-request for printer A. The owner reconfigures to printer B,
    // so stop() then start() run. The old worker's response finally arrives.
    const std::uint32_t worker_for_a = next_generation(kNoLease);

    const std::uint32_t after_stop = next_generation(worker_for_a);
    const std::uint32_t worker_for_b = next_generation(after_stop);

    // The A worker must not publish. Without this, printer A's state would be dispatched as
    // though it belonged to printer B — the wrong printer's data on the panel, with no way for
    // a reader to tell.
    EXPECT_FALSE(may_publish(worker_for_a, worker_for_b));
    EXPECT_TRUE(should_exit(worker_for_a, worker_for_b));

    // And the B worker is unaffected by the A worker's existence.
    EXPECT_TRUE(may_publish(worker_for_b, worker_for_b));
}

TEST(WorkerLifecycleTests, HAZARD_StopAloneIsEnoughToSilenceAWorker) {
    // stop() without a following start(). The worker must still be silenced immediately, because
    // the reason for stopping may be that the configuration is about to be erased.
    const std::uint32_t mine = next_generation(kNoLease);
    const std::uint32_t after_stop = next_generation(mine);

    EXPECT_FALSE(may_publish(mine, after_stop));
    EXPECT_TRUE(should_exit(mine, after_stop));
}

TEST(WorkerLifecycleTests, HAZARD_ANewWorkerCannotStartWhileOneIsAlive) {
    // The one-worker invariant. "Alive" includes a worker that has been asked to stop and has
    // not yet noticed — which is the whole difficulty of a non-blocking stop, and exactly the
    // window in which two sockets could otherwise be open at once (§10.4).
    EXPECT_TRUE(may_start(false));
    EXPECT_FALSE(may_start(true));
}

TEST(WorkerLifecycleTests, AWorkerHoldingNoLeaseCanNeverPublish) {
    // Defensive, and it costs one comparison: a worker whose captured lease was somehow
    // kNoLease must be inert rather than matching a current lease that happens to be kNoLease
    // too. Otherwise a worker started before any lease was issued could publish freely.
    EXPECT_FALSE(may_publish(kNoLease, kNoLease));
    EXPECT_TRUE(should_exit(kNoLease, kNoLease));

    EXPECT_FALSE(may_publish(kNoLease, next_generation(kNoLease)));
}

TEST(WorkerLifecycleTests, LeasesAdvanceMonotonicallyAndSkipTheNoOneValueOnWrap) {
    // A device reconfigured enough times to wrap the counter must not hand out kNoLease, which
    // would make the holder inert and silently stop all polling until a reboot.
    std::uint32_t lease = next_generation(kNoLease);

    for (int step = 0; step < 8; ++step) {
        const std::uint32_t next = next_generation(lease);
        EXPECT_NE(next, lease);
        EXPECT_NE(next, kNoLease);
        lease = next;
    }

    const std::uint32_t wrapped = next_generation(std::numeric_limits<std::uint32_t>::max());
    EXPECT_NE(wrapped, kNoLease);
    EXPECT_TRUE(may_publish(wrapped, wrapped));
}

TEST(WorkerLifecycleTests, MayPublishAndShouldExitAreExactComplements) {
    // They are asked at different points in the cycle, so they are separate functions — but they
    // must never disagree, or a worker could be told to keep running and refused permission to
    // publish, and would spin issuing requests whose results are all discarded.
    constexpr std::uint32_t kValues[] = {kNoLease, 1,    2,
                                         3,        4096, std::numeric_limits<std::uint32_t>::max()};

    for (const std::uint32_t mine : kValues) {
        for (const std::uint32_t current : kValues) {
            EXPECT_NE(may_publish(mine, current), should_exit(mine, current))
                << mine << " vs " << current;
        }
    }
}

TEST(WorkerLifecycleTests, ARestartedWorkerForTheSameConfigurationIsStillADifferentLease) {
    // Restarting after a network drop takes a new lease even though the target is identical.
    // NEGATIVE CONTROL for a tempting simplification: if the lease were derived from the target
    // instead of from a counter, the old and new workers would share one, and a result from
    // before the drop could be published after it as though it were current.
    const std::uint32_t before = next_generation(kNoLease);
    const std::uint32_t after = next_generation(next_generation(before));

    EXPECT_NE(before, after);
    EXPECT_FALSE(may_publish(before, after));
}

} // namespace
} // namespace prd::prusalink::lifecycle
