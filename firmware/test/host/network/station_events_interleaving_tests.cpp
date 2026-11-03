// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include <algorithm>
#include <cstdint>
#include <tuple>

#include "station_events.hpp"
#include "support/station_ring_interleaving.hpp"
#include "gtest/gtest.h"

namespace prd::network::station::test {
namespace {
struct Burst {
    EventRing* ring;
    Snapshot after;
    std::uint32_t refusals;
    bool fired{false};
};

// Existing contention tests have no hook installed, including on their producer
// and consumer threads. There is no shared hook state or additional synchronisation.
thread_local Burst* active = nullptr;

struct ScopedBurst {
    explicit ScopedBurst(Burst& burst) {
        active = &burst;
    }
    ~ScopedBurst() {
        active = nullptr;
    }
    ScopedBurst(const ScopedBurst&) = delete;
    ScopedBurst& operator=(const ScopedBurst&) = delete;
};
} // namespace

void after_load(EventRing& ring, const Snapshot snapshot) {
    if (active == nullptr || active->ring != &ring || active->after != snapshot || active->fired) {
        return;
    }
    active->fired = true;
    // A legal SPSC interleaving: consumer paused, the single producer fills the
    // previously empty ring and refuses its tail, then consumer resumes. No private
    // counter is seeded and no atomic load/store is replaced with a model.
    for (std::uint32_t index = 0; index < kEventSlots + active->refusals; ++index) {
        ring.publish(Event{});
    }
}

namespace {
using Schedule = std::tuple<Snapshot, std::uint32_t, std::uint32_t>;
class StationEventRingInterleavingTests : public ::testing::TestWithParam<Schedule> {};

TEST_P(StationEventRingInterleavingTests, ConcurrentBurstCannotPreAcknowledgeStoredRecords) {
    const auto [after, refusals, prefix] = GetParam();
    EventRing ring;
    Event event;
    for (std::uint32_t index = 1; index <= prefix; ++index) {
        ring.publish(Event{});
        ASSERT_EQ(ring.take(event), Delivery::Delivered);
        ASSERT_EQ(event.sequence, index);
    }

    Burst burst{&ring, after, refusals};
    ScopedBurst hook{burst};
    Delivery result = ring.take(event);
    ASSERT_TRUE(burst.fired);
    EXPECT_NE(result, Delivery::EvidenceLost)
        << "a refusal must not overtake earlier stored records; Empty or Delivered is valid";

    std::uint32_t delivered = 0;
    std::uint32_t losses = 0;
    std::uint32_t max_acknowledged = 0;
    bool drained = false;
    // Strictly bounded even for the original infinite-drain defect. The first
    // Empty may precede the concurrent burst and is not an end-of-stream claim.
    for (std::size_t call = 0; call < 2 * kEventSlots + refusals + 4; ++call) {
        max_acknowledged = std::max(max_acknowledged, ring.acknowledged());
        if (result == Delivery::Delivered) {
            ++delivered;
            EXPECT_EQ(event.sequence, prefix + delivered);
        } else if (result == Delivery::EvidenceLost) {
            ++losses;
        } else if (call != 0) {
            drained = true;
            break;
        }
        result = ring.take(event);
    }
    EXPECT_TRUE(drained) << "bounded drain exhausted: acknowledged=" << ring.acknowledged()
                         << " refused=" << ring.refused();
    EXPECT_LE(max_acknowledged, ring.refused()) << "consumer counted a future loss too early";
    EXPECT_EQ(delivered, kEventSlots);
    EXPECT_EQ(losses, refusals);
    EXPECT_EQ(ring.acknowledged(), refusals);
    EXPECT_EQ(ring.refused(), refusals);
    EXPECT_EQ(ring.pending(), 0U);
    if (!drained) {
        return;
    }
    // A later successor is contiguous: tail refusals are never reported twice.
    ring.publish(Event{});
    ASSERT_EQ(ring.take(event), Delivery::Delivered);
    EXPECT_EQ(event.sequence, prefix + kEventSlots + refusals + 1);
    EXPECT_EQ(ring.take(event), Delivery::Empty);
}

INSTANTIATE_TEST_SUITE_P(SnapshotSchedules, StationEventRingInterleavingTests,
                         ::testing::Combine(::testing::Values(Snapshot::Refused, Snapshot::Write),
                                            ::testing::Values(1U, 3U, 12U),
                                            ::testing::Values(0U, 40U)));
} // namespace
} // namespace prd::network::station::test
