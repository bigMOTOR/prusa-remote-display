// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <string_view>

// The architectural state layers of UI Architecture Decision 001, one enum per
// section of that decision.
//
// These are deliberately NOT one flat enum. UIAD 001 §14 prohibits "one flat enum
// containing every UI concept", and §2 prohibits collapsing subject truth,
// confidence and device interaction into "one mutually exclusive list of
// screens". Keeping them separate is what makes Attention able to modify Printing
// instead of replacing it, and what lets a modal surface exist over a subject
// state that carries on underneath.
//
// Nothing here is a screen. Which of these layers is presented, and therefore
// which scene the renderer builds, is resolved by the projection
// (presentation.hpp) using UIAD 001 §12's deterministic priority order.

namespace prd::app_model {

// UIAD 001 §5. The four provisioning states, plus the absence of provisioning.
//
// §5 lists four; a fifth value is needed because "not provisioning" is a real
// condition and the alternative — a separate boolean — would let the two disagree.
enum class ProvisioningPhase {
    // Not provisioning. A valid configuration exists, or none is required yet.
    Inactive,

    SetupRequired,
    SetupActive,
    ValidatingConfiguration,
    SetupFailed,
};

[[nodiscard]] std::string_view log_label(ProvisioningPhase phase);

// UIAD 001 §6. The stable semantic state of the represented subject.
//
// "Exactly one Subject State may be current whenever trustworthy subject
// information exists." Attention and Error are NOT here — they are conditions
// (§7) — and neither is any connectivity state, which is confidence (§7).
enum class SubjectState {
    // Acquiring the *first* trustworthy state. §6: "not used for routine retries
    // while a valid state remains representable."
    AcquiringState,

    Ready,
    Preparing,
    Printing,
    Paused,

    // Residual terminal context, not a notification or an overlay (§6).
    Completed,
    Stopped,
};

[[nodiscard]] std::string_view log_label(SubjectState state);

// UIAD 001 §7. Attaches to the current subject state; never replaces it.
enum class SubjectCondition {
    None,
    Attention,
    Error,
};

[[nodiscard]] std::string_view log_label(SubjectCondition condition);

// UIAD 001 §7. Whether the subject state may be presented as current.
//
// Communication health is represented here rather than as a peer subject state,
// which is what keeps "the printer is idle" and "we cannot currently see the
// printer" from being the same kind of fact.
enum class Confidence {
    Current,

    // A recent known subject state may still be shown, with reduced confidence.
    Degraded,

    // No previous subject state may be presented as current.
    Unavailable,
};

[[nodiscard]] std::string_view log_label(Confidence confidence);

// UIAD 001 §8. Prevents trustworthy acquisition; neither a subject state nor a
// device fault. Supersedes AcquiringState once positively classified.
enum class AccessCondition {
    None,
    AuthenticationRequired,
    UnsupportedInterface,
};

[[nodiscard]] std::string_view log_label(AccessCondition condition);

// UIAD 001 §9. Modal presentations over the resolved background state, which
// continues to exist underneath.
enum class ModalSurface {
    None,
    Settings,
    FactoryResetConfirmation,
};

[[nodiscard]] std::string_view log_label(ModalSurface surface);

// UIAD 001 §10. Device-owned states. The exclusive ones temporarily supersede
// normal subject presentation.
enum class DeviceOperation {
    None,
    FirmwareUpdating,
    FactoryResetting,
    RecoveryMode,

    // Highest presentation priority (§10). Structurally incapable of
    // representing a subject error, an authentication failure, an unsupported
    // interface or ordinary connectivity loss: the only way to reach it is a
    // DeviceFaultReason, and that enum contains none of those causes.
    DeviceFault,
};

[[nodiscard]] std::string_view log_label(DeviceOperation operation);

} // namespace prd::app_model
