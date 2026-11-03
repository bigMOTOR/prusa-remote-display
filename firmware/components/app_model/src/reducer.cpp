// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include "app_model/reducer.hpp"

// ⛔ FOR THE ONE AUTHORITATIVE PRECEDENCE SEAM, and for nothing else. The reducer needs
// subject_presentation_owns_panel() to decide who owns a lost-observation acknowledgement;
// keeping its own copy of §12's order is what let the two drift on BootPhase::FatalError.
#include "app_model/presentation.hpp"

#include "app_model/display_policy.hpp"

// The reducer is written as one small function per fact, plus one derivation pass
// that runs after every fact. That shape is deliberate: the derivations —
// subject, condition, confidence, access condition, device operation — must be
// recomputed even for events that carry no printer data at all, because
// ConfidenceAged changes nothing but the age. Recomputing them in one place is
// what stops each individual event handler from having to remember the whole
// architecture.

namespace prd::app_model {
namespace {

// UIAD 001 §12.2, and §18.5: an exclusive operation, or a device fault, blocks
// ordinary user-driven mutation. A firmware install must not be interrupted by
// someone opening Settings.
[[nodiscard]] bool blocks_user_mutations(const DeviceOperation operation) {
    return operation == DeviceOperation::FirmwareUpdating ||
           operation == DeviceOperation::FactoryResetting ||
           operation == DeviceOperation::DeviceFault;
}

// The OTA phases during which the device is doing exclusive work. Checking and
// Available deliberately are not: a background check must not take over the
// screen, and §18.6 forbids silent installation, so "available" is information
// rather than an operation.

// ⛔ RAISING A DEVICE FAULT, WITH PRECEDENCE — audit item 5, 2026-08-28. The ONE place any fault is
// set, so the rule cannot be forgotten at a fifth call site.
//
// A fault that holds the device (see app_model::fault_holds_device) is not replaced by a different
// reason. Without this, a NetworkObserved already queued when a factory reset failed reached the
// next drain and overwrote FactoryResetIncomplete with WifiUnavailable — releasing the exclusive
// barrier and replacing a truthful "Reset could not finish" on the panel with "Wi-Fi unavailable".
//
// ⬅️ IT IS A FLOOR, NOT A RANKING. Nothing here orders the six ordinary reasons against each other;
// they replace one another exactly as they did. All this refuses is LOWERING a held fault, which is
// the only case where replacement changes what the device is allowed to do.
//
// ⬅️ AND THE SAME REASON RE-RAISED IS ACCEPTED, so a repeated observation stays idempotent.
void raise_device_fault(AppState& state, const DeviceFaultReason reason) {
    if (state.device_fault != reason && fault_holds_device(state.device_fault)) {
        return;
    }

    state.device_fault = reason;

    // ⛔ AND THE EXECUTION FACT IS LATCHED SEPARATELY. The panel's reason and "may the reset executor
    // run again" are different questions; deriving the second from the first is what let a late
    // observation restart a destructive operation.
    if (reason == DeviceFaultReason::FactoryResetIncomplete) {
        state.factory_reset_failed = true;
    }
}

// UIAD 001 §10's resolution order, in one place. DeviceFault is first because §10
// gives it the highest priority. Factory reset precedes a firmware install only
// for determinism; the two cannot both be live, because confirming a reset is
// blocked while an install runs.
[[nodiscard]] DeviceOperation resolve_device_operation(const AppState& state) {
    if (state.device_fault != DeviceFaultReason::None) {
        return DeviceOperation::DeviceFault;
    }

    if (state.factory_resetting) {
        return DeviceOperation::FactoryResetting;
    }

    if (ota_holds_device(state.ota.phase)) {
        return DeviceOperation::FirmwareUpdating;
    }

    if (state.recovery_mode) {
        return DeviceOperation::RecoveryMode;
    }

    return DeviceOperation::None;
}

// UIAD 001 §8: an access condition is set once positively classified and is not
// transient (§16.2 — "Authentication errors and unsupported API errors are not
// transient network failures"). It therefore persists through connection retries
// and is cleared only by a positive reclassification: a successful connection, or
// a configuration change (handled where configuration is applied).
[[nodiscard]] AccessCondition access_from(const ConnectionPhase phase,
                                          const AccessCondition previous) {
    switch (phase) {
    case ConnectionPhase::Unauthorized:
        return AccessCondition::AuthenticationRequired;

    case ConnectionPhase::UnsupportedApi:
        return AccessCondition::UnsupportedInterface;

    case ConnectionPhase::Connected:
        return AccessCondition::None;

    case ConnectionPhase::Unconfigured:
    case ConnectionPhase::WaitingForNetwork:
    case ConnectionPhase::Resolving:
    case ConnectionPhase::Probing:
    case ConnectionPhase::Authenticating:
    case ConnectionPhase::Degraded:
    case ConnectionPhase::PrinterUnavailable:
    case ConnectionPhase::InvalidResponse:
        break;
    }

    return previous;
}

// §8.1: "rejects stale responses using request generation or sequence IDs".
//
// Sequence 0 means unsequenced and is always accepted, so a producer with no
// generation counter — a fixture, a fake source — still works without inventing
// one. Any non-zero sequence must strictly exceed the last accepted one, which
// makes both a duplicate and a late arrival harmless.
//
// ⬅️ THIS IS THE GENERAL JOB PATH'S RULE AND IT IS UNCHANGED. A job read from a STATUS RESPONSE is
// ordered by that response's own `{generation, sequence}` in apply_status() instead, because the
// two halves of one reading cannot be older or newer than one another.
[[nodiscard]] bool sequence_is_current(const std::uint32_t sequence,
                                       const std::uint32_t last_accepted) {
    return sequence == 0 || sequence > last_accepted;
}

} // namespace

bool snapshot_is_current(const SnapshotOrder incoming, const SnapshotOrder last) {
    // A different worker: its ordering starts fresh, so its first snapshot is accepted whatever the
    // previous worker had reached. Inequality rather than "greater than" on purpose — magnitude
    // would reintroduce the watermark problem the moment a generation wrapped.
    if (incoming.generation != last.generation) {
        return true;
    }

    // Same worker: §8.1's rule, unchanged. Zero remains the escape hatch for a producer that does
    // not sequence at all.
    return incoming.sequence == 0 || incoming.sequence > last.sequence;
}

namespace {

// --- Boot and device lifecycle ---------------------------------------------

void apply_boot_phase(AppState& state, const BootPhaseObservation& observation) {
    state.boot.phase = observation.phase;

    // ENTERING `Starting` CLEARS THE DEVICE-OWNED CONDITIONS.
    //
    // A device that is starting has not yet faulted, is not in recovery, is not resetting
    // and has no modal surface open. On real hardware that is true by construction — a
    // boot begins from a default-constructed AppState — so this branch is a no-op in
    // production and cannot change observed behaviour.
    //
    // It exists because the model can also be *replayed*, and there the difference
    // matters. UIAD 001 §10 gives Device Fault the highest presentation priority and §16.1
    // makes it terminal: no event clears one, because on a real device the recovery is a
    // reboot. So a scripted sequence that raised a fault and then replayed a boot left the
    // fault standing, every later presentation resolved to Device Fault, and nothing else
    // could ever be presented again. That was observed on the physical unit during
    // Milestone 2 Phase C: 160 presentations at layer=device-fault against 58 at
    // subject-presentation, and the product scene never drawn after the fault scenario.
    //
    // `Starting` is the right place for it precisely because a boot prelude IS the reboot
    // the recovery requires. Nothing is being papered over: a genuine persistent fault is
    // re-raised moments later by apply_hardware_probe(), which is the only thing that can
    // still see it.
    if (observation.phase == BootPhase::Starting) {
        // ⬅️ ASSIGNED DIRECTLY, NOT THROUGH raise_device_fault(). This is the clear, and the whole
        // point of the precedence rule above is that nothing else can perform one — an actual boot
        // is the recovery the panel asks the user for, and it is the only thing that ends a held
        // fault or the reset-failure latch.
        state.device_fault = DeviceFaultReason::None;
        state.factory_reset_failed = false;
        state.recovery_mode = false;
        state.factory_resetting = false;
        state.modal_surface = ModalSurface::None;

        // AND THE OBSERVATION WATERMARKS. A device that is starting has observed nothing, so
        // it has no sequence to compare against — the next report it receives is by definition
        // the first, whatever number the printer happens to put on it.
        //
        // Same shape as the conditions above, and the same argument: on real hardware this is
        // a no-op, because a boot begins from a default AppState where both are already zero.
        // It matters only when the model is REPLAYED, and there it is the difference between a
        // fresh sequence being accepted and being rejected as stale.
        //
        // Found by replaying two fixtures into one store: the second's observations carried
        // sequence numbers that did not exceed the first's, so the reducer discarded them and
        // the state was left as a mixture of the two — a subject from one and a confidence
        // from the other, resolving to a presentation with no scene. On the panel that is a
        // black screen.
        state.last_printer_generation = 0;
        state.last_printer_sequence = 0;
        state.last_job_sequence = 0;

        // AND THE JOB ITSELF. One step further than the watermarks, and the same argument:
        // a device that is starting has observed nothing, so it holds no job.
        //
        // UI Design Decision 004 §7 is the rule — a component is present if and only if the
        // machine supplies its value "and that value is true of the machine now". A job
        // carried over from before a reboot fails the second test outright: the device has
        // not been told anything about it since starting, and it may not even be the same
        // printer. Resetting the watermarks without resetting this left the model asserting
        // a job on no evidence.
        //
        // On real hardware this is a no-op for the third time in this branch: a boot begins
        // from a default-constructed AppState, where current_job is already empty.
        //
        // Replayed, it is the F.6 finding. A fixture whose script observes no job inherited
        // whatever the store still held, so `printing-unknown-progress` showed Printing's
        // job at E.6, and Ready and Preparing showed `156/250 Layer` in the F.6 photographs
        // — a layer count belonging to a print that, as far as the replayed device was
        // concerned, had never happened.
        //
        // `last_completed_job` IS DELIBERATELY NOT CLEARED HERE. It is §8.1's completion
        // snapshot, which exists precisely to survive the job it describes: Architecture
        // Spec §15.1 loads it from storage during boot (step 11), so clearing it here would
        // erase, moments later, the very record the boot sequence had just restored.
        //
        // ⛔ THAT IS A STATEMENT ABOUT RESTART, NOT ABOUT LIFETIME IN GENERAL. Owner ruling,
        // 2026-08-24: the snapshot is owned by the printer that produced it, so it survives a
        // restart only while the configured printer identity is unchanged, and a printer-identity
        // boundary invalidates it. forget_previous_printer() below is where that happens. Boot and
        // boundary are different questions and this branch answers only the first.
        state.current_job = PrintJob{};
    }
}

void apply_hardware_probe(AppState& state, const HardwareProbeObservation& observation) {
    state.boot.board_target = observation.board_target;
    state.boot.reset_reason = observation.reset_reason;
    state.boot.flash_validation = observation.flash_validation;
    state.boot.psram_validation = observation.psram_validation;

    // §12.4: a capacity mismatch "should generate a clear diagnostic and, when
    // possible, a visible unsupported-hardware screen", and §16.1 classifies a
    // board mismatch as a device error. This is the only place a board mismatch
    // becomes a fault, and it does not abort the boot — it decides what would be
    // presented, which is the reducer's business rather than the boot flow's.
    if (observation.flash_validation == ValidationState::Failed ||
        observation.psram_validation == ValidationState::Failed) {
        raise_device_fault(state, DeviceFaultReason::BoardMismatch);
    }
}

void apply_storage(AppState& state, const StorageObservation& observation) {
    state.boot.storage = observation.status;

    if (observation.status == StorageStatus::Failed) {
        raise_device_fault(state, DeviceFaultReason::StorageFailure);
    }
}

// --- Setup and configuration lifecycle -------------------------------------

// Whether an observed configuration names a DIFFERENT PRINTER IDENTITY than the one
// currently held — a different endpoint, a different authentication identity, a
// replaced credential, or the appearance or removal of a printer configuration at all.
//
// The distinction matters twice over. §16.2 permits a retry "only after configuration
// change", and a configuration is re-reported at every boot and after any unrelated
// setting changes: treating every report as a change would retry an endpoint already
// known to reject us; treating none as a change would strand a user who has just
// corrected their API key. And §11.5 forbids presenting stale printer information as
// current, which is what makes this predicate the invalidation boundary below.
//
// ⛔ WHAT IS DELIBERATELY NOT HERE. `printer_alias` is a label the user chose for a
// printer that has not changed, and brightness, dim and sleep delays never reach this
// function at all. Neither may discard a valid observation — owner ruling, 2026-08-24.
[[nodiscard]] bool printer_identity_changed(const ConfigurationState& current,
                                            const ConfigurationObservation& observation) {
    return current.printer_address != observation.printer_address ||
           current.auth_method != observation.auth_method ||
           current.has_printer_configuration != observation.has_printer_configuration ||
           current.credential_generation != observation.credential_generation;
}

// Everything observed FROM a printer, forgotten because the printer it was observed
// from is no longer the printer being configured — audit item 4, owner rulings 1 and 2
// of 2026-08-24.
//
// ⛔ CLEARED, NOT BADGED STALE, AND THAT IS THE RULING. §11.5's stale indicator exists
// for temporary loss of the SAME known target: the values were true of that printer and
// are merely old. Across an identity boundary they are not old, they are UNRELATED — the
// relationship between the previous printer's job and the newly configured one is not
// "recent", it is unknown. Presenting them behind a badge would still be presenting them.
//
// ⬅️ WHY EACH ONE, because "clear the printer data" is not a specific enough instruction
// to be checkable:
//
//   printer_identity   the reported name, hostname, serial and firmware of the old target
//   printer            normalized/raw state, busy and attention flags, observed_at, and
//                      ever_observed — the last of which is what makes confidence fall to
//                      Unavailable through confidence_from()
//   temperatures       readings from a different machine's heaters
//   current_job        the file, progress, layers and estimates of a print that is not
//                      running on the printer now configured
//   alert              a condition raised by the previous target
//   last_completed_job §8.1's completion snapshot — see the ruling below
//   the watermarks     §8.1 ordering whose meaning was "later than the old worker's Nth
//                      report"; a replacement worker starts at 1 and must not lose to it
//   subject            set explicitly, because subject_from_printer_state() answers
//                      "keep what you had" for Unknown — see the note at the call site
//
// ⛔ subject_condition, confidence, access_condition and device_operation are NOT set here.
// derive_layers() recomputes all four from the fields above on this same reduction, so
// assigning them would be a second opinion that could disagree.
//
// ⛔ last_completed_job IS CLEARED HERE — owner ruling, 2026-08-24, and it settles a question this
// function previously recorded as open. The completion snapshot is DATA OWNED BY THE PRINTER THAT
// PRODUCED IT. Its persisted lifetime lets it survive a RESTART, and only while the configured
// printer identity is unchanged; a change of identity invalidates it on exactly the same footing as
// the identity, the operational state, the temperatures, the current job and the alert. "The last
// print finished at 62 %" is not a fact about the printer now configured.
//
// ⬅️ AND THE BOOT PATH IS UNCHANGED, WHICH IS THE OTHER HALF OF THE RULING. The
// BootPhase::Starting branch above still leaves it alone, so a snapshot restored for the SAME
// configured printer still survives a reboot — §15.1 step 11's restore is not undone moments later.
// Only the identity boundary invalidates it.
//
// ⛔ AND NO PERSISTENCE IS IMPLEMENTED OR IMPLIED BY THIS. There is no completion-snapshot storage
// path in this firmware yet: no key, no record, no erase effect, no identity namespacing. This
// clears the IN-MEMORY snapshot only, which a reducer may do — §5.2 forbids it from writing NVS.
// When the persisted path arrives it must be bound to the printer identity or ignored at the
// boundary; that requirement is recorded in the Architecture Specification beside the §8.1 contract
// rather than anticipated in code here.
void forget_previous_printer(AppState& state) {
    state.printer_identity = PrinterIdentity{};
    state.printer = PrinterOperationalState{};
    state.temperatures = TemperatureState{};
    state.current_job = PrintJob{};
    state.last_completed_job = CompletedJobSnapshot{};
    state.alert = PrinterAlert{};

    state.last_printer_generation = 0;
    state.last_printer_sequence = 0;
    state.last_job_sequence = 0;

    // ⛔ EXPLICIT, BECAUSE THE DERIVATION CANNOT DO IT. subject_from_printer_state() returns
    // the PREVIOUS subject for PrinterState::Unknown — "the absence of news" must not wipe a
    // known state during a routine retry. A new printer identity is not absent news, it is a
    // first acquisition, which is exactly what UIAD 001 §6 reserves AcquiringState for.
    state.subject = SubjectState::AcquiringState;
}

void apply_configuration(AppState& state, const ConfigurationObservation& observation) {
    const bool identity_changed = printer_identity_changed(state.configuration, observation);

    state.configuration.status = observation.status;
    state.configuration.schema_version = observation.schema_version;
    state.configuration.credential_generation = observation.credential_generation;
    state.configuration.has_wifi_credentials = observation.has_wifi_credentials;
    state.configuration.has_printer_configuration = observation.has_printer_configuration;
    state.configuration.printer_configuration_pending = observation.printer_configuration_pending;
    state.configuration.printer_address = observation.printer_address;
    state.configuration.printer_alias = observation.printer_alias;
    state.configuration.auth_method = observation.auth_method;

    if (identity_changed) {
        // §16.2: an access condition is retried "only after configuration change".
        // This is that change, so the classification is discarded and acquisition
        // may begin again.
        state.access_condition = AccessCondition::None;

        // And everything recorded about the previous connection goes with it. The
        // phase in particular has to be cleared, not merely ignored: it is the input
        // the access condition is derived from, so leaving Unauthorized in place
        // would re-derive the classification that was just discarded. §14.3 also
        // re-resolves and re-probes after a configuration change, so no earlier
        // endpoint, latency or response code still describes anything real.
        state.printer_connection = PrinterConnectionState{};

        // ⛔ AND EVERYTHING OBSERVED FROM THE PREVIOUS PRINTER — audit item 4. Clearing the
        // CONNECTION while keeping the OBSERVATIONS is what made the panel assert a previous
        // printer's job, progress and temperatures against a newly configured one, at full
        // confidence, because `ever_observed` and `observed_at` still described the old target
        // and derive_layers() reads the clock rather than the identity. §11.5: "Stale printer
        // information must never be presented as current."
        //
        // The configuration itself — address, alias, auth method, generation, status — has
        // already been written above and is deliberately NOT touched here: the new
        // configuration is the one fact this event does establish.
        forget_previous_printer(state);
    }

    // §14.1's entry condition — "no valid configuration exists" — and its
    // converse. The intermediate statuses are deliberately left alone: §13.4
    // makes entering provisioning after a migration or storage failure a decision
    // with more inputs than this event carries, so it stays with the orchestrator.
    //
    // Note what this does NOT do: a connection failure never reaches here, so it
    // can never move the product into provisioning. UIAD 001 §5 requires exactly
    // that separation.
    if (observation.status == ConfigurationStatus::Missing) {
        state.provisioning = ProvisioningPhase::SetupRequired;
    } else if (observation.status == ConfigurationStatus::Valid) {
        state.provisioning = ProvisioningPhase::Inactive;
    }
}

// --- Network and connection lifecycle --------------------------------------

void apply_network(AppState& state, const NetworkObservation& observation, const Timestamp now) {
    state.network.phase = observation.phase;
    state.network.ip_address = observation.ip_address;
    state.network.signal_known = observation.signal_known;
    state.network.signal_strength_dbm = observation.signal_strength_dbm;
    state.network.last_error = observation.error;
    state.network.fault = observation.fault;

    // ⛔ A QUARANTINED WI-FI DRIVER IS A LOCAL DEVICE FAULT — the owner's DriverStalled ruling. It
    // reaches UIAD 001 §12.1's highest presentation priority through the SAME device-fault mechanism
    // every other local fault uses, rather than through a second parallel priority path, and it is
    // raised from a typed enumerator rather than from `last_error`'s words.
    //
    // ⛔ IT IS RAISED AND NEVER CLEARED HERE, WHICH IS THE RULING. Recovery requires an explicit
    // restart, so nothing the station reports afterwards may lower it — not a late completion, not a
    // successful association on some other identity. The only things that clear a device fault are
    // the paths that already do: a boot, and a factory reset.
    if (observation.fault == NetworkFault::DriverUnresponsive) {
        raise_device_fault(state, DeviceFaultReason::WifiUnavailable);
    }

    if (observation.phase == NetworkPhase::Connected) {
        state.network.last_connected_at = now;
        state.network.consecutive_failures = 0;
    } else if (observation.phase == NetworkPhase::Unavailable) {
        ++state.network.consecutive_failures;
    }
}

void apply_connection(AppState& state, const ConnectionObservation& observation,
                      const Timestamp now) {
    PrinterConnectionState& connection = state.printer_connection;

    connection.phase = observation.phase;
    connection.endpoint = observation.endpoint;
    connection.auth_method = observation.auth_method;
    connection.capability_tier = observation.capability_tier;
    connection.response_code_known = observation.response_code_known;
    connection.last_response_code = observation.response_code;
    connection.latency_known = observation.latency_known;
    connection.last_latency_ms = observation.latency_ms;
    connection.last_error = observation.error;
    connection.last_attempt_at = now;

    switch (observation.phase) {
    case ConnectionPhase::Connected:
        connection.last_success_at = now;
        connection.consecutive_failures = 0;
        break;

    // Transient transport failures, which §16.2 retries with backoff. The count
    // is what drives that backoff.
    case ConnectionPhase::Degraded:
    case ConnectionPhase::PrinterUnavailable:
    case ConnectionPhase::InvalidResponse:
        ++connection.consecutive_failures;
        break;

    // §16.2: not transient, and must not be retried aggressively. They do not
    // feed the backoff counter, because they are not failures backoff can fix.
    case ConnectionPhase::Unauthorized:
    case ConnectionPhase::UnsupportedApi:
    case ConnectionPhase::Unconfigured:
    case ConnectionPhase::WaitingForNetwork:
    case ConnectionPhase::Resolving:
    case ConnectionPhase::Probing:
    case ConnectionPhase::Authenticating:
        break;
    }
}

// --- Printer snapshot updates ----------------------------------------------

// §8.1: "persist completion snapshot when printing → finished". Called on the
// transition into a terminal state, while the job it describes is still current.
void persist_completion(AppState& state, const PrinterState terminal, const Timestamp now) {
    if (!state.current_job.present) {
        return;
    }

    CompletedJobSnapshot& completed = state.last_completed_job;

    completed.present = true;
    completed.id = state.current_job.id;
    completed.display_name = state.current_job.display_name;
    completed.terminal_state = terminal;
    completed.progress = state.current_job.progress;
    completed.layers = state.current_job.layers;
    completed.completed_at = now;
}

[[nodiscard]] bool is_terminal(const PrinterState state) {
    return state == PrinterState::Finished || state == PrinterState::Stopped;
}

// The printer half of a reading, written without any ordering decision. Returns the state that
// was current before it, which is what the lifecycle transition below is judged against.
//
// ⛔ SPLIT OUT FROM apply_printer_snapshot() 2026-09-03, AND THE SPLIT IS THE POINT. A status
// response carries a printer fact and a job fact, and the completion snapshot has to be taken
// after BOTH are in — see apply_status(). Two copies of these assignments is how the composite
// path and the single-event path would come to write different states from the same response.
PrinterState write_printer(AppState& state, const PrinterSnapshotObservation& observation,
                           const Timestamp now) {
    const PrinterState previous_state = state.printer.normalized;

    // §24.8: an unrecognised value is preserved rather than discarded. The raw
    // string is stored either way; the counter records that a mapping was missing.
    if (observation.normalized == PrinterState::Unknown && !observation.raw.empty()) {
        ++state.diagnostics.unknown_printer_states_seen;
    }

    state.printer.normalized = observation.normalized;
    state.printer.raw = observation.raw;
    state.printer.is_busy = observation.is_busy;
    state.printer.requires_attention = observation.requires_attention;
    state.printer.observed_at = now;
    state.printer.ever_observed = true;

    state.temperatures.nozzle_current = observation.nozzle_current;
    state.temperatures.nozzle_target = observation.nozzle_target;
    state.temperatures.bed_current = observation.bed_current;
    state.temperatures.bed_target = observation.bed_target;
    state.temperatures.chamber_current = observation.chamber_current;
    state.temperatures.chamber_target = observation.chamber_target;
    state.temperatures.observed_at = now;

    return previous_state;
}

// The lifecycle transition, taken from the reported state alone. §16.3 forbids the other route: a
// stale zero remaining time must never be turned into a completion, and nothing here reads a time
// to decide a state.
void settle_lifecycle(AppState& state, const PrinterState reported, const PrinterState previous,
                      const Timestamp now) {
    if (is_terminal(reported) && !is_terminal(previous)) {
        persist_completion(state, reported, now);
    }
}

bool apply_printer_snapshot(AppState& state, const PrinterSnapshotObservation& observation,
                            const Timestamp now) {
    if (!snapshot_is_current({observation.generation, observation.sequence},
                             {state.last_printer_generation, state.last_printer_sequence})) {
        ++state.diagnostics.events_rejected_stale;
        return false;
    }

    state.last_printer_generation = observation.generation;
    state.last_printer_sequence = observation.sequence;

    const PrinterState previous_state = write_printer(state, observation, now);
    settle_lifecycle(state, observation.normalized, previous_state, now);

    return true;
}

// --- Job lifecycle ---------------------------------------------------------

// The job half of a reading, written without any ordering decision — see write_printer() above.
void write_job(AppState& state, const JobObservation& observation, const Timestamp now) {
    PrintJob& job = state.current_job;

    // A different identity is a different job, so nothing carries over. Without
    // this, the previous job's elapsed time and layer count would appear against
    // the new one for as long as the printer had not reported them yet.
    const bool same_job = job.present && job.id == observation.id;
    if (!same_job) {
        job = PrintJob{};
        job.locally_inferred_started_at = now;
    }

    job.present = true;
    job.id = observation.id;
    job.display_name = observation.display_name;
    job.original_file_name = observation.original_file_name;
    job.source_path = observation.source_path;
    job.normalized_state = observation.normalized_state;
    job.raw_state = observation.raw_state;
    job.progress = observation.progress;
    job.elapsed = observation.elapsed;
    job.remaining = observation.remaining;
    job.layers = observation.layers;
    job.estimate_quality = observation.estimate_quality;
    job.last_updated_at = now;
    job.reported_missing = false;
    job.reported_missing_at = Timestamp::zero();

    // §7.8: the original out-of-range value is recorded, and the clamp happens
    // for display only.
    if (observation.progress.known && observation.progress.percent > 100) {
        ++state.diagnostics.out_of_range_progress_seen;
    }
}

bool apply_job(AppState& state, const JobObservation& observation, const Timestamp now) {
    // ⬅️ THE STANDALONE GENERAL JOB EVENT, which a fixture publishes on its own, with §8.1's
    // original bare-sequence rule. A job read from a STATUS RESPONSE arrives through apply_status()
    // instead and is ordered by that response's own identity.
    if (!sequence_is_current(observation.sequence, state.last_job_sequence)) {
        ++state.diagnostics.events_rejected_stale;
        return false;
    }

    state.last_job_sequence = observation.sequence;

    write_job(state, observation, now);
    return true;
}

// §7.8: "missing job data does not immediately erase a recently active job." The absence is
// recorded so expiry can act on it later; the job itself stays until it has been missing long
// enough to be meaningless.
void write_job_absence(AppState& state, const Timestamp now) {
    if (state.current_job.present && !state.current_job.reported_missing) {
        state.current_job.reported_missing = true;
        state.current_job.reported_missing_at = now;
    }
}

bool apply_job_unavailable(AppState& state, const JobUnavailableObservation& observation,
                           const Timestamp now) {
    // Ordered against the same watermark the updates are — see apply_job() above.
    if (!sequence_is_current(observation.sequence, state.last_job_sequence)) {
        ++state.diagnostics.events_rejected_stale;
        return false;
    }

    state.last_job_sequence = observation.sequence;

    write_job_absence(state, now);
    return true;
}

// The compact status job telemetry, EXPANDED into the domain's PrintJob.
//
// ⛔ THE ONLY PLACE THE TWO HALVES OF A READING ARE COMBINED. `raw_state` and `normalized_state`
// come from the printer half, because the reading has one state and StatusJobTelemetry
// deliberately does not carry a second copy of it.
//
// ⛔ AND IT WRITES ONLY WHAT THE ENDPOINT SUPPLIED. `display_name`, `original_file_name`,
// `source_path`, `layers` and `estimate_quality` are left exactly as they are: `/api/v1/status`
// states nothing about them, so it changes nothing about them, and blanking them would be a claim
// the printer did not make. On a job-identity change the whole record is reset first, so nothing
// carries over from a different print.
void write_status_job(AppState& state, const StatusObservation& observation, const Timestamp now) {
    PrintJob& job = state.current_job;

    // ⛔ AN ABSENT IDENTITY IS UNKNOWN, NOT EMPTY, AND NOT PROOF OF A CHANGE. §6.1 makes every
    // member of the `job` object optional, `id` included, so three cases have to be told apart and
    // only the middle one may discard anything:
    //
    //   known and equal      the same job. Update what was reported; keep everything else.
    //   known and different  a different job. Reset first, so no figure from the previous print
    //                        appears against this one.
    //   NOT KNOWN            no statement about identity was made. Keep the record — and keep its
    //                        id, so a terminal response with no id still completes the job the
    //                        device knows it was running.
    //
    // ⬅️ THE MIDDLE CASE USED TO SWALLOW THE THIRD, and it was a real defect: comparing against an
    // empty id made every ID-less poll look like a new job, resetting the locally inferred start,
    // the elapsed counter and any `/api/v1/job`-owned metadata on every cycle, and persisting a
    // completion with no identity.
    const bool changed_job =
        job.present && observation.job.id_known && job.id != observation.job.id;

    if (changed_job || !job.present) {
        job = PrintJob{};
        job.locally_inferred_started_at = now;
    }

    job.present = true;

    // ⛔ ONLY WHEN THE RESPONSE SAID SO. With no id, an existing record keeps the one it has, and a
    // record created here stays anonymous — which is truthful: this printer reported a job and did
    // not name it.
    if (observation.job.id_known) {
        job.id = observation.job.id;
    }

    job.normalized_state = observation.printer.normalized;
    job.raw_state = observation.printer.raw;
    job.progress = observation.job.progress;
    job.remaining = observation.job.remaining;
    job.elapsed = observation.job.elapsed;
    job.last_updated_at = now;
    job.reported_missing = false;
    job.reported_missing_at = Timestamp::zero();

    // §7.8: the original out-of-range value is recorded, and the clamp happens for display only.
    if (observation.job.progress.known && observation.job.progress.percent > 100) {
        ++state.diagnostics.out_of_range_progress_seen;
    }
}

// ⛔ ONE STATUS RESPONSE, ONE ORDERING DECISION, ONE MUTATION. This is the seam the whole product
// path goes through, and it exists because the split it replaces was incoherent in two ways that
// mattered.
//
// THE FIRST WAS OBSERVABLE. Dispatching a printer event and then a job event is two store
// mutations and therefore two projections, so a reviewer — and the panel — saw an intermediate
// composition that no response ever described: the new state word beside the previous response's
// figures, for one frame.
//
// ⛔ THE SECOND WAS A PRODUCT DEFECT, AND IT WAS REAL. persist_completion() reads
// `state.current_job`, so on a direct PRINTING 79% → FINISHED 100% response the completion
// snapshot was taken while the store still held 79 % — the same response's 100 % had not been
// applied yet. The completed job then recorded the progress of the reading BEFORE it. A timeline
// that happens to send PRINTING 100% before FINISHED hides this completely, which is why the
// regression test sends no such intervening response.
//
// So: one watermark, checked once; the job written first; the printer second; and the lifecycle
// settled last, when `current_job` is finally the job this response described.
bool apply_status(AppState& state, const StatusObservation& observation, const Timestamp now) {
    // ⬅️ THE READING'S OWN WATERMARK, ASKED ONCE. Both halves were read from one body and the
    // ordering identity lives on the printer half, so there is one question and half-acceptance is
    // not representable.
    if (!snapshot_is_current({observation.printer.generation, observation.printer.sequence},
                             {state.last_printer_generation, state.last_printer_sequence})) {
        ++state.diagnostics.events_rejected_stale;
        return false;
    }

    state.last_printer_generation = observation.printer.generation;
    state.last_printer_sequence = observation.printer.sequence;

    if (observation.job.reported) {
        write_status_job(state, observation, now);
    } else {
        write_job_absence(state, now);
    }

    const PrinterState previous_state = write_printer(state, observation.printer, now);

    // ⛔ LAST, AND THAT ORDER IS THE FIX. Anything that reads `current_job` to record a terminal
    // outcome must run after this response's job has been written.
    settle_lifecycle(state, observation.printer.normalized, previous_state, now);

    return true;
}

// --- Warnings and errors ---------------------------------------------------

void apply_alert(AppState& state, const AlertObservation& observation, const Timestamp now) {
    PrinterAlert& alert = state.alert;

    alert.active = true;
    alert.severity = observation.severity;
    alert.source = observation.source;
    alert.code = observation.code;
    alert.title = observation.title;
    alert.message = observation.message;
    alert.help_url = observation.help_url;
    alert.is_actionable = observation.is_actionable;
    alert.observed_at = now;
}

// --- Firmware update and reset lifecycle -----------------------------------

void apply_ota(AppState& state, const OtaObservation& observation) {
    const bool new_available = observation.phase == OtaPhase::Available &&
                               (state.ota.phase != OtaPhase::Available ||
                                state.ota.available_version != observation.available_version);

    state.ota.phase = observation.phase;
    state.ota.available_version = observation.available_version;
    state.ota.progress = observation.progress;
    state.ota.error = observation.error;
    state.ota.pending_validation = observation.pending_validation;

    if (new_available || observation.phase != OtaPhase::Available) {
        state.ota.notice_spent = false;
        state.ota.notice_presenting = false;
    }

    if (observation.phase == OtaPhase::Failed && observation.pending_validation) {
        raise_device_fault(state, DeviceFaultReason::OtaValidationFailure);
    }
}

// Whether an armed confirmation may still be completed at `now`.
//
// Unarmed counts as expired: a zero arming moment means nothing was ever armed, and
// `now - 0` would otherwise be a very large interval that happens to be > the window, which
// is the right answer for the wrong reason. Stated explicitly so it stays right if the
// comparison ever changes.
[[nodiscard]] bool confirmation_is_live(const AppState& state, const Timestamp now) {
    if (state.factory_reset_armed_at == Timestamp{}) {
        return false;
    }

    // Clamped at zero: an interval that ran backwards must not appear to be inside the window by
    // arithmetic that underflowed.
    //
    // ⬅️ NOT REACHABLE THROUGH reduce(), AND SAID SO RATHER THAN LEFT AS FALSE COMFORT. reduce()
    // already clamps every event's clock forward to the last one seen, so `now` can never predate
    // anything recorded in the state. This is defence for a future direct caller of the helper, and
    // it is one comparison; removing it would make the function's correctness depend on a property
    // of its caller.
    if (now < state.factory_reset_armed_at) {
        return false;
    }

    return (now - state.factory_reset_armed_at) <= kFactoryResetConfirmationWindow;
}

void arm_factory_reset_confirmation(AppState& state, const Timestamp now) {
    if (blocks_user_mutations(state.device_operation)) {
        return;
    }

    state.modal_surface = ModalSurface::FactoryResetConfirmation;
    state.factory_reset_armed_at = now;
}

void withdraw_factory_reset_confirmation(AppState& state) {
    if (state.modal_surface == ModalSurface::FactoryResetConfirmation) {
        state.modal_surface = ModalSurface::None;
    }

    state.factory_reset_armed_at = Timestamp{};
}

// ⛔ THE ONLY PATH TO A WIPE. Nothing else in this firmware sets factory_resetting, and
// app_core runs the erase only when it becomes true — so every precondition a reset has is
// the set of conditions below, in one function, checkable by a test.
//
// Owner ruling A of Milestone 3.10: reachable only while the confirmation surface is armed,
// and only within its bounded window. §5.6 requires factory reset to be "difficult to trigger
// accidentally"; a second deliberate very-long press inside 15 s is that requirement expressed
// as a state machine rather than as a convention in the UI.
void apply_factory_reset_confirmed(AppState& state, const Timestamp now) {
    if (state.modal_surface != ModalSurface::FactoryResetConfirmation) {
        return;
    }

    // ⬅️ A STALE CONFIRMATION IS NOT A CONFIRMATION. Without this, a modal left on screen
    // could be completed by a press arriving arbitrarily later — which is the accidental
    // trigger §5.6 names, merely slowed down.
    if (!confirmation_is_live(state, now)) {
        withdraw_factory_reset_confirmation(state);
        return;
    }

    if (blocks_user_mutations(state.device_operation)) {
        return;
    }

    state.factory_resetting = true;
    state.modal_surface = ModalSurface::None;
    state.factory_reset_armed_at = Timestamp{};
}

// --- Display power and input ----------------------------------------------

// Defined with the other lost-observation episode rules further down; declared here so the
// display-power and button paths can reach them without moving the whole block.
void retire_loss_acknowledgement(AppState& state);
void replay_loss_acknowledgement(AppState& state);

void apply_display_idle(AppState& state) {
    // UI Behaviour Decision 001 §3: the product "may eventually dim or enter
    // display sleep while continuing to monitor the machine". Never escalates
    // upward — an idle timer must not wake a sleeping panel.
    if (state.display.power == DisplayPower::On) {
        state.display.power = DisplayPower::Dimmed;

        // Production UI §9: the acknowledgement "retires at the ordinary On -> Dimmed
        // transition". Here, at that transition, and nowhere else.
        retire_loss_acknowledgement(state);
    }

    state.display.idle_mode = true;
}

void apply_display_sleep(AppState& state) {
    state.display.power = DisplayPower::Off;
    state.display.idle_mode = true;
}

void apply_button_gesture(AppState& state, const ButtonGestureObservation& observation,
                          const Timestamp now) {
    // ⬅️ READ BEFORE THE WAKE OVERWRITES IT. The manual toggle below needs to know whether the
    // panel the user pressed was lit or dark, and the wake three lines down destroys that fact.
    const DisplayPower power_before = state.display.power;

    // Every gesture is an interaction, whatever else it means: §7.11's
    // lastInteractionAt and §17.8's "wake conditions may include button press".
    // This happens even while an exclusive operation is running, because waking
    // the panel is not a mutation — the user pressed a button and deserves to see
    // what the device is doing.
    state.display.last_interaction_at = now;
    state.display.power = DisplayPower::On;
    state.display.idle_mode = false;

    if (blocks_user_mutations(state.device_operation)) {
        return;
    }

    switch (observation.gesture) {
    case ButtonGesture::ShortPress:
        // ⬅️ AND IT CANCELS AN ARMED CONFIRMATION — owner ruling A of Milestone 3.10. The
        // cheapest input withdraws the destructive one, which is the asymmetry §5.6 asks for:
        // confirming is deliberate and repeated, cancelling is a tap.
        if (state.modal_surface == ModalSurface::FactoryResetConfirmation) {
            withdraw_factory_reset_confirmation(state);
            break;
        }

        // ⬅️ THE MANUAL DISPLAY-POWER TOGGLE — owner ruling, 2026-08-30. Off → On is the wake the
        // lines above have already performed. On or Dimmed → Off is this, and it is what makes
        // the Product Brief's "manual wake and sleep using the physical button" real: until now
        // only the wake half existed.
        //
        // ⛔ IT IS THE SLOT §5.6 LEFT EMPTY, NOT A NEW GESTURE. "Cycle a limited secondary view
        // when already awake" was deliberately not implemented because UIAD 001 defines no
        // secondary view; a press on a lit panel therefore did nothing at all. Sleep is a
        // display-power act rather than a product state, so it adds no state to the architecture.
        //
        // ⛔ AND IT DEFERS TO EVERY OWNER OF THE PANEL. short_press_may_sleep() is the SAME
        // predicate the recession timer consults, so a condition a timer may not darken cannot be
        // darkened by a stray tap either: setup, recovery, a device fault, an armed confirmation,
        // an access condition, an attention or error, and a live job all keep the panel lit. The
        // confirmation case never reaches here — it is claimed by the branch above, which is why
        // cancelling a reset can never be mistaken for a sleep command.
        if (power_before != DisplayPower::Off && display_policy::short_press_may_sleep(state)) {
            state.display.power = DisplayPower::Off;
            state.display.idle_mode = true;
            retire_loss_acknowledgement(state);
            break;
        }

        // ⬅️ THE EXPLICIT REPLAY, AND ITS CAUSE IS THE PRESS. Production UI §9 and UIBD 001 §5.1:
        // "a short press from Off while still Unavailable presents the same acknowledgement".
        //
        // ⛔ GATED ON `power_before == Off`, NOT ON THE RESULTING POWER. The wake above has already
        // set power to On, so a test against the current value would be true for every press and
        // would replay the acknowledgement on a lit panel too. And no display-power event reaches
        // this function at all, which is what makes an unrelated activity wake unable to replay.
        if (power_before == DisplayPower::Off) {
            replay_loss_acknowledgement(state);
        }

        break;

    case ButtonGesture::LongPress:
        // ⬅️ DELIBERATE RECONFIGURATION ENTRY — owner ruling 1 of Milestone 3.10, settling
        // §5.6's "enter local setup/diagnostics flow" and R1's "later re-entry into
        // provisioning requires deliberate physical presence through the device button".
        //
        // ⛔ THE REDUCER RECORDS NO STATE FOR IT, AND THAT IS THE POINT. Re-entry starts an
        // access point and a privileged surface, which is driver work: §5.2 forbids this
        // function from performing side effects, so app_core observes the gesture on the
        // application task and acts. What the reducer does here is refuse to let the gesture
        // mean anything while a destructive confirmation is armed.
        //
        // UIAD 001 §9's Settings is NO LONGER REACHED FROM THE BUTTON (owner ruling 1). The
        // surface stays in the architecture; Production UI §9 specifies no screen for it, and
        // inventing one was never this checkpoint's work.
        break;

    case ButtonGesture::VeryLongPress:
        // §5.6: "request factory reset with an on-screen confirmation sequence" — and owner
        // ruling A makes the SEQUENCE two of these presses. The first arms; the second, inside
        // the window, confirms. Nothing brief can ever reach a wipe.
        if (state.modal_surface == ModalSurface::FactoryResetConfirmation) {
            apply_factory_reset_confirmed(state, now);
            break;
        }

        arm_factory_reset_confirmation(state, now);
        break;
    }
}

// --- The fact application dispatcher ---------------------------------------

// Returns false when the event was rejected rather than applied.
bool apply(AppState& state, const Event& event, const Timestamp now) {
    switch (event.type) {
    case EventType::None:
        return false;

    case EventType::BootPhaseObserved:
        apply_boot_phase(state, event.boot);
        return true;

    case EventType::HardwareProbed:
        apply_hardware_probe(state, event.hardware);
        return true;

    case EventType::StorageObserved:
        apply_storage(state, event.storage);
        return true;

    case EventType::DeviceFaultRaised:
        raise_device_fault(state, event.fault.reason);
        return true;

    case EventType::RecoveryModeEntered:
        state.recovery_mode = true;
        return true;

    case EventType::RecoveryModeExited:
        state.recovery_mode = false;
        return true;

    case EventType::ConfigurationObserved:
        apply_configuration(state, event.configuration);
        return true;

    case EventType::ProvisioningPhaseObserved:
        state.provisioning = event.provisioning.phase;
        return true;

    case EventType::NetworkObserved:
        apply_network(state, event.network, now);
        return true;

    case EventType::PrinterConnectionObserved:
        apply_connection(state, event.connection, now);
        return true;

    case EventType::PrinterIdentityObserved:
        state.printer_identity = event.identity.identity;
        return true;

    case EventType::PrinterSnapshotObserved:
        return apply_printer_snapshot(state, event.printer, now);

    case EventType::PrinterStatusObserved:
        return apply_status(state, event.status, now);

    case EventType::JobObserved:
        return apply_job(state, event.job, now);

    case EventType::JobUnavailable:
        return apply_job_unavailable(state, event.job_absence, now);

    case EventType::AlertRaised:
        apply_alert(state, event.alert, now);
        return true;

    case EventType::AlertCleared:
        state.alert = PrinterAlert{};
        return true;

    case EventType::ConfidenceAged:
        // Carries no payload. Its whole effect is the derivation pass that runs
        // after every event, using the timestamp it brought with it.
        return true;

    case EventType::OtaObserved:
        apply_ota(state, event.ota);
        return true;

    case EventType::FactoryResetConfirmed:
        apply_factory_reset_confirmed(state, now);
        return true;

    case EventType::FactoryResetCancelled:
        withdraw_factory_reset_confirmation(state);
        return true;

    case EventType::DisplayIdleTimerFired:
        apply_display_idle(state);
        return true;

    case EventType::DisplayWakeRequested:
        // ⛔ THE WAKE DOES NOT TOUCH lastInteractionAt. §7.11's field means a person touched the
        // device, and a printer coming back online did not. The recession's own activity moment
        // is app_core's, and it restarts on any meaningful change — this only lights the panel.
        state.display.power = DisplayPower::On;
        state.display.idle_mode = false;
        return true;

    case EventType::DisplaySleepTimerFired:
        apply_display_sleep(state);
        return true;

    case EventType::DisplayBrightnessObserved:
        state.display.brightness_percent = event.brightness.brightness_percent;
        return true;

    case EventType::ButtonGestureObserved:
        apply_button_gesture(state, event.button, now);
        return true;

    case EventType::ModalSurfaceDismissed:
        state.modal_surface = ModalSurface::None;
        return true;
    }

    return false;
}

// --- Derivations -----------------------------------------------------------

// §11.5: "expired: sensitive dynamic values such as remaining time are removed."
//
// Done in the state rather than only in the projection, so that "stale data is
// never presented as current" cannot be violated by a projection that forgot to
// check. What cannot survive is a value whose meaning is "as of now" — the two
// live counters.
//
// PROGRESS, LAYERS AND TEMPERATURES SURVIVE *HERE*, WHICH IS NOT THE SAME AS BEING
// PRESENTED. This function is about the STATE, and keeping them is what lets
// confidence recover without waiting for a poll (§11.5's aging: "recent enough to
// retain visible state"). What the panel shows at reduced confidence is the
// projection's decision, and since Milestone 2 Phase G.5 it is a tighter one: UI
// Design Decision 004 §10 makes confidence remove every value whose meaning is "as
// of now", temperatures and a live job's figures included. §7's example is what
// keeps a terminal job's count — "it is the count the machine reached".
//
// The comment this replaces read the two rules as one and described the §10
// position wrongly. Nothing about this function changed with it, and note it never
// runs for the operational family in any case: Expired maps to Unavailable, which
// the scene selector routes away from the operational scene entirely.
void expire_dynamic_values(AppState& state, const DataFreshness freshness) {
    if (freshness != DataFreshness::Expired) {
        return;
    }

    state.current_job.remaining = DurationValue{};
    state.current_job.elapsed = DurationValue{};
}

// §7.8's rule has a limit: a job missing for long enough is gone, not recent.
void expire_missing_job(AppState& state, const ReducerPolicy& policy, const Timestamp now) {
    const PrintJob& job = state.current_job;

    if (!job.present || !job.reported_missing) {
        return;
    }

    if (classify_freshness(now - job.reported_missing_at, policy.freshness) ==
        DataFreshness::Expired) {
        state.current_job = PrintJob{};
    }
}

// --- The lost-observation episode (Production UI §9, UIAD 001 §11.1, UIBD 001 §5.1) --------
//
// ⛔ ONE PLACE, AND IT IS THE ONLY PLACE. Every rule about who owns an acknowledgement lives here,
// so no other site can create, replay or reveal one. Nothing anywhere reads
// `confidence == Unavailable && display.power == On` and concludes an acknowledgement is owed.
//
// ⛔ AND THE PRECEDENCE TEST IS NOT WRITTEN HERE. subject_presentation_owns_panel() in
// presentation.cpp is the single authoritative seam, shared with the projection. A hand-written
// copy lived here briefly and had already drifted on BootPhase::FatalError.

// Which position on UIDD 004 §2.2's axis a subject state's form sits at.
//
// ⛔ ONE POSITION PER STATE, NOT TWO BUCKETS. Collapsing the axis to "resting" and "carrying" made
// four of the six states jump to an endpoint's shape at the instant of loss, which is exactly the
// discontinuity Candidate D's accepted contract forbids.
//
// TOTAL OVER SubjectState, so adding a seventh state fails to compile until it has been placed here
// — the same rule scene::energy_for() enforces on the axis itself.
[[nodiscard]] LossOriginForm origin_form_of(const SubjectState subject) {
    switch (subject) {
    case SubjectState::Ready:
        return LossOriginForm::Ready;
    case SubjectState::Stopped:
        return LossOriginForm::Stopped;
    case SubjectState::Preparing:
        return LossOriginForm::Preparing;
    case SubjectState::Completed:
        return LossOriginForm::Completed;
    case SubjectState::Paused:
        return LossOriginForm::Paused;
    case SubjectState::Printing:
        return LossOriginForm::Printing;

    // ⬅️ NO OPERATIONAL FORM WAS ON THE PANEL AT ALL. Production UI §9 routes AcquiringState to its
    // own Connecting screen and scene::energy_for() deliberately keeps it off the axis, so there is
    // no body to carry out of.
    //
    // ⛔ AND IT IS NOT `Ready`. It said `Ready` until 2026-09-02, which made the accepted
    // Ready-only B2 entry run for a loss that carried no form — a scope leak the owner never
    // ruled on. The two facts are now separate values, and this switch is TOTAL, so a seventh
    // subject state cannot arrive without a decision here.
    case SubjectState::AcquiringState:
        return LossOriginForm::NoOperationalForm;
    }

    return LossOriginForm::NoOperationalForm;
}

// Starts an acknowledgement presentation and stamps it with a fresh, never-reset generation.
void begin_loss_presentation(AppState& state) {
    state.loss_episode.presenting = true;

    // Unsigned wrap is defined and is the intended overflow behaviour; consumers compare for
    // inequality, never for order. The helper is what keeps the wrap off zero — see its contract.
    state.loss_presentation_generation =
        next_presentation_generation(state.loss_presentation_generation);
}

// Runs after every derivation, on the confidence the reducer has just settled.
void update_loss_episode(AppState& state) {
    if (state.confidence != Confidence::Unavailable || initial_acquisition_in_progress(state)) {
        // ⬅️ RECOVERY CLOSES THE EPISODE IMMEDIATELY, which is what makes the next genuine loss a
        // NEW episode with its own single acknowledgement. The generation counter is deliberately
        // NOT reset with it.
        state.loss_episode = LossEpisode{};
        return;
    }

    const bool eligible = subject_presentation_owns_panel(state);

    if (!state.loss_episode.active) {
        state.loss_episode.active = true;
        state.loss_episode.automatic_spent = false;
        state.loss_episode.presenting = false;

        // The abstract structural origin, captured once. Not a value, not cached subject data —
        // only which of the axis's six positions the form was at.
        state.loss_episode.origin = origin_form_of(state.subject);
    }

    // ⛔ PRIORITY ARRIVING *DURING* THE ACKNOWLEDGEMENT RETIRES AND FORFEITS IT, and merely hiding
    // it in the projection was a confirmed leak: the presentation stayed `presenting`, so when the
    // higher-priority surface cleared the old acknowledgement reappeared as though the loss had
    // just happened. Sampling eligibility only when the episode opened could not see this at all.
    if (state.loss_episode.presenting && !eligible) {
        state.loss_episode.presenting = false;
        state.loss_episode.automatic_spent = true;
        return;
    }

    if (state.loss_episode.automatic_spent) {
        return;
    }

    if (eligible) {
        state.loss_episode.automatic_spent = true;
        begin_loss_presentation(state);
        return;
    }

    // Lost while something of higher priority owned the panel: the automatic acknowledgement is
    // forfeited outright. It does NOT queue up for when that surface clears.
    state.loss_episode.automatic_spent = true;
}

// The ordinary On -> Dimmed transition retires the acknowledgement — Production UI §9.
void retire_loss_acknowledgement(AppState& state) {
    state.loss_episode.presenting = false;
}

// An explicit short press from true Off, while the same episode is still open.
//
// ⛔ THE CAUSE IS THE PRESS, AND IT IS NOT THE AUTOMATIC ENTRY. `automatic_spent` deliberately does
// not block this: the accepted contract gives the press its own permission for as long as the
// episode lives, including after the automatic entry was forfeited under priority. What DOES block
// it is priority *now* — checked live, through the same authoritative seam.
void replay_loss_acknowledgement(AppState& state) {
    if (!state.loss_episode.active || state.loss_episode.presenting) {
        return;
    }

    if (!subject_presentation_owns_panel(state)) {
        return;
    }

    begin_loss_presentation(state);
}

void derive_layers(AppState& state, const ReducerPolicy& policy, const Timestamp now) {
    const DataFreshness freshness =
        state.printer.ever_observed
            ? classify_freshness(now - state.printer.observed_at, policy.freshness)
            : DataFreshness::Unknown;

    state.printer_connection.freshness = freshness;
    state.confidence = confidence_from(freshness, state.printer.ever_observed);
    state.subject = subject_from_printer_state(state.subject, state.printer.normalized);
    state.subject_condition =
        condition_from(state.printer.normalized, state.printer.requires_attention, state.alert);
    state.access_condition = access_from(state.printer_connection.phase, state.access_condition);
    state.device_operation = resolve_device_operation(state);

    // Availability checking never takes the panel. Its one statement begins only when Ready owns
    // a lit panel, and the ordinary On -> Dimmed boundary (or any higher-priority state) retires it.
    if (state.ota.phase == OtaPhase::Available) {
        const bool eligible =
            state.subject == SubjectState::Ready && state.confidence != Confidence::Unavailable &&
            state.display.power == DisplayPower::On && subject_presentation_owns_panel(state);
        if (state.ota.notice_presenting && !eligible) {
            state.ota.notice_presenting = false;
        } else if (!state.ota.notice_spent && eligible) {
            state.ota.notice_spent = true;
            state.ota.notice_presenting = true;
        }
    }

    expire_dynamic_values(state, freshness);
    expire_missing_job(state, policy, now);

    // After the derivations, on the confidence they settled.
    update_loss_episode(state);
}

} // namespace

// --- The public derivations ------------------------------------------------

SubjectState subject_from_printer_state(const SubjectState previous, const PrinterState reported) {
    switch (reported) {
    case PrinterState::Idle:
    case PrinterState::Ready:
        return SubjectState::Ready;

    case PrinterState::Preparing:
        return SubjectState::Preparing;

    case PrinterState::Printing:
        return SubjectState::Printing;

    case PrinterState::Paused:
        return SubjectState::Paused;

    case PrinterState::Finished:
        return SubjectState::Completed;

    case PrinterState::Stopped:
        return SubjectState::Stopped;

    // The three that mean "keep what you had". Attention and Error are conditions
    // (UIAD 001 §7: "They never replace Ready, Preparing, Printing or Paused"),
    // Offline is confidence, and Unknown is the absence of news — §6 reserves
    // AcquiringState for the first acquisition, not for routine retries.
    case PrinterState::Attention:
    case PrinterState::Error:
    case PrinterState::Offline:
    case PrinterState::Unknown:
        break;
    }

    return previous;
}

SubjectCondition condition_from(const PrinterState reported, const bool requires_attention,
                                const PrinterAlert& alert) {
    // Error outranks Attention: §7.10's severity order, and a product that
    // reported a warning while an error was live would understate the problem.
    if (reported == PrinterState::Error) {
        return SubjectCondition::Error;
    }

    if (alert.active && alert.severity == AlertSeverity::Error) {
        return SubjectCondition::Error;
    }

    if (reported == PrinterState::Attention || requires_attention) {
        return SubjectCondition::Attention;
    }

    if (alert.active && alert.severity == AlertSeverity::Warning) {
        return SubjectCondition::Attention;
    }

    return SubjectCondition::None;
}

Confidence confidence_from(const DataFreshness freshness, const bool ever_observed) {
    // Nothing has been observed, so there is nothing to be confident about. This
    // is what stops a default-constructed state from presenting as a real,
    // current printer (§24.17).
    if (!ever_observed) {
        return Confidence::Unavailable;
    }

    // §11.5's four freshness levels map onto UIAD 001 §7's three confidence
    // values. Stale becomes Degraded rather than Unavailable because §11.5 keeps
    // stale values presentable behind an explicit indicator, which is exactly
    // §7's Degraded: "a recent known Subject State may still be represented with
    // reduced confidence".
    //
    // AGING IS CURRENT, AND THAT IS A BINDING REQUIREMENT RATHER THAN A PREFERENCE.
    // It mapped to Degraded until 2026-08-13, which made a routine missed poll
    // visible: at the 2 s active interval, aging begins one to two polls after the
    // last success, and from that moment the explicit indicator appeared and §10's
    // decay removed every telemetry value, the progress and the layer count. UI
    // Behaviour Decision 001 §4, in binding language: "Routine polling, retries and
    // small telemetry changes shall not become visible."
    //
    // §11.5's own definition of the level says the same thing from the other side —
    // aging is "missed updates but recent enough to retain visible state" — and
    // retaining a state that is still recent is not presenting stale information as
    // current (§7), because "stale" there names the level below, which still maps to
    // Degraded and still carries its indicator.
    //
    // NO ACCEPTED SOURCE PRESCRIBES THIS MAPPING. §11.5 is proposed, in a document
    // whose own status is "Proposed for review"; UIAD 001 §7 defines the three
    // confidence values and not what produces them. So this is the smallest change
    // that stops the violation, not a re-specification of the ladder. The thresholds
    // remain implementation baselines: 4 / 15 / 60 s appear in no accepted source.
    switch (freshness) {
    case DataFreshness::Fresh:
    case DataFreshness::Aging:
        return Confidence::Current;

    case DataFreshness::Stale:
        return Confidence::Degraded;

    case DataFreshness::Expired:
    case DataFreshness::Unknown:
        break;
    }

    return Confidence::Unavailable;
}

// --- The reducer -----------------------------------------------------------

AppState reduce(const AppState& previous, const Event& event, const ReducerPolicy& policy) {
    AppState next = previous;

    // Time only moves forward. An event whose timestamp predates the last one
    // seen is out of order — a producer with an unsynchronised clock, or a
    // reordered queue — and rewinding here would make an old observation look
    // fresh. The event is still applied; only its clock is clamped, and the fact
    // is counted rather than hidden.
    Timestamp now = event.observed_at;
    if (now < previous.last_event_at) {
        ++next.diagnostics.events_out_of_order;
        now = previous.last_event_at;
    }
    next.last_event_at = now;

    const bool applied = apply(next, event, now);

    if (applied) {
        ++next.diagnostics.events_applied;
    }

    // Runs whether or not the fact was applied: a rejected snapshot still leaves
    // time advanced, and confidence depends on time.
    derive_layers(next, policy, now);

    // §7.1: the revision "increases after every accepted state mutation" — so a
    // duplicate observation, or one that changes nothing a consumer can see,
    // publishes no new revision. That is what keeps UI Behaviour Decision 001 §4
    // ("Routine polling, retries and small telemetry changes shall not become
    // visible") true at the source rather than by filtering later.
    if (!content_equals(next, previous)) {
        ++next.revision;
        next.last_change_at = now;
    }

    return next;
}

AppState reduce(const AppState& previous, const Event& event) {
    static constexpr ReducerPolicy kDefaultPolicy{};
    return reduce(previous, event, kDefaultPolicy);
}

} // namespace prd::app_model
