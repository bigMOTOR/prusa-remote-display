// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

// The text resource boundary — Architecture Spec §5.12 `ui_assets`.
//
// INTERNAL to the ui component. LVGL-free, so it compiles in the host test build and
// the catalogue's completeness is asserted by tests rather than by inspection.
//
// WHY THIS EXISTS. Product scenes must not own user-visible strings. A scene that
// holds an English literal has to be edited to add a language, which means every
// future language change is a change to composition code — and composition code is
// where the layout, the geometry and the redraw logic live. Putting the strings behind
// an id makes a catalogue an additive change and leaves the scenes untouched.
//
// WHAT THIS IS NOT, deliberately and by instruction:
//
//   - not a localization system. There is one catalogue, English, and it is compiled
//     in. There is no runtime language selection, no translation, no plural-rule
//     engine, no locale file, no persisted preference and no third-party library.
//   - not a formatter. Numbers, temperatures, durations and layer counts are formatted
//     by ui/src/formatters.cpp and never pass through here. The two are separate on
//     purpose: a number's presentation is a §17.7 rule, and a word's is a catalogue
//     entry. Nothing in this file interpolates a value into a string.
//   - not dynamic. lookup() returns a view of a string with static storage duration.
//     Nothing here allocates, and there is no state to initialise.
//
// ---------------------------------------------------------------------------
// GLYPH COVERAGE — read this before adding an entry
// ---------------------------------------------------------------------------
//
// The generated Inter faces carry the Phase C subset and nothing more: printable ASCII
// (U+0020…U+007E) plus the degree sign (U+00B0). That subset is chosen in
// firmware/tools/generate-fonts.py and is what makes the six faces cost 29 kB instead
// of several hundred.
//
// A FUTURE LANGUAGE WILL THEREFORE NEED ITS OWN GENERATED SUBSET. Adding a catalogue
// is not sufficient on its own: Czech, German or Polish text needs Latin Extended-A,
// and anything beyond Latin needs a different range again. That is a font-generation
// change alongside the catalogue, and it has a flash cost that has to be budgeted.
//
// UNSUPPORTED GLYPHS ARE NEVER SILENTLY SUBSTITUTED. LVGL's behaviour for a codepoint
// a font does not contain is to skip it — the character simply does not appear, with no
// warning and no replacement mark. A catalogue entry outside the subset would therefore
// reach the panel with letters missing and look like a rendering fault rather than a
// missing translation. is_renderable() below exists so that cannot happen: every entry
// is checked against the generated subset at COMPILE TIME, and an entry that is not
// renderable fails the build.
//
// The 36 px face carries a narrower subset still — digits and numeric punctuation only.
// Nothing from this catalogue is ever drawn in it: it renders formatter output, which is
// digits. If a composition ever needs a word at display size, that face's subset has to
// grow with it.

namespace prd::ui::text {

// Every user-visible string the Phase C compositions draw.
//
// The enumeration is the catalogue's index — see the completeness assertion in
// text.cpp — so an id added here without an entry fails to compile, and an entry
// without an id does too.
enum class TextId : std::uint8_t {
    // Subject state titles (UIAD 001 §6). One word per operational state, except Ready —
    // the accepted artwork shows the printer's own name there rather than a state word, and
    // Production UI §9 lists "Printer identity" among Ready's visible components and no
    // state title. A resting device names the machine, not its condition.
    StatePrinting = 0,

    // Telemetry row labels (Production UI §8's Temperature Row and Telemetry Column).
    // These name the row rather than carrying its value, so they are structure and not
    // a placeholder — see the note in components/telemetry_column.hpp.
    TelemetryNozzle = 1,
    TelemetryBed = 2,
    TelemetryLayer = 3,

    // The percent unit. A unit symbol rather than prose, but still a user-visible
    // literal that a scene would otherwise own, and its spelling and spacing are not
    // universal. The NUMBER beside it comes from format::percent() and never from here.
    UnitPercent = 4,

    // The rest of the operational family (Production UI §9).
    StatePreparing = 5,
    StatePaused = 6,
    StateCompleted = 7,
    StateStopped = 8,

    // NO SUBTITLE FOR PREPARING. `SubtitleHeating = 9` — "Heating up" — was removed in
    // Milestone 2 Phase G.4. UIDD 004 §8 makes the third line "a single slot holding whichever
    // machine fact is currently true", and API Research Report §16.4 (Accepted) establishes
    // that this is not one: the phase may be "heating; homing; mesh probing; pre-print
    // activity; another busy operation", and "the API may not identify the exact preparation
    // phase". A word the machine never sent is not a machine fact, and §7 excludes it.
    //
    // Board 2 panel 4's "Resume print" was already excluded for a different reason — it names
    // an action a read-only MVP cannot perform (Architecture Spec §2.2 and invariant 18). The
    // artwork's two subtitles therefore both fail, on separate grounds, and this catalogue
    // holds no subtitle at all.
    //
    // Adding one back requires an accepted source that establishes a real, machine-supplied
    // activity value — not a new word for the same guess.

    // The degraded-confidence indicator (§11.5, UI Behaviour Decision 001 §7's "the product
    // must never present stale information as current"). Written from the reader's side: what
    // matters to them is that the figures are not current, not that a confidence enum changed.
    StaleIndicator = 9,

    // --- Provisioning (UIAD 001 §5, Production UI §9 Initial Setup) ----------
    //
    // The provisional provisioning scene's words. Milestone 3.6 exists to make the
    // Milestone 3.5 setup access point joinable without a serial cable, so these are the
    // minimum a user needs: what state the device is in, and what the two values beneath
    // are called.
    //
    // WRITTEN FROM THE READER'S SIDE. "Join this network" names what the person does, not
    // what the firmware is doing — UIAD 001 §5's own label for the state is "Setup Active",
    // which describes the device to itself. It also names an action the PRODUCT cannot
    // perform, which is deliberate and the opposite of the excluded "Resume print": the
    // reader performs this one, on their own phone.
    StateSetupRequired = 10,
    StateSetupActive = 11,

    // The two field captions. They name their row rather than carrying its value, on the
    // same footing as the telemetry labels above — the SSID and the password are runtime
    // values and never pass through this catalogue.
    SetupFieldNetwork = 12,
    SetupFieldPassword = 13,

    // The local address of the setup interface.
    //
    // ⚠️ BUILT BUT NOT DRAWN IN MILESTONE 3.6. Owner ruling, 2026-08-15: the URL is not
    // shown while no HTTP server is listening, because a browser that cannot reach it reads
    // as a broken device. The row exists so M3.7 populates a value rather than restructuring
    // a composition, and it stays hidden under the same presence rule everything else obeys.
    SetupFieldAddress = 14,

    // Milestone 3.8's two new provisioning phases.
    StateValidatingConfiguration = 15,
    StateSetupFailed = 16,

    // --- Recovery (Milestone 3.10, owner Decision 3 and ruling B) -------------
    //
    // A stored configuration that cannot be used — Invalid, MigrationRequired or
    // StorageFailure. UIAD 001 §10 calls it "a recoverable device-owned condition that
    // prevents normal operation" and §12.3 puts it in the same presentation tier as the
    // provisioning phases, which is why it shares their composition.
    //
    // WRITTEN FROM THE READER'S SIDE, and about the device rather than the printer: the
    // settings this display was using are unusable, and nothing has been thrown away.
    StateRecoveryRequired = 17,

    // The instruction beneath it, drawn in Production UI §8's Status Message.
    //
    // ⛔ IT DESCRIBES ONLY BEHAVIOUR THAT EXISTS IN THIS CHECKPOINT — owner ruling B. Holding
    // the button really does start setup from Milestone 3.10 onward, which is what makes this
    // sentence sayable at all; the same words in Milestone 3.9 would have named a gesture the
    // firmware did not have. No Settings, no factory reset, no recovery action that is not
    // implemented, and no promise that anything will be repaired on its own.
    RecoveryHoldButton = 18,

    // --- Factory Reset (Production UI §9, Milestone 3.10) ---------------------
    //
    // §9: "Factory Reset. Visible: Reset confirmation, Reset progress."
    //
    // ⛔ THE QUESTION NAMES THE CONSEQUENCE, NOT THE MECHANISM. "Erase all settings?" is what the
    // user loses; a word like "factory reset" describes what the firmware calls it. And the
    // instruction names BOTH actions, because §5.6 requires the sequence to be difficult to
    // trigger accidentally and a prompt that only tells you how to proceed is not that.
    FactoryResetConfirmTitle = 19,
    FactoryResetConfirmMessage = 20,

    // The progress half. One line: the erase is over in milliseconds, so there is nothing to
    // instruct and no duration to represent.
    //
    // ⬅️ IT CARRIES ITS THREE DOTS, LIKE StateConnecting, AND FOR THE SAME REASON — owner ruling
    // after physical review, 2026-09-12. The scene trims the entry to one, two or three dots on a
    // 500 ms cycle; the catalogue holds the longest form so the trimming is a substring of a real
    // entry rather than a sentence assembled in a scene. The dots are ACTIVITY, not progress: they
    // represent nothing the firmware measures and no duration it could claim.
    FactoryResetProgressTitle = 21,

    // --- Device Fault: the Wi-Fi driver stopped answering (checkpoint 3c) ------
    //
    // The owner's DriverStalled ruling gives this copy exactly, and it is exact because both halves
    // are load-bearing. The title names the CONDITION as the user experiences it — the product's
    // Wi-Fi is unavailable — without claiming their network is at fault, because it may be perfectly
    // healthy. The message names the ONLY action that works: the station is quarantined and issues
    // no further driver operation, so "try again" would be false and "check your password" would
    // send them to fix something that was never tested.
    StateWifiUnavailable = 22,
    WifiRestartDisplay = 23,

    // --- Device Fault: a confirmed factory reset did not finish (audit item 5) -
    //
    // ⛔ THE TITLE MAKES NO PERSISTENCE CLAIM IN EITHER DIRECTION, AND THAT IS THE WHOLE POINT. It
    // cannot say nothing was erased — on the route where the removals began, records really are
    // gone. It cannot say the settings are safe, nor that the new ones are: neither is established.
    // What it can say is exactly what happened, which is that the reset did not finish.
    //
    // The message names the only action that works. There is no automatic retry and no automatic
    // reboot in this product; a restart is what re-probes the journal and tries once more.
    StateFactoryResetIncomplete = 24,
    FactoryResetRestartDisplay = 25,

    // --- Device Fault: the store would not answer (audit item 5) --------------
    //
    // Raised when NVS bring-up fails, and when the reset journal could not be read or held a value
    // this firmware did not write. On that second route NO RESET IS PROVED TO EXIST, which is
    // precisely why this cannot borrow the sentence above: "Reset could not finish" would assert an
    // operation that may never have been requested.
    //
    // ⬅️ AND IT IS NOT StateRecoveryRequired. That one means the settings were READ and cannot be
    // used, and it goes on to promise that nothing has been erased. This one means they could not
    // be read at all, so no such promise is available.
    StateStorageUnavailable = 26,
    StorageRestartDisplay = 27,

    // --- Lost observation of the printer (Production UI §9) -------------------
    //
    // §9's Status Message for Printer Unavailable, drawn once as the transient acknowledgement of
    // UIAD 001 §11.1 and then retired. The only user-visible string that condition has.
    //
    // ⛔ IT ASSERTS NO CAUSE, AND §9 REQUIRES THAT EXPLICITLY: it "shall not state or imply that the
    // printer is offline, powered off, disconnected, network-isolated, unreachable or failed,
    // because the device does not know which of these is true." All the device knows is that it
    // cannot currently observe the printer, and "unavailable" is the accepted word for exactly that
    // — UIAD 001 §7's own name for the confidence value.
    //
    // ⬅️ WHY NOT "Printer offline". It was drafted and rejected: offline names a cause the device
    // has not established. A printer the owner switched off, a printer on a sleeping switch and a
    // printer behind a dropped Wi-Fi link are indistinguishable from here.
    //
    // ⛔ AND IT NAMES NO ACTION. There is nothing the reader can do that the product can verify,
    // and §9 permits only the status message — no instruction, no identity, no previous value.
    StatusPrinterUnavailable = 28,
    ProgressUnavailable = 29,
    StateConnecting = 30,
    StateAuthenticationRequired = 31,
    StateUnsupportedInterface = 32,
    AccessCheckKey = 33,
    AccessUnsupported = 34,
    AccessWarningMark = 35,
    PrinterError = 36,
    CheckPrinterDisplay = 37,
    LastSubject = 38,
    LastSubjectProgress = 39,
    StateReady = 40,
    PrinterAttention = 41,
    CurrentSubject = 42,
    CurrentSubjectProgress = 43,

    // --- Device Fault: a pending firmware image did not validate ---------------
    //
    // Owner-supplied copy, verbatim and exact. The title says what happened without naming a
    // mechanism the reader has no use for — an image, a partition, a signature, a health window
    // are all firmware vocabulary — and without claiming which half failed.
    //
    // ⛔ IT PROMISES NO RESTORATION. Rolling back to the previous image is intended pending-image
    // bootloader behaviour, not a result this firmware has observed and may assert; the PM
    // correction of 2026-09-12 is explicit about that. "Restart the display." is therefore the
    // only action named, on the same footing as the three faults above it: it is what re-runs the
    // boot path, and it is the one thing that works.
    //
    // ⬅️ AND IT IS ITS OWN PAIR, NOT A BORROWED ONE. StateWifiUnavailable, StateStorageUnavailable
    // and StateFactoryResetIncomplete each carry their own "Restart the display." for exactly the
    // reason recorded below this enumeration: a rewording written for one cause must not silently
    // reword three others.
    StateUpdateFailed = 44,
    UpdateRestartDisplay = 45,

    // --- Firmware update operation ------------------------------------------
    // Downloading alone carries a measured percentage. The other three are activity words,
    // with the same one/two/three-dot language as Connecting and Erasing settings.
    StateUpdateDownloading = 46,
    UpdateKeepPowered = 47,
    StateUpdateVerifying = 48,
    StateUpdateInstalling = 49,
    StateUpdateRestarting = 50,
    UpdateFirmwareLabel = 51,
    UpdateAvailableTitle = 52,
    UpdateHoldForSettings = 53,
};

// ⛔ FIVE IDS FOR TWO SENTENCES, DELIBERATELY. FactoryResetRestartDisplay, StorageRestartDisplay,
// WifiRestartDisplay and UpdateRestartDisplay currently carry identical bytes. They are separate
// entries so the four causes can be reworded independently — sharing one id would mean a change
// written for one fault silently rewording three others, which is the "a sentence is a claim about
// every route that emits it" rule applied to the catalogue itself.
inline constexpr std::size_t kTextIdCount = 54;

// The English string. Never empty, and always within the generated glyph subset —
// both properties are asserted at compile time in text.cpp.
[[nodiscard]] std::string_view lookup(TextId text_id);

// Index-addressable, so a test can walk every id without repeating the list and
// without being able to miss one.
[[nodiscard]] TextId text_id_at(std::size_t index);

// --- Glyph coverage --------------------------------------------------------

// Whether every codepoint in `text` is present in the generated Inter faces.
//
// The subset is printable ASCII plus U+00B0, so this checks the UTF-8 byte sequence
// directly: single bytes in 0x20…0x7E, or the two-byte pair for the degree sign.
// Anything else — a Latin Extended letter, a typographic quote, a real ellipsis —
// would be dropped silently by LVGL, which is the failure this prevents.
//
// constexpr so it can be applied to the whole catalogue by static_assert. A build is
// the right place to catch this: the alternative is discovering it on a panel, where a
// missing letter looks like a font fault rather than a coverage gap.
[[nodiscard]] constexpr bool is_renderable(const std::string_view text) {
    for (std::size_t index = 0; index < text.size(); ++index) {
        const auto byte = static_cast<unsigned char>(text[index]);

        if (byte >= 0x20U && byte <= 0x7EU) {
            continue;
        }

        // U+00B0 DEGREE SIGN, the one non-ASCII codepoint in the subset.
        if (byte == 0xC2U && (index + 1) < text.size() &&
            static_cast<unsigned char>(text[index + 1]) == 0xB0U) {
            ++index;
            continue;
        }

        return false;
    }

    return true;
}

} // namespace prd::ui::text
