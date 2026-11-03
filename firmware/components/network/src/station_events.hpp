// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "network/wifi.hpp"

// THE ONE CHANNEL FROM THE ESP-IDF EVENT LOOP TO THE APPLICATION TASK — Architecture Spec §8.1,
// §10.7, §13.5.
//
// INTERNAL HEADER of the network component. Pure apart from <atomic>: no ESP-IDF, no FreeRTOS, no
// clock — so the ring below is host-tested, and stress-tested against two real threads, against the
// same code the firmware runs.
//
// ⛔ WHAT THIS REPLACES, AND WHY THE OLD ONE WAS FORMALLY BROKEN. Until this checkpoint the station
// adapter carried a 12-slot ring whose head, tail and drop counter were `volatile std::size_t`.
// CONFIG_FREERTOS_UNICORE is not set, so the producer (ESP-IDF's `sys_evt` task) and the consumer
// genuinely run at the same instant on different cores — and `volatile` orders only
// volatile-to-volatile accesses. It establishes NO happens-before with the ordinary stores that
// filled the slot, so a consumer could legally observe an advanced head against a half-written
// record. Nothing was measured going wrong; the guarantee simply was not there.
//
// ⬅️ AND THE REPLACEMENT IS NOT JUST "ADD std::atomic". Three properties are new:
//
//   1. NO OVERWRITE, EVER. A full ring refuses the record. Overwriting the oldest would discard
//      exactly the evidence a transaction is judged on, silently.
//   2. THE CONSUMER DETECTS THE LOSS ITSELF. Every observed event consumes a sequence number,
//      whether or not it fits, so a refused record leaves a HOLE the consumer sees structurally
//      before it interprets anything. A counter alone is a report; this is a detection.
//   3. LOSS IS A FAULT, NOT A CASE. A gap yields Delivery::EvidenceLost, which the manager turns
//      into DriverStalled. §10.7 requires a saturating counter AND a controlled recovery policy;
//      the counter is `refused()` and the policy is the owner's DriverStalled ruling.
//
// ⛔ THE PRODUCER MAY NOT LOG, ALLOCATE, CALL THE DRIVER, DISPATCH OR BLOCK. That is not style: the
// handler runs on `sys_evt`, whose stack is 2,304 bytes (CONFIG_ESP_SYSTEM_EVENT_TASK_STACK_SIZE),
// and on 2026-08-18 formatting a timeline record there panicked the board with "A stack overflow in
// task sys_evt has been detected". publish() below is a bounds check, five scalar stores and one
// release store.
//
// ⛔ NO SECRET AND NO POINTER FITS IN A RECORD. Event is five fixed-width integers. There is
// deliberately no char array, no pointer and no ESP-IDF object, so no SSID, passphrase, API key,
// BSSID or address string can reach this channel even by mistake (§13.5, §24.4). The IPv4 lease is
// a `std::uint32_t` — a number, not a string — and is converted to its public form on the
// application task, which is the only task allowed to build a NetworkObservation.

namespace prd::network::station {

// What the driver reported, in this component's terms rather than ESP-IDF's.
enum class EventKind : std::uint8_t {
    // WIFI_EVENT_STA_START. Informational: submit() established the station synchronously.
    Started,

    // WIFI_EVENT_STA_CONNECTED. The link is up. NOT a lease.
    Associated,

    // WIFI_EVENT_STA_DISCONNECTED — the ONE documented terminal completion. `reason` carries the
    // access point's own code.
    Disassociated,

    // IP_EVENT_STA_GOT_IP. `ipv4` carries the lease.
    GotIp,

    // IP_EVENT_STA_LOST_IP. Not an association failure — see the LostIp rules in
    // station_intent.hpp.
    LostIp,

    // WIFI_EVENT_STA_STOP. The station interface went away underneath us.
    Stopped,
};

[[nodiscard]] std::string_view log_label(EventKind kind);

// One record. FIXED-WIDTH SCALARS ONLY — see the note above.
struct Event {
    // Assigned by the producer to EVERY observed event, including one it could not store. That is
    // what makes a refusal visible as a hole rather than as a counter nobody read.
    std::uint32_t sequence{0};

    // The request identity current when the driver operation this event completes was issued.
    // §8.1's "request generation or sequence IDs", and the reason candidate B's evidence cannot
    // validate candidate C.
    Tag tag{kNoTag};

    // The lease, in host byte order. Meaningful for GotIp only.
    std::uint32_t ipv4{0};

    EventKind kind{EventKind::Started};

    // WIFI_EVENT_STA_DISCONNECTED's reason code. A small integer describing the access point's
    // answer; it contains nothing about the credential. Meaningful for Disassociated only.
    std::uint8_t reason{0};
};

// What one take() produced.
enum class Delivery : std::uint8_t {
    // Nothing was waiting.
    Empty,

    // A whole record, continuous with the previous one.
    Delivered,

    // ⛔ A SEQUENCE HOLE. One or more records were refused, so this transaction's evidence is
    // incomplete. The record that was read is NOT delivered — it must not be interpreted — and the
    // manager quarantines. §10.7's "controlled recovery policy" for this channel.
    EvidenceLost,
};

// Eight, by owner instruction. Two is the deepest burst the documented station flow can produce
// (CONNECTED then GOT_IP), and a 100 ms consumer tick against a driver that posts at most a few
// events per association leaves the remaining six as headroom rather than as an expected depth.
inline constexpr std::size_t kEventSlots = 8;

// --- The wrap proof, at compile time ---------------------------------------
//
// `write_` and `read_` are free-running counters, never reduced modulo anything. The fill level is
// `write_ - read_`, which unsigned arithmetic computes correctly across the counters' own wrap
// because the true fill never exceeds kEventSlots. The slot index is `counter & kIndexMask`, which
// stays continuous across that wrap only because 2^32 is a whole number of rings — true for every
// power of two. Both facts are asserted rather than argued.
inline constexpr std::size_t kIndexMask = kEventSlots - 1;

static_assert((kEventSlots & kIndexMask) == 0, "kEventSlots must be a power of two so that the "
                                               "slot index stays continuous across a counter wrap");
static_assert(kEventSlots >= 2, "a single-slot ring cannot hold the documented CONNECTED/GOT_IP "
                                "burst");

// --- The atomic width, and what can honestly be asserted about it ----------
//
// ⛔ THE OBVIOUS ASSERTION IS FALSE ON THIS TARGET, AND FINDING OUT WHY IS THE POINT.
// `std::atomic<std::uint32_t>::is_always_lock_free` does NOT hold for the pinned ESP-IDF build:
// CONFIG_SPIRAM=y selects CONFIG_STDATOMIC_S32C1I_SPIRAM_WORKAROUND, which puts
// -mdisable-hardware-atomics on every translation unit because the S32C1I compare-and-swap
// instruction does not work against external PSRAM. GCC therefore stops emitting inline atomic
// instructions and emits calls instead, and the TYPE is no longer unconditionally lock-free —
// because whether it is depends on where the object lives.
//
// What IS true of these objects is asserted at the ESP-IDF boundary, in wifi.cpp, where sdkconfig
// is visible: ESP-IDF's __atomic_load_4/__atomic_store_4 dispatch on the ADDRESS and tail-call the
// S32C1I hardware routine for anything outside external RAM. Writing an assertion here that quietly
// passed on the host and could not be evaluated on the target would be worse than none.
//
// These two hold everywhere and are the part that belongs to the data structure: the counters carry
// no embedded lock word and no extra alignment, so the channel is exactly what it appears to be.
static_assert(sizeof(std::atomic<std::uint32_t>) == sizeof(std::uint32_t),
              "an atomic counter with a lock word inside it is not the channel this class "
              "describes");
static_assert(alignof(std::atomic<std::uint32_t>) == alignof(std::uint32_t),
              "an atomic counter that needs more alignment than its own width is not the channel "
              "this class describes");

// A fixed-capacity single-producer/single-consumer ring.
//
// ⛔ ONE PRODUCER AND ONE CONSUMER, AND BOTH ARE STRUCTURAL RATHER THAN AGREED. The producer is
// ESP-IDF's default event loop task, which posts WIFI_EVENT and IP_EVENT from one task. The
// consumer is whoever calls station::service(), and §10.2 gives that to the application task alone.
// No mutex appears below because there is nothing for two writers to contend over.
class EventRing {
  public:
    // --- Producer side ----------------------------------------------------
    //
    // Called from the ESP-IDF event handler. No allocation, no formatting, no logging, no driver
    // call, no block.
    //
    // ⬅️ A WHOLE RECORD RATHER THAN FOUR SCALARS, WHICH IS NOT A STYLE CHOICE. Three of those
    // scalars — the identity, the reason and the lease — are all unsigned integers, and only their
    // ORDER at the call site would have told them apart. Swapping a disconnect reason with a lease
    // would compile, and nothing downstream could notice. clang-tidy's
    // bugprone-easily-swappable-parameters is what asked for this; the handler now names each field
    // as it fills it.
    //
    // `record.sequence` is ignored and overwritten: only the ring can number EVERY observed event,
    // including one it goes on to refuse, and that numbering is the whole loss-detection mechanism.
    void publish(Event record);

    // --- Consumer side ----------------------------------------------------
    //
    // Non-blocking. On Delivered, `out` holds a whole record. On EvidenceLost the record that was
    // read is consumed and discarded, and the expectation resynchronises — so the gap is reported
    // exactly once and later records are still delivered, which is what lets a late terminal event
    // clear an owed completion without reviving the transaction it belonged to.
    //
    // ⛔ A REFUSAL IS REPORTED EVEN IF NOTHING IS EVER PUBLISHED AFTER IT — R1. Detecting a hole by
    // reading its SUCCESSOR is only half a detector: the record that matters most is the LAST one,
    // and a refused final record left no successor to notice it by. §10.7 requires the counter AND a
    // controlled recovery policy, and a policy that fires only when unrelated later traffic happens
    // to arrive is not one. So an empty ring whose refusal counter has moved past what the consumer
    // has acknowledged returns EvidenceLost once per refusal, and the quarantine follows from that
    // exactly as it does for a hole between two stored records.
    [[nodiscard]] Delivery take(Event& out);

    // How many records the ring refused. SATURATES: 0 means "never happened", so a wrap would
    // corrupt the one reading a reader must be able to trust.
    [[nodiscard]] std::uint32_t refused() const;

    // How many of those refusals the consumer has reported — §10.7's other half, and the property
    // R1 exists to make true. When this equals refused(), nothing was lost without the recovery
    // policy being given the chance to act on it.
    //
    // ⬅️ ONE HOLE CAN ACCOUNT FOR SEVERAL REFUSALS, which is why this is not simply a count of
    // EvidenceLost returns: a gap between two stored records reveals its whole width at once.
    [[nodiscard]] std::uint32_t acknowledged() const;

    // Diagnostic only.
    [[nodiscard]] std::uint32_t pending() const;

  private:
    std::array<Event, kEventSlots> slots_{};

    // ⛔ PRODUCER-ONLY, AND THERE IS DELIBERATELY NO WAY FOR THE CONSUMER TO TOUCH IT. Not atomic
    // because nothing else reads or writes it; its value crosses to the consumer inside a record,
    // under the release/acquire pair below. This is also why there is no reset()/clear() on this
    // class: resynchronising the expectation from the consumer would be exactly that cross-task
    // access, and a re-submission has no reason to discard evidence — a stale record's tag already
    // makes it harmless.
    std::uint32_t produced_{0};

    // Consumer-only, for the mirror reason.
    std::uint32_t expected_{0};

    // ⛔ HOW MANY REFUSALS THE CONSUMER HAS ALREADY REPORTED — consumer-only, and what makes
    // "exactly once" true from both directions. A hole between two stored records reveals
    // `sequence - expected` refusals at once and acknowledges all of them; an empty ring
    // acknowledges one at a time. Neither can report a refusal the other already has, which is what
    // stops a successor arriving after an empty-ring report from double-counting it.
    std::uint32_t acknowledged_{0};

    std::atomic<std::uint32_t> write_{0};
    std::atomic<std::uint32_t> read_{0};
    std::atomic<std::uint32_t> refused_{0};
};

} // namespace prd::network::station
