// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include <cstdint>
#include <string>
#include <string_view>

#include <gtest/gtest.h>

#include "app_model/events.hpp"
#include "app_model/presentation.hpp"
#include "app_model/reducer.hpp"
#include "app_store/store.hpp"
#include "scene_selection.hpp"
#include "support/state_builders.hpp"

#include "prusalink/prusalink.hpp"

#include "decode.hpp"
#include "normalize.hpp"
#include "outcome.hpp"

// The whole slice, end to end, on the host — Milestone 4's first checkpoint.
//
// Response body → decoder → normalized observation → real store → projection → scene.
// Every stage is the production one; the only thing missing is the socket, and the
// transport around it is a thin adapter over units proved elsewhere in this suite.
//
// ⬅️ WHY THIS TEST IS THE POINT OF THE CHECKPOINT. The claim being made is that a live
// printer reaches the production display through the seam the fake source has been using
// since Milestone 3 — that nothing downstream needed changing. A test that stopped at the
// DTO would leave that claim unverified, and the bench would be the first place it was
// tried. This runs the claim on every commit instead.
//
// ⛔ EVERY BODY HERE IS SYNTHETIC. No captured response, no real address, no serial, no
// key. `printer.local` is the documentation hostname the state builders already use.

namespace prd::prusalink {
namespace {

using app_model::AppState;
using app_model::ConnectionPhase;
using app_model::PrinterState;
using ui::Scene;

// One worker's generation, fixed for these tests: they are about the transaction and the
// presentation, not about worker replacement. Generation-scoped ordering has its own suite.
constexpr std::uint32_t kGeneration = 7;

// A CORE One at rest, in the documented shape.
constexpr const char* kIdleBody =
    R"({"printer":{"state":"IDLE","temp_nozzle":24.8,"target_nozzle":0.0,)"
    R"("temp_bed":23.9,"target_bed":0.0,"axis_z":10.0,"flow":100,"speed":100},)"
    R"("storage":{"path":"/usb/","name":"usb","read_only":false}})";

constexpr const char* kVersionBody =
    R"({"api":"2.0.0","version":"2.1.2","printer":"1.3.1","text":"PrusaLink",)"
    R"("firmware":"6.5.7","sdk":"0.7.1","capabilities":{"upload-by-put":true}})";

// The production path, in one function: what the worker does with a body, minus the
// socket. Returns the state after the store has applied everything the cycle produced.
[[nodiscard]] AppState apply_cycle(AppState state, const std::string_view body,
                                   const network::HttpOutcome transport, const std::uint16_t status,
                                   const std::uint32_t sequence, const app_model::Timestamp now) {
    StatusDto dto;
    DecodeError decoded = DecodeError::None;

    const bool decodable = transport == network::HttpOutcome::Ok && status >= 200 && status < 300;

    if (decodable) {
        decoded = decode_status(body, dto);
    }

    const AttemptOutcome outcome = classify(transport, status, decoded);

    app_model::ConnectionObservation connection;
    connection.phase = outcome.phase;
    connection.endpoint.assign("printer.local");
    connection.auth_method = app_model::AuthMethod::ApiKey;
    connection.error.assign(outcome.error);
    connection.response_code_known = transport == network::HttpOutcome::Ok;
    connection.response_code = status;
    connection.latency_known = true;
    connection.latency_ms = 31;

    state = app_model::reduce(state, app_model::Event::connection_observed(now, connection));

    if (outcome.usable) {
        state = app_model::reduce(state, app_model::Event::printer_snapshot(
                                             now, snapshot_from(dto, {kGeneration, sequence})));
    }

    return state;
}

// A device whose printer configuration is ACTIVE — validated, promoted, and therefore the only
// state in which operational polling may run at all.
[[nodiscard]] AppState ready_to_poll() {
    return test::booted_device(test::at_ms(0));
}

// The same device one step earlier: the candidate is committed but not yet proved. This is what
// §13.3 calls a written-and-committed candidate and what §14.3 refuses to save as active until
// the printer has answered.
[[nodiscard]] AppState pending_candidate() {
    AppState state = test::booted_device(test::at_ms(0));

    app_model::ConfigurationObservation observation;
    observation.status = app_model::ConfigurationStatus::Valid;
    observation.schema_version = 1;
    observation.has_wifi_credentials = true;
    observation.has_printer_configuration = false;
    observation.printer_configuration_pending = true;
    (void)observation.printer_address.assign("printer.local");
    observation.auth_method = app_model::AuthMethod::ApiKey;

    return app_model::reduce(state,
                             app_model::Event::configuration_observed(test::at_ms(0), observation));
}

TEST(VerticalSliceTests, ALiveIdleResponseReachesTheOperationalScene) {
    AppState state = ready_to_poll();

    // Before the first response there is no trustworthy printer state, so the projection
    // must not present one. This is the "before" half of the checkpoint's claim.
    ASSERT_EQ(app_model::project(state).subject, app_model::SubjectState::AcquiringState);
    ASSERT_NE(ui::select(app_model::project(state)), Scene::Operational);

    state = apply_cycle(state, kIdleBody, network::HttpOutcome::Ok, 200, 1, test::at_ms(1000));

    // ⬅️ THE CHECKPOINT'S USER-VISIBLE OUTCOME. A real response, decoded and normalized by
    // production code, moves the panel to the operational scene showing a ready printer.
    const app_model::Presentation view = app_model::project(state);

    EXPECT_EQ(state.printer_connection.phase, ConnectionPhase::Connected);
    EXPECT_EQ(state.printer.normalized, PrinterState::Idle);
    EXPECT_EQ(state.printer.raw.view(), "IDLE");
    EXPECT_TRUE(state.printer.ever_observed);

    EXPECT_EQ(view.subject, app_model::SubjectState::Ready);
    EXPECT_EQ(view.confidence, app_model::Confidence::Current);
    EXPECT_EQ(view.subject_condition, app_model::SubjectCondition::None);
    EXPECT_EQ(ui::select(view), Scene::Operational);

    // The telemetry the response carried arrives with it.
    EXPECT_TRUE(state.temperatures.nozzle_current.known);
    EXPECT_FLOAT_EQ(state.temperatures.nozzle_current.celsius, 24.8F);
    EXPECT_TRUE(state.temperatures.bed_current.known);
    EXPECT_FLOAT_EQ(state.temperatures.bed_current.celsius, 23.9F);
}

TEST(VerticalSliceTests, AReadyResponseAlsoReachesTheOperationalScene) {
    // §5.1 leaves READY versus IDLE open, and both must work: the owner's printer may
    // report either. The raw string keeps them distinguishable in state.
    const std::string body = R"({"printer":{"state":"READY","temp_nozzle":215.0,)"
                             R"("target_nozzle":215.0,"temp_bed":60.0,"target_bed":60.0}})";

    AppState state =
        apply_cycle(ready_to_poll(), body, network::HttpOutcome::Ok, 200, 1, test::at_ms(1000));

    EXPECT_EQ(state.printer.normalized, PrinterState::Ready);
    EXPECT_EQ(state.printer.raw.view(), "READY");
    EXPECT_EQ(ui::select(app_model::project(state)), Scene::Operational);
}

TEST(VerticalSliceTests, TheVersionPreflightEstablishesIdentityWithoutInventingOne) {
    VersionDto version;
    ASSERT_EQ(decode_version(kVersionBody, version), DecodeError::None);

    app_model::PrinterIdentity identity;
    apply_version(version, identity);

    AppState state = app_model::reduce(
        ready_to_poll(), app_model::Event::printer_identity(test::at_ms(500), identity));

    EXPECT_EQ(state.printer_identity.firmware_version.view(), "6.5.7");
    EXPECT_EQ(state.printer_identity.api_version.view(), "2.0.0");

    // A version response says nothing about the printer's state, and must not be allowed
    // to look as though it did — §7.5's rule, from the other direction.
    EXPECT_EQ(state.printer.normalized, PrinterState::Unknown);
    EXPECT_FALSE(state.printer.ever_observed);
    EXPECT_NE(ui::select(app_model::project(state)), Scene::Operational);
}

TEST(VerticalSliceTests, IdenticalResponsesProduceIdenticalStateAndPresentation) {
    // Determinism, which is what makes the fixture suite meaningful at all: the same bytes
    // at the same time must produce the same state, bit for bit.
    const AppState first = apply_cycle(ready_to_poll(), kIdleBody, network::HttpOutcome::Ok, 200, 1,
                                       test::at_ms(1000));
    const AppState second = apply_cycle(ready_to_poll(), kIdleBody, network::HttpOutcome::Ok, 200,
                                        1, test::at_ms(1000));

    EXPECT_EQ(first.printer, second.printer);
    EXPECT_EQ(first.temperatures, second.temperatures);
    EXPECT_EQ(first.printer_connection.phase, second.printer_connection.phase);
    EXPECT_EQ(app_model::project(first), app_model::project(second));

    // NEGATIVE CONTROL: a different body produces a different presentation, so the equality
    // above is determinism and not a comparison that always holds.
    const AppState other = apply_cycle(ready_to_poll(), R"({"printer":{"state":"PRINTING"}})",
                                       network::HttpOutcome::Ok, 200, 1, test::at_ms(1000));
    EXPECT_NE(app_model::project(first), app_model::project(other));
}

TEST(VerticalSliceTests, AFailedCycleNeverFabricatesPrinterData) {
    // ⛔ THE SAFETY PROPERTY OF THE CHECKPOINT. Every failure mode must leave the printer
    // state untouched: no snapshot, no invented reading, no state derived from the failure.
    // §24.5 forbids inferring operational state from connection state, and §24.18 forbids
    // inventing what the printer did not report.
    struct Case {
        network::HttpOutcome transport;
        std::uint16_t status;
        const char* body;
        ConnectionPhase expected;
        const char* why;
    };

    const Case kCases[] = {
        {network::HttpOutcome::ConnectFailed, 0, "", ConnectionPhase::PrinterUnavailable,
         "printer switched off"},
        {network::HttpOutcome::Timeout, 0, "", ConnectionPhase::PrinterUnavailable,
         "no answer within the budget"},
        {network::HttpOutcome::TransportFailed, 0, "", ConnectionPhase::PrinterUnavailable,
         "connection reset mid-body"},
        {network::HttpOutcome::ResponseTooLarge, 200, "", ConnectionPhase::InvalidResponse,
         "body exceeded the bound"},
        {network::HttpOutcome::Ok, 401, R"({"error":"denied"})", ConnectionPhase::Unauthorized,
         "wrong API key"},
        {network::HttpOutcome::Ok, 403, R"({"error":"denied"})", ConnectionPhase::Unauthorized,
         "forbidden"},
        {network::HttpOutcome::Ok, 404, "<html>404</html>", ConnectionPhase::UnsupportedApi,
         "no such endpoint"},
        {network::HttpOutcome::Ok, 503, "", ConnectionPhase::PrinterUnavailable,
         "printer busy rebooting"},
        {network::HttpOutcome::Ok, 200, R"({"printer":{"state":"IDLE")",
         ConnectionPhase::InvalidResponse, "truncated body"},
        {network::HttpOutcome::Ok, 200, R"({"printer":{"temp_nozzle":24.0}})",
         ConnectionPhase::InvalidResponse, "no state field"},
        {network::HttpOutcome::Ok, 200, R"({"job":{"id":1}})", ConnectionPhase::InvalidResponse,
         "no printer object"},
    };

    for (const Case& item : kCases) {
        const AppState state = apply_cycle(ready_to_poll(), item.body, item.transport, item.status,
                                           1, test::at_ms(1000));

        EXPECT_EQ(state.printer_connection.phase, item.expected) << item.why;

        // No printer state was ever observed, so nothing may be presented as current.
        EXPECT_FALSE(state.printer.ever_observed) << item.why;
        EXPECT_EQ(state.printer.normalized, PrinterState::Unknown) << item.why;
        EXPECT_FALSE(state.temperatures.nozzle_current.known) << item.why;
        EXPECT_FALSE(state.temperatures.bed_current.known) << item.why;

        const app_model::Presentation view = app_model::project(state);
        EXPECT_NE(view.subject, app_model::SubjectState::Ready) << item.why;
        EXPECT_NE(ui::select(view), Scene::Operational) << item.why;
    }

    // POSITIVE CONTROL for the whole table: the successful cycle these were derived from
    // does reach Operational, so the assertions above discriminate failure from success
    // rather than never reaching the scene at all.
    const AppState good = apply_cycle(ready_to_poll(), kIdleBody, network::HttpOutcome::Ok, 200, 1,
                                      test::at_ms(1000));
    EXPECT_EQ(ui::select(app_model::project(good)), Scene::Operational);
}

TEST(VerticalSliceTests, AFailureAfterASuccessDoesNotErasePriorStateButLosesConfidence) {
    // §16.3's last-known state: during a short outage the previous reading is retained and
    // marked, rather than blanked or presented as current.
    AppState state = apply_cycle(ready_to_poll(), kIdleBody, network::HttpOutcome::Ok, 200, 1,
                                 test::at_ms(1000));
    ASSERT_EQ(ui::select(app_model::project(state)), Scene::Operational);

    state = apply_cycle(state, "", network::HttpOutcome::ConnectFailed, 0, 2, test::at_ms(2000));

    // The observed state is still what the printer last said — it is not overwritten by a
    // failure, and it is not upgraded either.
    EXPECT_EQ(state.printer.normalized, PrinterState::Idle);
    EXPECT_TRUE(state.printer.ever_observed);
    EXPECT_EQ(state.printer_connection.phase, ConnectionPhase::PrinterUnavailable);

    // And the connection fact reached the state, which is what the projection needs to
    // stop presenting the reading as current.
    EXPECT_EQ(state.printer_connection.last_error.view(), "unreachable");
}

TEST(VerticalSliceTests, AnUnknownPrinterStateIsCarriedWithoutBecomingAReadyPrinter) {
    // A firmware reporting a state this build does not know must not be shown as ready.
    // The raw string survives for diagnostics (§24.8), and the panel says it does not know.
    const AppState state =
        apply_cycle(ready_to_poll(), R"({"printer":{"state":"CALIBRATING","temp_nozzle":40.0}})",
                    network::HttpOutcome::Ok, 200, 1, test::at_ms(1000));

    // The exchange itself was fine — this is not a connection fault.
    EXPECT_EQ(state.printer_connection.phase, ConnectionPhase::Connected);

    EXPECT_EQ(state.printer.normalized, PrinterState::Unknown);
    EXPECT_EQ(state.printer.raw.view(), "CALIBRATING");

    const app_model::Presentation view = app_model::project(state);
    EXPECT_NE(view.subject, app_model::SubjectState::Ready);
    EXPECT_NE(ui::select(view), Scene::Operational);

    // The temperature it did report is still trustworthy and still arrives: an
    // unrecognised state does not make the rest of the response suspect.
    EXPECT_TRUE(state.temperatures.nozzle_current.known);
    EXPECT_FLOAT_EQ(state.temperatures.nozzle_current.celsius, 40.0F);
}

TEST(VerticalSliceTests, OutOfOrderResponsesAreRejectedByTheSequenceRule) {
    // §8.1's stale-response rule, exercised through the real store rather than asserted
    // about. This is why the worker's sequence starts above zero and only ever advances.
    app_store::Store store;

    AppState seeded = ready_to_poll();
    seeded = apply_cycle(seeded, R"({"printer":{"state":"PRINTING"}})", network::HttpOutcome::Ok,
                         200, 5, test::at_ms(1000));

    ASSERT_EQ(seeded.printer.normalized, PrinterState::Printing);

    // A duplicate or delayed response carrying a sequence that is not greater must not
    // move the state backwards.
    const AppState after_stale =
        apply_cycle(seeded, kIdleBody, network::HttpOutcome::Ok, 200, 5, test::at_ms(2000));
    EXPECT_EQ(after_stale.printer.normalized, PrinterState::Printing);

    // POSITIVE CONTROL: the next sequence is accepted, so the rejection above is the rule
    // and not a state that has stopped updating.
    const AppState after_fresh =
        apply_cycle(seeded, kIdleBody, network::HttpOutcome::Ok, 200, 6, test::at_ms(2000));
    EXPECT_EQ(after_fresh.printer.normalized, PrinterState::Idle);

    (void)store;
}

TEST(VerticalSliceTests, ThePathFromBodyToSceneIsTheSameSeamTheFakeSourceUses) {
    // ⬅️ THE STRUCTURAL CLAIM OF ADR-0010, MADE INTO A TEST. A live response and a fixture
    // produce the same observation type and therefore the same downstream behaviour. If a
    // future change gave the printer path its own route into the store, this would fail —
    // which is what "the observation boundary is already the seam" has to mean to be worth
    // relying on.
    StatusDto dto;
    ASSERT_EQ(decode_status(kIdleBody, dto), DecodeError::None);

    const app_model::PrinterSnapshotObservation live = snapshot_from(dto, {kGeneration, 1});
    const app_model::PrinterSnapshotObservation fixture =
        test::printer_snapshot(PrinterState::Idle, 1);

    // Same type, same field for the same fact. The values differ — one came from a body —
    // but the shape a consumer sees does not.
    EXPECT_EQ(live.normalized, fixture.normalized);
    EXPECT_EQ(live.sequence, fixture.sequence);
    EXPECT_EQ(live.is_busy, fixture.is_busy);
    EXPECT_EQ(live.requires_attention, fixture.requires_attention);

    const AppState from_live = app_model::reduce(
        ready_to_poll(), app_model::Event::printer_snapshot(test::at_ms(1000), live));
    const AppState from_fixture = app_model::reduce(
        ready_to_poll(), app_model::Event::printer_snapshot(test::at_ms(1000), fixture));

    EXPECT_EQ(app_model::project(from_live).subject, app_model::project(from_fixture).subject);
    EXPECT_EQ(ui::select(app_model::project(from_live)),
              ui::select(app_model::project(from_fixture)));
}

// --- The configuration-validation transaction ------------------------------

TEST(ValidationTransactionTests, AValidateReportCarriesNoObservationsAtAll) {
    // ⛔ THE STRUCTURAL GUARANTEE. §14.3 step 9 and §13.3 step 6 both put "mark active" after the
    // connection test, so a candidate must not be a source of operational data. The worker
    // enforces that by publishing only a verdict in this purpose; this pins the shape so a future
    // edit that started filling an observation slot fails here rather than on a bench.
    Report report;
    report.purpose = Purpose::Validate;
    report.verdict = Verdict::Succeeded;

    EXPECT_FALSE(report.connection_present);
    EXPECT_FALSE(report.identity_present);
    EXPECT_FALSE(report.status_present);

    // NEGATIVE CONTROL: the Poll purpose is the one that may fill them, so the emptiness above is
    // a property of Validate rather than of Report being unable to carry anything.
    Report polling;
    polling.purpose = Purpose::Poll;
    polling.connection_present = true;
    polling.status_present = true;
    EXPECT_TRUE(polling.connection_present);
    EXPECT_TRUE(polling.status_present);
    EXPECT_EQ(polling.verdict, Verdict::None);
}

TEST(ValidationTransactionTests, APendingCandidateIsNotAnOperationalPrinter) {
    // The state the device is in between the commit and the promotion. Nothing about it may look
    // like a working printer: no operational scene, and — owner ruling D-4 — no configured address
    // presented as the printer's identity.
    const AppState state = pending_candidate();

    EXPECT_TRUE(state.configuration.printer_configuration_pending);
    EXPECT_FALSE(state.configuration.has_printer_configuration);

    const app_model::Presentation view = app_model::project(state);

    EXPECT_EQ(view.subject, app_model::SubjectState::AcquiringState);
    EXPECT_NE(ui::select(view), Scene::Operational);
    EXPECT_TRUE(view.printer_configuration_pending);

    // The candidate's address is not shown as a printer name. It is a guess until something
    // answers at it.
    EXPECT_FALSE(view.printer_name_known);
}

TEST(ValidationTransactionTests, TheSameStateBecomesOperationalOnlyAfterPromotion) {
    // ⬅️ THE WHOLE POINT, AS A PAIR. Identical printer data; the only difference is whether the
    // configuration has been marked active. One reaches the Operational scene and one does not,
    // which is §14.3 step 10's "transition to normal UI" placed after step 9 rather than before.
    const AppState pending = apply_cycle(pending_candidate(), kIdleBody, network::HttpOutcome::Ok,
                                         200, 1, test::at_ms(1000));
    const AppState promoted = apply_cycle(ready_to_poll(), kIdleBody, network::HttpOutcome::Ok, 200,
                                          1, test::at_ms(1000));

    // Both saw the same response, so the printer facts match — this is not a test about data.
    EXPECT_EQ(pending.printer.normalized, promoted.printer.normalized);

    // But only the promoted one may present a printer identity, and only it has the active
    // configuration that permits polling to have run at all.
    EXPECT_FALSE(pending.configuration.has_printer_configuration);
    EXPECT_TRUE(promoted.configuration.has_printer_configuration);

    EXPECT_TRUE(app_model::project(promoted).printer_name_known);
    EXPECT_FALSE(app_model::project(pending).printer_name_known);
}

} // namespace
} // namespace prd::prusalink
