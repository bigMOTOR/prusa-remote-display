// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include "text/text.hpp"

#include <array>

namespace prd::ui::text {
namespace {

struct Entry {
    TextId id;
    std::string_view english;
};

// THE CATALOGUE. The only place in the firmware where a user-visible string literal
// appears — enforced by firmware/tools/check-tokens.sh, which rejects prose literals in
// scene and component sources.
//
// The words are the accepted artwork's. Copy is defined by no accepted source: UI
// Architecture Decision 001 §1 excludes it explicitly and the Production UI
// Specification defines typography without defining words, so the artwork is the only
// source that has them.
//
// A second language would be a second array of the same shape and a way to choose
// between them. Neither exists, by instruction — English is the MVP's only language.
// What matters is that adding one would touch this file and nothing else: no scene, no
// component, no layout.
constexpr std::array<Entry, kTextIdCount> kEnglish{{
    {TextId::StatePrinting, "Printing"},
    {TextId::TelemetryNozzle, "Nozzle"},
    {TextId::TelemetryBed, "Bed"},
    {TextId::TelemetryLayer, "Layer"},
    {TextId::UnitPercent, "%"},
    {TextId::StatePreparing, "Preparing"},
    {TextId::StatePaused, "Paused"},
    {TextId::StateCompleted, "Completed"},
    {TextId::StateStopped, "Stopped"},
    {TextId::StaleIndicator, "Reconnecting: %lu s old"},
    {TextId::StateSetupRequired, "Setup required"},
    {TextId::StateSetupActive, "Join this network"},
    {TextId::SetupFieldNetwork, "Network"},
    {TextId::SetupFieldPassword, "Password"},
    {TextId::SetupFieldAddress, "Address"},
    {TextId::StateValidatingConfiguration, "Checking that network"},
    {TextId::StateSetupFailed, "That did not work"},

    // Milestone 3.10. "Cannot be used" rather than "corrupt" or "invalid": the reader does not
    // need the failure mode, and the two things they do need are that it is not their fault and
    // that nothing was lost. "Hold the button" is the gesture owner ruling 1 assigned.
    {TextId::StateRecoveryRequired, "Settings cannot be used"},
    {TextId::RecoveryHoldButton, "Nothing has been erased. Hold the button to set up again."},

    // Milestone 3.10. "Hold the button again" is the second very-long press; "a short press
    // cancels" is the other input that exists. The confirmation window expiring also cancels and is
    // deliberately not mentioned: it is a safe outcome the user does not need to act on, and naming
    // it would manufacture the urgency UI Behaviour Decision 001 keeps out of the product.
    {TextId::FactoryResetConfirmTitle, "Erase all settings?"},
    {TextId::FactoryResetConfirmMessage, "Hold the button again to erase. A short press cancels."},
    {TextId::FactoryResetProgressTitle, "Erasing settings..."},

    // Checkpoint 3c. The owner's DriverStalled ruling supplies both lines verbatim.
    {TextId::StateWifiUnavailable, "Wi-Fi unavailable"},
    {TextId::WifiRestartDisplay, "Restart the display."},

    // Audit item 5. Both lines are the owner's, verbatim. "Could not finish" rather than "failed":
    // it says what happened without blaming the user or the device, and it makes no claim about
    // what survived — because on one route records are gone and on another nothing was touched.
    {TextId::StateFactoryResetIncomplete, "Reset could not finish"},
    {TextId::FactoryResetRestartDisplay, "Restart the display."},

    // Audit item 5. "Storage unavailable" parallels "Wi-Fi unavailable": a part of the device is
    // not answering. It deliberately does not say what was or was not stored, because on this route
    // the firmware could not read the store to find out.
    {TextId::StateStorageUnavailable, "Storage unavailable"},
    {TextId::StorageRestartDisplay, "Restart the display."},
    {TextId::StatusPrinterUnavailable, "Printer unavailable"},
    {TextId::ProgressUnavailable, "Progress unavailable"},
    {TextId::StateConnecting, "Connecting..."},
    {TextId::StateAuthenticationRequired, "Authentication required"},
    {TextId::StateUnsupportedInterface, "Unsupported interface"},
    {TextId::AccessCheckKey, "Check the PrusaLink API key."},
    {TextId::AccessUnsupported, "This printer interface is not supported."},
    {TextId::AccessWarningMark, "!"},
    {TextId::PrinterError, "Printer error"},
    {TextId::CheckPrinterDisplay, "Check printer display"},
    {TextId::LastSubject, "Last: %s"},
    {TextId::LastSubjectProgress, "Last: %s / %s%%"},
    {TextId::StateReady, "Ready"},
    {TextId::PrinterAttention, "Attention"},
    {TextId::CurrentSubject, "%s"},
    {TextId::CurrentSubjectProgress, "%s / %s%%"},

    // The pending-image validation fault. Both lines are the owner's, verbatim and exact.
    {TextId::StateUpdateFailed, "Update failed"},
    {TextId::UpdateRestartDisplay, "Restart the display."},
    {TextId::StateUpdateDownloading, "Downloading"},
    {TextId::UpdateKeepPowered, "Keep the display powered."},
    {TextId::StateUpdateVerifying, "Verifying..."},
    {TextId::StateUpdateInstalling, "Installing..."},
    {TextId::StateUpdateRestarting, "Restarting..."},
    {TextId::UpdateFirmwareLabel, "Firmware"},
    {TextId::UpdateAvailableTitle, "Update available"},
    {TextId::UpdateHoldForSettings, "Hold for settings"},
    {TextId::RemainingMinutes, "%s%lum left"},
    {TextId::RemainingHoursMinutes, "%s%luh%02lum left"},
}};

// --- Compile-time catalogue integrity --------------------------------------
//
// Three properties, all checked at build time rather than by a test that could be
// deleted or by a review that could miss one.
//
// All three are written as accumulating loops rather than as std::all_of, because they
// are evaluated inside a static_assert and <algorithm>'s predicates are not constexpr
// until C++20 — and C++17 is the binding standard (accepted decision D4).

// COMPLETE AND UNIQUE. The table is indexed by the enumerator's own value, so entry i
// must carry id i. That single property rules out three separate defects at once: a
// missing entry shifts every later index and fails; a duplicate id displaces another
// and fails; a reordered table fails. It is also what makes lookup() an array index
// rather than a search.
constexpr bool catalogue_is_indexed_by_id() {
    bool indexed = true;

    for (std::size_t index = 0; index < kEnglish.size(); ++index) {
        indexed = indexed && static_cast<std::size_t>(kEnglish[index].id) == index;
    }

    return indexed;
}

static_assert(kEnglish.size() == kTextIdCount,
              "the catalogue must have exactly one entry per TextId");
static_assert(catalogue_is_indexed_by_id(),
              "every catalogue entry must sit at the index of its own TextId: a missing, "
              "duplicated or reordered entry breaks this");

// NON-EMPTY. An empty entry would draw nothing and read as a rendering fault rather
// than as a catalogue gap.
constexpr bool catalogue_has_no_empty_entry() {
    bool populated = true;

    for (const auto& entry : kEnglish) {
        populated = populated && !entry.english.empty();
    }

    return populated;
}

static_assert(catalogue_has_no_empty_entry(), "a catalogue entry must not be empty");

// RENDERABLE. Every entry lies inside the generated Inter subset. See the glyph-coverage
// note in text.hpp: LVGL drops a codepoint the font lacks, silently and with no
// substitute, so an out-of-subset entry would reach the panel with letters missing. This
// is the check that makes "unsupported glyphs are never silently substituted" a
// property of the build rather than a promise.
constexpr bool catalogue_is_renderable() {
    bool renderable = true;

    for (const auto& entry : kEnglish) {
        renderable = renderable && is_renderable(entry.english);
    }

    return renderable;
}

static_assert(catalogue_is_renderable(),
              "a catalogue entry uses a codepoint the generated Inter faces do not "
              "contain; extend the subset in tools/generate-fonts.py or change the entry");

} // namespace

std::string_view lookup(const TextId text_id) {
    const auto index = static_cast<std::size_t>(text_id);

    // Cannot happen while the enumeration and the catalogue agree, which the assertions
    // above guarantee. Guarded anyway because the alternative is an out-of-bounds read
    // in the render path, and returning an empty view draws nothing rather than drawing
    // whatever followed the array.
    if (index >= kEnglish.size()) {
        return {};
    }

    return kEnglish[index].english;
}

TextId text_id_at(const std::size_t index) {
    if (index >= kEnglish.size()) {
        return TextId::StatePrinting;
    }

    return kEnglish[index].id;
}

} // namespace prd::ui::text
