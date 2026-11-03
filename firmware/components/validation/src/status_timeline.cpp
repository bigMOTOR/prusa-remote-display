// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include "status_timeline.hpp"

#include <array>

#include "dto.hpp"
#include "normalize.hpp"

// The bodies. Every one is shaped to the official contract and none is a capture — see the header.
//
// SHAPE NOTES, so a reader can check the fixtures against the contract rather than against
// intuition:
//
//   - `printer` is the only required object (OpenAPI). `state` is the only required field in it.
//   - `job` is OPTIONAL and is omitted entirely whenever no job exists, which is what v6.8.1 does.
//   - inside `job`, v6.8.1 emits `id`, `progress` and `time_printing` whenever a job exists, and
//     `time_remaining` only when it holds a valid estimate. Step 8 is that case.
//   - `progress` is PERCENT, 0...100. Never the legacy `/api/job.progress.completion` fraction.
//   - there is no current or total layer field anywhere in the contract, so there is none here.
//     `axis_z` is present because the firmware sends it, and is deliberately never read.
//   - `flow`, `speed`, `fan_hotend`, `fan_print`, `storage` and `camera` are fields the product
//     does not read. They are present so that the tolerance rule is exercised by the instrument
//     and not only by a unit test.

namespace prd::validation {
namespace {

// --- Resting -----------------------------------------------------------------

constexpr std::string_view kReady =
    R"({"printer":{"state":"READY","temp_nozzle":24.9,"target_nozzle":0,)"
    R"("temp_bed":24.1,"target_bed":0,"axis_z":0.4,"flow":100,"speed":100,)"
    R"("fan_hotend":0,"fan_print":0},)"
    R"("storage":{"path":"/usb/","name":"usb","read_only":false}})";

// --- Preparing: the temperatures move and there is still no job ---------------
//
// v6.8.1 reports BUSY for the preparation phases without saying which (§5.1), and a job object
// does not exist yet. Three bodies rather than one, because "the temperatures changed" is a thing
// the reviewer has to SEE change.

constexpr std::string_view kHeatingEarly =
    R"({"printer":{"state":"BUSY","temp_nozzle":48.3,"target_nozzle":215,)"
    R"("temp_bed":31.6,"target_bed":60,"axis_z":0.4,"flow":100,"speed":100,)"
    R"("fan_hotend":3100,"fan_print":0}})";

constexpr std::string_view kHeatingMid =
    R"({"printer":{"state":"BUSY","temp_nozzle":131.7,"target_nozzle":215,)"
    R"("temp_bed":52.4,"target_bed":60,"axis_z":0.4,"flow":100,"speed":100,)"
    R"("fan_hotend":6200,"fan_print":0}})";

constexpr std::string_view kHeatingReady =
    R"({"printer":{"state":"BUSY","temp_nozzle":214.8,"target_nozzle":215,)"
    R"("temp_bed":60.1,"target_bed":60,"axis_z":0.4,"flow":100,"speed":100,)"
    R"("fan_hotend":8500,"fan_print":0}})";

// --- Printing ----------------------------------------------------------------

constexpr std::string_view kPrintingStart =
    R"({"printer":{"state":"PRINTING","temp_nozzle":215.1,"target_nozzle":215,)"
    R"("temp_bed":60.0,"target_bed":60,"axis_z":0.2,"flow":100,"speed":100,)"
    R"("fan_hotend":8500,"fan_print":0},)"
    R"("job":{"id":173,"progress":0,"time_remaining":4020,"time_printing":4}})";

constexpr std::string_view kPrintingEarly =
    R"({"printer":{"state":"PRINTING","temp_nozzle":215.4,"target_nozzle":215,)"
    R"("temp_bed":59.8,"target_bed":60,"axis_z":1.8,"flow":100,"speed":100,)"
    R"("fan_hotend":8500,"fan_print":4300},)"
    R"("job":{"id":173,"progress":7,"time_remaining":3720,"time_printing":300}})";

// A fractional percentage, which the contract's own example (42.0) invites and which the decoder
// must round rather than truncate or refuse.
constexpr std::string_view kPrintingDecimal =
    R"({"printer":{"state":"PRINTING","temp_nozzle":214.6,"target_nozzle":215,)"
    R"("temp_bed":60.2,"target_bed":60,"axis_z":14.6,"flow":100,"speed":100,)"
    R"("fan_hotend":8500,"fan_print":4300},)"
    R"("job":{"id":173,"progress":41.6,"time_remaining":2280,"time_printing":1680}})";

// ⛔ `time_remaining` ABSENT. v6.8.1 emits it only when the estimate is valid, so this is the
// ordinary case and not a fault. Progress and elapsed are still there, and the panel must show
// them with no remaining time rather than blanking the row or inventing one.
constexpr std::string_view kPrintingNoEstimate =
    R"({"printer":{"state":"PRINTING","temp_nozzle":215.0,"target_nozzle":215,)"
    R"("temp_bed":60.0,"target_bed":60,"axis_z":22.1,"flow":100,"speed":100,)"
    R"("fan_hotend":8500,"fan_print":4300},)"
    R"("job":{"id":173,"progress":63,"time_printing":2580}})";

// Additive members the product has never seen, at both levels, plus one inside `job`. §3.2
// requires them to be ignored; this is where the instrument proves it rather than asserting it.
constexpr std::string_view kPrintingAdditive =
    R"({"printer":{"state":"PRINTING","temp_nozzle":215.2,"target_nozzle":215,)"
    R"("temp_bed":60.0,"target_bed":60,"axis_z":26.4,"flow":100,"speed":100,)"
    R"("fan_hotend":8500,"fan_print":4300,"chamber_fan":2200},)"
    R"("job":{"id":173,"progress":78,"time_remaining":900,"time_printing":3120,)"
    R"("inhibit_object_cancel":false,"serial_print":false},)"
    R"("camera":{"id":"0","registered":false},"future_object":{"nested":{"deep":1}}})";

// --- Suspended and resumed ---------------------------------------------------

constexpr std::string_view kPaused =
    R"({"printer":{"state":"PAUSED","temp_nozzle":205.0,"target_nozzle":215,)"
    R"("temp_bed":60.0,"target_bed":60,"axis_z":26.4,"flow":100,"speed":100,)"
    R"("fan_hotend":8500,"fan_print":0},)"
    R"("job":{"id":173,"progress":78,"time_remaining":900,"time_printing":3120}})";

constexpr std::string_view kResumed =
    R"({"printer":{"state":"PRINTING","temp_nozzle":215.1,"target_nozzle":215,)"
    R"("temp_bed":60.0,"target_bed":60,"axis_z":27.0,"flow":100,"speed":100,)"
    R"("fan_hotend":8500,"fan_print":4300},)"
    R"("job":{"id":173,"progress":79,"time_remaining":860,"time_printing":3180}})";

// --- Individual optional fields the build cannot use -------------------------
//
// ⛔ BOTH OF THESE MUST LEAVE THE OTHER FIELDS INTACT. The failure they guard against is a decoder
// that abandons the job — or the whole response — because one member of it was unusable. The id
// and the times are still perfectly readable in both.

// `progress` as a string. Wrong JSON type, so not a percentage this build can read.
constexpr std::string_view kProgressWrongType =
    R"({"printer":{"state":"PRINTING","temp_nozzle":215.0,"target_nozzle":215,)"
    R"("temp_bed":60.0,"target_bed":60,"axis_z":28.2,"flow":100,"speed":100,)"
    R"("fan_hotend":8500,"fan_print":4300},)"
    R"("job":{"id":173,"progress":"almost","time_remaining":700,"time_printing":3300}})";

// `progress` as a well-formed JSON number that is not finite once converted, and a negative
// `time_remaining`. Neither is representable as the value it claims to be.
constexpr std::string_view kProgressNonFinite =
    R"({"printer":{"state":"PRINTING","temp_nozzle":215.0,"target_nozzle":215,)"
    R"("temp_bed":60.0,"target_bed":60,"axis_z":29.0,"flow":100,"speed":100,)"
    R"("fan_hotend":8500,"fan_print":4300},)"
    R"("job":{"id":173,"progress":1e400,"time_remaining":-90,"time_printing":3360}})";

// --- Completion and back to rest ---------------------------------------------

// ⛔ THE LAST PRINTING RESPONSE IS NOT AT 100 %, AND THAT IS DELIBERATE. An earlier version of this
// timeline sent PRINTING 100% immediately before FINISHED, which MASKED a real defect: the
// terminal-completion snapshot reads the store's current job, so a direct jump to FINISHED used to
// record the completion at the PRECEDING response's progress. With 100 % already in the store the
// wrong answer and the right answer were the same number. A printer does not promise to report
// every percentage, so the direct jump is the case the instrument must show.
constexpr std::string_view kPrintingLate =
    R"({"printer":{"state":"PRINTING","temp_nozzle":215.0,"target_nozzle":215,)"
    R"("temp_bed":60.0,"target_bed":60,"axis_z":31.4,"flow":100,"speed":100,)"
    R"("fan_hotend":8500,"fan_print":4300},)"
    R"("job":{"id":173,"progress":92,"time_remaining":330,"time_printing":3690}})";

constexpr std::string_view kFinished =
    R"({"printer":{"state":"FINISHED","temp_nozzle":198.4,"target_nozzle":0,)"
    R"("temp_bed":58.2,"target_bed":0,"axis_z":31.4,"flow":100,"speed":100,)"
    R"("fan_hotend":8500,"fan_print":0},)"
    R"("job":{"id":173,"progress":100,"time_printing":4020}})";

// ⛔ THE `job` OBJECT IS GONE. The printer has retired the job, which §4.3 permits at any time and
// v6.8.1 does once the job is cleared. §7.8 says missing job data must not immediately erase a
// recently active job, so this step is where that rule is visible on the panel.
constexpr std::string_view kJobRetired =
    R"({"printer":{"state":"FINISHED","temp_nozzle":141.7,"target_nozzle":0,)"
    R"("temp_bed":48.9,"target_bed":0,"axis_z":31.4,"flow":100,"speed":100,)"
    R"("fan_hotend":0,"fan_print":0}})";

constexpr std::string_view kBackToReady =
    R"({"printer":{"state":"READY","temp_nozzle":42.1,"target_nozzle":0,)"
    R"("temp_bed":33.5,"target_bed":0,"axis_z":31.4,"flow":100,"speed":100,)"
    R"("fan_hotend":0,"fan_print":0},)"
    R"("storage":{"path":"/usb/","name":"usb","read_only":false}})";

// The timeline. Offsets are PRINTER time and are strictly increasing; the six-second stride is
// what a 2-second poll would look like having skipped two cycles, and is short enough that the
// whole print fits inside a review.
constexpr std::array<StatusStep, 17> kTimeline{{
    {0, "ready, no job", kReady},
    {6000, "heating 48/215, no job", kHeatingEarly},
    {12000, "heating 132/215, no job", kHeatingMid},
    {18000, "at temperature, no job", kHeatingReady},
    {24000, "printing 0%", kPrintingStart},
    {30000, "printing 7%", kPrintingEarly},
    {36000, "printing 41.6% decimal", kPrintingDecimal},
    {42000, "printing 63%, no estimate", kPrintingNoEstimate},
    {48000, "printing 78% + additive fields", kPrintingAdditive},
    {54000, "paused at 78%", kPaused},
    {60000, "resumed at 79%", kResumed},
    {66000, "progress wrong type", kProgressWrongType},
    {72000, "progress non-finite, remaining negative", kProgressNonFinite},
    {78000, "printing 92%", kPrintingLate},
    {84000, "finished 100% direct from 92%", kFinished},
    {90000, "job object retired", kJobRetired},
    {96000, "back to ready", kBackToReady},
}};

} // namespace

std::size_t status_timeline_step_count() {
    return kTimeline.size();
}

StatusStep status_timeline_step_at(const std::size_t index) {
    if (index >= kTimeline.size()) {
        return StatusStep{};
    }

    return kTimeline.at(index);
}

StatusApplication apply_status_body(app_store::Store& store, const std::string_view body,
                                    const app_model::Timestamp moment,
                                    const app_model::SnapshotOrder order) {
    StatusApplication result;

    prusalink::StatusDto dto;
    result.decoded = prusalink::decode_status(body, dto);

    if (result.decoded != prusalink::DecodeError::None) {
        // A body the production decoder refuses produces no observation on a real device either;
        // it produces a connection fact, which this instrument does not simulate.
        return result;
    }

    const app_model::StatusObservation observation = prusalink::status_from(dto, order);
    result.job_reported = observation.job.reported;
    const app_store::DispatchOutcome outcome =
        store.dispatch(app_model::Event::printer_status(moment, observation));

    // `accepted` is the store's reentrancy refusal, not the reducer's staleness answer; a bench
    // instrument that ignored it would report a dispatch that never happened.
    result.dispatched = outcome.accepted;
    result.mutated = outcome.mutated;

    return result;
}

} // namespace prd::validation
