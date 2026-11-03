// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include "scene_selection.hpp"

namespace prd::ui {
namespace {

// Which device faults this checkpoint has copy for.
//
// ⛔ EXHAUSTIVE, WITH NO PERMISSIVE DEFAULT. A fault without its own words must resolve to
// Unimplemented rather than borrow a sentence written for a different cause — the comment below
// says so, and before this it was enforced by a single `==` that a new reason could quietly slip
// past. Now a new DeviceFaultReason fails to compile until someone decides whether it has copy.
[[nodiscard]] bool fault_has_composition(const app_model::DeviceFaultReason reason) {
    switch (reason) {
    case app_model::DeviceFaultReason::WifiUnavailable:
    case app_model::DeviceFaultReason::FactoryResetIncomplete:
    case app_model::DeviceFaultReason::StorageFailure:

    // ⬅️ AND OtaValidationFailure JOINED THEM — 2026-09-12. It had resolved to Unimplemented since
    // the first-boot health work landed, so the one fault that says a firmware update did not take
    // drew a BLANK panel — and, because holds_awake() is true for every device fault, a blank LIT
    // panel that never dimmed. The owner named it a delivery blocker; the copy is its own pair, and
    // the composition is the one every other statement now draws.
    case app_model::DeviceFaultReason::OtaValidationFailure:
        return true;

    case app_model::DeviceFaultReason::None:
    case app_model::DeviceFaultReason::BoardMismatch:
    case app_model::DeviceFaultReason::DisplayInitializationFailure:
    case app_model::DeviceFaultReason::LowMemory:
    case app_model::DeviceFaultReason::TaskWatchdog:
        return false;
    }

    return false;
}

} // namespace

Scene select(const app_model::Presentation& presentation) {
    // ⬅️ FACTORY RESET — MILESTONE 3.10, REORDERED AT 3.11. §12.2's exclusive operation and
    // §12.5's modal surface are different tiers, and both resolve to the one Production UI §9
    // screen that covers the operation's two halves: the confirmation and the progress.
    //
    // ⛔ THIS IS WHAT MAKES THE DESTRUCTIVE PATH VISIBLE, AND WHY IT IS FIRST. Architecture §5.6
    // requires the reset to be requested through an on-screen confirmation sequence, and ADR-0008
    // decision 7 builds the two-press rule on top of that premise. Milestone 3.11 T16 found the
    // premise broken: these two checks sat BELOW the recovery-or-provisioning block, which returns
    // unconditionally, so on an unprovisioned device the armed confirmation resolved to the setup
    // screen instead. The prompt never drew, the window ran its full fifteen seconds invisibly, and
    // a second press would have erased with nothing ever shown — the same defect 3.10 fixed for
    // every other background, reaching this one through a different door.
    //
    // ⛔ THE PRESENTATION-LAYER PRIORITY MODEL IS UNCHANGED, AND DELIBERATELY SO. resolve_layer()
    // is untouched, `presentation.layer` still reports whatever §12 ranked highest, and the subject
    // and provisioning state still exist beneath — UIAD 001 §9's "the background continues to
    // exist". What moved is only which SCENE an armed modal resolves to, which is the overlay's
    // own question and not a re-ranking of the tiers.
    //
    // The operation is checked before the confirmation because §12 ranks it above the modal, and
    // because a reset in progress must not still be asking the question.
    // ⛔ §12.1 IS FIRST, ALWAYS — checkpoint 3c. A local device fault outranks every other tier,
    // including an exclusive operation and the provisioning session beneath it, and the Wi-Fi
    // quarantine the owner's DriverStalled ruling defines is one. It is keyed on the TYPED reason
    // rather than on the layer alone, so the composition below is reached only by a fault it has
    // copy for; any other device fault still resolves to Unimplemented rather than borrowing words
    // written for this one.
    //
    // ⬅️ AND THE SETUP ACCESS POINT AND ITS SERVER STAY UP, which is not a contradiction. The ruling
    // keeps them physically alive so a browser mid-flow is not stranded; what the fault wins is the
    // PANEL, and further submissions are refused by the quarantined station itself rather than by
    // hiding the form.
    //
    // ⬅️ AND StorageFailure JOINED IT — audit item 5, 2026-08-28. It had been raised since Milestone
    // 3.10 and resolved to Unimplemented, so the one fault that says the device cannot read its own
    // settings drew a BLANK PANEL. Correcting that is explicitly in scope for this checkpoint,
    // because the journal-probe repair routes an unreadable or invalid reset journal to the same
    // reason and a fault the user cannot see is not a fault they can act on.
    if (fault_has_composition(presentation.device_fault)) {
        return Scene::Provisioning;
    }

    if (presentation.device_operation == app_model::DeviceOperation::FactoryResetting) {
        return Scene::FactoryReset;
    }

    if (presentation.device_operation == app_model::DeviceOperation::FirmwareUpdating) {
        return Scene::FirmwareUpdate;
    }

    // A download/install failure is still an update statement even though the exclusive operation
    // has ended. Pending-image validation failure is already caught above by the higher-priority
    // typed device fault. Both use ProvisioningScene only as the existing owner of the already
    // built shared MessageForm; this avoids allocating a second full copy in the 64 KiB LVGL pool.
    if (presentation.ota_phase == app_model::OtaPhase::Failed) {
        return Scene::Provisioning;
    }

    if (presentation.update_notice) {
        return Scene::Provisioning;
    }

    if (presentation.modal_surface == app_model::ModalSurface::FactoryResetConfirmation) {
        return Scene::FactoryReset;
    }

    // §12.3's recovery-or-provisioning tier, which Milestone 3.6 implements for the two
    // phases the setup access point produces. Placed before the subject test below and
    // touching nothing in it: the operational path resolves exactly as it did.
    if (presentation.layer == app_model::PresentationLayer::RecoveryOrProvisioning) {
        // ⬅️ MILESTONE 3.10: RECOVERY SHARES THIS COMPOSITION, and it is the same tier that
        // already put it here. UIAD 001 §12.3 groups Recovery Mode with Setup Required, Setup
        // Active, Validating Configuration and Setup Failed as one presentation priority, and
        // UI Design Decision 003 says failure states "interrupt the acquisition of confidence but
        // do not establish a different visual identity". Production UI §9 specifies no Recovery
        // screen, so inventing one would be the redesign owner ruling B forbids — while leaving
        // the state with no scene at all would put an automatically-entered condition on a blank
        // panel. The composition draws its title and Production UI §8's Status Message; the
        // session rows stay absent because there is no session.
        //
        // Checked BEFORE the phase, because a device in recovery has ProvisioningPhase::Inactive
        // — it is in recovery precisely because provisioning has not been entered.
        if (presentation.device_operation == app_model::DeviceOperation::RecoveryMode) {
            return Scene::Provisioning;
        }

        switch (presentation.provisioning) {
        case app_model::ProvisioningPhase::SetupRequired:
        case app_model::ProvisioningPhase::SetupActive:

        // Milestone 3.8. Both are the same composition with different content: the scene
        // already hides a row whose value is absent, so validating and failed reuse it
        // rather than introducing screens the provisional UI has no design for.
        case app_model::ProvisioningPhase::ValidatingConfiguration:
        case app_model::ProvisioningPhase::SetupFailed:
            return Scene::Provisioning;

        // Inactive: the layer resolved to provisioning while the phase says there is none.
        // Not reachable through the projection, and not a screen.
        //
        // Listed rather than defaulted, so a new phase cannot arrive without a decision.
        case app_model::ProvisioningPhase::Inactive:
            return Scene::Unimplemented;
        }

        return Scene::Unimplemented;
    }

    if (presentation.layer == app_model::PresentationLayer::AccessCondition &&
        (presentation.access_condition == app_model::AccessCondition::AuthenticationRequired ||
         presentation.access_condition == app_model::AccessCondition::UnsupportedInterface)) {
        return Scene::Access;
    }

    // Anything else above the subject in UIAD 001 §12's order — a device fault, an
    // exclusive operation, boot, an access condition, a modal surface — is a Phase D or
    // later composition. The projection has already decided that one of them wins, so
    // there is nothing to weigh here.
    if (presentation.layer != app_model::PresentationLayer::SubjectPresentation) {
        return Scene::Unimplemented;
    }

    // A reported printer error needs no invented prior subject. Access/device/modal
    // priority above still wins; unavailable observations keep their existing route.
    if ((presentation.subject_condition == app_model::SubjectCondition::Error ||
         presentation.subject_condition == app_model::SubjectCondition::Attention) &&
        presentation.confidence != app_model::Confidence::Unavailable) {
        return Scene::Operational;
    }

    // Initial acquisition has no previous subject to misrepresent. Lost observation has its
    // own transient acknowledgement, and must not acquire a persistent Connecting screen.
    if (presentation.subject == app_model::SubjectState::AcquiringState &&
        presentation.acquiring_connection &&
        presentation.access_condition == app_model::AccessCondition::None) {
        return Scene::Access;
    }

    // Lost observation owns only the transient acknowledgement, then an intentionally blank
    // composition. Degraded keeps the operational scene with its accepted age indicator.
    if (presentation.confidence == app_model::Confidence::Unavailable) {
        return Scene::Unimplemented;
    }

    switch (presentation.subject) {
    case app_model::SubjectState::Ready:
    case app_model::SubjectState::Preparing:
    case app_model::SubjectState::Printing:
    case app_model::SubjectState::Paused:
    case app_model::SubjectState::Completed:
    case app_model::SubjectState::Stopped:
        return Scene::Operational;

    case app_model::SubjectState::AcquiringState:
        // Acquisition not in progress (or superseded by lost observation) has no persistent UI.
        return Scene::Unimplemented;
    }

    return Scene::Unimplemented;
}

std::string_view log_label(const Scene scene) {
    switch (scene) {
    case Scene::Unimplemented:
        return "unimplemented";

    case Scene::Operational:
        return "operational";

    case Scene::FactoryReset:
        return "factory-reset";

    case Scene::Provisioning:
        return "provisioning";
    case Scene::Access:
        return "access";

    case Scene::FirmwareUpdate:
        return "firmware-update";
    }

    return "unknown";
}

} // namespace prd::ui
