// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstdint>

// OWNERSHIP OF THE PROVISIONING SUBMISSION AS IT CROSSES TASKS — Architecture Spec §10.7, §13.5.
//
// INTERNAL HEADER of app_core. Pure — no ESP-IDF, no FreeRTOS, no clock, no allocator — so the
// ownership transaction below is host-tested against the same code the firmware runs. The binding
// supplies four operations and obeys the result; it decides nothing.
//
// ⛔ WHAT THIS CLOSES. Until this checkpoint the submission crossed the task boundary BY VALUE
// through a depth-one FreeRTOS queue: note_submission() copied a whole provisioning_flow::Submission
// — Wi-Fi passphrase and printer API key included — into the queue's internal storage with
// xQueueOverwrite(), and the application task copied it back out. Both endpoint copies were wiped.
// The queue's storage was not, and nothing can wipe it from outside: FreeRTOS's
// prvCopyDataFromQueue() memcpy's OUT of the storage and advances its read pointer, and
// xQueueGenericReset() resets pointers and counters only. Neither zeroes the bytes. So the
// credential stayed in that buffer for the life of the queue, and §13.5's "embedded in crash dumps
// where avoidable" is the binding clause with coredump-to-flash enabled.
//
// ⬅️ SO THE QUEUE CARRIES A POINTER AND NOTHING ELSE. One heap object per submission, owned by
// exactly one holder at every instant, wiped before it is freed.
//
// ⛔ THE ONE INVARIANT, AND EVERYTHING HERE EXISTS TO MAKE IT MECHANICAL.
//
//     At every instant, each allocated submission is owned by exactly one of:
//       - the server-task local owner (inside publish(), before try_put succeeds);
//       - the queue;
//       - the app-task local owner (inside consume() or drain()).
//
// A queue operation's RETURN VALUE is the only thing that moves ownership. No successful put
// retains local ownership. No successful take leaves ownership with the queue. No failure path
// assumes a transfer happened. That is why every op below returns bool and why not one of them is
// called for its side effect alone.
//
// ⛔ AND WHY consume() TAKES A CALLBACK RATHER THAN RETURNING THE POINTER. The caller never holds
// it, so the caller cannot leak it: the release is on this side of the call, so a stage refusal, an
// early return or any future branch inside the callback still releases exactly once. Returning a
// raw pointer would have made every one of those paths a place to forget. Exceptions are off
// (CONFIG_COMPILER_CXX_EXCEPTIONS is not set), so the callback either returns or the device is
// already dead.

namespace prd::app_core::submission_handoff {

// --- The Ops contract ------------------------------------------------------
//
// A binding supplies a type with:
//
//   using Payload = <the submission type>;
//
//   Payload* allocate();
//       A zero-initialised Payload, or nullptr. MUST NOT throw and MUST NOT abort: the caller is
//       an HTTP request handler and a refused allocation is an ordinary 503, not a fault.
//
//   void wipe_and_release(Payload*);
//       Overwrite EVERY byte, then free. Called exactly once per allocated object, never with
//       nullptr. The wipe must not be elidable — the production binding stores through a volatile
//       pointer for the same reason wipe_bytes() does.
//
//   bool try_take(Payload*& out);
//       Non-blocking. Returns true iff ownership moved from the queue to `out`. On false, `out` is
//       untouched and the queue keeps whatever it had.
//
//   bool try_put(Payload* owned);
//       Non-blocking. Returns true iff ownership moved from the caller to the queue. On false the
//       caller still owns `owned` and MUST dispose of it.
//
// No operation may block, take a lock, or call into HTTP or the driver. §10.7's "no HTTP/network
// operation may occur while holding a synchronization primitive" is satisfied trivially, because
// there is no primitive to hold: each queue call is atomic on its own and nothing is held across
// two of them.

// Whether the submission reached the application task's mailbox.
//
// THE SINK'S ANSWER, AND THE HTTP RESPONSE'S. A request whose body validated but whose handoff was
// refused must not be told "accepted": the browser would sit waiting for a validation that will
// never begin. See provisioning::SubmissionHandoff, which this maps onto.
enum class Outcome : std::uint8_t {
    Accepted,
    Unavailable,
};

// Why a handoff did not happen. NON-SECRET BY CONSTRUCTION — a class of failure, never a value.
// §13.5 and §24.4 forbid the submitted bytes anywhere in diagnostics, and a caller holding one of
// these has nothing it could disclose even if it tried.
enum class Failure : std::uint8_t {
    None,

    // No object could be allocated. Nothing was constructed and the queue was never touched, so a
    // submission already pending is still pending and still valid.
    AllocationFailed,

    // The object was allocated but a bounded field would not hold its value. Same guarantee as
    // above: the queue was not touched. §16.4's "abort the current request safely".
    ConversionFailed,

    // try_put refused with nothing displaced — the mailbox would not take it and there was no
    // pending submission to preserve.
    QueueUnavailable,

    // ⛔ try_put refused AFTER the pending submission had been acquired, and the pending one was put
    // back. The NEW submission is the one that was lost; the OLD one is intact and still pending.
    // The user's retry is the recovery, and the previous transaction was not damaged to serve it.
    NewRejectedOldRestored,

    // ⛔ THE WORST CASE, AND IT IS STILL DEFINED. try_put refused for the new object AND restoring
    // the displaced one refused too, so neither could be left in the mailbox. Both are released
    // here, exactly once each, and the caller is told Unavailable. Nothing dangles, nothing leaks,
    // and no pointer to a freed object survives in the queue.
    //
    // ⬅️ UNREACHABLE THROUGH A CONFORMING DEPTH-ONE MAILBOX, AND IMPLEMENTED ANYWAY. On depth one,
    // try_take failing means the mailbox is empty, and try_take succeeding leaves it empty — so the
    // put that follows has a free slot either way and cannot refuse for capacity. This branch
    // therefore describes a mailbox that broke its own contract. It is handled and host-tested by
    // injection rather than argued away, because "cannot happen" is not a disposal policy.
    NewAndOldReleased,
};

struct Report {
    Outcome outcome{Outcome::Unavailable};
    Failure failure{Failure::None};

    // Whether a submission that was still pending got superseded by this one. Owner ruling D-6: a
    // new Wi-Fi submission supersedes an attempt still running, so displacing one is ordinary and
    // is worth a diagnostic rather than a warning.
    bool displaced{false};
};

// Builds one owned submission and makes it THE pending submission, replacing any predecessor.
//
// `fill` is called with the freshly allocated object and returns false if any bounded field would
// not hold its value. It runs BEFORE the mailbox is touched, which is the whole ordering rule: a
// submission that cannot be built completely must not be able to disturb one that already exists.
template <class Ops, class Fill> [[nodiscard]] Report publish(Ops& ops, Fill&& fill) {
    using Payload = typename Ops::Payload;

    // --- Step 1. Build it completely. The mailbox is not touched on either failure path. ---
    Payload* const fresh = ops.allocate();
    if (fresh == nullptr) {
        return {Outcome::Unavailable, Failure::AllocationFailed, false};
    }

    if (!fill(*fresh)) {
        ops.wipe_and_release(fresh);
        return {Outcome::Unavailable, Failure::ConversionFailed, false};
    }

    // --- Step 2. Acquire the predecessor, if one is still pending. ---
    //
    // ⛔ AND A FAILED TAKE IS NOT AN ERROR. It means the application task already received the
    // previous submission and owns it, which is the documented race and not double ownership: that
    // transaction is somebody else's now, and this object simply becomes the new pending one.
    Payload* displaced = nullptr;
    const bool had_pending = ops.try_take(displaced);

    // --- Step 3. Offer the new one. ---
    if (ops.try_put(fresh)) {
        // --- Step 4. The handoff moved. ONLY NOW may the predecessor go. ---
        if (had_pending) {
            ops.wipe_and_release(displaced);
        }

        return {Outcome::Accepted, Failure::None, had_pending};
    }

    // --- Step 5. The put refused. Put the predecessor back rather than destroying it. ---
    if (had_pending && ops.try_put(displaced)) {
        ops.wipe_and_release(fresh);
        return {Outcome::Unavailable, Failure::NewRejectedOldRestored, false};
    }

    // --- Step 6. Nothing can be left pending. Release everything held, exactly once each. ---
    ops.wipe_and_release(fresh);

    if (had_pending) {
        ops.wipe_and_release(displaced);
        return {Outcome::Unavailable, Failure::NewAndOldReleased, false};
    }

    return {Outcome::Unavailable, Failure::QueueUnavailable, false};
}

// Takes the pending submission, hands it to `apply`, then wipes and releases it — on every path out
// of `apply`, including an early return or a stage refusal.
//
// Returns false when nothing was pending. `apply` sees a reference and must copy whatever it needs
// into application-owned state before returning; the object does not outlive this call.
template <class Ops, class Apply> [[nodiscard]] bool consume(Ops& ops, Apply&& apply) {
    using Payload = typename Ops::Payload;

    Payload* taken = nullptr;
    if (!ops.try_take(taken)) {
        return false;
    }

    apply(*taken);

    // ⛔ UNCONDITIONAL, AND THAT IS THE DESIGN. `apply` cannot skip it, cannot forget it on a new
    // branch, and cannot be given the pointer to mishandle.
    ops.wipe_and_release(taken);
    return true;
}

// Empties the mailbox, wiping and releasing every submission it held. Returns how many.
//
// For the exclusive-operation barrier: a submission the server accepted but the application task
// has not taken must not survive an erase. The count is what makes the release visible in a
// diagnostic instead of silent.
template <class Ops> [[nodiscard]] unsigned drain(Ops& ops) {
    using Payload = typename Ops::Payload;

    unsigned released = 0;
    Payload* taken = nullptr;

    while (ops.try_take(taken)) {
        ops.wipe_and_release(taken);
        taken = nullptr;
        ++released;
    }

    return released;
}

[[nodiscard]] constexpr const char* log_label(const Failure failure) {
    switch (failure) {
    case Failure::None:
        return "none";
    case Failure::AllocationFailed:
        return "allocation-failed";
    case Failure::ConversionFailed:
        return "conversion-failed";
    case Failure::QueueUnavailable:
        return "queue-unavailable";
    case Failure::NewRejectedOldRestored:
        return "new-rejected-old-restored";
    case Failure::NewAndOldReleased:
        return "new-and-old-released";
    }

    return "unknown";
}

} // namespace prd::app_core::submission_handoff
