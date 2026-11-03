// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include <fstream>
#include <sstream>
#include <string>

#include <gtest/gtest.h>

// The served provisioning page, asserted as a product surface.
//
// WHY THESE ARE STATIC ASSERTIONS AND WHAT THAT DOES AND DOES NOT BUY. There is no JavaScript
// engine in this build and adding one would be a new dependency and a new test category, so
// these tests read the three real embedded assets and assert the guarantees that failed
// physical acceptance. They prove the page CODE still contains each correction; they do not
// execute it. The behavioural check is a browser run against the real page, recorded in the
// handoff — this file is what stops a silent regression between those runs.
//
// The assets are product surface in their entirety (the M3.7 lesson: an HTML comment ships),
// so reading them here is reading exactly what the browser receives.

namespace prd::provisioning {
namespace {

[[nodiscard]] std::string asset(const std::string& name) {
    const std::string path = std::string{PRD_PROVISIONING_ASSETS_DIR} + "/" + name;
    std::ifstream file{path};
    EXPECT_TRUE(file.is_open()) << "cannot open served asset: " << path;

    std::ostringstream contents;
    contents << file.rdbuf();
    return contents.str();
}

[[nodiscard]] bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

// Where `needle` first appears, for ordering assertions.
[[nodiscard]] std::size_t at(const std::string& haystack, const std::string& needle) {
    const std::size_t found = haystack.find(needle);
    EXPECT_NE(found, std::string::npos) << "expected to find: " << needle;
    return found;
}

} // namespace

// --- B2: a validation error may never wear success treatment ---------------

TEST(ServedPageTests, TheErrorSummaryHasItsOwnSemanticClass) {
    const std::string css = asset("style.css");

    EXPECT_TRUE(contains(css, "--error:")) << "the error hue must be a defined token";
    EXPECT_TRUE(contains(css, ".summary.bad"));
    EXPECT_TRUE(contains(css, "border-left-color: var(--error)"));
}

TEST(ServedPageTests, ASuccessSummaryCannotRetainSuccessStylingWhenItBecomesAnError) {
    const std::string js = asset("app.js");

    // The mechanism that failed: enterPrinterStep() styles this element as a success, and
    // applyReport() used to write only textContent. It must ASSIGN the class.
    EXPECT_TRUE(contains(js, "summary.className = \"summary bad\""))
        << "applyReport must assign the error class, not merely set the text";

    // And the success assignment must still be an assignment too, so exactly one of the two
    // is ever in effect.
    EXPECT_TRUE(contains(js, "summary.className = \"summary good\""));
}

TEST(ServedPageTests, TheErrorRuleIsDeclaredAfterTheSuccessRuleSoErrorWins) {
    // Defence in depth for the same defect: equal specificity means source order decides, so
    // even a future edit that forgets to reset the class still renders an error as an error.
    const std::string css = asset("style.css");

    EXPECT_LT(at(css, ".summary.good"), at(css, ".summary.bad"));
}

TEST(ServedPageTests, ValidationErrorsUseTheErrorTokenRatherThanTheAccent) {
    const std::string css = asset("style.css");

    // A marked field and its message both carry the error hue. The accent is this page's
    // ordinary emphasis — the submit button wears it — so it cannot also mean "wrong".
    const std::size_t invalid = at(css, "input.invalid");
    const std::string after = css.substr(invalid, 120);
    EXPECT_TRUE(contains(after, "var(--error)")) << "input.invalid must use the error token";
    EXPECT_FALSE(contains(after, "var(--accent)"));

    const std::size_t error_text = at(css, "\n.error {");
    EXPECT_TRUE(contains(css.substr(error_text, 120), "color: var(--error)"));
}

TEST(ServedPageTests, SuccessMessagesKeepSuccessTreatment) {
    const std::string css = asset("style.css");

    EXPECT_TRUE(contains(css, ".summary.good"));
    EXPECT_TRUE(contains(css, "border-left-color: var(--success)"));
    EXPECT_TRUE(contains(css, "border-left: 4px solid var(--success)")) << "the done panel";
}

TEST(ServedPageTests, ThePaletteIntroducesNoColourOutsideTheSpecification) {
    // Production UI Specification §5, transcribed. Adding a hue here would be inventing a
    // token; every value below is one the specification already names.
    const std::string css = asset("style.css");

    EXPECT_TRUE(contains(css, "--error: #e06262"));
    EXPECT_TRUE(contains(css, "--success: #5cb85c"));
    EXPECT_TRUE(contains(css, "--accent: #f77f2f"));
}

// --- B3: the reveal affordance ---------------------------------------------

TEST(ServedPageTests, ThePasswordIsConcealedByDefault) {
    const std::string html = asset("index.html");

    EXPECT_TRUE(contains(html, "id=\"wifiPassword\" name=\"wifiPassword\" type=\"password\""));
    EXPECT_TRUE(contains(html, "aria-pressed=\"false\""));
    EXPECT_TRUE(contains(html, "aria-label=\"Show password\""));
}

TEST(ServedPageTests, TheRevealIsATrailingAffordanceInsideTheField) {
    const std::string css = asset("style.css");

    // Positioned within the field rather than laid out beside it, which is what physical
    // review rejected.
    const std::size_t toggle = at(css, ".reveal-toggle {");
    const std::string rule = css.substr(toggle, 320);
    EXPECT_TRUE(contains(rule, "position: absolute"));
    EXPECT_TRUE(contains(rule, "right: 0"));

    EXPECT_TRUE(contains(css.substr(at(css, ".reveal {"), 120), "position: relative"));

    // Room reserved so a long password never runs under the affordance.
    EXPECT_TRUE(contains(css, "padding-right: 48px"));
}

TEST(ServedPageTests, TheRevealCarriesBothGlyphsAndSwapsThemFromTheAriaState) {
    const std::string html = asset("index.html");
    const std::string css = asset("style.css");
    const std::string js = asset("app.js");

    // Both icons ship in the markup, and they are decorative — the accessible name is on the
    // button.
    EXPECT_TRUE(contains(html, "class=\"reveal-eye\""));
    EXPECT_TRUE(contains(html, "class=\"reveal-eye-off\""));
    EXPECT_TRUE(contains(html, "aria-hidden=\"true\""));

    // ⬅️ WHY NOT THE `hidden` ATTRIBUTE. Found by driving the real page: SVGElement does not
    // implement the `hidden` IDL property, so `svg.hidden = true` set a JS expando that no
    // selector could see and BOTH eyes drew at once. The visible glyph is therefore derived
    // in CSS from the ARIA state the button already carries.
    EXPECT_TRUE(contains(css, ".reveal-toggle[aria-pressed=\"false\"] .reveal-eye-off"));
    EXPECT_TRUE(contains(css, ".reveal-toggle[aria-pressed=\"true\"] .reveal-eye"));
    EXPECT_FALSE(contains(js, "revealEyeOff")) << "no second source of truth for the glyph";

    // ONE implementation for both fields. A second reveal design would drift from the one that
    // is hardware-accepted.
    EXPECT_TRUE(contains(js, "wireReveal(\"wifiPasswordReveal\", \"wifiPassword\""));
    EXPECT_TRUE(contains(js, "wireReveal(\"printerApiKeyReveal\", \"printerApiKey\""));
    EXPECT_TRUE(contains(html, "id=\"printerApiKeyReveal\""));
    EXPECT_TRUE(contains(html, "aria-label=\"Show API key\""));

    // ⬅️ THE DOT MATTERS. A bare "innerHTML" also appears in the comments that FORBID it, so
    // asserting on the bare word made this test match the prose rather than the code — the
    // same shape as the secret scan that rejected its own redaction. Real use is always a
    // property access on a node.
    EXPECT_FALSE(contains(js, ".innerHTML"));
}

TEST(ServedPageTests, TheRevealKeepsItsAccessibleStateAndIsKeyboardReachable) {
    const std::string js = asset("app.js");
    const std::string css = asset("style.css");

    EXPECT_TRUE(contains(js, "aria-pressed"));
    EXPECT_TRUE(contains(js, "\"Hide \""));
    EXPECT_TRUE(contains(js, "\"Show \""));

    // A real button, and focus is visible on it.
    EXPECT_TRUE(contains(asset("index.html"), "type=\"button\" class=\"reveal-toggle\""));
    EXPECT_TRUE(contains(css, ".reveal-toggle:focus-visible"));
}

TEST(ServedPageTests, TheRevealTouchesOnlyTheInputType) {
    const std::string js = asset("app.js");

    // Presentation only: it flips the type and never reads, copies or re-sends the value.
    const std::size_t handler = at(js, "function wireReveal(");
    const std::string body = js.substr(handler, 900);

    EXPECT_TRUE(contains(body, "input.type = willShow ? \"text\" : \"password\""));
    EXPECT_FALSE(contains(body, "post(")) << "the reveal must never resubmit";
    EXPECT_FALSE(contains(body, "fetch(")) << "the reveal must never reach the network";
    EXPECT_FALSE(contains(body, "input.value")) << "the reveal must never read the credential";
}

// --- B1: the page submits what the grammar accepts -------------------------

TEST(ServedPageTests, ThePrinterFieldsAreSubmittedWithSurroundingWhitespaceRemoved) {
    const std::string js = asset("app.js");

    EXPECT_TRUE(contains(js, "printerHost: normalised(\"printerHost\")"));
    EXPECT_TRUE(contains(js, "printerPort: normalised(\"printerPort\")"));
    EXPECT_TRUE(contains(js, "printerApiKey: normalised(\"printerApiKey\")"));
    EXPECT_TRUE(contains(js, "input.value.trim()"));
}

TEST(ServedPageTests, NoPrinterFieldCarriesAPlaceholderThatLooksLikeData) {
    const std::string html = asset("index.html");

    // The defect: a grey "192.168.1.50" and "80" in empty fields read as configuration already
    // present, and Save then reported both as missing. Behaviour is covered by the browser
    // suite; this is the cheap net that stops the attribute coming back.
    EXPECT_FALSE(contains(html, "placeholder=\"192.168.1.50\""));
    EXPECT_FALSE(contains(html, "placeholder=\"80\""));
}

TEST(ServedPageTests, PortIsARealValueAndAddressCarriesAHint) {
    const std::string html = asset("index.html");

    EXPECT_TRUE(contains(html, "value=\"80\"")) << "port is an actual editable value";
    EXPECT_TRUE(contains(html, "id=\"printerHostHint\""));
    EXPECT_TRUE(contains(html, "192.168.1.50")) << "the example survives, as a hint";

    // The hint is wired to the field for assistive technology, alongside its error.
    EXPECT_TRUE(contains(html, "aria-describedby=\"printerHostError printerHostHint\""));
}

TEST(ServedPageTests, ASubmissionInFlightCannotBeDuplicated) {
    const std::string js = asset("app.js");

    // Found by the browser suite: a disabled button stops a tap but not a submit event, and
    // three POSTs went out where one was intended. Each duplicate on the Wi-Fi step restarts
    // the station and abandons the attempt already running.
    EXPECT_TRUE(contains(js, "var inFlight = false"));
    EXPECT_TRUE(contains(js, "if (inFlight) {"));
    EXPECT_TRUE(contains(js, "inFlight = true"));
    EXPECT_TRUE(contains(js, "inFlight = false"));
}

TEST(ServedPageTests, TheWifiFieldsAreSubmittedVerbatim) {
    const std::string js = asset("app.js");

    // An SSID and a WPA2 passphrase may legitimately begin or end with a space, so these two
    // must NOT be normalised — and stage 1 is already hardware-validated as it stands.
    EXPECT_TRUE(contains(js, "wifiSsid: byId(\"wifiSsid\").value"));
    EXPECT_TRUE(contains(js, "wifiPassword: byId(\"wifiPassword\").value"));
}

TEST(ServedPageTests, AFieldStopsBeingMarkedAsSoonAsItIsEdited) {
    const std::string js = asset("app.js");

    // "Populated fields do not remain stale-invalid": the listener is attached to BOTH sets.
    EXPECT_TRUE(contains(js, "WIFI_FIELDS.concat(PRINTER_FIELDS).forEach"));
    EXPECT_TRUE(contains(js, "clearField(field)"));
}

TEST(ServedPageTests, OnlyTheFieldsTheServerReportedAsFailingAreMarked) {
    const std::string js = asset("app.js");

    // An absent outcome is treated as passing, which is what keeps a refusal — whose body
    // names no field — from marking the whole form.
    EXPECT_TRUE(contains(js, "if (!outcome || outcome === \"ok\")"));
}

TEST(ServedPageTests, NothingTheUserTypedIsEverEchoedBack) {
    const std::string js = asset("app.js");

    // The messages are fixed text chosen by a rule name. §14.4's "escape all displayed
    // values", honoured by never displaying one. See the note above on why the dot matters.
    EXPECT_FALSE(contains(js, ".innerHTML"));
    EXPECT_TRUE(contains(js, "FIELD_MESSAGES"));
    EXPECT_TRUE(contains(js, "textContent"));
}

} // namespace prd::provisioning
