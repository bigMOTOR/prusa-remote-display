// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include <gtest/gtest.h>

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "components/telemetry_slots.hpp"
#include "text/scene_text.hpp"
#include "text/text.hpp"

// The text resource boundary suite.
//
// Three things are protected here.
//
// The catalogue is complete and consistent: every id resolves, nothing is empty, nothing
// is duplicated. Most of that is a static_assert in text.cpp — a defect there should not
// reach a build, let alone a test run — and these tests confirm the resolved behaviour
// rather than restating the assertions.
//
// Every entry is renderable by the generated Inter faces. This is the one that matters
// most in practice: LVGL drops a codepoint a font does not contain, silently and with no
// substitute, so an out-of-subset entry reaches the panel with letters missing and looks
// like a font fault. The check exists so a future catalogue cannot do that.
//
// The compositions request their text by id. The Printing scene and the Telemetry Column
// are LVGL and cannot be compiled here, but their text BINDINGS are pure and are — so
// what is asserted is that the binding is a TextId that resolves to the expected word,
// and that the scene sources contain no literal (which firmware/tools/check-tokens.sh
// enforces at build time, in both directions).

using namespace prd::ui::text;
using prd::ui::component::TelemetrySlot;

namespace {

constexpr std::array<TextId, kTextIdCount> kEveryId{
    TextId::StatePrinting,
    TextId::TelemetryNozzle,
    TextId::TelemetryBed,
    TextId::TelemetryLayer,
    TextId::UnitPercent,
    TextId::StatePreparing,
    TextId::StatePaused,
    TextId::StateCompleted,
    TextId::StateStopped,
    TextId::StaleIndicator,
    TextId::StateSetupRequired,
    TextId::StateSetupActive,
    TextId::SetupFieldNetwork,
    TextId::SetupFieldPassword,
    TextId::SetupFieldAddress,
    TextId::StateValidatingConfiguration,
    TextId::StateSetupFailed,
    TextId::StateRecoveryRequired,
    TextId::RecoveryHoldButton,
    TextId::FactoryResetConfirmTitle,
    TextId::FactoryResetConfirmMessage,
    TextId::FactoryResetProgressTitle,

    // Checkpoint 3c: the Wi-Fi device fault the owner's DriverStalled ruling requires on the panel.
    TextId::StateWifiUnavailable,
    TextId::WifiRestartDisplay,

    // Audit item 5: the two device faults an incomplete factory reset and an unanswerable store
    // need. Four ids for two sentences, deliberately — see the note beside kTextIdCount.
    TextId::StateFactoryResetIncomplete,
    TextId::FactoryResetRestartDisplay,
    TextId::StateStorageUnavailable,
    TextId::StorageRestartDisplay,
    TextId::StatusPrinterUnavailable,
    TextId::ProgressUnavailable,
    TextId::StateConnecting,
    TextId::StateAuthenticationRequired,
    TextId::StateUnsupportedInterface,
    TextId::AccessCheckKey,
    TextId::AccessUnsupported,
    TextId::AccessWarningMark,
    TextId::PrinterError,
    TextId::PrinterAttention,
    TextId::CurrentSubject,
    TextId::CurrentSubjectProgress,
    TextId::CheckPrinterDisplay,
    TextId::LastSubject,
    TextId::LastSubjectProgress,
    TextId::StateReady,

    // 2026-09-12: the pending-image validation fault, which had no composition and no words.
    TextId::StateUpdateFailed,
    TextId::UpdateRestartDisplay,

    // 2026-09-20: the accepted live OTA operation family. Downloading owns the measured
    // value/version/power instruction; the remaining three titles carry the shared dot cadence.
    TextId::StateUpdateDownloading,
    TextId::UpdateKeepPowered,
    TextId::StateUpdateVerifying,
    TextId::StateUpdateInstalling,
    TextId::StateUpdateRestarting,
    TextId::UpdateFirmwareLabel,
    TextId::UpdateAvailableTitle,
    TextId::UpdateHoldForSettings,
    TextId::RemainingMinutes,
    TextId::RemainingHoursMinutes,
};

} // namespace

// --- Completeness -----------------------------------------------------------

TEST(FirmwareUpdateCopyTests, AvailabilityHierarchyNoticeIsExact) {
    EXPECT_EQ(lookup(TextId::UpdateAvailableTitle), "Update available");
    EXPECT_EQ(lookup(TextId::UpdateHoldForSettings), "Hold for settings");
}

// --- T20: the recovery composition's copy must fit its approved roles ------
//
// ⛔ WHY A CHARACTER BUDGET AND NOT A RENDERED WIDTH. There is no glyph metric in this build: the
// host suite links neither LVGL nor the generated faces, so nothing here can sum advances. What it
// CAN do is hold the copy to a width budget derived from two real measurements taken through the
// LVGL host renderer against the committed title face:
//
//   "Saved settings cannot be used"  29 chars -> ~297 px, WRAPPED at 296 px of canvas.content
//   "Settings cannot be used"        23 chars ->  233 px, single line, 63 px spare
//
// 10.25 px/char is the upper of the two ratios, so the bound is conservative for Inter at 20 px.
// The real measurement still happens twice: the scene's runtime overflow guard measures the actual
// face on the device every recovery draw, and the renderer is re-run when this copy changes.
//
// ⬅️ WHAT THIS ACTUALLY PROTECTS is the T20 decision that the recovery heading is ONE line at
// StateTitle. A longer replacement wraps, orphans a word, and trips the guard — which is exactly
// what happened with the previous copy and is why it was shortened.
constexpr double kTitlePxPerChar = 10.25;
constexpr double kContentWidthPx = 296.0;

TEST(RecoveryCopyTests, TheApprovedTitleIsTheOneDecidedAtT20) {
    // Owner/PM decision 2026-08-22. A change here is a change to user-facing product copy and
    // forces a re-measurement, which is the point of pinning it.
    EXPECT_EQ(lookup(TextId::StateRecoveryRequired), "Settings cannot be used");

    // And explicitly not the longer copy that wrapped at StateTitle.
    EXPECT_NE(lookup(TextId::StateRecoveryRequired), "Saved settings cannot be used");
}

TEST(RecoveryCopyTests, TheTitleStaysOnOneLineAtTheProductionContentWidth) {
    const double projected =
        static_cast<double>(lookup(TextId::StateRecoveryRequired).size()) * kTitlePxPerChar;

    EXPECT_LT(projected, kContentWidthPx)
        << "the recovery heading no longer fits one StateTitle line in " << kContentWidthPx
        << " px";
}

TEST(RecoveryCopyTests, TheOldTitleWouldNotHaveFitOneLine) {
    // The negative control, kept as a test rather than performed by hand: the budget above is only
    // meaningful if it rejects the copy that actually wrapped on the panel.
    const double projected =
        static_cast<double>(std::string_view{"Saved settings cannot be used"}.size()) *
        kTitlePxPerChar;

    EXPECT_GT(projected, kContentWidthPx);
}

TEST(RecoveryCopyTests, TheInstructionIsUnchangedAndFitsThreeBodyLines) {
    // Production UI §8 pins the Status Message to Body, and the scene budgets three of those lines.
    // The renderer measures this copy at two lines, so there is one line of margin.
    EXPECT_EQ(lookup(TextId::RecoveryHoldButton),
              "Nothing has been erased. Hold the button to set up again.");

    // Body is 14 px; the same conservative ratio scaled by the size step gives the line estimate.
    constexpr double kBodyPxPerChar = kTitlePxPerChar * 14.0 / 20.0;
    const double lines =
        (static_cast<double>(lookup(TextId::RecoveryHoldButton).size()) * kBodyPxPerChar) /
        kContentWidthPx;

    EXPECT_LT(lines, 3.0) << "the recovery instruction no longer fits its three-Body-line budget";
}

TEST(TextCatalogueTests, EveryIdResolvesToANonEmptyString) {
    for (const auto id : kEveryId) {
        const auto value = lookup(id);
        EXPECT_FALSE(value.empty()) << "id " << static_cast<int>(id) << " resolved to nothing";
    }
}

TEST(TextCatalogueTests, EveryIdReachableByIndexResolves) {
    // Walked by index rather than by the list above, so an id added to the enumeration
    // without being added to kEveryId is still covered.
    for (std::size_t index = 0; index < kTextIdCount; ++index) {
        EXPECT_FALSE(lookup(text_id_at(index)).empty()) << "index " << index;
    }
}

TEST(TextCatalogueTests, TheIdListCoversTheEnumerationExactlyOnce) {
    // A missing id would make the set smaller; a duplicate would too. Together with the
    // index walk above, this is the runtime counterpart of text.cpp's
    // catalogue_is_indexed_by_id() assertion.
    std::set<TextId> distinct;
    for (std::size_t index = 0; index < kTextIdCount; ++index) {
        distinct.insert(text_id_at(index));
    }

    EXPECT_EQ(distinct.size(), kTextIdCount);
}

TEST(TextCatalogueTests, TheOnlySharedStringIsTheOneTheOwnerDECIDEDToShare) {
    // ⬅️ THIS USED TO REQUIRE EVERY ENTRY TO BE UNIQUE, on the grounds that a duplicate is more
    // likely a copy-paste error than a decision. Audit item 5 made one duplicate a decision: four
    // device faults each end with "Restart the display.", and the owner's ruling is that they keep
    // SEPARATE ids so the three causes can be reworded independently. Sharing one id would mean a
    // change written for the Wi-Fi fault silently rewording the reset and storage ones.
    //
    // ⛔ SO THE PROTECTION IS NARROWED, NOT DROPPED. Exactly one string may be shared, it must be
    // that instruction, and it must be shared by exactly those three ids. Any other duplicate — and
    // any fourth sharer of this one — still fails, which is the copy-paste case the original test
    // was written for.
    std::map<std::string_view, std::vector<TextId>> by_string;
    for (const auto id : kEveryId) {
        by_string[lookup(id)].push_back(id);
    }

    std::vector<std::string_view> shared;
    for (const auto& [word, ids] : by_string) {
        if (ids.size() > 1) {
            shared.push_back(word);
        }
    }

    ASSERT_EQ(shared.size(), 1U) << "a catalogue entry was duplicated without a decision behind it";
    EXPECT_EQ(shared.front(), "Restart the display.");

    const std::vector<TextId> expected{TextId::WifiRestartDisplay,
                                       TextId::FactoryResetRestartDisplay,
                                       TextId::StorageRestartDisplay, TextId::UpdateRestartDisplay};
    std::vector<TextId> actual = by_string.at("Restart the display.");
    std::sort(actual.begin(), actual.end());
    std::vector<TextId> wanted = expected;
    std::sort(wanted.begin(), wanted.end());

    EXPECT_EQ(actual, wanted) << "the shared instruction is shared by a different set of ids";

    // And every other entry is still unique — the property the original test protected.
    EXPECT_EQ(by_string.size(), kTextIdCount - 3) << "four ids share one string, so the catalogue "
                                                     "holds three fewer distinct strings than ids";
}

TEST(TextCatalogueTests, AnIdOutsideTheCatalogueResolvesToNothingRatherThanReadingPastIt) {
    // Cannot happen while the enumeration and the catalogue agree, and the assertions in
    // text.cpp guarantee they do. Pinned anyway because the alternative in the render
    // path is an out-of-bounds read.
    EXPECT_TRUE(lookup(static_cast<TextId>(kTextIdCount)).empty());
    EXPECT_TRUE(lookup(static_cast<TextId>(200)).empty());
}

// --- Glyph coverage ---------------------------------------------------------

TEST(TextCoverageTests, EveryCatalogueEntryIsRenderableByTheGeneratedFaces) {
    // The subset is printable ASCII plus U+00B0 (tools/generate-fonts.py). An entry
    // outside it would lose characters on the panel with no warning.
    for (const auto id : kEveryId) {
        EXPECT_TRUE(is_renderable(lookup(id)))
            << "entry '" << lookup(id) << "' uses a codepoint the Inter subset lacks";
    }
}

TEST(TextCoverageTests, TheRenderabilityCheckAcceptsTheWholeSupportedSubset) {
    // Both ends of printable ASCII, and the degree sign. If this were vacuously true the
    // test above would pass for anything.
    EXPECT_TRUE(is_renderable(" "));
    EXPECT_TRUE(is_renderable("~"));
    EXPECT_TRUE(is_renderable("Printing 100% 1:42 -5\xC2\xB0"));
    EXPECT_TRUE(is_renderable(""));
}

TEST(TextCoverageTests, TheRenderabilityCheckRejectsGlyphsTheSubsetLacks) {
    // The realistic failures: an accented Latin letter a Czech or German catalogue would
    // need, a typographic ellipsis, a non-breaking space, and a bare continuation byte.
    EXPECT_FALSE(is_renderable("Tisk\xC3\xAD")); // "Tiskí" — U+00ED
    EXPECT_FALSE(is_renderable("\xE2\x80\xA6")); // U+2026 HORIZONTAL ELLIPSIS
    EXPECT_FALSE(is_renderable("\xC2\xA0"));     // U+00A0 NO-BREAK SPACE
    EXPECT_FALSE(is_renderable("\xC2"));         // truncated two-byte sequence
    EXPECT_FALSE(is_renderable("\n"));           // control character
}

// --- Composition bindings ---------------------------------------------------

TEST(SceneTextTests, ThePrintingSceneRequestsItsTitleByIdAndItResolvesToTheArtworkWord) {
    // The structural half of "the scene does not own the literal": the binding is a
    // TextId, and it is the catalogue that holds the word. The other half — that no
    // literal remains in the scene source — is a build check, because it is a property of
    // the source rather than of a value.
    EXPECT_EQ(kPrintingStateTitle, TextId::StatePrinting);
    EXPECT_EQ(lookup(kPrintingStateTitle), "Printing");
}

TEST(SceneTextTests, ThePrintingSceneRequestsItsPercentUnitById) {
    EXPECT_EQ(kPrintingPercentUnit, TextId::UnitPercent);
    EXPECT_EQ(lookup(kPrintingPercentUnit), "%");
}

TEST(SceneTextTests, EveryIdThePrintingSceneUsesResolves) {
    for (const auto id : kPrintingTextIds) {
        EXPECT_FALSE(lookup(id).empty());
        EXPECT_TRUE(is_renderable(lookup(id)));
    }
}

TEST(SceneTextTests, TheStateTitleIsNotAFormattedValue) {
    // State titles remain literal. Only the formatter fills the explicitly accepted
    // age, subject-context and remaining-time templates.
    for (const auto id : kEveryId) {
        if (id == TextId::StaleIndicator) {
            EXPECT_EQ(lookup(id), "Reconnecting: %lu s old");
            continue;
        }
        if (id == TextId::CurrentSubject || id == TextId::CurrentSubjectProgress) {
            EXPECT_EQ(lookup(id), id == TextId::CurrentSubject ? "%s" : "%s / %s%%");
            continue;
        }
        if (id == TextId::LastSubject || id == TextId::LastSubjectProgress) {
            EXPECT_EQ(lookup(id), id == TextId::LastSubject ? "Last: %s" : "Last: %s / %s%%");
            continue;
        }
        if (id == TextId::RemainingMinutes || id == TextId::RemainingHoursMinutes) {
            EXPECT_EQ(lookup(id),
                      id == TextId::RemainingMinutes ? "%s%lum left" : "%s%luh%02lum left");
            continue;
        }
        EXPECT_EQ(lookup(id).find('%'), lookup(id) == "%" ? 0U : std::string_view::npos)
            << "entry '" << lookup(id) << "' looks like a format string";
    }
}

TEST(SceneTextTests, EveryTelemetrySlotHasADistinctResolvingLabel) {
    const TelemetrySlot slots[] = {TelemetrySlot::Nozzle, TelemetrySlot::Bed, TelemetrySlot::Layer};

    std::set<std::string_view> distinct;
    for (const auto slot : slots) {
        const auto value = lookup(label_for(slot));
        EXPECT_FALSE(value.empty());
        EXPECT_TRUE(is_renderable(value));
        distinct.insert(value);
    }

    EXPECT_EQ(distinct.size(), prd::ui::component::kTelemetrySlotCount)
        << "two telemetry rows would be labelled identically";
}

TEST(SceneTextTests, TheTelemetryLabelsAreBoundInSlotOrder) {
    // A reordering would mislabel a row — bed temperature under "Nozzle" — which is worse
    // than a missing label because it is plausible. Also asserted at compile time in
    // scene_text.hpp; here so the resolved words are checked too.
    EXPECT_EQ(lookup(label_for(TelemetrySlot::Nozzle)), "Nozzle");
    EXPECT_EQ(lookup(label_for(TelemetrySlot::Bed)), "Bed");
    EXPECT_EQ(lookup(label_for(TelemetrySlot::Layer)), "Layer");
}

// --- The provisioning scene's words (Milestone 3.6) -------------------------

TEST(SceneTextTests, EveryIdTheProvisioningSceneUsesResolvesAndIsRenderable) {
    // The scene has no other way to put a word on screen, and an entry outside the
    // generated subset would reach the panel with letters missing rather than failing.
    for (const auto id : prd::ui::text::kProvisioningTextIds) {
        const auto value = lookup(id);
        EXPECT_FALSE(value.empty());
        EXPECT_TRUE(is_renderable(value));
    }
}

TEST(SceneTextTests, TheTwoProvisioningTitlesAreDistinct) {
    // The titles are the only thing separating "the device needs setting up" from "the
    // network is up, join it". Identical words would make the two states indistinguishable
    // at exactly the moment the reader has to act.
    EXPECT_NE(lookup(prd::ui::text::kSetupRequiredTitle), lookup(prd::ui::text::kSetupActiveTitle));
}

TEST(SceneTextTests, TheProvisioningFieldLabelsAreDistinct) {
    std::set<std::string_view> distinct;
    for (const auto id : prd::ui::text::kSetupFieldLabels) {
        distinct.insert(lookup(id));
    }

    EXPECT_EQ(distinct.size(), prd::ui::text::kSetupFieldLabels.size())
        << "two provisioning rows would be labelled identically";
}

TEST(SceneTextTests, TheProvisioningLabelsAreBoundInRowOrder) {
    // A reordering would put the password under "Network", which is the plausible kind of
    // wrong: the reader types the SSID into the password field and the join fails with no
    // explanation.
    EXPECT_EQ(lookup(prd::ui::text::kSetupFieldLabels[0]), "Network");
    EXPECT_EQ(lookup(prd::ui::text::kSetupFieldLabels[1]), "Password");
    EXPECT_EQ(lookup(prd::ui::text::kSetupFieldLabels[2]), "Address");
}

TEST(SceneTextTests, TheProvisioningWordsNameNoActionTheProductCannotPerform) {
    // Architecture Spec invariant 18 and §2.2's read-only MVP. "Join this network" names an
    // action the READER performs on their own phone, which is why it is admissible where
    // "Resume print" was not — the product is not claiming it can do it.
    //
    // Guarded against the specific regression: a future edit that turns the title into an
    // instruction to the machine.
    for (const auto id : prd::ui::text::kProvisioningTextIds) {
        const auto value = lookup(id);
        EXPECT_EQ(value.find("Resume"), std::string_view::npos);
        EXPECT_EQ(value.find("Start"), std::string_view::npos);
        EXPECT_EQ(value.find("Stop"), std::string_view::npos);
    }
}

// --- Checkpoint 3c: the Wi-Fi Device Fault's copy fits the same approved roles ---
//
// ⛔ IT SHARES THE COMPOSITION, SO IT SHARES THE BUDGET. draw_statement() draws this pair at exactly
// the geometry recovery uses — StateTitle heading on one line, Status Message in at most three Body
// lines — so the same two measured ratios bound it. The scene's runtime guard measures the real face
// on the device on every draw; this is what stops a copy change reaching that guard by surprise.

TEST(WifiDeviceFaultCopyTests, TheCopyIsExactlyWhatTheOwnerRuled) {
    // ⛔ PINNED VERBATIM. The owner's DriverStalled ruling supplies both lines, and both halves are
    // load-bearing: the title names the condition without blaming the user's network, and the
    // message names the only action that works. A change here is a change to a product decision.
    EXPECT_EQ(lookup(TextId::StateWifiUnavailable), "Wi-Fi unavailable");
    EXPECT_EQ(lookup(TextId::WifiRestartDisplay), "Restart the display.");
}

TEST(WifiDeviceFaultCopyTests, TheTitleStaysOnOneLineAtTheProductionContentWidth) {
    const double projected =
        static_cast<double>(lookup(TextId::StateWifiUnavailable).size()) * kTitlePxPerChar;

    EXPECT_LT(projected, kContentWidthPx)
        << "the fault heading no longer fits one StateTitle line in " << kContentWidthPx << " px";
}

TEST(WifiDeviceFaultCopyTests, TheInstructionFitsInsideTheThreeBodyLinesTheSceneBudgets) {
    constexpr double kBodyPxPerChar = kTitlePxPerChar * 14.0 / 20.0;
    constexpr double kBudgetLines = 3.0;

    const double lines =
        (static_cast<double>(lookup(TextId::WifiRestartDisplay).size()) * kBodyPxPerChar) /
        kContentWidthPx;

    EXPECT_LT(lines, kBudgetLines)
        << "the Status Message would overflow the three Body lines the composition budgets";
}

TEST(WifiDeviceFaultCopyTests, TheCopySaysNothingItCannotHonour) {
    // ⛔ IT MUST NOT INVITE A RETRY, BLAME THE CREDENTIAL, OR CLAIM A REPAIR. The station is
    // quarantined: it issues no further driver operation until a restart, so "try again" would be
    // false, and the network and the password were never tested, so naming either would send the
    // user to fix something that is very likely fine.
    const std::string title{lookup(TextId::StateWifiUnavailable)};
    const std::string message{lookup(TextId::WifiRestartDisplay)};
    const std::string both = title + " " + message;

    for (const auto* const forbidden :
         {"try again", "password", "network name", "again later", "erased", "settings"}) {
        EXPECT_EQ(both.find(forbidden), std::string::npos)
            << "the fault copy must not say \"" << forbidden << "\"";
    }

    // And it does name the one action that works.
    EXPECT_NE(message.find("Restart"), std::string::npos);
}

TEST(WifiDeviceFaultCopyNegativeControlTests, TheOneLineBudgetCanReject) {
    // The width bound is only meaningful if something plausible exceeds it. A longer, natural
    // alternative for this title does — which is why the ruled copy is the short one.
    const double projected =
        static_cast<double>(std::string_view{"The display cannot reach Wi-Fi"}.size()) *
        kTitlePxPerChar;

    EXPECT_GT(projected, kContentWidthPx);
}

// --- Audit item 5's device-fault copy ----------------------------------------

TEST(ResetFaultCopyTests, BothTitlesAndBothMessagesArePinnedVerbatim) {
    // ⛔ THE OWNER SUPPLIES ALL FOUR LINES. A change here is a change to a product decision.
    EXPECT_EQ(lookup(TextId::StateFactoryResetIncomplete), "Reset could not finish");
    EXPECT_EQ(lookup(TextId::FactoryResetRestartDisplay), "Restart the display.");
    EXPECT_EQ(lookup(TextId::StateStorageUnavailable), "Storage unavailable");
    EXPECT_EQ(lookup(TextId::StorageRestartDisplay), "Restart the display.");
}

TEST(ResetFaultCopyTests, THE_TWO_FAULTS_HAVE_SEPARATE_IDS_EVEN_WHERE_THE_BYTES_MATCH) {
    // ⛔ WHY THIS IS A TEST AND NOT A COMMENT. Three ids currently carry "Restart the display.".
    // Sharing one would mean a rewording written for one fault silently rewording the others — the
    // "a sentence is a claim about every route that emits it" rule, applied to the catalogue.
    EXPECT_NE(TextId::FactoryResetRestartDisplay, TextId::StorageRestartDisplay);
    EXPECT_NE(TextId::FactoryResetRestartDisplay, TextId::WifiRestartDisplay);
    EXPECT_NE(TextId::StorageRestartDisplay, TextId::WifiRestartDisplay);
    EXPECT_NE(TextId::StateFactoryResetIncomplete, TextId::StateStorageUnavailable);
}

TEST(ResetFaultCopyTests, NEITHER_TITLE_MAKES_A_PERSISTENCE_CLAIM_IN_EITHER_DIRECTION) {
    // ⛔ THE BOUNDED-CLAIM RULE. The incomplete-reset route may have removed records, so the copy
    // cannot say nothing was erased; it also cannot promise the old or new settings are safe,
    // because neither is established. It says only what happened.
    const std::string reset = std::string{lookup(TextId::StateFactoryResetIncomplete)} + " " +
                              std::string{lookup(TextId::FactoryResetRestartDisplay)};
    const std::string storage = std::string{lookup(TextId::StateStorageUnavailable)} + " " +
                                std::string{lookup(TextId::StorageRestartDisplay)};

    for (const auto* const forbidden : {"Nothing has been erased", "nothing was erased", "safe",
                                        "saved", "unchanged", "try again", "will be", "password"}) {
        EXPECT_EQ(reset.find(forbidden), std::string::npos)
            << "the incomplete-reset copy must not say \"" << forbidden << "\"";
        EXPECT_EQ(storage.find(forbidden), std::string::npos)
            << "the storage-fault copy must not say \"" << forbidden << "\"";
    }
}

TEST(ResetFaultCopyTests, THE_STORAGE_FAULT_DOES_NOT_CLAIM_A_RESET_HAPPENED) {
    // ⛔ THE ROUTE THAT FORCED TWO SENTENCES. An unreadable or invalid journal reaches this fault on
    // a device where NO reset is proved to exist, so "Reset could not finish" would assert an
    // operation the user may never have requested.
    const std::string storage = std::string{lookup(TextId::StateStorageUnavailable)} + " " +
                                std::string{lookup(TextId::StorageRestartDisplay)};

    for (const auto* const forbidden : {"Reset", "reset", "erase", "Erase"}) {
        EXPECT_EQ(storage.find(forbidden), std::string::npos);
    }

    // And each names the one action that works.
    EXPECT_NE(std::string{lookup(TextId::StorageRestartDisplay)}.find("Restart"),
              std::string::npos);
    EXPECT_NE(std::string{lookup(TextId::FactoryResetRestartDisplay)}.find("Restart"),
              std::string::npos);
}

TEST(ResetFaultCopyTests, BothTitlesStayOnOneStateTitleLine) {
    for (const TextId title :
         {TextId::StateFactoryResetIncomplete, TextId::StateStorageUnavailable}) {
        const double projected = static_cast<double>(lookup(title).size()) * kTitlePxPerChar;

        EXPECT_LT(projected, kContentWidthPx)
            << "\"" << lookup(title) << "\" no longer fits one StateTitle line";
    }
}

TEST(ResetFaultCopyNegativeControlTests, TheOneLineBudgetCanRejectTheseTitlesToo) {
    // The bound is only meaningful if a plausible longer alternative exceeds it.
    for (const auto* const longer :
         {"The factory reset could not be completed", "The display cannot read its storage"}) {
        const double projected =
            static_cast<double>(std::string_view{longer}.size()) * kTitlePxPerChar;

        EXPECT_GT(projected, kContentWidthPx) << longer;
    }
}

// --- The lost-observation status message (Production UI §9) -----------------

TEST(TextTests, ThePrinterUnavailableStatusMessageIsExactAndRenderable) {
    const auto message = lookup(TextId::StatusPrinterUnavailable);

    EXPECT_EQ(message, "Printer unavailable");
    EXPECT_TRUE(is_renderable(message)) << "every glyph must exist in the generated subset";
}

// ⛔ IT ASSERTS NO CAUSE — §9 forbids stating or implying offline, powered off, disconnected,
// network-isolated, unreachable or failed. The earlier "Printer offline" draft failed exactly here.
TEST(TextTests, ThePrinterUnavailableMessageNamesNoCauseAndNoAction) {
    const auto message = lookup(TextId::StatusPrinterUnavailable);

    for (const std::string_view forbidden :
         {"offline", "off", "disconnect", "unreachable", "failed", "network", "power", "check",
          "restart", "retry", "try"}) {
        EXPECT_EQ(message.find(forbidden), std::string_view::npos)
            << "the status message must assert no cause and name no action: " << forbidden;
    }
}
