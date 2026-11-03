// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstdint>

#include "prusalink/transaction.hpp"

// Who owns the request worker, and for how long.
//
// ⛔ THE PROBLEM THIS SOLVES. The worker must be stoppable without the application task ever
// blocking: that task is the store's only dispatcher and the thing that services button gestures,
// and a request can sit in a socket for its whole budget. So stop() cannot join. A non-blocking
// stop leaves four ways to be wrong, and the 2026-08-23 audit found all four in the previous
// implementation:
//
//   1. **Stale publication.** A worker finishes a request for a configuration that has since been
//      replaced, and publishes a printer state belonging to the old printer.
//   2. **Replaced state.** A new start() overwrites a shared target while the old worker is still
//      reading it, so the old worker issues requests against the new host.
//   3. **Two workers at once.** A new worker begins while the old one is still in a socket call.
//   4. **Lease adoption.** The worker sampled the CURRENT lease at task entry rather than being
//      told which lease it holds — so a stop() between xTaskCreate() and entry advanced the lease
//      and the worker then read the new value as its own, believing it was still current.
//
// ⬅️ HOW THEY ARE CLOSED NOW.
//
//   • Hazard 4 was the design error, and it is gone: a worker is HANDED its generation in an
//     immutable launch context through the task parameter, and this module never offers a way to
//     ask "what is the current generation" from inside a worker. There is no sampling to get wrong.
//   • Hazard 2 is gone with it — the target lives in that same per-worker context, never in a
//     shared slot.
//   • Hazard 3 is the one-worker invariant below, held by an atomic flag cleared only by the
//     departing worker.
//   • Hazard 1 is closed END TO END rather than by a lock. The worker checks its generation before
//     publishing, and — because a check followed by a callback can never be atomic without holding
//     a lock across foreign code — every report also CARRIES its generation, and the application
//     task revalidates it before acting. The narrow check-then-publish window therefore becomes
//     harmless rather than impossible, which is the correct trade: a mutex held across a sink
//     callback into app_core would invert the very ordering §10.2 exists to protect.
//
// Everything here is pure so the interleavings can be stated as tests. What is NOT claimed is that
// pure arithmetic proves concurrency: the production worker's use of these answers is exercised
// through the extracted sequencing units, and the report says so plainly.

namespace prd::prusalink::lifecycle {

// The identity types are the public ones from transaction.hpp: a report carries them across the
// component boundary, so declaring a second private copy here would be two sources of one truth.
using prd::prusalink::Generation;

// The generation nothing holds. A worker never runs with this, so a report stamped with it is
// always rejectable — which is what makes "stopped" representable without a second flag.
inline constexpr Generation kNoLease = kNoGeneration;

// The generation a new start() should take.
//
// Wraps past the maximum straight back to 1 rather than to kNoLease, so a device that has been
// reconfigured 2^32 times cannot hand out a generation that reads as "nobody".
[[nodiscard]] Generation next_generation(Generation current);

// Whether a worker holding `mine` may publish now.
//
// False once the current generation has moved on, and false for kNoLease. Asked by the worker
// before publishing AND by the application task on receipt — the second call is what makes the
// first one's non-atomicity survivable.
[[nodiscard]] bool may_publish(Generation mine, Generation current);

// Whether a worker holding `mine` should stop.
//
// The exact complement of may_publish, deliberately named separately: they are asked at different
// points and a reader must not have to remember that one implies the other.
[[nodiscard]] bool should_exit(Generation mine, Generation current);

// Whether start() may create a worker.
//
// ⛔ THE ONE-WORKER INVARIANT. False while any worker is alive, including one that has been asked
// to stop and has not yet noticed. A caller that is refused simply tries again on its next tick,
// which costs nothing and blocks nobody. This is what bounds concurrent sockets to one (§10.4) —
// and it is only safe because a worker cannot outlive its request budget.
[[nodiscard]] bool may_start(bool worker_alive);

} // namespace prd::prusalink::lifecycle
