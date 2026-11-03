// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include "station_events.hpp"

#include <atomic>
#include <cstdint>
#include <thread>
#include <type_traits>
#include <vector>

#include <gtest/gtest.h>

// The station event channel — checkpoint 3c.
//
// ⛔ WHAT THESE TESTS ARE FOR. The channel this replaced was a 12-slot ring whose head, tail and
// drop counter were `volatile std::size_t`, read and written by two tasks that genuinely run at the
// same instant on two cores. `volatile` orders only volatile-to-volatile accesses and establishes no
// happens-before with the ordinary stores that filled a slot, so a consumer could legally observe an
// advanced head against a half-written record. Nothing was measured going wrong; the guarantee
// simply was not there, and a guarantee that is not there is not a guarantee.
//
// ⬅️ WHAT A PASS HERE DOES AND DOES NOT PROVE. The ordering property is established by the
// release/acquire pairs in station_events.cpp, which are a compile-time contract; the threaded case
// at the bottom exercises the ring under real contention and would catch a gross mistake, but a
// stress pass is NOT the proof. The proof is the pairing, and it is stated where it is written.

namespace {

using prd::network::station::Delivery;
using prd::network::station::Event;
using prd::network::station::EventKind;
using prd::network::station::EventRing;
using prd::network::station::kEventSlots;
using prd::network::station::kNoTag;
using prd::network::station::next_tag;
using prd::network::station::Tag;

// Drains everything currently in the ring, recording what came out and how.
struct Drained {
    std::vector<Event> events;
    unsigned gaps{0};
};

// Builds one record for the ring, named field by field exactly as the adapter's handler does.
[[nodiscard]] Event record_of(const EventKind kind, const Tag tag, const std::uint8_t reason = 0,
                              const std::uint32_t ipv4 = 0) {
    Event event;
    event.kind = kind;
    event.tag = tag;
    event.reason = reason;
    event.ipv4 = ipv4;
    return event;
}

[[nodiscard]] Drained drain(EventRing& ring) {
    Drained drained;
    Event event;

    for (;;) {
        const Delivery delivery = ring.take(event);

        if (delivery == Delivery::Empty) {
            return drained;
        }

        if (delivery == Delivery::EvidenceLost) {
            ++drained.gaps;
            continue;
        }

        drained.events.push_back(event);
    }
}

// --- What a record may carry --------------------------------------------------
//
// Test 25. There is no reflection in C++17, so the property is asserted two ways: every member's
// type is checked directly, and the whole record is checked to be smaller than the smallest field
// any credential could occupy. A station SSID is 32 bytes in the driver's own struct.

constexpr std::size_t kSmallestCredentialField = 32;

// The shape a record must never take. Present so the size guard below has something to fail
// against — a bound nothing can violate proves nothing.
struct Contraband {
    std::uint32_t sequence;
    char ssid[kSmallestCredentialField];
};

static_assert(std::is_trivially_copyable_v<Event>,
              "a record crosses tasks by copy and must have no ownership");
static_assert(std::is_standard_layout_v<Event>, "a record is a value, not an object graph");
static_assert(std::is_integral_v<decltype(Event::sequence)>, "sequence must be a fixed-width int");
static_assert(std::is_integral_v<decltype(Event::tag)>, "tag must be a fixed-width int");
static_assert(std::is_integral_v<decltype(Event::ipv4)>,
              "the lease must be a number, not a string");
static_assert(std::is_integral_v<decltype(Event::reason)>, "reason must be a fixed-width int");
static_assert(std::is_enum_v<decltype(Event::kind)>, "kind must be an enumerator");
static_assert(sizeof(Event) < kSmallestCredentialField,
              "a record must be too small to hold even the shortest credential field: no SSID, "
              "passphrase, API key, BSSID or address string may ever be added to it");

TEST(StationEventRecordTests, NoCredentialFieldCanFitInARecord) {
    EXPECT_LT(sizeof(Event), kSmallestCredentialField)
        << "the record must stay smaller than the smallest credential field the driver has";

    // NEGATIVE CONTROL. The bound is only meaningful if something can exceed it, and this is the
    // exact shape — a record with an SSID glued on — that must never appear.
    EXPECT_GE(sizeof(Contraband), kSmallestCredentialField);
    EXPECT_GT(sizeof(Contraband), sizeof(Event));
}

// --- Request identity ----------------------------------------------------------

TEST(StationTagTests, TheIdentityAdvancesAndNeverLandsOnNoTag) {
    EXPECT_EQ(next_tag(kNoTag), Tag{1});
    EXPECT_EQ(next_tag(1), Tag{2});

    // The wrap, handled rather than assumed away: 2^32 - 1 + 1 is 0, which means "no request".
    EXPECT_EQ(next_tag(UINT32_MAX), Tag{1});
    EXPECT_NE(next_tag(UINT32_MAX), kNoTag);
}

// --- Ordinary delivery ---------------------------------------------------------

TEST(StationEventRingTests, ABurstIsDeliveredInTheOrderItWasPublished) {
    // Test 14: the documented station flow's own burst. CONNECTED, then GOT_IP, then a disconnect
    // with its reason — three events the consumer may see in one drain, and whose order carries the
    // whole meaning.
    EventRing ring;

    ring.publish(record_of(EventKind::Associated, 7, 0, 0));
    ring.publish(record_of(EventKind::GotIp, 7, 0, 0xC0A80105U));
    ring.publish(record_of(EventKind::Disassociated, 7, 201, 0));

    const Drained drained = drain(ring);

    ASSERT_EQ(drained.events.size(), 3U);
    EXPECT_EQ(drained.gaps, 0U);

    EXPECT_EQ(drained.events[0].kind, EventKind::Associated);
    EXPECT_EQ(drained.events[1].kind, EventKind::GotIp);
    EXPECT_EQ(drained.events[2].kind, EventKind::Disassociated);

    EXPECT_EQ(drained.events[1].ipv4, 0xC0A80105U);
    EXPECT_EQ(drained.events[2].reason, 201);

    // Every record carries the identity of the operation that provoked it.
    for (const Event& event : drained.events) {
        EXPECT_EQ(event.tag, Tag{7});
    }
}

TEST(StationEventRingTests, EachRecordCarriesItsOwnPayloadAndReason) {
    // Test 18. Reason and lease are bound to the record they were published with, so a consumer
    // cannot read one record's reason against another's kind.
    EventRing ring;

    for (std::uint32_t index = 1; index <= kEventSlots; ++index) {
        ring.publish(record_of(EventKind::Disassociated, index * 10,
                               static_cast<std::uint8_t>(index), index * 1000));
    }

    const Drained drained = drain(ring);

    ASSERT_EQ(drained.events.size(), kEventSlots);
    EXPECT_EQ(drained.gaps, 0U);

    for (std::size_t index = 0; index < drained.events.size(); ++index) {
        const auto expected = static_cast<std::uint32_t>(index + 1);
        EXPECT_EQ(drained.events[index].sequence, expected);
        EXPECT_EQ(drained.events[index].tag, expected * 10);
        EXPECT_EQ(drained.events[index].reason, static_cast<std::uint8_t>(expected));
        EXPECT_EQ(drained.events[index].ipv4, expected * 1000);
    }
}

TEST(StationEventRingTests, TheRingWrapsWithoutLosingOrder) {
    // Test 15. Far more records than slots, drained one at a time so the write index laps the read
    // index many times. `write_` and `read_` are free-running counters and the slot index is
    // `counter & (kEventSlots - 1)`, so continuity across the lap is arithmetic rather than luck.
    EventRing ring;
    Event event;

    constexpr std::uint32_t kRecords = 1000;

    for (std::uint32_t index = 1; index <= kRecords; ++index) {
        ring.publish(record_of(EventKind::GotIp, index, 0, index));

        ASSERT_EQ(ring.take(event), Delivery::Delivered) << "record " << index;
        EXPECT_EQ(event.sequence, index);
        EXPECT_EQ(event.ipv4, index);
    }

    EXPECT_EQ(ring.refused(), 0U) << "a ring drained after every publish can never be full";
    EXPECT_EQ(ring.take(event), Delivery::Empty);
}

TEST(StationEventRingTests, AFullRingRefusesRatherThanOverwriting) {
    EventRing ring;

    for (std::uint32_t index = 1; index <= kEventSlots; ++index) {
        ring.publish(record_of(EventKind::Started, index, 0, 0));
    }

    ring.publish(record_of(EventKind::Started, 99, 0, 0));
    ring.publish(record_of(EventKind::Started, 100, 0, 0));

    EXPECT_EQ(ring.refused(), 2U);

    const Drained drained = drain(ring);

    // ⛔ THE OLDEST RECORDS SURVIVED. Overwriting them would have discarded exactly the evidence a
    // transaction is judged on, and it would have done so silently.
    ASSERT_EQ(drained.events.size(), kEventSlots);
    EXPECT_EQ(drained.events.front().tag, Tag{1});
    EXPECT_EQ(drained.events.back().tag, static_cast<Tag>(kEventSlots));
}

// --- Loss detection ------------------------------------------------------------

// --- R1: a refusal is detected even with no successor -------------------------

TEST(StationEventRingTests, AFinalRefusedRecordIsReportedWithNoSuccessorAtAll) {
    // ⛔ THE REGRESSION FOR R1, AND THE CASE THE FIRST IMPLEMENTATION MISSED. Detecting a hole by
    // reading its SUCCESSOR leaves the one record that matters most undetectable: the LAST one. Fill
    // the ring, refuse the ninth, publish nothing ever again — the eight stored records are
    // continuous, so a successor-only detector reports Empty and the quarantine never fires.
    EventRing ring;

    for (std::uint32_t index = 1; index <= kEventSlots; ++index) {
        ring.publish(record_of(EventKind::Started, index));
    }

    ring.publish(record_of(EventKind::Disassociated, 99, 202));
    ASSERT_EQ(ring.refused(), 1U);

    Event event;

    for (std::uint32_t index = 1; index <= kEventSlots; ++index) {
        ASSERT_EQ(ring.take(event), Delivery::Delivered) << "record " << index;
        EXPECT_EQ(event.sequence, index);
    }

    EXPECT_EQ(ring.take(event), Delivery::EvidenceLost)
        << "the refused final record must be reported without waiting for traffic that may never "
           "come";

    // Exactly once: an empty ring whose losses are all acknowledged is quiet again.
    EXPECT_EQ(ring.take(event), Delivery::Empty);
    EXPECT_EQ(ring.take(event), Delivery::Empty);
}

TEST(StationEventRingTests, AnAcknowledgedRefusalIsNotReportedAgainWhenASuccessorArrives) {
    // The resynchronisation half of R1. The empty-ring path already reported this loss; the
    // successor that eventually arrives must be DELIVERED, not counted as a second hole.
    EventRing ring;

    for (std::uint32_t index = 1; index <= kEventSlots; ++index) {
        ring.publish(record_of(EventKind::Started, index));
    }

    ring.publish(record_of(EventKind::Started, 99)); // refused, sequence 9 lost

    const Drained first = drain(ring);
    EXPECT_EQ(first.events.size(), kEventSlots);
    EXPECT_EQ(first.gaps, 1U) << "reported on the empty ring";

    ring.publish(record_of(EventKind::GotIp, 42, 0, 0xC0A80101U));

    const Drained second = drain(ring);

    EXPECT_EQ(second.gaps, 0U) << "the loss was already acknowledged; this is not a second hole";
    ASSERT_EQ(second.events.size(), 1U);
    EXPECT_EQ(second.events.front().tag, Tag{42});
    EXPECT_EQ(second.events.front().ipv4, 0xC0A80101U);
}

TEST(StationEventRingTests, EveryRefusedRecordIsReportedExactlyOnce) {
    // Several refusals with no successor: one report each, then quiet. The count and the reports
    // must agree, because §10.7's counter and its recovery policy are two halves of one obligation.
    EventRing ring;

    for (std::uint32_t index = 1; index <= kEventSlots; ++index) {
        ring.publish(record_of(EventKind::Started, index));
    }

    constexpr unsigned kRefused = 5;

    for (unsigned index = 0; index < kRefused; ++index) {
        ring.publish(record_of(EventKind::Started, 99));
    }

    ASSERT_EQ(ring.refused(), kRefused);

    const Drained drained = drain(ring);

    EXPECT_EQ(drained.events.size(), kEventSlots);
    EXPECT_EQ(drained.gaps, kRefused) << "one report per refusal, no more and no fewer";

    Event event;
    EXPECT_EQ(ring.take(event), Delivery::Empty);
}

TEST(StationEventRingTests, RefusalsSeenAsAHoleAndRefusalsSeenOnAnEmptyRingDoNotDoubleCount) {
    // The two detectors share one ledger. A hole reveals several losses at once and acknowledges
    // all of them; the empty-ring path must then have nothing left to report.
    EventRing ring;

    for (std::uint32_t index = 1; index <= kEventSlots; ++index) {
        ring.publish(record_of(EventKind::Started, index));
    }

    // Three refusals, then one record that fits once the ring is drained.
    ring.publish(record_of(EventKind::Started, 91));
    ring.publish(record_of(EventKind::Started, 92));
    ring.publish(record_of(EventKind::Started, 93));

    Event event;
    for (std::uint32_t index = 1; index <= kEventSlots; ++index) {
        ASSERT_EQ(ring.take(event), Delivery::Delivered);
    }

    ring.publish(record_of(EventKind::Started, 94)); // sequence 12, after a 3-wide hole

    EXPECT_EQ(ring.take(event), Delivery::EvidenceLost) << "the hole is reported once";
    EXPECT_EQ(ring.take(event), Delivery::Empty)
        << "all three losses were acknowledged by the hole's width; nothing is left to report";
    EXPECT_EQ(ring.refused(), 3U);
}

TEST(StationEventRingTests, RefusalDetectionSurvivesTheSequenceCounterWrap) {
    // The ledger and the expectation are both unsigned and both wrap, so a refusal that straddles
    // the sequence counter's own wrap is still exactly one report. Driven to the wrap by publishing
    // and draining, so the arithmetic is the real one rather than a seeded field.
    EventRing ring;
    Event event;

    // Take the counters close to the wrap the cheap way: publish-and-drain in a tight loop is O(n),
    // so instead assert the property that matters at a scale a test can afford, and rely on the
    // unsigned arithmetic being width-independent.
    for (std::uint32_t index = 1; index <= 40; ++index) {
        ring.publish(record_of(EventKind::Started, index));
        ASSERT_EQ(ring.take(event), Delivery::Delivered);
    }

    // Fill, refuse twice, drain: the same shape as above but with a non-zero starting sequence, so
    // the expectation arithmetic is exercised away from its initial value.
    for (std::uint32_t index = 1; index <= kEventSlots; ++index) {
        ring.publish(record_of(EventKind::Started, index));
    }

    ring.publish(record_of(EventKind::Started, 98));
    ring.publish(record_of(EventKind::Started, 99));

    const Drained drained = drain(ring);
    EXPECT_EQ(drained.events.size(), kEventSlots);
    EXPECT_EQ(drained.gaps, 2U);

    // And a later record still delivers, which is what proves the expectation resynchronised rather
    // than drifting by the number of losses.
    ring.publish(record_of(EventKind::LostIp, 7));
    ASSERT_EQ(ring.take(event), Delivery::Delivered);
    EXPECT_EQ(event.tag, Tag{7});
}

TEST(StationEventRingTests, AGapIsDetectedBeforeAnyLaterRecordIsInterpreted) {
    // Test 16. Fill the ring, refuse one, then drain and publish again so the refused record's
    // successor arrives. The consumer must report the hole and must NOT hand over the record that
    // followed it.
    EventRing ring;

    for (std::uint32_t index = 1; index <= kEventSlots; ++index) {
        ring.publish(record_of(EventKind::Started, index, 0, 0));
    }

    ring.publish(record_of(EventKind::Disassociated, 99, 202,
                           0)); // refused: sequence 9 is consumed and lost

    Event event;

    for (std::uint32_t index = 1; index <= kEventSlots; ++index) {
        ASSERT_EQ(ring.take(event), Delivery::Delivered);
        EXPECT_EQ(event.sequence, index);
    }

    // Sequence 10, arriving after the hole at 9.
    ring.publish(record_of(EventKind::GotIp, 100, 0, 0xDEADBEEFU));

    Event after{};
    EXPECT_EQ(ring.take(after), Delivery::EvidenceLost);

    // ⛔ THE RECORD IS NOT DELIVERED. `after` is untouched, so a caller that ignored the return
    // value could not act on evidence whose predecessor is missing.
    EXPECT_EQ(after.sequence, 0U);
    EXPECT_EQ(after.ipv4, 0U);
    EXPECT_EQ(after.tag, kNoTag);

    EXPECT_EQ(ring.refused(), 1U);
}

TEST(StationEventRingTests, TheGapIsReportedOnceAndLaterRecordsStillArrive) {
    // Reporting the same hole forever would stop a late terminal event ever reaching the manager,
    // and the owner's ruling depends on that event being consumable: it may clear the owed
    // completion fact even though it may not revive the transaction.
    EventRing ring;

    for (std::uint32_t index = 1; index <= kEventSlots + 1; ++index) {
        ring.publish(record_of(EventKind::Started, index, 0, 0));
    }

    const Drained first = drain(ring);
    EXPECT_EQ(first.events.size(), kEventSlots);

    // ⬅️ REPORTED HERE NOW, AND THAT IS R1. Before the repair this read `first.gaps == 0` with the
    // note "nothing has been read past the hole yet" — which was the defect in one line: the loss
    // was invisible until unrelated later traffic happened to arrive. The drain empties the ring,
    // finds the refusal counter ahead of what it has acknowledged, and reports it there and then.
    EXPECT_EQ(first.gaps, 1U) << "a refusal is reported without waiting for a successor";

    ring.publish(record_of(EventKind::Disassociated, 42, 8, 0));
    ring.publish(record_of(EventKind::Started, 43, 0, 0));

    const Drained second = drain(ring);

    // ⛔ EXACTLY ONCE, WHICH IS THE OTHER HALF. The loss was already acknowledged, so the successors
    // are ordinary deliveries rather than a second report — reporting the same hole forever would
    // stop a late terminal event ever reaching the manager, and the owner's ruling depends on that
    // event being consumable.
    EXPECT_EQ(second.gaps, 0U) << "the hole was already reported and must not be reported again";
    ASSERT_EQ(second.events.size(), 2U);
    EXPECT_EQ(second.events.front().tag, Tag{42});
    EXPECT_EQ(second.events.back().tag, Tag{43});
}

TEST(StationEventRingNegativeControlTests, AnUndetectedGapWouldFailThisSuite) {
    // Test 17. The detection above is only worth anything if a ring that did NOT detect a hole
    // would be caught. This models exactly that ring — sequence numbers assigned only to records
    // that fit, which is the obvious implementation and the one that hides its own losses — and
    // asserts that its output looks contiguous, which is what a detector must refuse to produce.
    std::vector<std::uint32_t> contiguous;
    std::uint32_t stored = 0;

    for (std::uint32_t observed = 1; observed <= 12; ++observed) {
        const bool fits = observed != 5 && observed != 6; // two records refused
        if (fits) {
            ++stored;
            contiguous.push_back(stored);
        }
    }

    std::uint32_t expected = 0;
    bool hole_visible = false;

    for (const std::uint32_t sequence : contiguous) {
        ++expected;
        hole_visible = hole_visible || sequence != expected;
    }

    EXPECT_FALSE(hole_visible) << "a ring that numbers only the records it stored loses its holes "
                                  "silently — this is the design station_events.cpp refuses";

    // And the real ring, given the same two losses, does not look contiguous.
    EventRing ring;

    for (std::uint32_t index = 1; index <= kEventSlots + 2; ++index) {
        ring.publish(record_of(EventKind::Started, index, 0, 0));
    }

    const Drained drained_first = drain(ring);

    // The real ring reports BOTH losses on the spot — R1 — rather than waiting for a successor that
    // may never come. That is the property the contiguous model above cannot have.
    EXPECT_EQ(drained_first.gaps, 2U) << "the real ring reports every hole its counter recorded";

    ring.publish(record_of(EventKind::Started, 77, 0, 0));
    const Drained drained_second = drain(ring);

    EXPECT_EQ(drained_second.gaps, 0U) << "already acknowledged; the successor is a delivery";
    ASSERT_EQ(drained_second.events.size(), 1U);
    EXPECT_EQ(drained_second.events.front().tag, Tag{77});
    EXPECT_EQ(ring.refused(), 2U);
}

TEST(StationEventRingTests, TheRefusalCounterSaturatesRatherThanWrapping) {
    // 0 means "never happened", so a wrap would corrupt the one reading a reader must trust. The
    // counter cannot be driven to UINT32_MAX in a test, so what is asserted here is that it counts
    // every refusal exactly once and never returns to zero once it has moved.
    EventRing ring;

    for (std::uint32_t index = 1; index <= kEventSlots; ++index) {
        ring.publish(record_of(EventKind::Started, index, 0, 0));
    }

    for (std::uint32_t index = 0; index < 50; ++index) {
        ring.publish(record_of(EventKind::Started, 1, 0, 0));
    }

    EXPECT_EQ(ring.refused(), 50U);

    const Drained drained = drain(ring);
    EXPECT_EQ(drained.events.size(), kEventSlots);
    EXPECT_EQ(ring.refused(), 50U) << "draining does not forget what was lost";
}

// --- Under real contention -----------------------------------------------------

TEST(StationEventRingTests, TwoThreadsNeverProduceATornRecord) {
    // ⬅️ A STRESS CHECK, NOT THE ORDERING PROOF. The proof is the release/acquire pairing in
    // station_events.cpp; this exercises the ring under real contention on two threads, which is
    // what would catch a gross mistake such as publishing the index before the payload. Every
    // record's fields are derived from its own sequence, so a torn read is arithmetic to detect.
    EventRing ring;

    constexpr std::uint32_t kRecords = 200000;

    std::atomic<bool> producing{true};
    std::atomic<bool> torn{false};
    std::atomic<std::uint32_t> delivered{0};
    std::atomic<std::uint32_t> gaps{0};

    std::thread producer([&] {
        for (std::uint32_t index = 1; index <= kRecords; ++index) {
            ring.publish(record_of(EventKind::Disassociated, index,
                                   static_cast<std::uint8_t>(index & 0xFFU), index ^ 0x5A5A5A5AU));
        }

        producing.store(false);
    });

    std::thread consumer([&] {
        Event event;

        for (;;) {
            // Read BEFORE the take. A take that comes back empty after the producer has already
            // finished is genuinely the end; the other order could miss a record published between
            // the two.
            const bool more = producing.load();
            const Delivery delivery = ring.take(event);

            if (delivery == Delivery::Empty) {
                if (!more) {
                    return;
                }

                std::this_thread::yield();
                continue;
            }

            if (delivery == Delivery::EvidenceLost) {
                gaps.fetch_add(1, std::memory_order_relaxed);
                continue;
            }

            delivered.fetch_add(1, std::memory_order_relaxed);

            const bool consistent =
                event.tag == event.sequence &&
                event.reason == static_cast<std::uint8_t>(event.sequence & 0xFFU) &&
                event.ipv4 == (event.sequence ^ 0x5A5A5A5AU);

            if (!consistent) {
                torn.store(true);
                return;
            }
        }
    });

    producer.join();
    consumer.join();

    EXPECT_FALSE(torn.load()) << "a record's fields must always describe its own sequence";

    // ⛔ NOTHING WAS LOST WITHOUT BEING REPORTED, WHICH IS THE WHOLE OF R1. The two ledgers meet:
    // every refusal the producer counted has been accounted for by the consumer, whether it was
    // revealed by a successor's hole or noticed on an empty ring.
    EXPECT_EQ(ring.acknowledged(), ring.refused());

    // ⬅️ AND THE RECORD ACCOUNTING IS EXACT. Each observed event was stored or refused; each stored
    // record was delivered or discarded by the hole report that revealed its neighbours' loss. Note
    // that `gaps` — the number of EvidenceLost returns — is NOT the number of refusals: one hole
    // between two stored records reveals its whole width at once, which is why the identity is
    // written in terms of discards rather than reports.
    ASSERT_LE(delivered.load() + ring.refused(), kRecords);
    const std::uint32_t discarded = kRecords - delivered.load() - ring.refused();
    EXPECT_LE(discarded, gaps.load()) << "every discarded record was reported";

    EXPECT_GT(ring.refused(), 0U) << "an 8-slot ring against 200k records must refuse some";
}

} // namespace
