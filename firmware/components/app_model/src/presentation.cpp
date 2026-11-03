// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include "app_model/presentation.hpp"

#include <algorithm>
#include <cstdint>

namespace prd::app_model {
namespace {

constexpr std::uint8_t kMaxProgressPercent = 100;

// UIAD 001 §4: Starting is "device startup before normal operation, provisioning
// or recovery has been resolved". FatalError is included because §15.2 sends a
// failed boot to "the relevant error/setup screen" rather than into normal
// operation — and a boot failure severe enough to matter also raises a device
// fault, which outranks this layer anyway.
[[nodiscard]] bool boot_is_unresolved(const BootPhase phase) {
    return phase != BootPhase::Ready;
}

} // namespace

// UIAD 001 §12, in order. The enumerator order in PresentationLayer is this
// function, which is why a reordering shows up as one diff rather than several.
PresentationLayer resolve_presentation_layer(const AppState& state) {
    // §12.1.
    if (state.device_operation == DeviceOperation::DeviceFault) {
        return PresentationLayer::DeviceFault;
    }

    // §12.2.
    if (state.device_operation == DeviceOperation::FirmwareUpdating ||
        state.device_operation == DeviceOperation::FactoryResetting) {
        return PresentationLayer::ExclusiveOperation;
    }

    if (boot_is_unresolved(state.boot.phase)) {
        return PresentationLayer::SystemLifecycle;
    }

    // §12.3.
    if (state.device_operation == DeviceOperation::RecoveryMode ||
        state.provisioning != ProvisioningPhase::Inactive) {
        return PresentationLayer::RecoveryOrProvisioning;
    }

    // §12.4.
    if (state.access_condition != AccessCondition::None) {
        return PresentationLayer::AccessCondition;
    }

    // §12.5.
    if (state.modal_surface != ModalSurface::None) {
        return PresentationLayer::ModalSurface;
    }

    // §12.6.
    return PresentationLayer::SubjectPresentation;
}

bool subject_presentation_owns_panel(const AppState& state) {
    // ⬅️ THE SUBJECT CONDITION IS PART OF THE TEST, THOUGH IT IS NOT A §12 LAYER. §7.10's Attention
    // and Error attach to the subject and stay until resolved, and holds_awake() already keeps the
    // panel lit for them. An acknowledgement drawn over an unresolved condition would replace
    // something a person still has to see.
    return resolve_presentation_layer(state) == PresentationLayer::SubjectPresentation &&
           state.subject_condition == SubjectCondition::None;
}

bool initial_acquisition_in_progress(const AppState& state) {
    if (!subject_presentation_owns_panel(state) || state.printer.ever_observed ||
        state.printer_connection.consecutive_failures != 0 ||
        state.subject != SubjectState::AcquiringState) {
        return false;
    }
    switch (state.printer_connection.phase) {
    case ConnectionPhase::WaitingForNetwork:
    case ConnectionPhase::Resolving:
    case ConnectionPhase::Probing:
    case ConnectionPhase::Authenticating:
    case ConnectionPhase::Connected:
        return true;
    case ConnectionPhase::Unconfigured:
    case ConnectionPhase::Degraded:
    case ConnectionPhase::Unauthorized:
    case ConnectionPhase::PrinterUnavailable:
    case ConnectionPhase::UnsupportedApi:
    case ConnectionPhase::InvalidResponse:
        return false;
    }
    return false;
}

namespace {

// Whether the subject may be presented at all.
//
// Two conditions, from different sources. UIAD 001 §9: a modal surface is drawn
// over the resolved background state, which "continues to exist" — so a modal does
// not hide the subject. UIAD 001 §7: Unavailable confidence means "no previous
// Subject State may be presented as current" — so nothing about the printer is
// shown, whatever the layer.
[[nodiscard]] bool subject_is_presentable(const AppState& state, const PresentationLayer layer) {
    const bool layer_allows =
        layer == PresentationLayer::SubjectPresentation || layer == PresentationLayer::ModalSurface;

    return layer_allows && state.confidence != Confidence::Unavailable;
}

// §9.4: "what fallback printer name to use". The user's own alias wins — they
// chose it — then what the printer calls itself, then the address they typed.
// When none is known the name is absent, and no placeholder is invented.
void resolve_printer_name(Presentation& view, const AppState& state) {
    // ⛔ NO IDENTITY WHILE OBSERVATION IS LOST — Production UI §9, accepted 2026-08-31: at Printer
    // Unavailable, "Printer identity" is Hidden **at all times**, and the acknowledgement carries
    // "Status message only".
    //
    // WHAT THIS REPLACES. This function used to run unconditionally, and the note at its call site
    // read: "The name survives loss of confidence: §9 Printer Unavailable shows 'Printer identity
    // (if known)' alongside the status message." That was true of the OLD §9 and is now false —
    // the revision removed identity from the Visible list precisely because identity is not
    // truthfully available here, and a name beside "Printer unavailable" invites the reading that
    // THAT printer is the one being described as current.
    //
    // Connecting permits known identity. Its explicit acquisition flag requires an in-progress
    // first attempt, not just "never observed": a failed attempt against an off printer must
    // still suppress identity, including while that attempt is retried.
    if (state.confidence == Confidence::Unavailable && !view.acquiring_connection) {
        return;
    }

    const auto assign_if_present = [&view](const std::string_view candidate) {
        if (view.printer_name_known || candidate.empty()) {
            return;
        }

        view.printer_name.assign(candidate);
        view.printer_name_known = true;
    };

    // ⛔ THE CONFIGURED ALIAS AND ADDRESS ARE GATED ON VALIDATION — owner ruling D-4,
    // 2026-08-16. Milestone 3.8 persists a printer the device has NEVER CONTACTED, as a
    // candidate (R4), and an unvalidated address shown as the printer's name would present
    // a guess as established identity: the panel would name a printer that may not exist.
    //
    // The three printer_identity fields need no gate. They exist only once the printer has
    // answered, which is Milestone 4's business — nothing can populate them before then.
    const bool printer_established = state.configuration.has_printer_configuration;

    assign_if_present(state.printer_identity.display_name.view());

    if (printer_established) {
        assign_if_present(state.configuration.printer_alias.view());
    }

    assign_if_present(state.printer_identity.configured_alias.view());
    assign_if_present(state.printer_identity.reported_name.view());

    if (printer_established) {
        assign_if_present(state.configuration.printer_address.view());
    }
}

void resolve_job_content(Presentation& view, const AppState& state, const bool telemetry_visible) {
    if (!telemetry_visible || !state.current_job.present) {
        return;
    }

    const PrintJob& job = state.current_job;

    // Recorded before §10's decay runs, which is the whole point of the field: it says what the
    // machine reported, not what is presentable now.
    view.job_record_supplied = job.progress.known;

    if (!job.display_name.empty()) {
        view.job_name.assign(job.display_name.view());
        view.job_name_known = true;
    } else if (!job.original_file_name.empty()) {
        view.job_name.assign(job.original_file_name.view());
        view.job_name_known = true;
    }

    // §7.8: clamped "only for display", which is here. The state keeps what the
    // printer actually said and the discrepancy is already counted.
    view.progress = job.progress;
    if (view.progress.known) {
        view.progress.percent = std::min(view.progress.percent, kMaxProgressPercent);
    }

    view.remaining = job.remaining;
    view.remaining_is_approximate = job.estimate_quality == EstimateQuality::Approximate;
    view.elapsed = job.elapsed;
    view.layers = job.layers;
}

void resolve_temperatures(Presentation& view, const AppState& state, const bool telemetry_visible) {
    if (!telemetry_visible) {
        return;
    }

    view.nozzle.current = state.temperatures.nozzle_current;
    view.nozzle.target = state.temperatures.nozzle_target;
    view.bed.current = state.temperatures.bed_current;
    view.bed.target = state.temperatures.bed_target;
    view.chamber.current = state.temperatures.chamber_current;
    view.chamber.target = state.temperatures.chamber_target;
}

// Carried whenever active, whatever layer won. An unresolved condition must not
// be swallowed by a modal surface drawn on top of it: UIAD 001 §11 forbids a
// transient presentation from blocking a persistent condition, and Production UI
// §9 keeps attention context visible "whenever still valid".
void resolve_alert(Presentation& view, const AppState& state) {
    if (!state.alert.active) {
        return;
    }

    view.alert.active = true;
    view.alert.severity = state.alert.severity;
    view.alert.title = state.alert.title;
    view.alert.message = state.alert.message;
    view.alert.is_actionable = state.alert.is_actionable;
}

// Production UI §8's component visibility rules.
//
// THE OPERATIONAL FORM IS PRESENT FOR EVERY MEMBER OF THE FAMILY. §6 makes the
// Ready Horizon "the terminal geometric transformation of the Progress Ring", so
// there is one component; UI Design Decision 004 §13.1 makes the inventory of
// primitives constant across the six operational states, and §3.1 makes the shape
// a function of energy rather than of anything decided here.
//
// So this no longer chooses between two shapes, and no longer consults the job's
// record. The earlier version set the ring's presence from `progress.known`,
// which meant a Printing state the printer had not yet reported progress for drew
// no form at all — the record deciding whether the form existed, which §3.2
// prohibits in its stronger form ("the record shall never determine the form's
// shape"). What the record still decides is whether a progress value is shown,
// and that is resolved separately.
//
// §9 Print Stopped is satisfied without the divergence this function used to
// carry: UIDD 004 §12 reads "progress remains visible" as the value rather than
// the arc, so Stopped keeps its record while its form recedes with its energy.
// The component inventory is unchanged, which is what §9 asks for.
// UI Behaviour Decision 001 §8's Level 0, as a predicate over what the model already carries.
//
// Ready is the only subject state that can be at rest: §8 defines the levels over "every
// non-printing condition", and the other five either are an operational activity or are §8's own
// Level 2 residual context. Confidence must be Current, because Degraded is Level 3 and owes the
// user an explicit statement of uncertainty. Both condition slots must be clear, because either
// one is Level 4 and remains visible until resolved.
[[nodiscard]] bool subject_is_at_visual_rest(const AppState& state, const PresentationLayer layer) {
    return layer == PresentationLayer::SubjectPresentation &&
           state.subject == SubjectState::Ready && state.confidence == Confidence::Current &&
           state.subject_condition == SubjectCondition::None &&
           state.access_condition == AccessCondition::None;
}

void resolve_components(Presentation& view, const AppState& state, const bool presentable) {
    if (!presentable) {
        return;
    }

    // ⛔ REST SHOWS THE FORM AND NOTHING ELSE. The values stay resolved on the presentation —
    // a temperature the printer reported is still true of the machine, so UIDD 004 §7's
    // presence test is not what excludes it — and this is the information policy of UI Behaviour
    // Decision 001 §2 deciding not to display a true thing. The distinction matters: the moment
    // confidence falls or a condition appears, the same values become visible again with no
    // re-resolution.
    view.show_telemetry = !view.at_visual_rest;

    switch (state.subject) {
    case SubjectState::Ready:
    case SubjectState::Preparing:
    case SubjectState::Printing:
    case SubjectState::Paused:
    case SubjectState::Completed:
    case SubjectState::Stopped:
        view.show_operational_form = true;
        break;

    case SubjectState::AcquiringState:
        // Not a member of the family. §9 Connecting is its own screen — connection
        // status and printer identity, with the form and the telemetry hidden —
        // and there is no resting composition to show before a first state has
        // ever been acquired. Listed rather than defaulted so that adding a
        // subject state fails to compile until it has been considered.
        break;
    }
}

// UI Design Decision 004 §7's worked example, as a predicate: "A terminal job's layer count is
// still true — it is the count the machine reached — and is therefore present."
//
// Paused is deliberately not terminal. Its job is suspended rather than finished, so its figures
// are still statements about a machine that may have moved on since it was last observed.
[[nodiscard]] bool subject_is_terminal(const SubjectState subject) {
    return subject == SubjectState::Completed || subject == SubjectState::Stopped;
}

// §10: "Confidence… states itself explicitly, and it tightens Section 7: values that decay cease
// to be true and become absent, while durable values remain."
//
// WHICH VALUES DECAY IS A DERIVATION, NOT A CHOICE. §10 gives the criterion, §7 gives the worked
// example, and Architecture Spec §11.5 names the archetype — "expired: sensitive dynamic values
// such as remaining time are removed". A value decays when its meaning is "as of now", because
// that is exactly the claim a machine that cannot be observed no longer supports:
//
//   temperatures  a reading is a measurement at an instant, and this instant has none;
//   remaining     §11.5's own example, and an estimate against a print that may have stopped;
//   elapsed       the same counter measured from the other end — paired with remaining here
//                 because the reducer pairs them for the same reason at expiry;
//   layers        a live job's layer count decays; a terminal job's count remains.
// Owner ruling 2026-09-08: progress remains the LAST OBSERVED percentage while Degraded,
// explicitly qualified by the existing age indicator. It does not advance or imply live data.
// Unavailable still withdraws the operational presentation at the existing expiry boundary.
//
// AND WHAT DOES NOT DECAY IS AS LOAD-BEARING AS WHAT DOES. The printer's name is identity rather
// than telemetry and survives even Unavailable (§9 Printer Unavailable keeps "Printer identity
// (if known)"). The job's name is the identity of a job that was and remains that job. The
// subject state, the energy it implies, and every component's presence-as-membership are
// untouched: §10 opens by making condition and confidence orthogonal to energy, and closes by
// insisting a degraded Printing "remains Printing at maximum energy… The machine has not slowed;
// it has become unobservable."
//
// NOT THE REDUCER'S JOB. Architecture Spec §9.4 gives the projection "whether cached job data is
// still safe to display" and "when stale data should disappear". Keeping the values in the state
// is what lets confidence recover without waiting for a poll, which §11.5's aging — "recent
// enough to retain visible state" — requires. The reducer's own expiry at Expired is a different
// rule at a different threshold and is untouched; note it never reaches the operational family
// anyway, because Expired maps to Unavailable and the scene selector routes that elsewhere.
void decay_under_reduced_confidence(Presentation& view, const AppState& state) {
    if (state.confidence != Confidence::Degraded) {
        return;
    }

    view.nozzle = TemperaturePair{};
    view.bed = TemperaturePair{};
    view.chamber = TemperaturePair{};

    view.remaining = DurationValue{};
    view.remaining_is_approximate = false;
    view.elapsed = DurationValue{};

    if (subject_is_terminal(view.subject)) {
        return;
    }

    view.layers = LayerProgress{};
}

} // namespace

std::string_view log_label(const PresentationLayer layer) {
    switch (layer) {
    case PresentationLayer::DeviceFault:
        return "device-fault";

    case PresentationLayer::ExclusiveOperation:
        return "exclusive-operation";

    case PresentationLayer::SystemLifecycle:
        return "system-lifecycle";

    case PresentationLayer::RecoveryOrProvisioning:
        return "recovery-or-provisioning";

    case PresentationLayer::AccessCondition:
        return "access-condition";

    case PresentationLayer::ModalSurface:
        return "modal-surface";

    case PresentationLayer::SubjectPresentation:
        return "subject-presentation";
    }

    return "unknown";
}

Presentation project(const AppState& state) {
    Presentation view;

    view.layer = resolve_presentation_layer(state);

    // UIAD 001 §9: the subject, its condition and its confidence continue to
    // exist whatever is drawn over them, so they are always reported.
    view.subject = state.subject;
    view.subject_condition = state.subject_condition;
    view.confidence = state.confidence;

    view.device_fault = state.device_fault;
    view.device_operation = state.device_operation;
    view.boot_phase = state.boot.phase;
    view.provisioning = state.provisioning;
    view.access_condition = state.access_condition;
    view.acquiring_connection = initial_acquisition_in_progress(state);
    view.modal_surface = state.modal_surface;

    view.ota_phase = state.ota.phase;
    view.update_version = state.ota.available_version;
    view.update_progress = state.ota.progress;
    view.update_notice = state.ota.notice_presenting;
    if (view.update_progress.known) {
        view.update_progress.percent = std::min(view.update_progress.percent, kMaxProgressPercent);
    }

    const bool presentable = subject_is_presentable(state, view.layer);

    // Before resolve_components(), which reads it, and before the content seam that reads it to
    // decide whether Ready carries a title at all.
    view.at_visual_rest = presentable && subject_is_at_visual_rest(state, view.layer);

    // §11.5: stale values are presentable only behind an explicit indicator, and
    // only while there is something to mark.
    view.stale_indicator = presentable && state.confidence == Confidence::Degraded;
    if (view.stale_indicator && state.printer.ever_observed) {
        view.stale_observed_at = state.printer.observed_at;
    }

    // Production UI §9 and UIAD 001 §11.1. The condition holds whenever the printer cannot be
    // observed; the acknowledgement is drawn only while the panel is On, which is how it inherits
    // the universal 20 s window and retires at the ordinary On → Dimmed transition instead of
    // owning a schedule of its own.
    //
    // ⛔ NOT GATED ON `presentable`. That gate answers whether a SUBJECT may be shown, and at
    // Unavailable the answer is no by construction — which is exactly when this must be true.
    view.observation_lost =
        state.confidence == Confidence::Unavailable && !view.acquiring_connection;

    // ⛔ COPIED FROM THE EPISODE, AND GATED ON THE LAYER. The reducer owns whether an
    // acknowledgement is presenting; §12's priority order owns whether this layer may draw at all.
    // Both must hold: a device fault, an exclusive operation, provisioning, an access condition or
    // a modal surface resolves above the subject presentation and the acknowledgement must never
    // overlay, delay or leak through it.
    // ⬅️ THE SAME AUTHORITATIVE SEAM THE REDUCER USES. The reducer already retires the
    // acknowledgement the moment priority takes ownership, so this is a belt-and-braces gate rather
    // than the mechanism: hiding alone was the leak.
    view.observation_lost_acknowledgement = state.loss_episode.presenting &&
                                            subject_presentation_owns_panel(state) &&
                                            !view.acquiring_connection;
    view.observation_lost_generation =
        view.observation_lost_acknowledgement ? state.loss_presentation_generation : 0U;

    // The same seam, published rather than re-derived downstream — see the field's contract.
    view.observation_lost_eligible = subject_presentation_owns_panel(state);
    view.observation_lost_origin = state.loss_episode.origin;

    // Identity is configuration rather than telemetry, so it survives DEGRADED confidence — but
    // not the loss of observation. §9, as revised 2026-08-31, hides it at Printer Unavailable at
    // all times; resolve_printer_name() enforces that and explains why.
    view.printer_configuration_pending = state.configuration.printer_configuration_pending;

    resolve_printer_name(view, state);

    resolve_job_content(view, state, presentable);
    resolve_temperatures(view, state, presentable);
    resolve_alert(view, state);
    resolve_components(view, state, presentable);

    // Last, and after every value is resolved, because §10 TIGHTENS §7 rather than replacing it:
    // what confidence removes is a subset of what the machine supplied. Running it here means
    // there is one place where a value can become absent for confidence, and it can only ever
    // take away — it cannot make something present that §7 said was not.
    decay_under_reduced_confidence(view, state);

    return view;
}

bool operator==(const Presentation& lhs, const Presentation& rhs) {
    if (lhs.acquiring_connection != rhs.acquiring_connection || lhs.layer != rhs.layer ||
        lhs.device_fault != rhs.device_fault || lhs.device_operation != rhs.device_operation ||
        lhs.boot_phase != rhs.boot_phase || lhs.provisioning != rhs.provisioning ||
        lhs.access_condition != rhs.access_condition || lhs.modal_surface != rhs.modal_surface) {
        return false;
    }

    if (lhs.subject != rhs.subject || lhs.subject_condition != rhs.subject_condition ||
        lhs.confidence != rhs.confidence) {
        return false;
    }

    if (lhs.printer_name_known != rhs.printer_name_known || lhs.printer_name != rhs.printer_name ||
        lhs.job_name_known != rhs.job_name_known || lhs.job_name != rhs.job_name ||
        lhs.job_record_supplied != rhs.job_record_supplied) {
        return false;
    }

    if (lhs.progress != rhs.progress || lhs.remaining != rhs.remaining ||
        lhs.remaining_is_approximate != rhs.remaining_is_approximate ||
        lhs.elapsed != rhs.elapsed || lhs.layers != rhs.layers) {
        return false;
    }

    if (lhs.nozzle.current != rhs.nozzle.current || lhs.nozzle.target != rhs.nozzle.target ||
        lhs.bed.current != rhs.bed.current || lhs.bed.target != rhs.bed.target ||
        lhs.chamber.current != rhs.chamber.current || lhs.chamber.target != rhs.chamber.target) {
        return false;
    }

    if (lhs.alert.active != rhs.alert.active || lhs.alert.severity != rhs.alert.severity ||
        lhs.alert.title != rhs.alert.title || lhs.alert.message != rhs.alert.message ||
        lhs.alert.is_actionable != rhs.alert.is_actionable) {
        return false;
    }

    // ⛔ EVERY LOST-OBSERVATION FIELD BELONGS HERE, AND NONE OF THEM IS DECORATIVE. controller.cpp
    // skips an identical presentation without drawing — §11's redraw rule — so a field that moved
    // while equality ignored it is a change the renderer never sees.
    //
    //   observation_lost_acknowledgement   a retirement that moved only this would be swallowed and
    //                                      the message would stay on a dimmed panel. Retiring the
    //                                      acknowledgement IS a change in what is visible.
    //
    //   observation_lost_eligible          the renderer reads it WITH `observation_lost` to decide
    //                                      whether a retired acknowledgement is owed the outgoing
    //                                      fade or must clear at once.
    //
    //                                      ⬅️ DEFENSIVE TODAY, AND DELIBERATELY SO. It is a pure
    //                                      function of `layer` and `subject_condition`, and both of
    //                                      those are compared above — so no presentation the reducer
    //                                      can produce differs only in this field, and leaving it out
    //                                      coalesced nothing away. The day the eligibility rule
    //                                      consults a third fact, that stops being true and this
    //                                      line is what keeps the renderer informed;
    //                                      EligibilityIsDerivableFromTwoAlreadyComparedFields fails
    //                                      on that day rather than letting it pass unnoticed.
    return lhs.stale_indicator == rhs.stale_indicator &&
           lhs.stale_observed_at == rhs.stale_observed_at &&
           lhs.observation_lost == rhs.observation_lost &&
           lhs.observation_lost_acknowledgement == rhs.observation_lost_acknowledgement &&
           lhs.observation_lost_generation == rhs.observation_lost_generation &&
           lhs.observation_lost_eligible == rhs.observation_lost_eligible &&
           lhs.observation_lost_origin == rhs.observation_lost_origin &&
           lhs.show_operational_form == rhs.show_operational_form &&
           lhs.show_telemetry == rhs.show_telemetry && lhs.at_visual_rest == rhs.at_visual_rest &&
           lhs.ota_phase == rhs.ota_phase && lhs.update_version == rhs.update_version &&
           lhs.update_progress == rhs.update_progress && lhs.update_notice == rhs.update_notice;
}

bool operator!=(const Presentation& lhs, const Presentation& rhs) {
    return !(lhs == rhs);
}

} // namespace prd::app_model
