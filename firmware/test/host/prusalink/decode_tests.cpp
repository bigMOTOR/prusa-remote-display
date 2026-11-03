// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include <fstream>
#include <ios>
#include <sstream>
#include <string>
#include <string_view>

#include <gtest/gtest.h>

#include "decode.hpp"

// Response body → DTO — Milestone 4's first vertical slice.
//
// ⛔ ALL BUT ONE OF THESE BODIES IS SYNTHETIC — the documented schema's shapes with invented
// contents. The exception is core_one_version() below, read from the captured payload at
// 2026-08-29 and mirrored in test/fixtures/prusalink/core-one/6.8.1+16182/. It was read and
// carries no address, serial or key: version strings, a nozzle diameter, the default hostname
// and a capability flag. Nothing else here was captured, and this comment must be narrowed
// again rather than deleted if that changes.
//
// ⛔ WHAT THESE PROTECT. §9.3 requires malformed required fields to be *classified*, not
// merely rejected: the connection phase a user sees differs between an unparseable body
// and a parseable one missing `state`, so a decoder that collapsed the two would make
// the panel lie about which fault it hit.

namespace prd::prusalink {
namespace {

// The minimum a status response can be and still be usable: §4.3 requires the `printer`
// object, and §5.1 requires `state` within it. Everything else is optional.
constexpr const char* kMinimalStatus = R"({"printer":{"state":"IDLE"}})";

// The shape a CORE One actually sends, as documented — all four temperatures present.
constexpr const char* kFullStatus =
    R"({"printer":{"state":"READY","temp_nozzle":215.0,"target_nozzle":215.0,)"
    R"("temp_bed":60.2,"target_bed":60,"axis_z":10.5,"flow":100,"speed":100,"fan_hotend":0},)"
    R"("storage":{"path":"/usb/","name":"usb","read_only":false}})";

// §4.1's required fields, plus the extensible capabilities object it describes.
constexpr const char* kVersion =
    R"({"api":"2.0.0","version":"2.1.2","printer":"1.3.1","text":"PrusaLink",)"
    R"("firmware":"6.5.7","sdk":"0.7.1","capabilities":{"upload-by-put":true}})";

// ⬅️ THE REAL ONE, AND IT IS READ RATHER THAN RESTATED. GET /api/version from the owner's CORE
// One, firmware 6.8.1+16182, captured 2026-08-29: 183 bytes, byte-for-byte the body the display's
// own probe decoded and rejected. It carries `server` and no `version`, which is the whole reason
// the alias exists.
//
// ⛔ ONE AUTHORITATIVE COPY. The payload lived here as a literal and again in the fixture tree,
// which is two copies of a measurement that is only worth anything while it matches what the
// printer actually sent. Reading the file makes drift impossible rather than unlikely: there is
// nothing left to keep in step.
[[nodiscard]] std::string core_one_version() {
    const std::string path =
        std::string{PRD_PRUSALINK_FIXTURES_DIR} + "/core-one/6.8.1+16182/idle/version.json";
    std::ifstream file{path, std::ios::binary};
    EXPECT_TRUE(file.is_open()) << "cannot open captured fixture: " << path;

    std::ostringstream contents;
    contents << file.rdbuf();
    return contents.str();
}

TEST(DecodeStatusTests, AcceptsTheMinimalDocumentedResponse) {
    StatusDto dto;
    ASSERT_EQ(decode_status(kMinimalStatus, dto), DecodeError::None);

    EXPECT_EQ(dto.printer_state.view(), "IDLE");

    // Absent stays absent. §9.3 keeps absent and zero distinguishable, and a bed the
    // printer did not report is not a bed at 0 °C.
    EXPECT_FALSE(dto.temp_nozzle_known);
    EXPECT_FALSE(dto.target_nozzle_known);
    EXPECT_FALSE(dto.temp_bed_known);
    EXPECT_FALSE(dto.target_bed_known);
}

TEST(DecodeStatusTests, ReadsEveryTemperatureFromTheFullResponse) {
    StatusDto dto;
    ASSERT_EQ(decode_status(kFullStatus, dto), DecodeError::None);

    EXPECT_EQ(dto.printer_state.view(), "READY");

    ASSERT_TRUE(dto.temp_nozzle_known);
    EXPECT_DOUBLE_EQ(dto.temp_nozzle, 215.0);
    ASSERT_TRUE(dto.target_nozzle_known);
    EXPECT_DOUBLE_EQ(dto.target_nozzle, 215.0);
    ASSERT_TRUE(dto.temp_bed_known);
    EXPECT_DOUBLE_EQ(dto.temp_bed, 60.2);

    // Integer notation for a field the other three send fractionally.
    ASSERT_TRUE(dto.target_bed_known);
    EXPECT_DOUBLE_EQ(dto.target_bed, 60.0);
}

TEST(DecodeStatusTests, UnknownMembersDoNotAffectTheResult) {
    // A future firmware adding fields must not change what this build decodes.
    const std::string extended =
        R"({"printer":{"state":"IDLE","temp_nozzle":24.0,"chamber":{"temp":30.0},)"
        R"("nozzle_diameter":0.4,"unknown_array":[1,2],"deep":{"a":{"b":1}}},)"
        R"("transfer":null,"camera":{"id":"c"},"future_top_level":true})";

    StatusDto extended_dto;
    ASSERT_EQ(decode_status(extended, extended_dto), DecodeError::None);

    StatusDto plain_dto;
    ASSERT_EQ(decode_status(R"({"printer":{"state":"IDLE","temp_nozzle":24.0}})", plain_dto),
              DecodeError::None);

    EXPECT_EQ(extended_dto.printer_state.view(), plain_dto.printer_state.view());
    EXPECT_EQ(extended_dto.temp_nozzle_known, plain_dto.temp_nozzle_known);
    EXPECT_DOUBLE_EQ(extended_dto.temp_nozzle, plain_dto.temp_nozzle);
    EXPECT_EQ(extended_dto.temp_bed_known, plain_dto.temp_bed_known);
}

TEST(DecodeStatusTests, ClassifiesMalformedSeparatelyFromSchemaFaults) {
    StatusDto dto;

    // Not JSON at all: syntax. §16.1's transport-adjacent failure.
    EXPECT_EQ(decode_status(R"({"printer":{"state":"IDLE")", dto), DecodeError::Malformed);
    EXPECT_EQ(decode_status("<html>404</html>", dto), DecodeError::Malformed);
    EXPECT_EQ(decode_status("", dto), DecodeError::Malformed);

    // Well-formed JSON that does not speak the contract: schema. The distinction is the
    // point — these must not report as Malformed.
    EXPECT_EQ(decode_status(R"({"job":{"id":1}})", dto), DecodeError::MissingObject);
    EXPECT_EQ(decode_status(R"({"printer":{"temp_nozzle":24.0}})", dto), DecodeError::MissingField);
    EXPECT_EQ(decode_status(R"({"printer":{"state":null}})", dto), DecodeError::MissingField);

    // Present but unusable: wrong type, or a value this build cannot represent.
    EXPECT_EQ(decode_status(R"({"printer":{"state":7}})", dto), DecodeError::UnusableField);
    EXPECT_EQ(decode_status(R"({"printer":{"state":""}})", dto), DecodeError::UnusableField);
    EXPECT_EQ(decode_status(R"({"printer":{"state":true}})", dto), DecodeError::UnusableField);

    // `printer` present but not an object is an absent object, not a usable one.
    EXPECT_EQ(decode_status(R"({"printer":"IDLE"})", dto), DecodeError::MissingObject);

    // POSITIVE CONTROL: the document all of the above were derived from decodes.
    EXPECT_EQ(decode_status(kMinimalStatus, dto), DecodeError::None);
}

TEST(DecodeStatusTests, RefusesAStateStringThatWouldBeTruncated) {
    // BoundedString<24> is the destination. A 25-character state must fail rather than
    // arrive clipped: a clipped enum name could match nothing, or worse, match something.
    const std::string too_long = R"({"printer":{"state":"ABCDEFGHIJKLMNOPQRSTUVWXY"}})";

    StatusDto dto;
    EXPECT_EQ(decode_status(too_long, dto), DecodeError::UnusableField);
    EXPECT_TRUE(dto.printer_state.empty());

    // POSITIVE CONTROL at the boundary: 23 characters fit and are read whole.
    const std::string fits = R"({"printer":{"state":"ABCDEFGHIJKLMNOPQRSTUVW"}})";
    StatusDto ok;
    ASSERT_EQ(decode_status(fits, ok), DecodeError::None);
    EXPECT_EQ(ok.printer_state.view(), "ABCDEFGHIJKLMNOPQRSTUVW");
}

TEST(DecodeStatusTests, AnUnusableOptionalFieldLeavesTheResponseUsable) {
    // §3.2's tolerance rule. A printer sending a string where a temperature belongs must
    // not cost the user the printer state, which is the value the product exists to show.
    StatusDto dto;
    ASSERT_EQ(decode_status(R"({"printer":{"state":"IDLE","temp_nozzle":"hot"}})", dto),
              DecodeError::None);

    EXPECT_EQ(dto.printer_state.view(), "IDLE");
    EXPECT_FALSE(dto.temp_nozzle_known);
}

TEST(DecodeVersionTests, ReadsAllFiveRequiredFields) {
    VersionDto dto;
    ASSERT_EQ(decode_version(kVersion, dto), DecodeError::None);

    EXPECT_EQ(dto.api.view(), "2.0.0");
    EXPECT_EQ(dto.version.view(), "2.1.2");
    EXPECT_EQ(dto.printer.view(), "1.3.1");
    EXPECT_EQ(dto.text.view(), "PrusaLink");
    EXPECT_EQ(dto.firmware.view(), "6.5.7");
}

TEST(DecodeVersionTests, EveryOneOfTheFiveRequiredFieldsIsActuallyRequired) {
    // ⛔ THE TEST THAT USED TO BE HERE ASSERTED THE DEFECT. It accepted `{"api":..,"firmware":..}` as
    // a valid version response, which meant any service answering with two strings could pass
    // validation and be promoted to an active printer configuration. §4.1's required set is api,
    // version, printer, text and firmware — all five — and the preflight is an identification, not a
    // liveness check.
    //
    // Each case below is the full document with exactly one field removed, so the table proves each
    // field individually rather than proving that some subset fails.
    struct Case {
        const char* body;
        const char* missing;
    };

    constexpr Case kCases[] = {
        {R"({"version":"2.1.2","printer":"1.3.1","text":"PrusaLink","firmware":"6.5.7"})", "api"},
        // Neither spelling of the version is present in this one, so the report names both.
        {R"({"api":"2.0.0","printer":"1.3.1","text":"PrusaLink","firmware":"6.5.7"})",
         "version|server"},
        {R"({"api":"2.0.0","version":"2.1.2","text":"PrusaLink","firmware":"6.5.7"})", "printer"},
        {R"({"api":"2.0.0","version":"2.1.2","printer":"1.3.1","firmware":"6.5.7"})", "text"},
        {R"({"api":"2.0.0","version":"2.1.2","printer":"1.3.1","text":"PrusaLink"})", "firmware"},
    };

    for (const Case& item : kCases) {
        VersionDto dto;
        std::string_view offending = "<untouched>";

        EXPECT_EQ(decode_version(item.body, dto, &offending), DecodeError::MissingField)
            << "removed: " << item.missing;

        // ⬅️ AND THE DECODE NAMES THE FIELD IT STOPPED ON. Asserted per case rather than once,
        // because a single assertion would pass on an implementation that always reported "api".
        EXPECT_EQ(offending, item.missing) << "removed: " << item.missing;
    }

    // POSITIVE CONTROL: the complete document these were each derived from decodes, so the table
    // discriminates missing fields rather than reflecting a decoder that refuses everything.
    VersionDto complete;
    EXPECT_EQ(
        decode_version(R"({"api":"2.0.0","version":"2.1.2","printer":"1.3.1","text":"PrusaLink",)"
                       R"("firmware":"6.5.7"})",
                       complete),
        DecodeError::None);
}

TEST(DecodeVersionTests, TheRealCoreOneResponseDecodes) {
    // The regression this whole checkpoint produced. Before the alias this body was rejected on
    // the device with `decode=missing-field field=version`, which stopped a real printer with a
    // valid configuration from ever being validated.
    const std::string body = core_one_version();

    // ⛔ BYTE-EXACT, ASSERTED. The device reported `bytes=183` for this exchange, and a fixture
    // that claims to be the measured payload is worth nothing if it is not that payload. A
    // trailing newline made this 184 and the claim false.
    ASSERT_EQ(body.size(), 183U) << "the fixture is no longer the body the printer sent";

    VersionDto dto;
    std::string_view offending = "<untouched>";

    ASSERT_EQ(decode_version(body, dto, &offending), DecodeError::None);
    EXPECT_EQ(offending, "<untouched>");

    // The PrusaLink version reaches the same destination it would have under the documented name.
    EXPECT_EQ(dto.version.view(), "2.1.2");

    EXPECT_EQ(dto.api.view(), "2.0.0");
    EXPECT_EQ(dto.printer.view(), "7.1.0");
    EXPECT_EQ(dto.text.view(), "PrusaLink");
    EXPECT_EQ(dto.firmware.view(), "6.8.1+16182");
}

TEST(DecodeVersionTests, EitherSpellingOfTheVersionIsAccepted) {
    // Documented name alone.
    VersionDto documented;
    EXPECT_EQ(decode_version(R"({"api":"2.0.0","version":"2.1.2","printer":"7.1.0",)"
                             R"("text":"PrusaLink","firmware":"6.8.1"})",
                             documented),
              DecodeError::None);
    EXPECT_EQ(documented.version.view(), "2.1.2");

    // Firmware's name alone.
    VersionDto shipped;
    EXPECT_EQ(decode_version(R"({"api":"2.0.0","server":"2.1.2","printer":"7.1.0",)"
                             R"("text":"PrusaLink","firmware":"6.8.1"})",
                             shipped),
              DecodeError::None);
    EXPECT_EQ(shipped.version.view(), "2.1.2");

    // ⬅️ THE DOCUMENTED NAME WINS when a response carries both, so a printer that starts sending
    // `version` is read from the field §4.1 actually specifies.
    VersionDto both;
    EXPECT_EQ(decode_version(R"({"api":"2.0.0","version":"documented","server":"fallback",)"
                             R"("printer":"7.1.0","text":"PrusaLink","firmware":"6.8.1"})",
                             both),
              DecodeError::None);
    EXPECT_EQ(both.version.view(), "documented");

    // ⛔ AND THE ALIAS DID NOT WEAKEN IDENTIFICATION. Neither spelling present is still a failure,
    // and it names both so a future bench log says what was looked for.
    VersionDto neither;
    std::string_view offending;
    EXPECT_EQ(decode_version(R"({"api":"2.0.0","printer":"7.1.0","text":"PrusaLink",)"
                             R"("firmware":"6.8.1"})",
                             neither, &offending),
              DecodeError::MissingField);
    EXPECT_EQ(offending, "version|server");
}

TEST(DecodeVersionTests, APresentButUnusableVersionIsNotMaskedByTheAlias) {
    // §9.3: a required field that is present and unreadable is a protocol fault, not an absence.
    // Falling back to `server` here would turn a broken response into a silently accepted one.
    VersionDto dto;
    std::string_view offending;

    EXPECT_EQ(decode_version(R"({"api":"2.0.0","version":7,"server":"2.1.2","printer":"7.1.0",)"
                             R"("text":"PrusaLink","firmware":"6.8.1"})",
                             dto, &offending),
              DecodeError::UnusableField);
    EXPECT_EQ(offending, "version");
}

TEST(DecodeVersionTests, TheOffendingFieldIsNamedOnlyWhenOneActuallyFails) {
    // Why this exists: checkpoint M4's first live transaction against the owner's CORE One returned
    // `decode=missing-field` on a 200, and the log could not say which of the five was absent — so
    // the bench could see that identification failed without being able to act on it.

    // A decode that succeeds must not write a name. Seeded with a sentinel rather than an empty
    // string so "left alone" is distinguishable from "set to empty".
    VersionDto complete;
    std::string_view untouched = "<untouched>";
    ASSERT_EQ(decode_version(kVersion, complete, &untouched), DecodeError::None);
    EXPECT_EQ(untouched, "<untouched>");

    // A field present but of the wrong type is UnusableField, and it is named too — the name
    // reports which field ended the decode, not which error class it ended with.
    VersionDto typed;
    std::string_view offending;
    EXPECT_EQ(decode_version(R"({"api":"2.0.0","version":2,"printer":"1.3.1",)"
                             R"("text":"PrusaLink","firmware":"6.5.7"})",
                             typed, &offending),
              DecodeError::UnusableField);
    EXPECT_EQ(offending, "version");

    // The pointer is optional, and omitting it must decode exactly as before. This is the form
    // every existing caller and test uses, so the parameter cannot have changed their behaviour.
    VersionDto ignored;
    EXPECT_EQ(decode_version(R"({"api":"2.0.0"})", ignored), DecodeError::MissingField);
    EXPECT_EQ(decode_version(kVersion, ignored), DecodeError::None);
}

TEST(DecodeVersionTests, AnUnrelatedServiceCannotPassValidation) {
    // The shapes something other than a PrusaLink printer might answer with on port 80. None may
    // decode: a router status page or a smart plug must not become an active printer configuration.
    constexpr const char* kImposters[] = {
        R"({"api":"1.0","firmware":"x"})",
        R"({"status":"ok"})",
        R"({"api":"2.0.0"})",
        R"({"name":"router","version":"1.2.3"})",
        R"({})",
        "<html><title>Login</title></html>",
    };

    for (const char* body : kImposters) {
        VersionDto dto;
        EXPECT_NE(decode_version(body, dto), DecodeError::None) << body;
    }
}

TEST(DecodeVersionTests, MalformedAndWrongTypedFieldsAreClassifiedApart) {
    VersionDto dto;

    EXPECT_EQ(decode_version("not json", dto), DecodeError::Malformed);
    EXPECT_EQ(decode_version("", dto), DecodeError::Malformed);

    // Present but not a string: a schema fault rather than a syntax one.
    EXPECT_EQ(
        decode_version(
            R"({"api":7,"version":"2.1.2","printer":"1.3.1","text":"P","firmware":"6.5.7"})", dto),
        DecodeError::UnusableField);

    // Null is the printer choosing to send nothing, which for a required field is absence.
    EXPECT_EQ(
        decode_version(
            R"({"api":null,"version":"2.1.2","printer":"1.3.1","text":"P","firmware":"6.5.7"})",
            dto),
        DecodeError::MissingField);
}

// --- The optional `job` object (Milestone 4, Printing telemetry) -------------
//
// ⛔ EVERY BODY BELOW IS SYNTHETIC, shaped to the OFFICIAL CONTRACT — the Prusa-Link-Web OpenAPI
// specification for `/api/v1/status` and the field set Prusa-Firmware-Buddy v6.8.1's status
// renderer emits. ⛔ NONE OF THEM IS A CAPTURE. No real Printing response body has ever been
// captured by this project, and nothing here may be cited as evidence of what a CORE One sends.
//
// ⛔ WHAT THESE PROTECT. §4.3 requires only the `printer` object, so nothing about a job may fail
// a response: a firmware that omits `job`, sends one that is not an object, or sends one whose
// every field is unusable must still yield a usable printer state, because the temperatures and
// the state word do not depend on a job existing. And absence must stay distinguishable from
// zero — a print at 0 % and a print whose progress was never reported are different panels.

// v6.8.1 with a job in flight: `id`, `progress` and `time_printing` always, `time_remaining` when
// the estimate is valid.
constexpr const char* kPrintingStatus =
    R"({"printer":{"state":"PRINTING","temp_nozzle":215.1,"target_nozzle":215,)"
    R"("temp_bed":60.0,"target_bed":60,"axis_z":14.6,"flow":100,"speed":100,)"
    R"("fan_hotend":8500,"fan_print":4300},)"
    R"("job":{"id":173,"progress":41.6,"time_remaining":2280,"time_printing":1680}})";

TEST(PrusaLinkDecodeJobTests, TheOfficialShapeYieldsEveryFieldTheContractCarries) {
    StatusDto dto;

    ASSERT_EQ(decode_status(kPrintingStatus, dto), DecodeError::None);

    EXPECT_EQ(dto.printer_state.view(), "PRINTING");
    EXPECT_TRUE(dto.job_present);

    EXPECT_TRUE(dto.job_id_known);
    EXPECT_EQ(dto.job_id.view(), "173");

    EXPECT_TRUE(dto.progress_known);
    EXPECT_DOUBLE_EQ(dto.progress_percent, 41.6);

    EXPECT_TRUE(dto.time_remaining_known);
    EXPECT_DOUBLE_EQ(dto.time_remaining_seconds, 2280.0);

    EXPECT_TRUE(dto.time_printing_known);
    EXPECT_DOUBLE_EQ(dto.time_printing_seconds, 1680.0);

    // The temperatures of the same response are unaffected by the job being read.
    EXPECT_TRUE(dto.temp_nozzle_known);
    EXPECT_DOUBLE_EQ(dto.temp_nozzle, 215.1);
}

TEST(PrusaLinkDecodeJobTests, NoJobObjectIsItsOwnAnswerAndNotAFailure) {
    // §4.3 makes `job` optional and v6.8.1 omits it entirely when no job exists. That is a fact
    // about the printer, so it is recorded rather than treated as silence: `job_present` false
    // with the response still usable.
    StatusDto dto;

    ASSERT_EQ(decode_status(kFullStatus, dto), DecodeError::None);

    EXPECT_FALSE(dto.job_present);
    EXPECT_FALSE(dto.job_id_known);
    EXPECT_FALSE(dto.progress_known);
    EXPECT_FALSE(dto.time_remaining_known);
    EXPECT_FALSE(dto.time_printing_known);

    EXPECT_EQ(dto.printer_state.view(), "READY");
    EXPECT_TRUE(dto.temp_bed_known) << "the rest of the response is still read";
}

TEST(PrusaLinkDecodeJobTests, ProgressIsReadAtBothEndsOfItsRangeAndInTheMiddle) {
    struct Case {
        const char* body;
        double expected;
    };

    // 0 is the case a sentinel design would lose: it is a real reading, not an absence.
    const Case cases[] = {
        {R"({"printer":{"state":"PRINTING"},"job":{"id":1,"progress":0}})", 0.0},
        {R"({"printer":{"state":"PRINTING"},"job":{"id":1,"progress":42}})", 42.0},
        {R"({"printer":{"state":"PRINTING"},"job":{"id":1,"progress":100}})", 100.0},
    };

    for (const Case& one : cases) {
        StatusDto dto;
        ASSERT_EQ(decode_status(one.body, dto), DecodeError::None) << one.body;
        EXPECT_TRUE(dto.progress_known) << one.body;
        EXPECT_DOUBLE_EQ(dto.progress_percent, one.expected) << one.body;
    }
}

TEST(PrusaLinkDecodeJobTests, ADecimalProgressIsReadAsWrittenAndNotAsAFraction) {
    // §6.1: `/api/v1/status.job.progress` is PERCENT and the specification's own example is 42.0.
    // The legacy `/api/job.progress.completion` has used 0.88 for the same idea, and §6.1 forbids
    // one unqualified decoder for both. 0.88 here is therefore 0.88 PERCENT — very nearly nothing
    // — and must never become 88.
    StatusDto dto;

    ASSERT_EQ(decode_status(R"({"printer":{"state":"PRINTING"},"job":{"progress":0.88}})", dto),
              DecodeError::None);

    EXPECT_TRUE(dto.progress_known);
    EXPECT_DOUBLE_EQ(dto.progress_percent, 0.88);
}

TEST(PrusaLinkDecodeJobTests, AnAbsentRemainingTimeIsTheOrdinaryCase) {
    // v6.8.1 emits `time_remaining` only when it holds a valid estimate, so its absence is not a
    // fault and must not cost the fields that are present.
    StatusDto dto;

    ASSERT_EQ(decode_status(R"({"printer":{"state":"PRINTING"},)"
                            R"("job":{"id":9,"progress":63,"time_printing":2580}})",
                            dto),
              DecodeError::None);

    EXPECT_TRUE(dto.job_present);
    EXPECT_FALSE(dto.time_remaining_known);
    EXPECT_TRUE(dto.progress_known);
    EXPECT_TRUE(dto.time_printing_known);
    EXPECT_EQ(dto.job_id.view(), "9");
}

TEST(PrusaLinkDecodeJobTests, AnUnusableFieldCostsOnlyItselfAndNeverTheResponse) {
    struct Case {
        const char* body;
        const char* what;
    };

    const Case cases[] = {
        {R"({"printer":{"state":"PRINTING"},)"
         R"("job":{"id":7,"progress":"almost","time_printing":10}})",
         "a string where a number belongs"},
        {R"({"printer":{"state":"PRINTING"},)"
         R"("job":{"id":7,"progress":null,"time_printing":10}})",
         "an explicit null"},
        {R"({"printer":{"state":"PRINTING"},)"
         R"("job":{"id":7,"progress":true,"time_printing":10}})",
         "a boolean"},
        {R"({"printer":{"state":"PRINTING"},)"
         R"("job":{"id":7,"progress":{"percent":50},"time_printing":10}})",
         "a nested object"},
        {R"({"printer":{"state":"PRINTING"},)"
         R"("job":{"id":7,"progress":1e400,"time_printing":10}})",
         "a finite token that converts to infinity"},
    };

    for (const Case& one : cases) {
        StatusDto dto;

        ASSERT_EQ(decode_status(one.body, dto), DecodeError::None) << one.what;

        EXPECT_TRUE(dto.job_present) << one.what;
        EXPECT_FALSE(dto.progress_known) << one.what;

        // ⛔ THE REST OF THE JOB SURVIVES. The defect this guards against is a decoder that
        // abandons the record — or the response — because one member of it was unreadable.
        EXPECT_TRUE(dto.job_id_known) << one.what;
        EXPECT_EQ(dto.job_id.view(), "7") << one.what;
        EXPECT_TRUE(dto.time_printing_known) << one.what;
        EXPECT_EQ(dto.printer_state.view(), "PRINTING") << one.what;
    }
}

TEST(PrusaLinkDecodeJobTests, ANonFiniteProgressIsRefusedRatherThanCarried) {
    // Named separately from the sweep above because it is the case a plain type check misses:
    // `1e400` is a WELL-FORMED JSON number, so the scanner accepts the token and strtod converts
    // it to infinity while consuming all of it. An infinity that reached the domain conversion
    // would fail every comparison there and be neither accepted nor rejected on its merits.
    StatusDto dto;

    ASSERT_EQ(decode_status(R"({"printer":{"state":"PRINTING"},"job":{"progress":-1e400}})", dto),
              DecodeError::None);

    EXPECT_TRUE(dto.job_present);
    EXPECT_FALSE(dto.progress_known);
}

TEST(PrusaLinkDecodeJobTests, AJobThatIsNotAnObjectReadsAsNoJob) {
    // Absent, null, a number and an array all mean the same thing to this product — no usable job
    // record arrived — and §3.2 makes none of them a protocol fault.
    const char* const bodies[] = {
        R"({"printer":{"state":"PRINTING"},"job":null})",
        R"({"printer":{"state":"PRINTING"},"job":5})",
        R"({"printer":{"state":"PRINTING"},"job":[]})",
        R"({"printer":{"state":"PRINTING"},"job":"none"})",
    };

    for (const char* const body : bodies) {
        StatusDto dto;
        ASSERT_EQ(decode_status(body, dto), DecodeError::None) << body;
        EXPECT_FALSE(dto.job_present) << body;
        EXPECT_EQ(dto.printer_state.view(), "PRINTING") << body;
    }
}

TEST(PrusaLinkDecodeJobTests, AdditiveMembersAreIgnoredAtEveryLevel) {
    // §3.2's tolerance rule, at the three places a future firmware could add a field: beside
    // `printer`, inside `printer`, and inside `job`.
    constexpr const char* kAdditive =
        R"({"printer":{"state":"PRINTING","temp_nozzle":215.2,"chamber_fan":2200,)"
        R"("future_scalar":1},)"
        R"("job":{"id":173,"progress":78,"time_printing":3120,"serial_print":false,)"
        R"("future_nested":{"deep":{"deeper":1}}},)"
        R"("camera":{"id":"0","registered":false},"future_object":{"nested":{"deep":1}}})";

    StatusDto dto;

    ASSERT_EQ(decode_status(kAdditive, dto), DecodeError::None);

    EXPECT_TRUE(dto.job_present);
    EXPECT_TRUE(dto.progress_known);
    EXPECT_DOUBLE_EQ(dto.progress_percent, 78.0);
    EXPECT_EQ(dto.job_id.view(), "173");
    EXPECT_TRUE(dto.temp_nozzle_known);
}

TEST(PrusaLinkDecodeJobTests, TheIdentityIsReadFromEitherSpellingAndRefusedWhenItIsNotOne) {
    struct Case {
        const char* body;
        bool known;
        const char* expected;
    };

    const Case cases[] = {
        // The contract types `id` as an integer, and v6.8.1 sends one.
        {R"({"printer":{"state":"PRINTING"},"job":{"id":173}})", true, "173"},
        {R"({"printer":{"state":"PRINTING"},"job":{"id":0}})", true, "0"},
        // Tolerated, at no cost: the two JSON types cannot be confused for one another.
        {R"({"printer":{"state":"PRINTING"},"job":{"id":"abc-9"}})", true, "abc-9"},
        // ⛔ HALF AN IDENTITY IS A DIFFERENT JOB, NOT A WEAKER READING OF THIS ONE.
        {R"({"printer":{"state":"PRINTING"},"job":{"id":17.5}})", false, ""},
        {R"({"printer":{"state":"PRINTING"},"job":{"id":-1}})", false, ""},
        {R"({"printer":{"state":"PRINTING"},"job":{"id":1e400}})", false, ""},
        {R"({"printer":{"state":"PRINTING"},"job":{"id":null}})", false, ""},
        {R"({"printer":{"state":"PRINTING"},"job":{"progress":5}})", false, ""},
    };

    for (const Case& one : cases) {
        StatusDto dto;

        ASSERT_EQ(decode_status(one.body, dto), DecodeError::None) << one.body;
        EXPECT_EQ(dto.job_id_known, one.known) << one.body;

        if (one.known) {
            EXPECT_EQ(dto.job_id.view(), one.expected) << one.body;
        } else {
            EXPECT_TRUE(dto.job_id.view().empty()) << one.body;
        }
    }
}

} // namespace
} // namespace prd::prusalink
