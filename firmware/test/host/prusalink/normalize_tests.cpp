// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include <cmath>
#include <limits>

#include <gtest/gtest.h>

#include <chrono>

#include "app_model/printer_data.hpp"
#include "app_model/reducer.hpp"

#include "normalize.hpp"

// DTO → domain observation — Milestone 4's first vertical slice.
//
// ⛔ WHAT THESE PROTECT. The mapping from a vendor string to a product state is where a
// wrong answer is most expensive and least visible: the panel would show a confident,
// wrong printer state, which is the one thing UIAD 001 §6 and §24.18 exist to prevent.
// An unrecognised value must degrade to Unknown with the raw string kept, never to a
// neighbouring state that happens to be close.

namespace prd::prusalink {
namespace {

[[nodiscard]] StatusDto with_state(const std::string_view raw) {
    StatusDto dto;
    (void)dto.printer_state.assign(raw);
    return dto;
}

TEST(NormalizeStateTests, MapsEveryDocumentedValue) {
    // API Research Report §5.1's table, exhaustively.
    EXPECT_EQ(state_from("IDLE"), app_model::PrinterState::Idle);
    EXPECT_EQ(state_from("READY"), app_model::PrinterState::Ready);
    EXPECT_EQ(state_from("BUSY"), app_model::PrinterState::Preparing);
    EXPECT_EQ(state_from("PRINTING"), app_model::PrinterState::Printing);
    EXPECT_EQ(state_from("PAUSED"), app_model::PrinterState::Paused);
    EXPECT_EQ(state_from("FINISHED"), app_model::PrinterState::Finished);
    EXPECT_EQ(state_from("STOPPED"), app_model::PrinterState::Stopped);
    EXPECT_EQ(state_from("ATTENTION"), app_model::PrinterState::Attention);
    EXPECT_EQ(state_from("ERROR"), app_model::PrinterState::Error);
}

TEST(NormalizeStateTests, UnknownValuesBecomeUnknownAndNotANeighbour) {
    // A future or misspelled value, a case variant, and a substring of a real one. Each
    // must be Unknown: guessing is what §24.8 and §24.18 forbid.
    EXPECT_EQ(state_from("PRINTING_PAUSED"), app_model::PrinterState::Unknown);
    EXPECT_EQ(state_from("idle"), app_model::PrinterState::Unknown);
    EXPECT_EQ(state_from("Idle"), app_model::PrinterState::Unknown);
    EXPECT_EQ(state_from("PRINT"), app_model::PrinterState::Unknown);
    EXPECT_EQ(state_from("CALIBRATING"), app_model::PrinterState::Unknown);
    EXPECT_EQ(state_from(""), app_model::PrinterState::Unknown);
    EXPECT_EQ(state_from(" IDLE"), app_model::PrinterState::Unknown);
}

TEST(NormalizeStateTests, OfflineIsUnreachableFromAPrinterState) {
    // §7.5's closing rule and §24.5: connection state is never inferred from operational
    // state. Offline is a connection fact, so no printer string may produce it.
    constexpr const char* kEveryDocumented[] = {
        "IDLE", "READY", "BUSY", "PRINTING", "PAUSED", "FINISHED", "STOPPED", "ATTENTION", "ERROR"};

    for (const char* raw : kEveryDocumented) {
        EXPECT_NE(state_from(raw), app_model::PrinterState::Offline) << raw;
    }

    EXPECT_NE(state_from("OFFLINE"), app_model::PrinterState::Offline);
    EXPECT_EQ(state_from("OFFLINE"), app_model::PrinterState::Unknown);
}

TEST(NormalizeSnapshotTests, KeepsTheRawStringWhateverTheMapping) {
    // §24.8: an unknown enum value is preserved for diagnostics rather than discarded —
    // it is the only evidence of what a new firmware actually said.
    const app_model::PrinterSnapshotObservation unknown =
        snapshot_from(with_state("WARMING"), {1, 1});
    EXPECT_EQ(unknown.normalized, app_model::PrinterState::Unknown);
    EXPECT_EQ(unknown.raw.view(), "WARMING");

    const app_model::PrinterSnapshotObservation known = snapshot_from(with_state("IDLE"), {1, 2});
    EXPECT_EQ(known.normalized, app_model::PrinterState::Idle);
    EXPECT_EQ(known.raw.view(), "IDLE");
}

TEST(NormalizeSnapshotTests, DerivesBusyAndAttentionFromTheState) {
    // isBusy is about the machine being occupied, which is why Paused counts: the job
    // still owns the printer.
    EXPECT_TRUE(snapshot_from(with_state("PRINTING"), {1, 1}).is_busy);
    EXPECT_TRUE(snapshot_from(with_state("BUSY"), {1, 1}).is_busy);
    EXPECT_TRUE(snapshot_from(with_state("PAUSED"), {1, 1}).is_busy);

    EXPECT_FALSE(snapshot_from(with_state("IDLE"), {1, 1}).is_busy);
    EXPECT_FALSE(snapshot_from(with_state("READY"), {1, 1}).is_busy);
    EXPECT_FALSE(snapshot_from(with_state("FINISHED"), {1, 1}).is_busy);

    EXPECT_TRUE(snapshot_from(with_state("ATTENTION"), {1, 1}).requires_attention);
    EXPECT_TRUE(snapshot_from(with_state("ERROR"), {1, 1}).requires_attention);

    EXPECT_FALSE(snapshot_from(with_state("IDLE"), {1, 1}).requires_attention);
    EXPECT_FALSE(snapshot_from(with_state("PRINTING"), {1, 1}).requires_attention);

    // An unknown state claims nothing. Asserting attention for a state nobody understands
    // would put a warning on the panel with no fact behind it.
    const app_model::PrinterSnapshotObservation unknown = snapshot_from(with_state("XYZ"), {1, 1});
    EXPECT_FALSE(unknown.is_busy);
    EXPECT_FALSE(unknown.requires_attention);
}

TEST(NormalizeSnapshotTests, CarriesTemperaturesThroughUnclamped) {
    StatusDto dto = with_state("PRINTING");
    dto.temp_nozzle_known = true;
    dto.temp_nozzle = 219.4;
    dto.target_nozzle_known = true;
    dto.target_nozzle = 220.0;
    dto.temp_bed_known = true;
    dto.temp_bed = 0.0;

    const app_model::PrinterSnapshotObservation snapshot = snapshot_from(dto, {1, 1});

    ASSERT_TRUE(snapshot.nozzle_current.known);
    EXPECT_FLOAT_EQ(snapshot.nozzle_current.celsius, 219.4F);
    ASSERT_TRUE(snapshot.nozzle_target.known);
    EXPECT_FLOAT_EQ(snapshot.nozzle_target.celsius, 220.0F);

    // A reported zero is a reading. §9.3 keeps it distinct from absence, and the projection
    // — not this layer — decides what to show.
    ASSERT_TRUE(snapshot.bed_current.known);
    EXPECT_FLOAT_EQ(snapshot.bed_current.celsius, 0.0F);

    EXPECT_FALSE(snapshot.bed_target.known);

    // §17.6: the documented status schema carries no chamber telemetry, so nothing may
    // populate it.
    EXPECT_FALSE(snapshot.chamber_current.known);
    EXPECT_FALSE(snapshot.chamber_target.known);
}

TEST(NormalizeSnapshotTests, TreatsANonFiniteReadingAsNotReported) {
    StatusDto dto = with_state("IDLE");
    dto.temp_nozzle_known = true;
    dto.temp_nozzle = std::numeric_limits<double>::infinity();
    dto.temp_bed_known = true;
    dto.temp_bed = std::numeric_limits<double>::quiet_NaN();

    const app_model::PrinterSnapshotObservation snapshot = snapshot_from(dto, {1, 1});

    // Classification, not clamping: an infinity is not a temperature, and a NaN would
    // propagate into every comparison the projection makes.
    EXPECT_FALSE(snapshot.nozzle_current.known);
    EXPECT_FALSE(snapshot.bed_current.known);

    // POSITIVE CONTROL: an extreme but finite value is carried, so the rejection above is
    // about finiteness and not about magnitude.
    StatusDto extreme = with_state("IDLE");
    extreme.temp_nozzle_known = true;
    extreme.temp_nozzle = 999.0;
    EXPECT_TRUE(snapshot_from(extreme, {1, 1}).nozzle_current.known);
}

TEST(NormalizeSnapshotTests, CarriesTheCallersSequence) {
    // §8.1 rejects a snapshot whose sequence is not greater than the last accepted one, so
    // this field is what makes an out-of-order response harmless.
    EXPECT_EQ(snapshot_from(with_state("IDLE"), {1, 1}).sequence, 1U);
    EXPECT_EQ(snapshot_from(with_state("IDLE"), {1, 4096}).sequence, 4096U);
}

TEST(NormalizeIdentityTests, FillsOnlyWhatTheResponseCarried) {
    VersionDto dto;
    (void)dto.api.assign("2.0.0");
    (void)dto.firmware.assign("6.5.7");
    (void)dto.text.assign("PrusaLink");

    app_model::PrinterIdentity identity;
    apply_version(dto, identity);

    EXPECT_EQ(identity.api_version.view(), "2.0.0");
    EXPECT_EQ(identity.firmware_version.view(), "6.5.7");
    EXPECT_EQ(identity.model.view(), "PrusaLink");

    // §7.6: nothing here resolves display_name — that is the projection's fallback rule.
    EXPECT_TRUE(identity.display_name.empty());

    // A later response that omits a field must not erase what an earlier one established.
    VersionDto partial;
    (void)partial.api.assign("2.0.1");
    apply_version(partial, identity);

    EXPECT_EQ(identity.api_version.view(), "2.0.1");
    EXPECT_EQ(identity.firmware_version.view(), "6.5.7");
    EXPECT_EQ(identity.model.view(), "PrusaLink");
}

TEST(NormalizeIdentityTests, NeverPopulatesTheSerial) {
    // §13.5 and the evidence rules treat a device identifier as redactable, and nothing in
    // the version response is one. This pins that no field leaks into it.
    VersionDto dto;
    (void)dto.api.assign("2.0.0");
    (void)dto.firmware.assign("6.5.7");
    (void)dto.text.assign("PrusaLink");

    app_model::PrinterIdentity identity;
    apply_version(dto, identity);

    EXPECT_TRUE(identity.serial.empty());
    EXPECT_TRUE(identity.hostname.empty());
}

// --- status_from(): the DTO's job block → the compact status telemetry ---------
//
// ⛔ WHAT THESE PROTECT. This is where a wire value becomes a figure on the panel, so it is where
// an unusable reading must be refused rather than coerced. A negative percentage is not 0 %, an
// infinite duration is not a long one, and neither is a weaker version of a real reading: turning
// either into a number would put a figure on the panel that the machine never stated (§24.18).
// The reverse error matters as much — 0 % and 0 seconds ARE readings, and §9.3 keeps them
// distinguishable from absence.

[[nodiscard]] StatusDto printing_job() {
    StatusDto dto;
    (void)dto.printer_state.assign("PRINTING");
    dto.job_present = true;
    dto.job_id_known = true;
    (void)dto.job_id.assign("173");
    dto.progress_known = true;
    dto.progress_percent = 41.6;
    dto.time_remaining_known = true;
    dto.time_remaining_seconds = 2280.0;
    dto.time_printing_known = true;
    dto.time_printing_seconds = 1680.0;
    return dto;
}

TEST(PrusaLinkJobNormalizeTests, TheContractsFieldsBecomeTheDomainsValues) {
    const app_model::StatusObservation reading = status_from(printing_job(), {4, 11});
    const app_model::StatusJobTelemetry& job = reading.job;

    EXPECT_TRUE(job.reported);
    EXPECT_EQ(job.id.view(), "173");

    // ⛔ THE ORDERING AND THE STATE LIVE ON THE PRINTER HALF, AND THERE IS NO SECOND COPY. That is
    // what keeps the status payload compact enough to leave the event queue's size unchanged.
    EXPECT_EQ(reading.printer.generation, 4U);
    EXPECT_EQ(reading.printer.sequence, 11U);
    EXPECT_EQ(reading.printer.normalized, app_model::PrinterState::Printing);
    EXPECT_EQ(reading.printer.raw.view(), "PRINTING");

    EXPECT_TRUE(job.progress.known);
    EXPECT_EQ(job.progress.percent, 42U) << "41.6 rounds to 42; a print at 99.6 is not at 99";

    EXPECT_TRUE(job.remaining.known);
    EXPECT_EQ(job.remaining.seconds, 2280U);

    EXPECT_TRUE(job.elapsed.known);
    EXPECT_EQ(job.elapsed.seconds, 1680U);
}

TEST(PrusaLinkJobNormalizeTests, TheStatusPayloadHasNowhereToPutANameAFileOrALayer) {
    // ⛔ STRUCTURAL NOW, NOT A CONVENTION. `display_name`, `original_file_name` and `source_path`
    // belong to `GET /api/v1/job`, which is separately bounded and unread; the status endpoint
    // exposes no current or total layer at all, and deriving one from `axis_z` would be invented.
    // The first version of this checkpoint carried the general JobObservation here and had to
    // ASSERT those fields stayed empty. StatusJobTelemetry does not have them, so no later edit
    // can fill them from a guess — and that is also what keeps 1248 bytes out of the event queue.
    static_assert(sizeof(app_model::StatusJobTelemetry) < 64,
                  "the compact payload must stay too small to carry a name, a file or a path");
    static_assert(sizeof(app_model::StatusJobTelemetry) * 4 < sizeof(app_model::JobObservation),
                  "and materially smaller than the general record it deliberately is not");

    // The domain's own copies are therefore left untouched: nothing supplies them, so a fresh
    // record keeps them empty and a status response never writes them.
    app_model::AppState state{};
    state = app_model::reduce(
        state, app_model::Event::printer_status(std::chrono::duration_cast<app_model::Timestamp>(
                                                    std::chrono::milliseconds{1000}),
                                                status_from(printing_job(), {1, 1})));

    EXPECT_TRUE(state.current_job.present);
    EXPECT_TRUE(state.current_job.display_name.view().empty());
    EXPECT_TRUE(state.current_job.original_file_name.view().empty());
    EXPECT_TRUE(state.current_job.source_path.view().empty());
    EXPECT_FALSE(state.current_job.layers.known);
    EXPECT_EQ(state.current_job.estimate_quality, app_model::EstimateQuality::Unknown)
        << "the endpoint states no estimate quality, and Unknown projects to 'not approximate'";
}

TEST(PrusaLinkJobNormalizeTests, ZeroIsAReadingAndAbsenceIsNot) {
    StatusDto dto = printing_job();
    dto.progress_percent = 0.0;
    dto.time_remaining_seconds = 0.0;
    dto.time_printing_seconds = 0.0;

    const app_model::StatusJobTelemetry zeroed = status_from(dto, {1, 1}).job;

    EXPECT_TRUE(zeroed.progress.known);
    EXPECT_EQ(zeroed.progress.percent, 0U);
    EXPECT_TRUE(zeroed.remaining.known);
    EXPECT_EQ(zeroed.remaining.seconds, 0U);
    EXPECT_TRUE(zeroed.elapsed.known);

    dto.progress_known = false;
    dto.time_remaining_known = false;
    dto.time_printing_known = false;

    const app_model::StatusJobTelemetry absent = status_from(dto, {1, 2}).job;

    EXPECT_FALSE(absent.progress.known);
    EXPECT_EQ(absent.progress.percent, 0U) << "and the value is left at its default, not invented";
    EXPECT_FALSE(absent.remaining.known);
    EXPECT_FALSE(absent.elapsed.known);
}

TEST(PrusaLinkJobNormalizeTests, AnOverHundredProgressIsKeptAsObservedForTheDiagnosticToCount) {
    // §7.8: progress is "clamped to 0...100 only for display", the original figure is kept, and
    // the reducer counts it in out_of_range_progress_seen. So 105 is STORED here and clamped at
    // the projection — the accepted behaviour — rather than corrected or refused on the way in.
    StatusDto dto = printing_job();
    dto.progress_percent = 105.0;

    const app_model::StatusJobTelemetry job = status_from(dto, {1, 1}).job;

    EXPECT_TRUE(job.progress.known);
    EXPECT_EQ(job.progress.percent, 105U);
}

TEST(PrusaLinkJobNormalizeTests, AValueTheDomainCannotHoldIsRefusedRatherThanWrapped) {
    struct Case {
        double progress;
        bool known;
        const char* what;
    };

    const Case cases[] = {
        {-1.0, false, "a negative percentage is not zero"},
        {-0.4, false, "and neither is one that would round to zero from below"},
        {255.0, true, "the largest byte the domain can hold is still a reading"},
        {255.6, false, "in range as reported, but not representable once rounded"},
        {256.0, false, "one more than that is not a weaker reading, it is unusable"},
        {1e9, false, "and nor is a wildly larger one"},
        {std::numeric_limits<double>::infinity(), false, "infinity is not a percentage"},
        {std::numeric_limits<double>::quiet_NaN(), false, "and NaN fails every comparison"},
    };

    for (const Case& one : cases) {
        StatusDto dto = printing_job();
        dto.progress_percent = one.progress;

        const app_model::StatusJobTelemetry job = status_from(dto, {1, 1}).job;

        EXPECT_EQ(job.progress.known, one.known) << one.what;

        if (!one.known) {
            EXPECT_EQ(job.progress.percent, 0U) << one.what;
        }
    }
}

TEST(PrusaLinkJobNormalizeTests, ANegativeOrUnrepresentableDurationIsRefused) {
    struct Case {
        double seconds;
        bool known;
        const char* what;
    };

    const Case cases[] = {
        {0.0, true, "zero seconds remaining is a reading"},
        {-1.0, false, "a negative remaining time is a firmware this build does not understand"},
        {-90.0, false, "and 0 s is a claim the printer did not make"},
        {4294967295.0, true, "the largest the domain can hold"},
        {4294967296.0, false, "one more than that wraps, so it is refused"},
        {std::numeric_limits<double>::infinity(), false, "infinity is not a duration"},
        {std::numeric_limits<double>::quiet_NaN(), false, "nor is NaN"},
    };

    for (const Case& one : cases) {
        StatusDto dto = printing_job();
        dto.time_remaining_seconds = one.seconds;
        dto.time_printing_seconds = one.seconds;

        const app_model::StatusJobTelemetry job = status_from(dto, {1, 1}).job;

        EXPECT_EQ(job.remaining.known, one.known) << one.what;
        EXPECT_EQ(job.elapsed.known, one.known) << one.what;

        if (!one.known) {
            EXPECT_EQ(job.remaining.seconds, 0U) << one.what;
        }
    }
}

TEST(PrusaLinkJobNormalizeTests, OneDecodedResponseAlwaysSaysSomethingAboutAJob) {
    // ⛔ THE PUBLICATION DECISION, HOST-REACHABLE ON PURPOSE. The worker's poll loop is the one
    // translation unit here the host suite cannot compile, so this is the seam that keeps the rule
    // testable: a decoded response reports Present or Absent, never None. None would mean nothing
    // was said, and §7.8's "missing job data does not immediately erase a recently active job"
    // cannot start its clock on a silence it was never told about.
    StatusDto with = printing_job();
    EXPECT_EQ(job_telemetry_of(with), JobTelemetry::Present);

    with.job_present = false;
    EXPECT_EQ(job_telemetry_of(with), JobTelemetry::Absent);

    StatusDto bare;
    (void)bare.printer_state.assign("READY");
    EXPECT_EQ(job_telemetry_of(bare), JobTelemetry::Absent)
        << "a response with no job object says so, rather than saying nothing";

    // Every value in the job being individually unusable is still a job the printer reported.
    StatusDto empty_job;
    (void)empty_job.printer_state.assign("PRINTING");
    empty_job.job_present = true;
    EXPECT_EQ(job_telemetry_of(empty_job), JobTelemetry::Present);
}

TEST(PrusaLinkJobNormalizeTests, OneResponseHasOneOrderingAndOneStateNotTwoCopies) {
    // ⛔ THE JOB CANNOT BE NEWER OR OLDER THAN THE PRINTER STATE IT ARRIVED WITH, so the reading
    // has ONE ordering identity and one state, both on the printer half. Nothing duplicates them —
    // which is what lets the reducer judge the whole reading against one watermark, and what keeps
    // the payload compact.
    StatusDto dto = printing_job();
    (void)dto.printer_state.assign("PAUSED");

    const app_model::SnapshotOrder order{7, 21};
    const app_model::StatusObservation reading = status_from(dto, order);

    EXPECT_EQ(reading.printer.generation, 7U);
    EXPECT_EQ(reading.printer.sequence, 21U);
    EXPECT_EQ(reading.printer.normalized, app_model::PrinterState::Paused);
    EXPECT_EQ(reading.printer.raw.view(), "PAUSED");

    // Identical to what snapshot_from() produces alone: the printer half is not re-derived.
    const app_model::PrinterSnapshotObservation alone = snapshot_from(dto, order);
    EXPECT_EQ(reading.printer.generation, alone.generation);
    EXPECT_EQ(reading.printer.sequence, alone.sequence);
    EXPECT_EQ(reading.printer.normalized, alone.normalized);

    // And the reducer takes the job's state from there when it expands the compact fields.
    app_model::AppState state{};
    state = app_model::reduce(
        state, app_model::Event::printer_status(std::chrono::duration_cast<app_model::Timestamp>(
                                                    std::chrono::milliseconds{1000}),
                                                reading));
    EXPECT_EQ(state.current_job.normalized_state, app_model::PrinterState::Paused);
    EXPECT_EQ(state.current_job.raw_state.view(), "PAUSED");
}

} // namespace
} // namespace prd::prusalink
