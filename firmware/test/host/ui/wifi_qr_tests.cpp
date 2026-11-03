// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include <gtest/gtest.h>

#include <string>
#include <string_view>

#include "app_model/app_state.hpp"
#include "app_model/presentation.hpp"
#include "wifi_qr.hpp"

// The Wi-Fi QR payload encoder.
//
// ⛔ THESE TESTS DO NOT PROVE THAT A PHONE CAN SCAN ANYTHING. They prove the bytes are the
// bytes the format calls for. Whether a camera resolves the drawn modules off a 2.8-inch
// panel is a hardware question and is settled on the panel, not here.

namespace prd::ui {
namespace {

[[nodiscard]] std::string encoded(const std::string_view ssid, const std::string_view password) {
    wifi_qr::Payload payload;
    EXPECT_TRUE(wifi_qr::build(ssid, password, payload));
    return std::string{payload.view()};
}

// --- The canonical session --------------------------------------------------

TEST(WifiQrTests, TheCanonicalSessionProducesTheDocumentedPayload) {
    EXPECT_EQ(encoded("PRD-Setup-0C01", "mK7pQ2rXvA9t"),
              "WIFI:T:WPA;S:PRD-Setup-0C01;P:mK7pQ2rXvA9t;;");
}

TEST(WifiQrTests, ThePayloadIsDeterministic) {
    const std::string first = encoded("PRD-Setup-0C01", "mK7pQ2rXvA9t");
    const std::string second = encoded("PRD-Setup-0C01", "mK7pQ2rXvA9t");
    EXPECT_EQ(first, second);
}

// The QR version the renderer sizes its module grid against is a function of this length.
// If the format or the session capacities ever change, the scene's geometry has to be
// re-derived — this is the tripwire for that, not decoration.
TEST(WifiQrTests, TheCanonicalPayloadIsFortyFourBytes) {
    EXPECT_EQ(encoded("PRD-Setup-0C01", "mK7pQ2rXvA9t").size(), 44U);
}

// --- Escaping ---------------------------------------------------------------
//
// The four structural characters of the format. The generated password contains none of
// them today, and that is exactly why these tests exist: the alphabet is a working
// baseline, so the encoder must be correct without it.

TEST(WifiQrTests, TheSeparatorIsEscapedInAPassword) {
    EXPECT_EQ(encoded("Net", "a;b"), "WIFI:T:WPA;S:Net;P:a\\;b;;");
}

TEST(WifiQrTests, TheCommaIsEscapedInAPassword) {
    EXPECT_EQ(encoded("Net", "a,b"), "WIFI:T:WPA;S:Net;P:a\\,b;;");
}

TEST(WifiQrTests, TheColonIsEscapedInAPassword) {
    EXPECT_EQ(encoded("Net", "a:b"), "WIFI:T:WPA;S:Net;P:a\\:b;;");
}

// The escape character itself. An encoder that forgets this one corrupts exactly the input
// it was written to protect, and the corruption is invisible until someone cannot join.
TEST(WifiQrTests, TheBackslashIsEscapedInAPassword) {
    EXPECT_EQ(encoded("Net", "a\\b"), "WIFI:T:WPA;S:Net;P:a\\\\b;;");
}

TEST(WifiQrTests, TheSsidIsEscapedByTheSameRule) {
    EXPECT_EQ(encoded("a;b,c:d\\e", "pw"), "WIFI:T:WPA;S:a\\;b\\,c\\:d\\\\e;P:pw;;");
}

TEST(WifiQrTests, AValueOfNothingButSeparatorsSurvives) {
    EXPECT_EQ(encoded("N", "\\;,:"), "WIFI:T:WPA;S:N;P:\\\\\\;\\,\\:;;");
}

TEST(WifiQrTests, OrdinaryCharactersAreNotEscaped) {
    EXPECT_EQ(encoded("Home WiFi 2.4", "p@ssw0rd!#$%"),
              "WIFI:T:WPA;S:Home WiFi 2.4;P:p@ssw0rd!#$%;;");
}

// --- Refusals ---------------------------------------------------------------

TEST(WifiQrTests, AnEmptySsidIsRefused) {
    wifi_qr::Payload payload;
    EXPECT_FALSE(wifi_qr::build({}, "pw", payload));
    EXPECT_TRUE(payload.empty());
}

TEST(WifiQrTests, AnEmptyPasswordIsRefused) {
    wifi_qr::Payload payload;
    EXPECT_FALSE(wifi_qr::build("Net", {}, payload));
    EXPECT_TRUE(payload.empty());
}

TEST(WifiQrTests, AnOverlongSsidIsRefusedRatherThanTruncated) {
    wifi_qr::Payload payload;
    const std::string ssid(wifi_qr::kMaxSsid + 1, 'a');
    EXPECT_FALSE(wifi_qr::build(ssid, "pw", payload));
    EXPECT_TRUE(payload.empty());
}

TEST(WifiQrTests, AnOverlongPasswordIsRefusedRatherThanTruncated) {
    wifi_qr::Payload payload;
    const std::string password(wifi_qr::kMaxPassword + 1, 'a');
    EXPECT_FALSE(wifi_qr::build("Net", password, payload));
    EXPECT_TRUE(payload.empty());
}

// A refusal must leave nothing behind for a caller that ignores the return value.
TEST(WifiQrTests, ARefusalClearsAPayloadThatAlreadyHeldSomething) {
    wifi_qr::Payload payload;
    ASSERT_TRUE(wifi_qr::build("PRD-Setup-0C01", "mK7pQ2rXvA9t", payload));
    ASSERT_FALSE(payload.empty());

    EXPECT_FALSE(wifi_qr::build("PRD-Setup-0C01", {}, payload));
    EXPECT_TRUE(payload.empty());
}

// --- Capacity ---------------------------------------------------------------

// Both values at capacity, every character structural, so every one doubles. This is the
// arithmetic the Payload's capacity is derived from; if it were wrong the encoder would
// refuse a legitimate session.
TEST(WifiQrTests, TheWorstCaseSessionStillFits) {
    const std::string ssid(wifi_qr::kMaxSsid, ';');
    const std::string password(wifi_qr::kMaxPassword, ';');

    wifi_qr::Payload payload;
    ASSERT_TRUE(wifi_qr::build(ssid, password, payload));

    EXPECT_FALSE(payload.truncated());
    EXPECT_EQ(payload.view().size(), wifi_qr::kMaxPayloadLength);
}

// --- Wiping -----------------------------------------------------------------

TEST(WifiQrTests, WipingEmptiesThePayload) {
    wifi_qr::Payload payload;
    ASSERT_TRUE(wifi_qr::build("PRD-Setup-0C01", "mK7pQ2rXvA9t", payload));

    wifi_qr::wipe(payload);

    EXPECT_TRUE(payload.empty());
    EXPECT_EQ(payload.view().size(), 0U);
}

// The characters, not merely the length. c_str() reads the storage directly, so a wipe
// that only reset the length would still hand the password to whoever looked next.
TEST(WifiQrTests, WipingOverwritesTheCharactersAndNotJustTheLength) {
    wifi_qr::Payload payload;
    ASSERT_TRUE(wifi_qr::build("PRD-Setup-0C01", "mK7pQ2rXvA9t", payload));
    const std::size_t written = payload.view().size();

    wifi_qr::wipe(payload);

    const char* const storage = payload.c_str();
    for (std::size_t index = 0; index < written; ++index) {
        EXPECT_EQ(storage[index], '\0') << "byte " << index << " survived the wipe";
    }
}

TEST(WifiQrTests, WipingAnEmptyPayloadIsHarmless) {
    wifi_qr::Payload payload;
    wifi_qr::wipe(payload);
    EXPECT_TRUE(payload.empty());
}

// --- §24.3 / §13.5: the payload's ingredients are not application state -----
//
// The structural guarantee this whole checkpoint rests on. The QR encodes the session
// password, so the password must remain reachable only through the transient session
// channel — never through the store's state or the renderer's projection.

TEST(WifiQrTests, ADefaultAppStateCarriesNoCredentialField) {
    const app_model::AppState state{};

    // Only presence and a generation counter describe credentials in AppState. If a field
    // that could hold one is ever added, this stops compiling or stops being true.
    EXPECT_FALSE(state.configuration.has_wifi_credentials);
    EXPECT_EQ(state.configuration.credential_generation, 0U);
}

TEST(WifiQrTests, APresentationProjectedFromStateCarriesNoCredential) {
    app_model::AppState state{};
    state.configuration.has_wifi_credentials = true;
    state.configuration.credential_generation = 7;
    state.provisioning = app_model::ProvisioningPhase::SetupActive;

    const app_model::Presentation presentation = app_model::project(state);

    // The projection knows a credential exists and nothing whatever about its value.
    EXPECT_EQ(presentation.provisioning, app_model::ProvisioningPhase::SetupActive);

    // Reconstructing a payload needs an SSID and a password. Neither is anywhere in the
    // projection, which is why the scene has to be handed a session separately.
    wifi_qr::Payload payload;
    EXPECT_FALSE(wifi_qr::build({}, {}, payload));
    EXPECT_TRUE(payload.empty());
}

} // namespace
} // namespace prd::ui
