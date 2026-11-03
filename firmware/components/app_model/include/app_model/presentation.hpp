// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <optional>
#include <string_view>

#include "app_model/app_state.hpp"
#include "app_model/bounded_string.hpp"
#include "app_model/device_data.hpp"
#include "app_model/printer_data.hpp"
#include "app_model/state_layers.hpp"
#include "app_model/values.hpp"

// The UI projection — Architecture Spec §9.4, and UIAD 001 §12's deterministic
// presentation priority.
//
//     AppState → Presentation
//
// SEMANTIC, NOT VISUAL. No LVGL type, no pixel, no coordinate, no colour, no font,
// no formatted string. Deciding *which* information is presentable is projection;
// deciding where it sits and what it looks like is the scene layer, governed by
// the Production UI Specification. The line is drawn at the token: this file may
// say "the progress ring is present", never "the progress ring is 96 px".
//
// FORMATTING IS ABSENT ON PURPOSE. §17.7's formatters are deterministic and
// host-testable and will live alongside the scenes, not here. A projection that
// emitted "1h 18m remaining" would already have chosen between the two remaining-
// time treatments the accepted artwork shows, which is one of the carried scene
// decisions. Values go out; wording comes later.
//
// WHERE THIS LIVES. §5.12 lists ui_projection inside the ui component. It is
// implemented in app_model instead, for two reasons that outrank the suggested
// file layout: the host test build cannot compile ui, which includes LVGL, and
// §9.4 requires this logic to be "host-testable and independent of LVGL". ui
// consumes Presentation and owns everything downstream of it.

namespace prd::app_model {

// Which architectural layer won UIAD 001 §12's priority resolution.
//
// The order of the enumerators IS the priority order, highest first, so the
// resolution reads as a sequence of tests and a reordering shows up as a diff
// here rather than as scattered condition changes.
enum class PresentationLayer {
    // §12.1. Highest, always.
    DeviceFault,

    // §12.2 exclusive system operation: firmware updating, factory resetting.
    ExclusiveOperation,

    // UIAD 001 §4. Not one of §12's seven tiers, because during startup none of
    // the lower tiers has been resolved yet — but Production UI §9 specifies a
    // Boot screen, so the layer has to be presentable. Placed below exclusive
    // operations because those are exclusive by definition, and above everything
    // else because §4 defines Starting as "startup before normal operation,
    // provisioning or recovery has been resolved". Implementation placement, not
    // a new product state.
    SystemLifecycle,

    // §12.3 recovery or required provisioning.
    RecoveryOrProvisioning,

    // §12.4 access or compatibility condition.
    AccessCondition,

    // §12.5 user-invoked modal surface.
    ModalSurface,

    // §12.6 resolved subject presentation: subject state, condition, confidence.
    SubjectPresentation,
};

[[nodiscard]] std::string_view log_label(PresentationLayer layer);

// UIAD 001 §12's priority resolution — THE authoritative one.
//
// ⛔ PUBLIC BECAUSE THERE MUST BE EXACTLY ONE. The reducer needs the same answer to decide who owns
// a lost-observation acknowledgement, and it briefly kept its own hand-written list. The two
// disagreed on BootPhase::FatalError — the resolver treats every non-Ready phase as an unresolved
// boot, the copy did not — which is precisely the drift a second list guarantees. There is now one
// function and both callers use it.
[[nodiscard]] PresentationLayer resolve_presentation_layer(const AppState& state);

// Whether the resolved subject presentation owns the panel, i.e. nothing of higher priority does.
//
// This is the eligibility test for the lost-observation acknowledgement: §12.1 to §12.5 and §4's
// unresolved boot all outrank it, and an unresolved subject condition is part of the subject
// presentation that the acknowledgement would otherwise replace.
[[nodiscard]] bool subject_presentation_owns_panel(const AppState& state);

// First acquisition in progress, not a failed attempt or a retry after lost observation.
[[nodiscard]] bool initial_acquisition_in_progress(const AppState& state);

// A temperature pair as the Temperature Row consumes it (Production UI §8).
struct TemperaturePair {
    TemperatureValue current;
    TemperatureValue target;
};

struct AlertSummary {
    bool active{false};
    AlertSeverity severity{AlertSeverity::Info};
    BoundedString<48> title;
    BoundedString<96> message;
    bool is_actionable{false};
};

// The resolved presentation.
//
// Every field is a value or a semantic flag. Absent data is absent — there is no
// placeholder text anywhere in this structure, which is what UI Design Decision
// 003's composition rule requires ("Information that does not yet exist shall
// remain absent rather than represented by placeholders") and what Production UI
// §9 requires twice more. A scene cannot render "---" from this without inventing
// it.
struct Presentation {
    PresentationLayer layer{PresentationLayer::SystemLifecycle};

    // --- Layer-specific resolution ---
    //
    // Each is meaningful when `layer` selects it, and carries its inert value
    // otherwise. They are not hidden behind the layer tag because UIAD 001 §9 is
    // explicit that lower layers keep existing while a higher one is presented.

    DeviceFaultReason device_fault{DeviceFaultReason::None};
    DeviceOperation device_operation{DeviceOperation::None};
    BootPhase boot_phase{BootPhase::Starting};
    ProvisioningPhase provisioning{ProvisioningPhase::Inactive};

    // R8's candidate printer configuration, projected — owner ruling D-4, 2026-08-16.
    //
    // WHY IT IS HERE AT ALL. Milestone 3.8 is the first checkpoint that can persist a
    // printer the device has never contacted, and the recorded M3.8 prerequisite required
    // this field before it could: without it, "no printer configured" and "configured but
    // unproven" are indistinguishable to anything reading a Presentation.
    //
    // ⛔ IT DOES NOT MAKE AN UNVALIDATED PRINTER LOOK ESTABLISHED. resolve_printer_name()
    // gates the configured alias and address on has_printer_configuration — validated —
    // so a pending record contributes no printer name. This flag says only that one is
    // waiting, which the provisioning layer may show and the Operational Family does not.
    bool printer_configuration_pending{false};
    AccessCondition access_condition{AccessCondition::None};
    bool acquiring_connection{false};
    ModalSurface modal_surface{ModalSurface::None};

    // --- Always carried (UIAD 001 §9) --------------------------------------
    //
    // "The underlying Subject State, conditions and confidence continue to exist
    // while [modal surfaces] are presented." So these are reported whatever won:
    // a modal draws over a live Printing composition, not over a blank screen.

    SubjectState subject{SubjectState::AcquiringState};
    SubjectCondition subject_condition{SubjectCondition::None};
    Confidence confidence{Confidence::Unavailable};

    // --- Semantic content --------------------------------------------------

    // Resolved by the fallback rules in §9.4 ("what fallback printer name to
    // use"): configured alias, else printer-reported name, else configured
    // address. Absent when none of the three is known.
    bool printer_name_known{false};
    BoundedString<32> printer_name;

    bool job_name_known{false};
    BoundedString<64> job_name;

    // Whether the machine has supplied a record for the current job at all.
    //
    // SEPARATE FROM `progress.known`, AND THE TWO DIVERGE FOR EXACTLY ONE REASON. UI Design
    // Decision 004 §10 gives reduced confidence the power to make a supplied record's values
    // absent while the record itself goes on existing. "The machine never reported a record"
    // and "the record is not presentable right now" are different facts about the machine, and
    // §10 forbids the composition changing between them: "Confidence shall change no geometry,
    // luminance, typography, layout or energy."
    //
    // A composition that read `progress.known` alone would therefore rearrange itself when
    // confidence fell — a state word moving as the machine became unobservable, which asserts
    // something about the machine that is not true. This field is what the arrangement is
    // decided from; `progress` remains what is drawn.
    //
    // Confidence-independent by construction: it is set from the state's own job, before §10's
    // decay is applied to the values.
    bool job_record_supplied{false};

    // Clamped to 0...100 here, and only here: §7.8 says progress is "clamped to
    // 0...100 only for display", so the state keeps what the printer said.
    ProgressValue progress;

    DurationValue remaining;

    // §17.7's "approximate marker for inaccurate remaining time" — whether the
    // value is an estimate, not how the marker is drawn.
    bool remaining_is_approximate{false};

    DurationValue elapsed;
    LayerProgress layers;

    TemperaturePair nozzle;
    TemperaturePair bed;
    TemperaturePair chamber;

    AlertSummary alert;

    // §11.5: stale values may be presented only behind an explicit indicator.
    // True exactly when confidence is Degraded and something is still shown.
    bool stale_indicator{false};
    // Last successful status observation, only while reduced confidence is presentable.
    // Same monotonic-since-boot clock as the renderer; no wall clock or polling inference.
    std::optional<Timestamp> stale_observed_at{};

    // --- Lost observation (Production UI §9, UIAD 001 §11.1) ----------------
    //
    // ⛔ THE CONDITION, WHICH OUTLIVES ITS PRESENTATION. True while confidence is
    // Unavailable, whatever the panel is doing. UIAD 001 §11.1: "Retirement of
    // the acknowledgement ends the presentation, not the condition."
    bool observation_lost{false};

    // ⛔ WHETHER THE TRANSIENT ACKNOWLEDGEMENT IS ON THE PANEL RIGHT NOW — and it is the MODEL's
    // answer, copied, not a conclusion drawn here.
    //
    // ⬅️ WHAT THIS REPLACES, AND WHY. It used to read
    //
    //     observation_lost && state.display.power == DisplayPower::On
    //
    // which was a confirmed defect: an unrelated activity wake replayed the acknowledgement, and a
    // higher-priority surface clearing later revealed it as though the loss had just happened.
    // `Unavailable && On` is not an episode. AppState::loss_episode is, and the reducer decides it
    // once at the transition — see update_loss_episode() there.
    //
    // ⛔ NOTHING DOWNSTREAM MAY RE-DERIVE THIS. The UI renders the phase; it must not infer
    // ownership from a timer, from the current brightness or from whichever scene is visible.
    bool observation_lost_acknowledgement{false};

    // Increments each time a presentation starts, so the render task knows to restart its
    // two-second motion. Monotonic across episodes and rebase-invariant: no absolute time enters
    // the projection, and no episode reset can republish a generation the renderer already saw.
    std::uint32_t observation_lost_generation{0};

    // Whether the subject presentation would own the panel for an acknowledgement RIGHT NOW — the
    // very predicate `observation_lost_acknowledgement` above is gated on.
    //
    // ⛔ PUBLISHED SO THAT NO RENDERER RE-DERIVES §12's PRECEDENCE. A render task has to tell WHY an
    // acknowledgement stopped: retired by the ordinary display recession while still eligible, which
    // owes the last frame the outgoing fade; or displaced by recovery or by something of higher
    // priority, which owes it nothing and must clear at once. Both arrive as
    // `observation_lost_acknowledgement == false`, and the difference is this field AND
    // `observation_lost` above — never a second copy of the precedence list.
    //
    // ⬅️ IT IS NOT A SECOND ACKNOWLEDGEMENT, AND CARRIES NO PERMISSION. It says only who owns the
    // panel. Whether an acknowledgement exists remains AppState::loss_episode's answer alone.
    bool observation_lost_eligible{false};

    // The abstract structural origin the motion begins from. Carries no value of any kind — see
    // LossOriginForm.
    LossOriginForm observation_lost_origin{LossOriginForm::Ready};

    // --- Shared component presence (Production UI §8) ----------------------
    //
    // Which canonical components exist in this presentation. Semantic, not
    // geometric: this may say "the operational form is present", never "the
    // operational form is 96 px".

    // Production UI §8's Progress Ring, of which §6 makes the Ready Horizon "the
    // terminal geometric transformation" rather than an independent primitive.
    //
    // ONE FLAG, BECAUSE THERE IS ONE COMPONENT. Two booleans described two
    // alternative shapes, which made presence and shape the same question and
    // needed a test to pin that they never disagreed. UI Design Decision 004 §3.1
    // settles it — one form whose shape is energy — so the question left here is
    // only whether the composition draws it.
    //
    // PRESENCE IS MEMBERSHIP OF THE FAMILY, NOT AVAILABILITY OF THE JOB'S RECORD.
    // §13.1 makes the inventory of primitives constant across the six operational
    // states: "no state introduces an element another state lacks". §3.2 is
    // blunter — "the record shall never determine the form's shape" — and a record
    // that could delete the form entirely is a stronger version of exactly that.
    // So a Printing state whose progress the printer has not reported still
    // carries its form, at full energy, with no sweep and no numeral: the machine
    // is printing, and only the figure is missing.
    bool show_operational_form{false};

    bool show_telemetry{false};

    // UI Behaviour Decision 001 §8, Level 0 — Visual Rest: "Stable. Healthy. No action required."
    //
    // ⛔ THE ONE PLACE THAT DECIDES REST, so the information policy is not restated per component.
    // §2 forbids normal Idle from permanently displaying temperatures and says outright that
    // "information shall never be shown merely because it exists"; §9 forbids telemetry
    // dashboards; UI Design Decision 002 makes the accepted F1 resting expression the composition
    // at minimum energy. At rest the operational form is the whole composition.
    //
    // ⬅️ IT IS NOT A SUBJECT STATE, AND IT MUST NOT BECOME ONE. Ready stays Ready when confidence
    // falls or a condition appears — UIDD 004 §10 keeps condition and confidence orthogonal to
    // state — and only this derived fact goes false, restoring the information those levels are
    // entitled to show. Rest is the absence of anything to say, not a seventh state.
    //
    // ⚠️ LEVEL 1 (QUIET CONFIRMATION) IS NOT REPRESENTED IN THE MODEL. §8 places a recent
    // transition into Ready between rest and reduced confidence, and nothing in app_model carries
    // "recently became Ready", so every healthy Ready resolves to Level 0 here. That gap predates
    // this field and is reported rather than invented: adding Level 1 is a state-model decision,
    // and it would narrow this predicate rather than replace it.
    bool at_visual_rest{false};

    // Firmware update progress, kept separate from `progress` because Production
    // UI §9 forbids representing it with the operational Progress Ring.
    OtaPhase ota_phase{OtaPhase::Idle};
    BoundedString<32> update_version;
    ProgressValue update_progress;
    bool update_notice{false};
};

[[nodiscard]] Presentation project(const AppState& state);

[[nodiscard]] bool operator==(const Presentation& lhs, const Presentation& rhs);
[[nodiscard]] bool operator!=(const Presentation& lhs, const Presentation& rhs);

} // namespace prd::app_model
