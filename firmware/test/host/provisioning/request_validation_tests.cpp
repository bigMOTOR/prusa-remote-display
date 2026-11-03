// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include <string>

#include <gtest/gtest.h>

#include "request_validation.hpp"

// §14.4's "validate host and port input", walked exhaustively.
//
// The host rules carry most of the weight: that field is the one a later milestone will
// concatenate into a URL, so everything that could change the meaning of that URL — a
// scheme, a path, a query, an embedded port, userinfo — has to die here rather than be
// escaped somewhere downstream by someone who remembered.

namespace prd::provisioning {
namespace {

using Outcome = FieldOutcome;

std::string repeated(const std::size_t count, const char character = 'a') {
    return std::string(count, character);
}

} // namespace

// --- SSID -------------------------------------------------------------------

TEST(SsidValidationTests, AnOrdinaryNameIsAccepted) {
    EXPECT_EQ(validate_ssid("Workshop"), Outcome::Ok);
    EXPECT_EQ(validate_ssid("My Network 5GHz"), Outcome::Ok);
}

TEST(SsidValidationTests, EmptyIsMissingRatherThanValid) {
    EXPECT_EQ(validate_ssid(""), Outcome::Missing);
}

TEST(SsidValidationTests, TheLengthBoundIsTheStandardsOwn) {
    EXPECT_EQ(validate_ssid(repeated(kSsidMax)), Outcome::Ok);
    EXPECT_EQ(validate_ssid(repeated(kSsidMax + 1)), Outcome::TooLong);
}

TEST(SsidValidationTests, ControlCharactersAreRefused) {
    // Not because the standard forbids them — it does not — but because a name that
    // cannot be shown on the panel or typed back is not usable by a person.
    EXPECT_EQ(validate_ssid(std::string("Work\nshop")), Outcome::BadCharacter);
    EXPECT_EQ(validate_ssid(std::string("Work\x7Fshop")), Outcome::BadCharacter);
    EXPECT_EQ(validate_ssid(std::string("Work\0shop", 9)), Outcome::BadCharacter);
}

TEST(SsidValidationTests, NonAsciiIsAcceptedBecauseAnSsidIsOctets) {
    EXPECT_EQ(validate_ssid("Werkstatt-\xC3\xA4"), Outcome::Ok);
}

// --- Wi-Fi password ---------------------------------------------------------

TEST(WifiPasswordValidationTests, TheWpa2RangeIsEnforcedAtBothEnds) {
    EXPECT_EQ(validate_wifi_password(repeated(kWifiPasswordMin - 1)), Outcome::TooShort);
    EXPECT_EQ(validate_wifi_password(repeated(kWifiPasswordMin)), Outcome::Ok);
    EXPECT_EQ(validate_wifi_password(repeated(kWifiPasswordMax)), Outcome::Ok);
    EXPECT_EQ(validate_wifi_password(repeated(kWifiPasswordMax + 1)), Outcome::TooLong);
}

TEST(WifiPasswordValidationTests, EmptyIsAnOpenNetworkNotAnOmission) {
    // A user with an open network is a real user. Refusing empty would make the product
    // unusable on their network, and there is no way for them to work around it.
    EXPECT_EQ(validate_wifi_password(""), Outcome::Ok);
}

TEST(WifiPasswordValidationTests, NonPrintableCharactersAreRefused) {
    EXPECT_EQ(validate_wifi_password(std::string("pass\tword")), Outcome::BadCharacter);
    EXPECT_EQ(validate_wifi_password("passwörd12"), Outcome::BadCharacter);
}

// --- Printer host -----------------------------------------------------------

TEST(PrinterHostValidationTests, TheThreeAcceptedShapes) {
    EXPECT_EQ(validate_printer_host("192.168.1.50"), Outcome::Ok);
    EXPECT_EQ(validate_printer_host("prusa-core-one"), Outcome::Ok);
    EXPECT_EQ(validate_printer_host("prusa.local"), Outcome::Ok);
    EXPECT_EQ(validate_printer_host("printer.workshop.lan"), Outcome::Ok);
}

TEST(PrinterHostValidationTests, EmptyIsMissing) {
    EXPECT_EQ(validate_printer_host(""), Outcome::Missing);
}

TEST(PrinterHostValidationTests, TheLengthBoundMatchesTheRecordItWillBeWrittenTo) {
    // Split into labels, because a single label may not exceed 63 characters whatever the
    // total length is — see the label test below.
    const std::string at_bound = repeated(kPrinterHostMax - 4) + ".abc";
    const std::string over_bound = repeated(kPrinterHostMax - 3) + ".abc";

    EXPECT_EQ(at_bound.size(), kPrinterHostMax);
    EXPECT_EQ(validate_printer_host(at_bound), Outcome::Ok);
    EXPECT_EQ(validate_printer_host(over_bound), Outcome::TooLong);
}

TEST(PrinterHostValidationTests, ALabelLongerThanRfc1123AllowsIsRefused) {
    // 63 is the label limit, independent of the 64-byte total this project imposes. Found
    // by a test that assumed the total bound was the only one.
    EXPECT_EQ(validate_printer_host(repeated(63)), Outcome::Ok);
    EXPECT_EQ(validate_printer_host(repeated(64)), Outcome::BadCharacter);
}

// ⬅️ THE ONES THAT MATTER. Each of these changes what a URL built from this value would
// mean, and each is refused by the character set before any structural rule runs.
TEST(PrinterHostValidationTests, EverythingThatCouldRewriteALaterUrlIsRefused) {
    EXPECT_EQ(validate_printer_host("http://printer"), Outcome::BadCharacter) << "scheme";
    EXPECT_EQ(validate_printer_host("https://printer"), Outcome::BadCharacter) << "scheme";
    EXPECT_EQ(validate_printer_host("printer/api/v1/status"), Outcome::BadCharacter) << "path";
    EXPECT_EQ(validate_printer_host("printer?x=1"), Outcome::BadCharacter) << "query";
    EXPECT_EQ(validate_printer_host("printer#frag"), Outcome::BadCharacter) << "fragment";
    EXPECT_EQ(validate_printer_host("printer:8080"), Outcome::BadCharacter) << "embedded port";
    EXPECT_EQ(validate_printer_host("user:pass@printer"), Outcome::BadCharacter) << "userinfo";
    EXPECT_EQ(validate_printer_host("printer evil"), Outcome::BadCharacter) << "space";
    EXPECT_EQ(validate_printer_host("printer\r\nHost: evil"), Outcome::BadCharacter)
        << "header injection";
    EXPECT_EQ(validate_printer_host("[::1]"), Outcome::BadCharacter) << "bracketed IPv6";
    // An explicit length: a string_view built from a literal would stop at the NUL and
    // test nothing at all.
    EXPECT_EQ(validate_printer_host(std::string_view("192.168.1.50\0evil", 17)),
              Outcome::BadCharacter)
        << "embedded NUL";
}

TEST(PrinterHostValidationTests, MalformedLabelsAreRefused) {
    EXPECT_EQ(validate_printer_host("-printer"), Outcome::BadCharacter) << "leading hyphen";
    EXPECT_EQ(validate_printer_host("printer-"), Outcome::BadCharacter) << "trailing hyphen";
    EXPECT_EQ(validate_printer_host("printer..local"), Outcome::BadCharacter) << "empty label";
    EXPECT_EQ(validate_printer_host(".printer"), Outcome::BadCharacter) << "leading dot";
    EXPECT_EQ(validate_printer_host("printer."), Outcome::BadCharacter) << "trailing dot";
    EXPECT_EQ(validate_printer_host("a-.b"), Outcome::BadCharacter) << "label ending in a hyphen";
}

TEST(PrinterHostValidationTests, AHyphenInsideALabelIsFine) {
    EXPECT_EQ(validate_printer_host("prusa-core-one.local"), Outcome::Ok);
}

// --- Port -------------------------------------------------------------------

TEST(PrinterPortValidationTests, TheUsualPortsParse) {
    EXPECT_EQ(validate_printer_port("80").outcome, Outcome::Ok);
    EXPECT_EQ(validate_printer_port("80").value, 80);
    EXPECT_EQ(validate_printer_port("8080").value, 8080);
    EXPECT_EQ(validate_printer_port("65535").value, 65535);
}

TEST(PrinterPortValidationTests, TheRangeIsEnforcedAtBothEnds) {
    EXPECT_EQ(validate_printer_port("0").outcome, Outcome::TooShort);
    EXPECT_EQ(validate_printer_port("65536").outcome, Outcome::TooLong);
}

TEST(PrinterPortValidationTests, OnlyDecimalDigits) {
    EXPECT_EQ(validate_printer_port("+80").outcome, Outcome::BadCharacter);
    EXPECT_EQ(validate_printer_port("-80").outcome, Outcome::BadCharacter);
    EXPECT_EQ(validate_printer_port(" 80").outcome, Outcome::BadCharacter);
    EXPECT_EQ(validate_printer_port("80 ").outcome, Outcome::BadCharacter);
    EXPECT_EQ(validate_printer_port("0x50").outcome, Outcome::BadCharacter);
    EXPECT_EQ(validate_printer_port("8o").outcome, Outcome::BadCharacter);
    EXPECT_EQ(validate_printer_port("").outcome, Outcome::Missing);
}

// The reason overflow is checked inside the accumulation loop rather than after it.
TEST(PrinterPortValidationTests, ALongDigitRunCannotWrapIntoAPlausiblePort) {
    // 4294967376 wraps to 80 in 32 bits. It must not become port 80.
    EXPECT_EQ(validate_printer_port("4294967376").outcome, Outcome::TooLong);
    EXPECT_EQ(validate_printer_port("4294967376").value, 0);

    // 65536 repeated until it would wrap several times over.
    EXPECT_EQ(validate_printer_port("99999999999999999999").outcome, Outcome::TooLong);
}

TEST(PrinterPortValidationTests, LeadingZeroesAreAcceptedAndDoNotChangeTheValue) {
    EXPECT_EQ(validate_printer_port("00080").outcome, Outcome::Ok);
    EXPECT_EQ(validate_printer_port("00080").value, 80);
}

// --- The whole submission ---------------------------------------------------

namespace {

Submission good() {
    Submission submission;
    submission.wifi_ssid = "Workshop";
    submission.wifi_password = "correct-horse";
    submission.printer_host = "prusa.local";
    submission.printer_port = "80";
    submission.printer_api_key = "abcdef0123456789";
    return submission;
}

} // namespace

TEST(SubmissionValidationTests, AGoodSubmissionIsAcceptedAtEitherStage) {
    EXPECT_TRUE(validate(good(), Stage::Wifi).accepted(Stage::Wifi));

    const SubmissionReport report = validate(good(), Stage::Printer);
    EXPECT_TRUE(report.accepted(Stage::Printer));
    EXPECT_EQ(report.port_value, 80);
}

// ⬅️ THE DEFECT THAT FAILED PHYSICAL ACCEPTANCE, AS A TEST. A user checking their Wi-Fi has
// not reached the printer section, so a Wi-Fi submission must not be refused for lacking it.
TEST(SubmissionValidationTests, TheWifiStageDoesNotRequirePrinterFields) {
    Submission submission;
    submission.wifi_ssid = "MyNet";
    submission.wifi_password = "supersecret";
    // printer_host, printer_port and printer_api_key deliberately empty.

    const SubmissionReport report = validate(submission, Stage::Wifi);

    EXPECT_TRUE(report.accepted(Stage::Wifi));
    EXPECT_EQ(report.wifi_ssid, Outcome::Ok);
    EXPECT_EQ(report.wifi_password, Outcome::Ok);
}

// And it reports nothing about them either, so the page cannot mark a section the reader has
// not seen — which is what the photographed screen did.
TEST(SubmissionValidationTests, TheWifiStageReportsNothingAboutPrinterFields) {
    Submission submission;
    submission.wifi_ssid = "MyNet";

    const SubmissionReport report = validate(submission, Stage::Wifi);

    EXPECT_EQ(report.printer_host, Outcome::Ok);
    EXPECT_EQ(report.printer_port, Outcome::Ok);
    EXPECT_EQ(report.printer_api_key, Outcome::Ok);
}

// Owner ruling 4: there is no finish-with-Wi-Fi-only path. All three are required at stage 2.
TEST(SubmissionValidationTests, ThePrinterStageRequiresAllThreePrinterFields) {
    Submission submission = good();
    submission.printer_api_key = "";

    const SubmissionReport report = validate(submission, Stage::Printer);

    EXPECT_FALSE(report.accepted(Stage::Printer));
    EXPECT_EQ(report.printer_api_key, Outcome::Missing);

    // Only the field that failed. The other two were fine and must read as fine — this is
    // the exact response shape the photographed submission produced.
    EXPECT_EQ(report.printer_host, Outcome::Ok);
    EXPECT_EQ(report.printer_port, Outcome::Ok);
}

TEST(SubmissionValidationTests, OneBadFieldFailsTheWholeSubmission) {
    // And only that field is reported bad — a report that blamed everything would make the
    // page unable to point at what the reader must fix.
    Submission submission = good();
    submission.printer_host = "http://printer";

    const SubmissionReport report = validate(submission, Stage::Printer);

    EXPECT_FALSE(report.accepted(Stage::Printer));
    EXPECT_EQ(report.printer_host, Outcome::BadCharacter);
    EXPECT_EQ(report.wifi_ssid, Outcome::Ok);
    EXPECT_EQ(report.wifi_password, Outcome::Ok);
    EXPECT_EQ(report.printer_port, Outcome::Ok);
    EXPECT_EQ(report.printer_api_key, Outcome::Ok);
}

TEST(SubmissionValidationTests, ARejectedPortCarriesNoValue) {
    Submission submission = good();
    submission.printer_port = "70000";

    const SubmissionReport report = validate(submission, Stage::Printer);

    EXPECT_FALSE(report.accepted(Stage::Printer));
    EXPECT_EQ(report.port_value, 0) << "a refused port must not reach the caller as a number";
}

TEST(SubmissionValidationTests, AnEmptyWifiStageFailsItsRequiredField) {
    const SubmissionReport report = validate({}, Stage::Wifi);

    EXPECT_FALSE(report.accepted(Stage::Wifi));
    EXPECT_EQ(report.wifi_ssid, Outcome::Missing);

    // The one field for which empty is a value rather than an omission: an open network.
    EXPECT_EQ(report.wifi_password, Outcome::Ok);
}

TEST(SubmissionValidationTests, AnEmptyPrinterStageFailsAllThreeOfItsFields) {
    const SubmissionReport report = validate({}, Stage::Printer);

    EXPECT_FALSE(report.accepted(Stage::Printer));
    EXPECT_EQ(report.printer_host, Outcome::Missing);
    EXPECT_EQ(report.printer_port, Outcome::Missing);
    EXPECT_EQ(report.printer_api_key, Outcome::Missing);
}

// --- The printer-stage blocker, and why it failed every field --------------
//
// Reproduced against the real firmware on 2026-08-17: a printer-stage submission whose three
// values each carried one trailing whitespace byte was answered
// `host=bad-character port=bad-character api_key=bad-character` — three failures from one
// paste. The page now removes surrounding whitespace before submitting; these tests pin both
// halves, so neither the grammar nor the normalisation can drift back.

TEST(SubmissionValidationTests, AValidPrinterTripleIsAccepted) {
    Submission submission;
    submission.printer_host = "192.168.1.50";
    submission.printer_port = "80";
    submission.printer_api_key = "Zx9QsT4bC1";

    const SubmissionReport report = validate(submission, Stage::Printer);

    EXPECT_TRUE(report.accepted(Stage::Printer));
    EXPECT_EQ(report.printer_host, Outcome::Ok);
    EXPECT_EQ(report.printer_port, Outcome::Ok);
    EXPECT_EQ(report.printer_api_key, Outcome::Ok);
    EXPECT_EQ(report.port_value, 80);
}

TEST(SubmissionValidationTests, SurroundingWhitespaceIsWhatFailedAllThreeFields) {
    // The grammar is deliberately unchanged: whitespace is still not a host character, still
    // not a digit, and a newline is still not printable. What changed is that the page no
    // longer submits it. This test exists so the *cause* stays visible in the suite.
    Submission submission;
    submission.printer_host = "192.168.1.50\n";
    submission.printer_port = "80\n";
    submission.printer_api_key = "Zx9QsT4bC1\n";

    const SubmissionReport report = validate(submission, Stage::Printer);

    EXPECT_FALSE(report.accepted(Stage::Printer));
    EXPECT_EQ(report.printer_host, Outcome::BadCharacter);
    EXPECT_EQ(report.printer_port, Outcome::BadCharacter);
    EXPECT_EQ(report.printer_api_key, Outcome::BadCharacter);
}

TEST(SubmissionValidationTests, ATrailingSpaceFailsTheAddressAndThePortButNotTheKey) {
    // Worth pinning separately, because it is the asymmetry that made the fault confusing:
    // 0x20 IS printable, so a trailing space fails two fields and leaves the third looking
    // fine — which is why "even when the API key is populated" was a true observation.
    Submission submission;
    submission.printer_host = "192.168.1.50 ";
    submission.printer_port = "80 ";
    submission.printer_api_key = "Zx9QsT4bC1 ";

    const SubmissionReport report = validate(submission, Stage::Printer);

    EXPECT_EQ(report.printer_host, Outcome::BadCharacter);
    EXPECT_EQ(report.printer_port, Outcome::BadCharacter);
    EXPECT_EQ(report.printer_api_key, Outcome::Ok);
}

TEST(SubmissionValidationTests, TheSameValuesAreAcceptedOnceTheirWhitespaceIsRemoved) {
    // What the page now sends for the submission above.
    Submission submission;
    submission.printer_host = "192.168.1.50";
    submission.printer_port = "80";
    submission.printer_api_key = "Zx9QsT4bC1";

    EXPECT_TRUE(validate(submission, Stage::Printer).accepted(Stage::Printer));
}

TEST(SubmissionValidationTests, EveryOutcomeHasADistinctLabel) {
    // The labels are what the response and the log both carry, in place of the value.
    EXPECT_NE(log_label(Outcome::Ok), log_label(Outcome::Missing));
    EXPECT_NE(log_label(Outcome::TooShort), log_label(Outcome::TooLong));
    EXPECT_NE(log_label(Outcome::BadCharacter), log_label(Outcome::Ok));
}

} // namespace prd::provisioning
