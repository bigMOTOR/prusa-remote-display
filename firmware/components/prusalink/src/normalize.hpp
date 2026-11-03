// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstdint>
#include <string_view>

#include "app_model/events.hpp"
#include "app_model/printer_data.hpp"
#include "app_model/reducer.hpp"
#include "prusalink/prusalink.hpp"

#include "dto.hpp"

// DTO → domain observation (Architecture Spec §5.8's `prusalink_normalizer`, §9.1
// step 6, §9.3's rules).
//
// This is the seam. The types produced here are the same `app_model` observation
// payloads the fake source has been dispatching since Milestone 3, so nothing
// downstream — reducer, store, projection, scenes — can tell a live printer from a
// fixture. That property is what makes this checkpoint one component wide instead of
// a rewrite, and it is why no new backend interface is introduced: there is already
// exactly one way in.
//
// Pure: no clock, no allocation, no I/O. The sequence number and any timestamp arrive
// from the caller.

namespace prd::prusalink {

// API Research Report §5.1's documented values, mapped to §7.7's normalized states.
//
// The comparison is exact and case-sensitive against the published spelling. An
// unrecognised value becomes Unknown and the caller keeps the raw string (§24.8), so a
// firmware that introduces a new state degrades to "state not understood" rather than
// to a wrong state confidently displayed.
//
// READY maps to Ready and IDLE to Idle. §5.1 leaves this open — "idle or ready,
// depending on product testing" — and both exist in the domain model, so the
// distinction is preserved here and resolved where it is presented:
// subject_from_printer_state() already folds both to SubjectState::Ready. Discarding
// it here would throw away a fact the projection may later want.
[[nodiscard]] app_model::PrinterState state_from(std::string_view raw);

// Builds the snapshot observation from a decoded status response.
//
// `generation` identifies the producing worker and `sequence` orders snapshots within it. §8.1's
// rejection rule applies inside a generation; a generation change resets the ordering, which is
// what lets a replacement worker's first snapshot be accepted at once rather than losing to the
// previous worker's watermark (app_model::snapshot_is_current).
//
// Temperatures are carried through unclamped within the representable range (§9.3: "clamp only at
// the UI boundary").
// A value that is not finite is treated as not reported rather than as a reading,
// which is classification and not clamping: an infinity is not a temperature.
[[nodiscard]] app_model::PrinterSnapshotObservation snapshot_from(const StatusDto& dto,
                                                                  app_model::SnapshotOrder order);

// ⬅️ `job_from()` LIVED HERE AND WAS RETIRED 2026-09-03. It produced a general 348-byte
// `app_model::JobObservation` from a status response, of which ~224 bytes were three bounded
// strings this endpoint never fills, plus a duplicate of the printer half's ordering and state.
// status_from() below fills the endpoint-specific `StatusJobTelemetry` instead, and the difference
// is 1248 bytes of STARTUP HEAP across the application event queue's twelve slots — measured. That
// queue comes from xQueueCreate(), so the cost is FreeRTOS heap taken at startup rather than
// `.bss`, on a device whose internal-heap low-water is near 916 bytes: the same pool.
//
// The general JobObservation path is untouched and still belongs to the fixtures.

// ONE DECODED STATUS RESPONSE, AS THE ONE OBSERVATION THE STORE APPLIES.
//
// ⛔ THE ONLY PLACE A StatusObservation IS BUILT. The poll worker, the diagnostic timeline flow and
// the host replay all call this; none of them assembles the printer and job halves itself. That is
// the whole point — three hand-written assemblies of "snapshot then job" is what this replaced,
// and they had already drifted into claiming an atomicity the code did not have.
//
// The printer half carries the response's ordering identity and its state; the job half carries
// only the four values §6.1's `job` object actually supplies. `job.reported` is
// job_telemetry_of()'s answer.
[[nodiscard]] app_model::StatusObservation status_from(const StatusDto& dto,
                                                       app_model::SnapshotOrder order);

// Which of the three job answers one decoded status response carries.
//
// ⛔ A DECISION, SO IT LIVES WHERE A TEST CAN REACH IT. The worker's poll loop is the one
// translation unit in this component the host suite cannot compile — it is the FreeRTOS adapter —
// so a publication rule written inline there could be reviewed and never executed. Exactly the
// argument wifi_restore.hpp's route_for() records for the same class of one-line policy, and the
// same remedy: three tokens of routing in an untestable file is how a whole consumer came to be
// silently skipped once already in this project.
//
// Never None: a response that decoded said something about a job, even if what it said was "there
// is not one". None is the answer for a cycle that produced no usable response at all, which is a
// fact about the connection rather than about this DTO.
[[nodiscard]] JobTelemetry job_telemetry_of(const StatusDto& dto);

// The largest percentage the domain's `ProgressValue` can carry, and the largest duration its
// `DurationValue` can.
//
// ⬅️ WHY A REPRESENTABILITY BOUND RATHER THAN THE CONTRACT'S 0...100. §7.8 says progress is
// "clamped to 0...100 only for display" and keeps the printer's original figure, and the reducer
// counts an over-100 reading in `out_of_range_progress_seen` rather than correcting it. So a
// reading of 105 is STORED, counted and clamped at the projection — the accepted behaviour — while
// one that cannot be held in the domain's byte at all is not a weaker reading, it is unusable, and
// is reported as not known. Same argument as temperature_is_representable() above, at the other
// end of the pipeline.
inline constexpr double kMaxReportedProgressPercent = 255.0;
inline constexpr double kMaxReportedDurationSeconds = 4294967295.0;

// The range a reported temperature must fall in to be treated as a reading.
//
// ⬅️ WHY A RANGE AND NOT JUST isfinite(). The domain stores a float; the wire carries a double. A
// finite double such as 1e300 passes every finiteness check and then becomes an INFINITE float on
// conversion, which propagates through every comparison the projection makes. So the guard is
// stated over the value, not over its representation. The bounds are deliberately far outside
// anything a 3D printer can report — this rejects nonsense, it does not clamp plausible readings,
// which §9.3 reserves for the UI boundary.
inline constexpr double kMinReportedCelsius = -273.15;
inline constexpr double kMaxReportedCelsius = 2000.0;

// Whether a reported value may be represented as a temperature at all.
[[nodiscard]] bool temperature_is_representable(double celsius);

// Fills the identity fields a version response can establish. Fields the response
// does not carry are left untouched, so a caller may merge into an existing identity
// without erasing what an earlier response established (§7.6: every value except the
// resolved display_name may be absent).
void apply_version(const VersionDto& dto, app_model::PrinterIdentity& identity);

} // namespace prd::prusalink
