// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include <cstdint>

#include <gtest/gtest.h>

#include "app_model/reducer.hpp"
#include "printer_session.hpp"
#include "support/state_builders.hpp"

// The printer-identity boundary — audit item 4, owner rulings 1 and 2 of 2026-08-24.
//
// ⛔ THE DEFECT. apply_configuration() cleared `access_condition` and `printer_connection` when the
// endpoint, authentication identity, credential generation or configured/unconfigured status
// changed — and nothing else. `printer_identity`, `printer`, `temperatures`, `current_job`, `alert`,
// `ever_observed`, `observed_at` and the §8.1 watermarks all survived. derive_layers() then computed
// freshness from `now - observed_at`, which still described the PREVIOUS printer, so confidence came
// out Current and the panel asserted the old printer's job, progress, temperatures and warning
// against the newly configured one. §11.5: "Stale printer information must never be presented as
// current."
//
// ⬅️ AND CLEARING IS THE RULING, NOT BADGING. §11.5's stale indicator is for temporary loss of the
// SAME known target — values that were true of that printer and are merely old. Across an identity
// boundary the relationship between the previous printer's job and the new one is not "recent", it
// is unknown, and a badge would still be presenting it.
//
// EVERY STATE HERE COMES FROM THE REAL REDUCER. Nothing is hand-assembled, so no test asserts
// against a state the product could not reach.
//
// ⬅️ WHY THIS FILE ALSO REACHES INTO app_core. The last group proves that a late report from a
// superseded worker cannot repopulate what the boundary cleared, and the seam that guarantees it is
// printer_session::classify() on the application task — not the reducer. Splitting that assertion
// away from the clearing it protects would leave each half looking sufficient on its own.

namespace prd::app_model {
namespace {

using prd::test::at_ms;
using prd::test::printer_snapshot;
using prd::test::printing_job;
using prd::test::with_connection;

constexpr std::uint32_t kFirstGeneration = 7;
constexpr std::uint32_t kSecondGeneration = 8;

// A transaction that names a profile. armed() refuses one that does not.
constexpr std::uint32_t kProfile = 3;
constexpr std::uint32_t kAttempt = 1;

[[nodiscard]] ConfigurationObservation configuration_for(const std::string_view address,
                                                         const std::uint32_t generation) {
    ConfigurationObservation observation;
    observation.status = ConfigurationStatus::Valid;
    observation.schema_version = 1;
    observation.credential_generation = generation;
    observation.has_wifi_credentials = true;
    observation.has_printer_configuration = true;
    observation.printer_address.assign(address);
    observation.printer_alias.assign("Workshop");
    observation.auth_method = AuthMethod::ApiKey;
    return observation;
}

[[nodiscard]] AlertObservation warning_alert() {
    AlertObservation observation;
    observation.severity = AlertSeverity::Warning;
    observation.source = AlertSource::PrinterState;
    observation.code.assign("W-1");
    observation.title.assign("Filament runout");
    observation.message.assign("Load filament to continue");
    observation.is_actionable = true;
    return observation;
}

// A device that has fully observed one printer: connection, identity, state, temperatures, a job
// with progress and layers, and an active warning. This is the state the defect leaked.
[[nodiscard]] AppState fully_observed_printer(const Timestamp now) {
    AppState state;

    HardwareProbeObservation probe;
    probe.board_target.assign("waveshare_esp32_s3_lcd_2_8");
    probe.reset_reason.assign("power-on");
    probe.flash_validation = ValidationState::Validated;
    probe.psram_validation = ValidationState::Validated;
    state = reduce(state, Event::hardware_probed(now, probe));
    state = reduce(state, Event::storage_observed(now, StorageStatus::Ready));
    state =
        reduce(state, Event::configuration_observed(now, configuration_for("printer-a.local", 3)));
    state = reduce(state, Event::boot_phase(now, BootPhase::Ready));

    state = with_connection(state, ConnectionPhase::Connected, now);

    PrinterIdentity identity;
    identity.display_name.assign("CORE One A");
    identity.reported_name.assign("CORE One A");
    identity.hostname.assign("printer-a.local");
    identity.firmware_version.assign("6.1.2");
    identity.api_version.assign("2.0.0");
    state = reduce(state, Event::printer_identity(now, identity));

    state =
        reduce(state, Event::printer_snapshot(now, printer_snapshot(PrinterState::Printing, 4)));
    state = reduce(state, Event::job_observed(now, printing_job(9)));
    state = reduce(state, Event::alert_raised(now, warning_alert()));

    return state;
}

// The same device, carried on to a FINISHED print so the reducer's own persist_completion() has run.
//
// ⛔ THROUGH THE PRODUCTION ROUTE, NOT BY ASSIGNMENT. `last_completed_job` is written in exactly one
// place — persist_completion(), reached when a present job's printer reports a terminal state — so a
// test that hand-assembled the snapshot would be asserting against a state the product cannot reach.
[[nodiscard]] AppState printer_with_completed_job(const Timestamp now) {
    AppState state = fully_observed_printer(now);

    // Sequence 5 beats the 4 the base builder used, so §8.1 does not discard this.
    return reduce(state, Event::printer_snapshot(now + at_ms(10),
                                                 printer_snapshot(PrinterState::Finished, 5)));
}

// Asserts that a completion snapshot really is present, so every "it was cleared" assertion below is
// checked against a state that genuinely held one.
void expect_completion_present(const AppState& state) {
    ASSERT_TRUE(state.last_completed_job.present);
    ASSERT_EQ(state.last_completed_job.terminal_state, PrinterState::Finished);
    ASSERT_EQ(state.last_completed_job.id.view(), "job-1");
    ASSERT_EQ(state.last_completed_job.display_name.view(), "CORE One.gcode");
    ASSERT_TRUE(state.last_completed_job.progress.known);
    ASSERT_TRUE(state.last_completed_job.layers.known);
    ASSERT_NE(state.last_completed_job.completed_at, Timestamp{});
}

// Asserts the whole of "nothing of the previous printer survives", in one place so every boundary
// test checks the same list rather than a convenient subset.
void expect_previous_printer_forgotten(const AppState& state) {
    // Identity.
    EXPECT_TRUE(state.printer_identity.display_name.empty());
    EXPECT_TRUE(state.printer_identity.reported_name.empty());
    EXPECT_TRUE(state.printer_identity.hostname.empty());
    EXPECT_TRUE(state.printer_identity.firmware_version.empty());
    EXPECT_TRUE(state.printer_identity.api_version.empty());

    // Operational state and its flags.
    EXPECT_EQ(state.printer.normalized, PrinterState::Unknown);
    EXPECT_TRUE(state.printer.raw.empty());
    EXPECT_FALSE(state.printer.is_busy);
    EXPECT_FALSE(state.printer.requires_attention);

    // ⛔ THE TWO THAT MADE THE STALE DATA LOOK CURRENT.
    EXPECT_FALSE(state.printer.ever_observed);
    EXPECT_EQ(state.printer.observed_at, Timestamp{});

    // Temperatures.
    EXPECT_FALSE(state.temperatures.nozzle_current.known);
    EXPECT_FALSE(state.temperatures.nozzle_target.known);
    EXPECT_FALSE(state.temperatures.bed_current.known);
    EXPECT_FALSE(state.temperatures.bed_target.known);
    EXPECT_FALSE(state.temperatures.chamber_current.known);
    EXPECT_FALSE(state.temperatures.chamber_target.known);
    EXPECT_EQ(state.temperatures.observed_at, Timestamp{});

    // Job, including progress and layers specifically — those are what the F.6 photographs caught.
    EXPECT_FALSE(state.current_job.present);
    EXPECT_TRUE(state.current_job.id.empty());
    EXPECT_TRUE(state.current_job.display_name.empty());
    EXPECT_FALSE(state.current_job.progress.known);
    EXPECT_FALSE(state.current_job.layers.known);
    EXPECT_FALSE(state.current_job.remaining.known);
    EXPECT_FALSE(state.current_job.elapsed.known);

    // Alert.
    EXPECT_FALSE(state.alert.active);
    EXPECT_TRUE(state.alert.code.empty());

    // ⛔ AND THE COMPLETION SNAPSHOT — owner ruling, 2026-08-24. It is owned by the printer that
    // produced it: "the last print finished at 62 %" is not a fact about the printer now configured.
    EXPECT_FALSE(state.last_completed_job.present);
    EXPECT_TRUE(state.last_completed_job.id.empty());
    EXPECT_TRUE(state.last_completed_job.display_name.empty());
    EXPECT_EQ(state.last_completed_job.terminal_state, PrinterState::Unknown);
    EXPECT_FALSE(state.last_completed_job.progress.known);
    EXPECT_FALSE(state.last_completed_job.layers.known);
    EXPECT_EQ(state.last_completed_job.completed_at, Timestamp{});

    // §8.1 ordering whose meaning belonged to the previous target.
    EXPECT_EQ(state.last_printer_generation, 0U);
    EXPECT_EQ(state.last_printer_sequence, 0U);
    EXPECT_EQ(state.last_job_sequence, 0U);

    // Derived truth: unknown/acquiring, never the previous printer at full confidence.
    EXPECT_EQ(state.confidence, Confidence::Unavailable);
    EXPECT_EQ(state.subject, SubjectState::AcquiringState);
    EXPECT_EQ(state.subject_condition, SubjectCondition::None);
    EXPECT_EQ(state.printer_connection.phase, ConnectionPhase::Unconfigured);
    EXPECT_EQ(state.access_condition, AccessCondition::None);
}

} // namespace

// --- The boundary itself ---------------------------------------------------

TEST(PrinterIdentityBoundaryTests, AN_ENDPOINT_CHANGE_FORGETS_THE_PREVIOUS_PRINTER) {
    const AppState observed = fully_observed_printer(at_ms(1000));

    // The control: the state really did hold all of it, so the assertions below are not passing on
    // a state that was empty to begin with.
    ASSERT_TRUE(observed.printer.ever_observed);
    ASSERT_EQ(observed.confidence, Confidence::Current);
    ASSERT_EQ(observed.subject, SubjectState::Printing);
    ASSERT_TRUE(observed.current_job.present);
    ASSERT_TRUE(observed.temperatures.nozzle_current.known);
    ASSERT_TRUE(observed.alert.active);
    ASSERT_FALSE(observed.printer_identity.display_name.empty());

    const AppState after =
        reduce(observed,
               Event::configuration_observed(at_ms(1100), configuration_for("printer-b.local", 3)));

    expect_previous_printer_forgotten(after);
}

TEST(PrinterIdentityBoundaryTests,
     A_CREDENTIAL_GENERATION_CHANGE_AT_THE_SAME_ADDRESS_IS_A_BOUNDARY) {
    // ⛔ THE CASE AN ADDRESS COMPARISON WOULD MISS. A user who re-enters an API key for the same
    // host may be pointing at a different machine behind that name, and in any case the previous
    // credential's observations were authorised differently.
    const AppState observed = fully_observed_printer(at_ms(1000));
    ASSERT_TRUE(observed.printer.ever_observed);

    const AppState after =
        reduce(observed,
               Event::configuration_observed(at_ms(1100), configuration_for("printer-a.local", 4)));

    expect_previous_printer_forgotten(after);
    EXPECT_EQ(after.configuration.credential_generation, 4U);
}

TEST(PrinterIdentityBoundaryTests, AnAuthenticationMethodChangeIsABoundary) {
    const AppState observed = fully_observed_printer(at_ms(1000));

    ConfigurationObservation changed = configuration_for("printer-a.local", 3);
    changed.auth_method = AuthMethod::Digest;

    expect_previous_printer_forgotten(
        reduce(observed, Event::configuration_observed(at_ms(1100), changed)));
}

TEST(PrinterIdentityBoundaryTests,
     REMOVING_AND_LATER_ADDING_A_PRINTER_CONFIGURATION_IS_TWO_BOUNDARIES) {
    const AppState observed = fully_observed_printer(at_ms(1000));

    // Removal: has_printer_configuration false is a boundary in its own right.
    ConfigurationObservation removed = configuration_for("", 3);
    removed.status = ConfigurationStatus::Missing;
    removed.has_printer_configuration = false;
    removed.printer_address.assign("");
    removed.printer_alias.assign("");
    removed.auth_method = AuthMethod::None;

    const AppState unconfigured =
        reduce(observed, Event::configuration_observed(at_ms(1100), removed));

    expect_previous_printer_forgotten(unconfigured);
    EXPECT_FALSE(unconfigured.configuration.has_printer_configuration);
    EXPECT_EQ(unconfigured.provisioning, ProvisioningPhase::SetupRequired);

    // A later addition is another boundary. Nothing may be resurrected across it either, so the
    // second half observes a printer and then adds a THIRD configuration.
    AppState added =
        reduce(unconfigured,
               Event::configuration_observed(at_ms(1200), configuration_for("printer-c.local", 5)));
    ASSERT_FALSE(added.printer.ever_observed)
        << "adding a configuration observes nothing by itself";

    added = reduce(added,
                   Event::printer_snapshot(at_ms(1300), printer_snapshot(PrinterState::Ready, 1)));
    ASSERT_TRUE(added.printer.ever_observed) << "the new printer's first report must be accepted";
    EXPECT_EQ(added.subject, SubjectState::Ready);

    expect_previous_printer_forgotten(
        reduce(added, Event::configuration_observed(at_ms(1400),
                                                    configuration_for("printer-d.local", 5))));
}

TEST(PrinterIdentityBoundaryTests, THE_NEW_CONFIGURATION_ITSELF_IS_PRESERVED) {
    // ⛔ THE BOUNDARY FORGETS OBSERVATIONS, NOT THE CONFIGURATION. The address and alias the user
    // just entered are the one thing this event does establish.
    const AppState after =
        reduce(fully_observed_printer(at_ms(1000)),
               Event::configuration_observed(at_ms(1100), configuration_for("printer-b.local", 6)));

    EXPECT_EQ(after.configuration.printer_address.view(), "printer-b.local");
    EXPECT_EQ(after.configuration.printer_alias.view(), "Workshop");
    EXPECT_EQ(after.configuration.auth_method, AuthMethod::ApiKey);
    EXPECT_EQ(after.configuration.credential_generation, 6U);
    EXPECT_TRUE(after.configuration.has_printer_configuration);
    EXPECT_EQ(after.configuration.status, ConfigurationStatus::Valid);
}

// --- Negative controls: what must NOT clear observations -------------------

TEST(PrinterIdentityBoundaryTests, AN_ALIAS_ONLY_CHANGE_KEEPS_EVERY_OBSERVATION) {
    // ⛔ THE PRINTER HAS NOT CHANGED. An alias is a label the user chose for it, and discarding a
    // live job because somebody renamed the printer would be its own defect.
    const AppState observed = fully_observed_printer(at_ms(1000));

    ConfigurationObservation renamed = configuration_for("printer-a.local", 3);
    renamed.printer_alias.assign("Bench");

    const AppState after = reduce(observed, Event::configuration_observed(at_ms(1100), renamed));

    EXPECT_EQ(after.configuration.printer_alias.view(), "Bench");
    EXPECT_TRUE(after.printer.ever_observed);
    EXPECT_EQ(after.printer.normalized, PrinterState::Printing);
    EXPECT_EQ(after.subject, SubjectState::Printing);
    EXPECT_EQ(after.confidence, Confidence::Current);
    EXPECT_TRUE(after.current_job.present);
    EXPECT_TRUE(after.current_job.progress.known);
    EXPECT_TRUE(after.temperatures.nozzle_current.known);
    EXPECT_TRUE(after.alert.active);
    EXPECT_FALSE(after.printer_identity.display_name.empty());
    EXPECT_EQ(after.last_printer_sequence, 4U);
    EXPECT_EQ(after.last_job_sequence, 9U);
}

TEST(PrinterIdentityBoundaryTests, AnIdenticalConfigurationReportKeepsEveryObservation) {
    // Configuration is re-reported at every boot and after unrelated setting changes. Treating that
    // as a boundary would erase a live job on a timer.
    const AppState observed = fully_observed_printer(at_ms(1000));
    const AppState after =
        reduce(observed,
               Event::configuration_observed(at_ms(1100), configuration_for("printer-a.local", 3)));

    EXPECT_TRUE(after.printer.ever_observed);
    EXPECT_TRUE(after.current_job.present);
    EXPECT_EQ(after.confidence, Confidence::Current);
    EXPECT_EQ(after.subject, SubjectState::Printing);
}

TEST(PrinterIdentityBoundaryTests, A_BRIGHTNESS_CHANGE_KEEPS_EVERY_OBSERVATION) {
    // A device setting is not a printer identity. This one does not even reach
    // apply_configuration(), and the test exists so that stays true.
    const AppState observed = fully_observed_printer(at_ms(1000));
    const AppState after = reduce(observed, Event::display_brightness(at_ms(1100), 40));

    EXPECT_EQ(after.display.brightness_percent, 40);
    EXPECT_TRUE(after.printer.ever_observed);
    EXPECT_TRUE(after.current_job.present);
    EXPECT_EQ(after.confidence, Confidence::Current);
}

TEST(PrinterIdentityBoundaryTests, A_SCHEMA_VERSION_CHANGE_ALONE_KEEPS_EVERY_OBSERVATION) {
    const AppState observed = fully_observed_printer(at_ms(1000));

    ConfigurationObservation migrated = configuration_for("printer-a.local", 3);
    migrated.schema_version = 2;

    const AppState after = reduce(observed, Event::configuration_observed(at_ms(1100), migrated));

    EXPECT_EQ(after.configuration.schema_version, 2U);
    EXPECT_TRUE(after.printer.ever_observed);
    EXPECT_TRUE(after.current_job.present);
}

// --- The unchanged target still ages, badges and expires as before --------

TEST(PrinterIdentityBoundaryTests,
     A_TRANSIENT_DISCONNECT_ON_AN_UNCHANGED_TARGET_KEEPS_STALE_SEMANTICS) {
    // ⛔ THE DISTINCTION THE RULING DRAWS. Temporary loss of the SAME known target is exactly what
    // §11.5's stale indicator is for: the values were true of this printer and are merely old. This
    // behaviour must be untouched by the boundary work.
    const AppState observed = fully_observed_printer(at_ms(1000));

    // The connection drops, and the configuration is re-reported unchanged alongside it.
    AppState degraded = with_connection(observed, ConnectionPhase::PrinterUnavailable, at_ms(1100));
    degraded =
        reduce(degraded,
               Event::configuration_observed(at_ms(1150), configuration_for("printer-a.local", 3)));

    // The subject and its job are RETAINED, which is the stale-state contract.
    EXPECT_TRUE(degraded.printer.ever_observed);
    EXPECT_EQ(degraded.subject, SubjectState::Printing);
    EXPECT_TRUE(degraded.current_job.present);

    // And ageing still walks the confidence ladder rather than jumping to Unavailable.
    const AppState aged = reduce(degraded, Event::confidence_aged(at_ms(1000 + 20000)));
    EXPECT_TRUE(aged.printer.ever_observed);
    EXPECT_EQ(aged.confidence, Confidence::Degraded)
        << "a stale unchanged target is Degraded, not Unavailable";
    EXPECT_EQ(aged.printer_connection.freshness, DataFreshness::Stale);
}

TEST(PrinterIdentityBoundaryTests, ABoundaryIsNotTheSameAsAgeing) {
    // The two paths must produce different confidence for the same elapsed time: ageing keeps a
    // known subject at reduced confidence, a boundary has no subject at all.
    const AppState observed = fully_observed_printer(at_ms(1000));

    const AppState aged = reduce(observed, Event::confidence_aged(at_ms(1000 + 20000)));
    const AppState crossed =
        reduce(observed,
               Event::configuration_observed(at_ms(1100), configuration_for("printer-b.local", 3)));

    EXPECT_EQ(aged.confidence, Confidence::Degraded);
    EXPECT_EQ(aged.subject, SubjectState::Printing);

    EXPECT_EQ(crossed.confidence, Confidence::Unavailable);
    EXPECT_EQ(crossed.subject, SubjectState::AcquiringState);
}

// --- A superseded worker cannot repopulate what the boundary cleared ------

TEST(PrinterIdentityBoundaryTests, THE_REDUCER_ALONE_WOULD_ACCEPT_A_SUPERSEDED_WORKERS_SNAPSHOT) {
    // ⛔ THIS IS WHY THE INGRESS SEAM IS LOAD-BEARING, AND IT IS ASSERTED RATHER THAN ASSUMED.
    // Clearing the watermarks to zero means snapshot_is_current() sees a DIFFERENT generation for
    // the old worker's late report and accepts it — that is the correct rule for a replacement
    // worker and it cannot also reject a departing one. So the reducer is not, and is not meant to
    // be, the thing that stops this.
    EXPECT_TRUE(snapshot_is_current({kFirstGeneration, 40}, {0, 0}));

    const AppState cleared =
        reduce(fully_observed_printer(at_ms(1000)),
               Event::configuration_observed(at_ms(1100), configuration_for("printer-b.local", 3)));
    ASSERT_FALSE(cleared.printer.ever_observed);

    const AppState repopulated =
        reduce(cleared,
               Event::printer_snapshot(at_ms(1200), printer_snapshot(PrinterState::Printing, 41)));

    EXPECT_TRUE(repopulated.printer.ever_observed)
        << "the reducer applies any accepted snapshot; the guard is upstream";
}

TEST(PrinterIdentityBoundaryTests, THE_INGRESS_SEAM_REJECTS_THE_SUPERSEDED_WORKERS_LATE_REPORT) {
    // ⛔ THE PRODUCTION GUARANTEE, AT THE SEAM THAT ACTUALLY OWNS IT. app_core arms a session for
    // the worker it started; printer_session::classify() runs on the application task and refuses
    // any report from another generation BEFORE a single field becomes an observation. So the late
    // report never reaches the reducer tested above.
    // Armed exactly as printer_launch() does: the lease start() created, and the transaction the
    // worker actually holds. A transaction naming no profile is refused by armed(), so a valid one
    // is required here for the same reason production supplies one.
    const prusalink::Transaction held{kProfile, kAttempt};
    const auto session =
        app_core::printer_session::armed(prusalink::Purpose::Poll, kSecondGeneration, held);
    ASSERT_TRUE(session.armed) << "the session must be armed, or every verdict below is NoSession";

    prusalink::Report late;
    late.purpose = prusalink::Purpose::Poll;
    late.generation = kFirstGeneration;
    late.transaction = held;
    late.status_present = true;

    EXPECT_EQ(app_core::printer_session::classify(session, late, false),
              app_core::printer_session::Ingress::GenerationSuperseded);

    // The control: the SAME report from the armed generation is accepted, so the rejection above is
    // about identity and not about the report being malformed or the session being unarmed.
    prusalink::Report current = late;
    current.generation = kSecondGeneration;
    EXPECT_EQ(app_core::printer_session::classify(session, current, false),
              app_core::printer_session::Ingress::Accept);
}

TEST(PrinterIdentityBoundaryTests, ADisarmedSessionRejectsEveryReport) {
    // Between the boundary and the replacement worker's launch, nothing is armed. A report arriving
    // in that window has no session to match.
    const app_core::printer_session::Session none;

    prusalink::Report late;
    late.purpose = prusalink::Purpose::Poll;
    late.generation = kFirstGeneration;
    late.transaction = prusalink::Transaction{kProfile, kAttempt};
    late.status_present = true;

    EXPECT_EQ(app_core::printer_session::classify(none, late, false),
              app_core::printer_session::Ingress::NoSession);
}

// --- The worker must also be superseded, or the clearing is undone ---------

TEST(PrinterIdentityBoundaryTests, AN_ELIGIBLE_WORKER_POINTED_AT_THE_OLD_PRINTER_IS_SUPERSEDED) {
    // ⛔ THE SECOND HALF OF ITEM 4, AND THE ONE THAT IS EASY TO MISS. printer_supervise() stopped a
    // worker only when polling became INELIGIBLE — no configuration, no network, provisioning open,
    // a reset running. A worker polling the PREVIOUS printer is eligible by every one of those
    // terms; it is simply pointed at the wrong machine. Left running, its next report matches the
    // armed session, classifies as Accept, and refills everything the reducer just cleared.
    const AppState observed = fully_observed_printer(at_ms(1000));

    app_core::printer_session::Polling polling;
    polling.session = app_core::printer_session::armed(prusalink::Purpose::Poll, kFirstGeneration,
                                                       prusalink::Transaction{kProfile, kAttempt});
    polling.target = app_core::printer_session::target_of(observed.configuration);

    // The control: nothing has changed, so the worker keeps running.
    EXPECT_FALSE(app_core::printer_session::target_superseded(polling, observed.configuration));

    // Endpoint, credential generation, auth method and configured-status each supersede it.
    const AppState moved =
        reduce(observed,
               Event::configuration_observed(at_ms(1100), configuration_for("printer-b.local", 3)));
    EXPECT_TRUE(app_core::printer_session::target_superseded(polling, moved.configuration));

    const AppState rekeyed =
        reduce(observed,
               Event::configuration_observed(at_ms(1100), configuration_for("printer-a.local", 4)));
    EXPECT_TRUE(app_core::printer_session::target_superseded(polling, rekeyed.configuration));

    ConfigurationObservation digest = configuration_for("printer-a.local", 3);
    digest.auth_method = AuthMethod::Digest;
    const AppState reauthed = reduce(observed, Event::configuration_observed(at_ms(1100), digest));
    EXPECT_TRUE(app_core::printer_session::target_superseded(polling, reauthed.configuration));

    ConfigurationObservation removed = configuration_for("printer-a.local", 3);
    removed.has_printer_configuration = false;
    const AppState unconfigured =
        reduce(observed, Event::configuration_observed(at_ms(1100), removed));
    EXPECT_TRUE(app_core::printer_session::target_superseded(polling, unconfigured.configuration));
}

TEST(PrinterIdentityBoundaryTests, AN_ALIAS_ONLY_CHANGE_DOES_NOT_SUPERSEDE_THE_WORKER) {
    // The same exclusion apply_configuration() makes. Stopping the worker and draining its channel
    // because somebody renamed the printer would discard a live job for a label.
    const AppState observed = fully_observed_printer(at_ms(1000));

    app_core::printer_session::Polling polling;
    polling.session = app_core::printer_session::armed(prusalink::Purpose::Poll, kFirstGeneration,
                                                       prusalink::Transaction{kProfile, kAttempt});
    polling.target = app_core::printer_session::target_of(observed.configuration);

    ConfigurationObservation renamed = configuration_for("printer-a.local", 3);
    renamed.printer_alias.assign("Bench");
    const AppState after = reduce(observed, Event::configuration_observed(at_ms(1100), renamed));

    EXPECT_FALSE(app_core::printer_session::target_superseded(polling, after.configuration));

    // And neither does an unrelated setting or a re-report of the same configuration.
    const AppState dimmed = reduce(observed, Event::display_brightness(at_ms(1100), 40));
    EXPECT_FALSE(app_core::printer_session::target_superseded(polling, dimmed.configuration));
}

TEST(PrinterIdentityBoundaryTests, AnUnarmedPollingRecordIsNeverSuperseded) {
    // ⛔ OTHERWISE AN IDLE DEVICE WOULD STOP AND DRAIN ON EVERY TICK. "Nothing is running" is not
    // "something wrong is running".
    const app_core::printer_session::Polling idle;
    const AppState observed = fully_observed_printer(at_ms(1000));

    EXPECT_FALSE(app_core::printer_session::target_superseded(idle, observed.configuration));

    const AppState moved =
        reduce(observed,
               Event::configuration_observed(at_ms(1100), configuration_for("printer-b.local", 3)));
    EXPECT_FALSE(app_core::printer_session::target_superseded(idle, moved.configuration));
}

TEST(PrinterIdentityBoundaryTests,
     THE_SUPERSEDED_WORKERS_REPORT_IS_REFUSED_ONCE_THE_SESSION_IS_FORGOTTEN) {
    // ⬅️ THE TWO HALVES, JOINED. printer_supervise() assigns a default Polling — session AND target
    // in one act — so by the time printer_ingest() runs on the same tick there is no session to
    // match. This is the state that assignment produces, and the verdict it forces.
    const app_core::printer_session::Polling forgotten;

    prusalink::Report late;
    late.purpose = prusalink::Purpose::Poll;
    late.generation = kFirstGeneration;
    late.transaction = prusalink::Transaction{kProfile, kAttempt};
    late.status_present = true;

    EXPECT_EQ(app_core::printer_session::classify(forgotten.session, late, false),
              app_core::printer_session::Ingress::NoSession);

    // And a replacement worker's own reports are accepted, so the supersession is not a permanent
    // silence.
    app_core::printer_session::Polling replacement;
    replacement.session = app_core::printer_session::armed(
        prusalink::Purpose::Poll, kSecondGeneration, prusalink::Transaction{kProfile + 1, 0});

    prusalink::Report fresh;
    fresh.purpose = prusalink::Purpose::Poll;
    fresh.generation = kSecondGeneration;
    fresh.transaction = prusalink::Transaction{kProfile + 1, 0};
    fresh.status_present = true;

    EXPECT_EQ(app_core::printer_session::classify(replacement.session, fresh, false),
              app_core::printer_session::Ingress::Accept);
}

// --- The completion snapshot follows the printer that produced it ----------
//
// ⛔ OWNER RULING, 2026-08-24. `last_completed_job` is data owned by the printer that produced it.
// Its persisted lifetime lets it survive a RESTART, and only while the configured printer identity is
// unchanged; a printer-identity boundary invalidates it exactly as it invalidates identity,
// operational state, temperatures, current job and alert.
//
// ⬅️ NO PERSISTENCE IS EXERCISED HERE BECAUSE NONE EXISTS. There is no completion-snapshot storage
// path in this firmware — no key, no record, no erase effect. These tests cover the in-memory
// snapshot the reducer owns, which is the whole of what the ruling changes today.

TEST(PrinterIdentityBoundaryTests, THE_PRE_BOUNDARY_STATE_GENUINELY_HOLDS_A_COMPLETION_SNAPSHOT) {
    // ⛔ THE CONTROL EVERY CLEARING ASSERTION BELOW DEPENDS ON. Without it, "the snapshot is empty
    // afterwards" would pass on a state that never had one.
    const AppState finished = printer_with_completed_job(at_ms(1000));

    expect_completion_present(finished);

    // And it came from the production route: the job was present and the printer reported terminal.
    EXPECT_EQ(finished.printer.normalized, PrinterState::Finished);
    EXPECT_TRUE(finished.current_job.present);
}

TEST(PrinterIdentityBoundaryTests, AN_ENDPOINT_CHANGE_CLEARS_THE_COMPLETION_SNAPSHOT) {
    const AppState finished = printer_with_completed_job(at_ms(1000));
    expect_completion_present(finished);

    expect_previous_printer_forgotten(
        reduce(finished, Event::configuration_observed(at_ms(1100),
                                                       configuration_for("printer-b.local", 3))));
}

TEST(PrinterIdentityBoundaryTests,
     A_CREDENTIAL_OR_AUTH_IDENTITY_CHANGE_CLEARS_THE_COMPLETION_SNAPSHOT) {
    const AppState finished = printer_with_completed_job(at_ms(1000));
    expect_completion_present(finished);

    // A re-entered API key at the same address.
    expect_previous_printer_forgotten(
        reduce(finished, Event::configuration_observed(at_ms(1100),
                                                       configuration_for("printer-a.local", 4))));

    // And a different authentication identity at the same address with the same generation.
    ConfigurationObservation digest = configuration_for("printer-a.local", 3);
    digest.auth_method = AuthMethod::Digest;
    expect_previous_printer_forgotten(
        reduce(finished, Event::configuration_observed(at_ms(1100), digest)));
}

TEST(PrinterIdentityBoundaryTests,
     A_NEW_CONFIGURED_IDENTITY_DOES_NOT_RESURRECT_THE_OLD_COMPLETION) {
    // ⛔ TWO BOUNDARIES IN SEQUENCE. Removing the configuration clears the snapshot; adding a
    // different printer later must not bring it back, and must not inherit it either.
    const AppState finished = printer_with_completed_job(at_ms(1000));
    expect_completion_present(finished);

    ConfigurationObservation removed = configuration_for("", 3);
    removed.status = ConfigurationStatus::Missing;
    removed.has_printer_configuration = false;
    removed.printer_address.assign("");
    removed.printer_alias.assign("");
    removed.auth_method = AuthMethod::None;

    const AppState unconfigured =
        reduce(finished, Event::configuration_observed(at_ms(1100), removed));
    expect_previous_printer_forgotten(unconfigured);

    const AppState reconfigured =
        reduce(unconfigured,
               Event::configuration_observed(at_ms(1200), configuration_for("printer-c.local", 5)));

    EXPECT_FALSE(reconfigured.last_completed_job.present)
        << "a new printer must not inherit the previous printer's completed print";
    EXPECT_TRUE(reconfigured.last_completed_job.id.empty());

    // And observing the new printer does not resurrect it either — only its OWN completion could.
    const AppState observing =
        reduce(reconfigured,
               Event::printer_snapshot(at_ms(1300), printer_snapshot(PrinterState::Ready, 1)));
    EXPECT_FALSE(observing.last_completed_job.present);
}

TEST(PrinterIdentityBoundaryTests, NON_IDENTITY_CHANGES_PRESERVE_THE_COMPLETION_SNAPSHOT) {
    // ⛔ THE NEGATIVE CONTROLS. The printer has not changed, so its completed print is still its own.
    const AppState finished = printer_with_completed_job(at_ms(1000));
    expect_completion_present(finished);

    // Alias only.
    ConfigurationObservation renamed = configuration_for("printer-a.local", 3);
    renamed.printer_alias.assign("Bench");
    const AppState after_rename =
        reduce(finished, Event::configuration_observed(at_ms(1100), renamed));
    EXPECT_TRUE(after_rename.last_completed_job.present);
    EXPECT_EQ(after_rename.last_completed_job.id.view(), "job-1");

    // Brightness only — a device setting, which never reaches apply_configuration() at all.
    const AppState after_dim = reduce(finished, Event::display_brightness(at_ms(1100), 40));
    EXPECT_TRUE(after_dim.last_completed_job.present);

    // An identical configuration re-report, which happens at every boot and after unrelated
    // settings changes.
    const AppState after_rereport =
        reduce(finished,
               Event::configuration_observed(at_ms(1100), configuration_for("printer-a.local", 3)));
    EXPECT_TRUE(after_rereport.last_completed_job.present);
    EXPECT_EQ(after_rereport.last_completed_job.terminal_state, PrinterState::Finished);
}

TEST(PrinterIdentityBoundaryTests, A_TRANSIENT_DISCONNECT_PRESERVES_THE_COMPLETION_SNAPSHOT) {
    // Temporary loss of the SAME known target. The print really did finish on this printer, and the
    // network dropping afterwards does not unmake that.
    const AppState finished = printer_with_completed_job(at_ms(1000));
    expect_completion_present(finished);

    AppState degraded = with_connection(finished, ConnectionPhase::PrinterUnavailable, at_ms(1100));
    degraded =
        reduce(degraded,
               Event::configuration_observed(at_ms(1150), configuration_for("printer-a.local", 3)));

    EXPECT_TRUE(degraded.last_completed_job.present);

    // Including after ageing has walked confidence down — expiry removes dynamic values, not the
    // record of a finished print.
    const AppState aged = reduce(degraded, Event::confidence_aged(at_ms(1000 + 120000)));
    EXPECT_TRUE(aged.last_completed_job.present);
    EXPECT_EQ(aged.last_completed_job.id.view(), "job-1");
}

TEST(PrinterIdentityBoundaryTests, BOOT_RESTORATION_FOR_AN_UNCHANGED_PRINTER_IS_UNAFFECTED) {
    // ⛔ REQUIREMENT 2 OF THE RULING, AND THE HALF THAT MUST NOT REGRESS. §15.1 step 11 restores the
    // snapshot during boot, and the BootPhase::Starting branch deliberately leaves it alone — so
    // clearing it here would erase, moments later, the record the boot had just restored.
    const AppState finished = printer_with_completed_job(at_ms(1000));
    expect_completion_present(finished);

    // The boot prelude clears the job and the watermarks...
    const AppState starting = reduce(finished, Event::boot_phase(at_ms(2000), BootPhase::Starting));

    EXPECT_FALSE(starting.current_job.present) << "the boot branch still clears the current job";
    EXPECT_EQ(starting.last_printer_sequence, 0U);

    // ...and the completion snapshot survives it.
    EXPECT_TRUE(starting.last_completed_job.present)
        << "a snapshot restored for the same printer must survive a restart";
    EXPECT_EQ(starting.last_completed_job.id.view(), "job-1");
    EXPECT_EQ(starting.last_completed_job.terminal_state, PrinterState::Finished);

    // And the configuration re-report the boot sequence performs for the SAME printer does not
    // invalidate it, which is what makes the survival real rather than momentary.
    const AppState booted =
        reduce(starting,
               Event::configuration_observed(at_ms(2100), configuration_for("printer-a.local", 3)));

    EXPECT_TRUE(booted.last_completed_job.present);
    EXPECT_EQ(booted.last_completed_job.id.view(), "job-1");
}

} // namespace prd::app_model
