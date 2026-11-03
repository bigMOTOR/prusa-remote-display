// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include "station_events.hpp"

#ifdef PRD_STATION_RING_TEST_INTERLEAVING
#ifdef ESP_PLATFORM
#error "The station ring interleaving seam is host-test-only"
#endif
#include "support/station_ring_interleaving.hpp"
#endif

namespace prd::network::station {
namespace {

// Adds `count` to a saturating consumer ledger.
//
// The ledger uses the same ceiling as the producer's refusal counter. Saturation
// alone does not establish correct accounting: take() must also observe refusals
// before the head snapshot, so a loss cannot overtake earlier stored records.
[[nodiscard]] std::uint32_t acknowledge(const std::uint32_t ledger, const std::uint32_t count) {
    return ledger > UINT32_MAX - count ? UINT32_MAX : ledger + count;
}

} // namespace

std::string_view log_label(const EventKind kind) {
    switch (kind) {
    case EventKind::Started:
        return "started";

    case EventKind::Associated:
        return "associated";

    case EventKind::Disassociated:
        return "disassociated";

    case EventKind::GotIp:
        return "got-ip";

    case EventKind::LostIp:
        return "lost-ip";

    case EventKind::Stopped:
        return "stopped";
    }

    return "started";
}

void EventRing::publish(Event record) {
    // ⛔ EVERY OBSERVED EVENT CONSUMES A SEQUENCE NUMBER, INCLUDING ONE THAT DOES NOT FIT. That is
    // the whole detection mechanism: a refused record leaves a hole the consumer sees before it
    // interprets anything, rather than a silently contiguous stream that lost a disconnect reason.
    ++produced_;
    record.sequence = produced_;

    const std::uint32_t write = write_.load(std::memory_order_relaxed);

    // ⬅️ ACQUIRE, SO THE SLOT THE CONSUMER HAS FINISHED WITH IS GENUINELY FREE. Pairs with the
    // consumer's release store of read_ below; without it the producer could reuse a slot whose
    // read the consumer has not completed.
    const std::uint32_t read = read_.load(std::memory_order_acquire);

    if (write - read >= static_cast<std::uint32_t>(kEventSlots)) {
        // LEGAL OVERWRITE: NONE. The oldest record is the one a transaction is judged on.
        // ⬅️ RELEASE, BECAUSE THE CONSUMER NOW READS THIS AS EVIDENCE RATHER THAN AS A STATISTIC
        // (R1). take() consults it on an empty ring to notice a refused final record, so the store
        // that records the refusal must be ordered against the loads that report it. Producer-only
        // read-modify-write: there is exactly one writer, so a plain load/store pair is sound and
        // no atomic arithmetic is needed inside the event handler.
        const std::uint32_t refused = refused_.load(std::memory_order_relaxed);
        if (refused != UINT32_MAX) {
            refused_.store(refused + 1, std::memory_order_release);
        }
        return;
    }

    slots_[write & kIndexMask] = record;

    // ⛔ THE WHOLE RECORD IS WRITTEN BEFORE THIS LINE, AND THIS LINE IS WHAT PUBLISHES IT. Release
    // pairs with the consumer's acquire load of write_, so every store above happens-before every
    // read of that slot. This is the guarantee `volatile` did not provide.
    write_.store(write + 1, std::memory_order_release);
}

Delivery EventRing::take(Event& out) {
    const std::uint32_t read = read_.load(std::memory_order_relaxed);
    // The producer publishes earlier slots before releasing a refusal. Acquiring
    // that refusal FIRST makes those publications happen-before the head load:
    // write-read coherence forbids an older, empty head hiding those records.
    // The reverse order can mix an old empty head with a new refusal, advance
    // expected_ prematurely, then underflow the next stored record's sequence gap.
    // A refusal after this snapshot may wait for the next take(); it cannot be
    // acknowledged here ahead of the records that preceded it.
    const std::uint32_t refused = refused_.load(std::memory_order_acquire);
#ifdef PRD_STATION_RING_TEST_INTERLEAVING
    test::after_load(*this, test::Snapshot::Refused);
#endif
    const std::uint32_t write = write_.load(std::memory_order_acquire);
#ifdef PRD_STATION_RING_TEST_INTERLEAVING
    test::after_load(*this, test::Snapshot::Write);
#endif

    if (read == write) {
        // ⛔ AN EMPTY RING IS NOT NECESSARILY A QUIET ONE — R1. If the producer refused a record and
        // nothing was published after it, there is no successor whose sequence could reveal the
        // hole. The refusal counter is the only witness, so it is consulted here: one report per
        // unacknowledged refusal, and `expected_` advances with it so a successor that does
        // eventually arrive is continuous and is NOT reported a second time.
        if (refused != acknowledged_) {
            acknowledged_ = acknowledge(acknowledged_, 1);
            ++expected_;
            return Delivery::EvidenceLost;
        }

        return Delivery::Empty;
    }

    const Event record = slots_[read & kIndexMask];

    // Frees the slot. Release, so the copy above happens-before the producer may reuse it.
    read_.store(read + 1, std::memory_order_release);

    // ⛔ CONTINUITY IS CHECKED BEFORE THE RECORD IS INTERPRETED, not after. Both sides of this
    // comparison wrap, and they wrap together: ++produced_ from UINT32_MAX gives 0, and
    // expected_ + 1 from UINT32_MAX gives 0. So the check survives the counter's own wrap without
    // a special case.
    const std::uint32_t expected = expected_ + 1;
    const std::uint32_t missing = record.sequence - expected;
    expected_ = record.sequence;

    if (missing != 0) {
        // ⬅️ THE HOLE'S WIDTH IS THE NUMBER OF REFUSALS IT ACCOUNTS FOR, and acknowledging all of
        // them here is what keeps the empty-ring path above from reporting the same losses again.
        // Unsigned arithmetic, so this is correct across the sequence counter's own wrap.
        acknowledged_ = acknowledge(acknowledged_, missing);

        // The record is consumed and DISCARDED. `out` is untouched, so a caller that ignores this
        // return value cannot accidentally act on evidence that arrived after a hole.
        return Delivery::EvidenceLost;
    }

    out = record;
    return Delivery::Delivered;
}

std::uint32_t EventRing::refused() const {
    return refused_.load(std::memory_order_relaxed);
}

std::uint32_t EventRing::acknowledged() const {
    return acknowledged_;
}

std::uint32_t EventRing::pending() const {
    return write_.load(std::memory_order_acquire) - read_.load(std::memory_order_relaxed);
}

} // namespace prd::network::station
