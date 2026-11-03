// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <array>
#include <cstddef>

#include "components/telemetry_slots.hpp"
#include "text/text.hpp"

// Which catalogue entry each composition draws where — the binding between the text
// resource boundary and the product scenes.
//
// INTERNAL to the ui component. LVGL-free on purpose: this is the part of a scene's text
// behaviour that CAN be host-tested, and separating it from the widget code is what makes
// "the scene requests its label by id rather than owning a literal" a checkable property
// instead of a claim. The scenes and components consume these constants; a host test
// asserts what they resolve to.
//
// Nothing here is a string. Every constant is a TextId, and the only place an English
// word appears is text.cpp.

namespace prd::ui::text {

// --- Production UI §9 Printing ---------------------------------------------

// UI Design Decision 001's centre stack: the progress percentage with its unit, then the
// state title, then the remaining duration. The duration and the percentage's digits are
// formatter output and have no catalogue entry — see the separation note in text.hpp.
inline constexpr TextId kPrintingStateTitle = TextId::StatePrinting;
inline constexpr TextId kPrintingPercentUnit = TextId::UnitPercent;

// Every catalogue entry the Printing composition draws, so a test can assert the whole
// set resolves rather than checking the two it happens to know about.
inline constexpr std::array<TextId, 2> kPrintingTextIds{
    kPrintingStateTitle,
    kPrintingPercentUnit,
};

// The degraded-confidence indicator. Not a state title and not a value — a statement about
// how much the values beside it can be trusted.
inline constexpr TextId kStaleIndicator = TextId::StaleIndicator;

// --- Production UI §8 Telemetry Column -------------------------------------

// One label per slot, in slot order. Indexed by the slot's own enumerator value, the
// same arrangement the catalogue itself uses, so a reordering cannot silently mislabel a
// row.
inline constexpr std::array<TextId, component::kTelemetrySlotCount> kTelemetrySlotLabels{
    TextId::TelemetryNozzle,
    TextId::TelemetryBed,
    TextId::TelemetryLayer,
};

[[nodiscard]] constexpr TextId label_for(const component::TelemetrySlot slot) {
    return kTelemetrySlotLabels[static_cast<std::size_t>(slot)];
}

static_assert(kTelemetrySlotLabels.size() == component::kTelemetrySlotCount,
              "every telemetry slot needs a label");
static_assert(label_for(component::TelemetrySlot::Nozzle) == TextId::TelemetryNozzle,
              "slot order and label order must agree");
static_assert(label_for(component::TelemetrySlot::Bed) == TextId::TelemetryBed,
              "slot order and label order must agree");
static_assert(label_for(component::TelemetrySlot::Layer) == TextId::TelemetryLayer,
              "slot order and label order must agree");

// --- Provisioning scene (Milestone 3.6) ------------------------------------

// The state title, one per provisioning phase the scene presents. The other three
// ProvisioningPhase values do not reach this scene — see scene_selection.cpp.
inline constexpr TextId kSetupRequiredTitle = TextId::StateSetupRequired;
inline constexpr TextId kSetupActiveTitle = TextId::StateSetupActive;

// Milestone 3.10: the same composition, entered automatically when a stored configuration
// cannot be used. The title and the one instruction beneath it.
inline constexpr TextId kRecoveryTitle = TextId::StateRecoveryRequired;
inline constexpr TextId kRecoveryMessage = TextId::RecoveryHoldButton;

// Checkpoint 3c: the Wi-Fi Device Fault, drawn as the same title-plus-Status-Message pair.
//
// ⛔ THE COMPOSITION IS SHARED; THE STATE IS NOT. Recovery and this fault are different conditions
// reached through different doors — DeviceFaultReason::WifiUnavailable at UIAD 001 §12.1 for this
// one, ProvisioningPhase/RecoveryMode at §12.3 for that one — and the scene keys on the typed
// device-fault reason, never on the recovery state and never on a string. What they share is the
// DRAWING: Production UI §8 lists a Status Message for both Unavailable and Failure, §9 specifies no
// screen for either condition, and inventing a second one-title-one-message composition would be a
// redesign owner ruling B forbids.
inline constexpr TextId kWifiUnavailableTitle = TextId::StateWifiUnavailable;
inline constexpr TextId kWifiUnavailableMessage = TextId::WifiRestartDisplay;

// Audit item 5's two device faults, drawn by the same one-title-one-status-message composition.
inline constexpr TextId kFactoryResetIncompleteTitle = TextId::StateFactoryResetIncomplete;
inline constexpr TextId kFactoryResetIncompleteMessage = TextId::FactoryResetRestartDisplay;
inline constexpr TextId kStorageUnavailableTitle = TextId::StateStorageUnavailable;
inline constexpr TextId kStorageUnavailableMessage = TextId::StorageRestartDisplay;

// The pending-image validation fault, drawn by the same shared message composition — 2026-09-12.
// Before this it had no composition at all: scene_selection resolved it to Unimplemented while
// holds_awake() kept the panel lit, so the one fault that says a firmware update did not take
// showed a blank, illuminated screen.
inline constexpr TextId kOtaValidationFailureTitle = TextId::StateUpdateFailed;
inline constexpr TextId kOtaValidationFailureMessage = TextId::UpdateRestartDisplay;

// The field captions, in the order the rows are stacked. The values beside them are
// runtime strings from the session channel and have no catalogue entry, exactly as the
// telemetry values have none.
inline constexpr std::array<TextId, 3> kSetupFieldLabels{
    TextId::SetupFieldNetwork,
    TextId::SetupFieldPassword,
    TextId::SetupFieldAddress,
};

// Every catalogue entry the provisioning composition can draw, so a test asserts the
// whole set resolves rather than the two it happens to know about.
inline constexpr std::array<TextId, 15> kProvisioningTextIds{
    kSetupRequiredTitle,
    kSetupActiveTitle,
    TextId::SetupFieldNetwork,
    TextId::SetupFieldPassword,
    TextId::SetupFieldAddress,
    kRecoveryTitle,
    kRecoveryMessage,
    kWifiUnavailableTitle,
    kWifiUnavailableMessage,
    kFactoryResetIncompleteTitle,
    kFactoryResetIncompleteMessage,
    kStorageUnavailableTitle,
    kStorageUnavailableMessage,
    kOtaValidationFailureTitle,
    kOtaValidationFailureMessage,
};

static_assert(kSetupFieldLabels.size() + 12 == kProvisioningTextIds.size(),
              "twelve titles-or-messages plus one label per row is the whole set the scene draws");

// --- Factory Reset scene (Production UI §9, Milestone 3.10) -----------------

inline constexpr TextId kFactoryResetConfirmTitle = TextId::FactoryResetConfirmTitle;
inline constexpr TextId kFactoryResetConfirmMessage = TextId::FactoryResetConfirmMessage;
inline constexpr TextId kFactoryResetProgressTitle = TextId::FactoryResetProgressTitle;

// Every catalogue entry that composition can draw, so a test asserts the whole set resolves.
inline constexpr std::array<TextId, 3> kFactoryResetTextIds{
    kFactoryResetConfirmTitle,
    kFactoryResetConfirmMessage,
    kFactoryResetProgressTitle,
};

// --- Firmware Update scene --------------------------------------------------
inline constexpr TextId kUpdateDownloadingTitle = TextId::StateUpdateDownloading;
inline constexpr TextId kUpdateKeepPowered = TextId::UpdateKeepPowered;
inline constexpr TextId kUpdateVerifyingTitle = TextId::StateUpdateVerifying;
inline constexpr TextId kUpdateInstallingTitle = TextId::StateUpdateInstalling;
inline constexpr TextId kUpdateRestartingTitle = TextId::StateUpdateRestarting;
inline constexpr TextId kUpdateAvailableTitle = TextId::UpdateAvailableTitle;
inline constexpr TextId kUpdateHoldForSettings = TextId::UpdateHoldForSettings;

inline constexpr std::array<TextId, 10> kFirmwareUpdateTextIds{
    kUpdateDownloadingTitle,      kUpdateKeepPowered,          kUpdateVerifyingTitle,
    kUpdateInstallingTitle,       kUpdateRestartingTitle,      kOtaValidationFailureTitle,
    kOtaValidationFailureMessage, TextId::UpdateFirmwareLabel, kUpdateAvailableTitle,
    kUpdateHoldForSettings,
};

// --- The shared message composition (accepted 2026-09-12) -------------------
//
// Eight full-screen statements drawn by one implementation — component::MessageForm. This table
// is the catalogue half of that: the pair each condition draws, and the semantic tone of its
// symbol. It exists so a test can walk every reachable message rather than the ones it happens
// to know about, and so the eight cannot drift apart in eight places.
//
// ⛔ THE TONE IS SEMANTIC, NOT A SUBSYSTEM LABEL — owner decision, 2026-09-12. Warning is an
// attention or a question: something is being asked of the reader, or is waiting on them.
// Error is a completed operation that failed. A question therefore carries Warning while its
// title stays Primary, which is what keeps "Erase all settings?" from reading as an accident
// that has already happened; the copy names both outcomes, so the meaning does not rest on the
// colour (Product Brief: never status by colour alone).
enum class MessageTone {
    Warning,
    Error,
};

struct MessageEntry {
    TextId title;
    TextId explanation;
    MessageTone tone;
};

inline constexpr std::array<MessageEntry, 8> kMessageFamily{{
    {TextId::StateAuthenticationRequired, TextId::AccessCheckKey, MessageTone::Warning},
    {TextId::StateUnsupportedInterface, TextId::AccessUnsupported, MessageTone::Warning},
    {kRecoveryTitle, kRecoveryMessage, MessageTone::Warning},
    {kFactoryResetConfirmTitle, kFactoryResetConfirmMessage, MessageTone::Warning},
    {kWifiUnavailableTitle, kWifiUnavailableMessage, MessageTone::Error},
    {kStorageUnavailableTitle, kStorageUnavailableMessage, MessageTone::Error},
    {kFactoryResetIncompleteTitle, kFactoryResetIncompleteMessage, MessageTone::Error},
    {kOtaValidationFailureTitle, kOtaValidationFailureMessage, MessageTone::Error},
}};

// The symbol's own glyph. Shared with the operational condition composition, which draws the
// same mark inside the progress ring — one construction, per Production UI §8's icon rules.
inline constexpr TextId kMessageMark = TextId::AccessWarningMark;

} // namespace prd::ui::text
