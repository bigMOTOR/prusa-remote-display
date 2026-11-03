// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include <cstddef>
#include <string>

#include <gtest/gtest.h>

#include "json_scan.hpp"

// The bounded JSON reader — Milestone 4's first vertical slice.
//
// ⛔ WHAT THESE PROTECT. This is the layer that meets a real printer's output, and the
// only one an attacker or a broken firmware controls the input to. A reader that
// accepts a truncated body reports a stale printer state as current (§9.3, §24.18); one
// that recurses without a bound meets a task stack instead of an error. Neither failure
// is observable on a healthy bench printer, which is exactly why it is proved here.
//
// ⬅️ THE POSITIVE CONTROL IS IN EVERY REJECTION TEST. Each malformed case is paired with
// the well-formed document it was derived from, so a test that passes because the reader
// rejects everything cannot hide: the pair would fail together.

namespace prd::prusalink {
namespace {

// The shape of a real status response, reduced to what this checkpoint reads.
constexpr const char* kStatus =
    R"({"printer":{"state":"IDLE","temp_nozzle":24.9,"target_nozzle":0,"temp_bed":23.5,)"
    R"("target_bed":0},"job":null,"storage":{"path":"/usb/","name":"usb"}})";

[[nodiscard]] JsonObject parsed(const std::string_view text) {
    JsonObject object;
    EXPECT_EQ(parse_object(text, object), ScanStatus::Ok) << text;
    return object;
}

[[nodiscard]] std::string read(const JsonField& field) {
    char buffer[64] = {};
    std::size_t length = 0;
    EXPECT_TRUE(read_string(field, buffer, sizeof(buffer), length));
    return std::string{buffer, length};
}

TEST(JsonScanTests, ReadsTheDocumentedStatusShape) {
    const JsonObject root = parsed(kStatus);

    JsonObject printer;
    ASSERT_TRUE(find_object(root, "printer", printer));

    const JsonField state = find(printer, "state");
    ASSERT_EQ(state.type, JsonType::String);
    EXPECT_EQ(read(state), "IDLE");

    double nozzle = 0.0;
    ASSERT_TRUE(read_number(find(printer, "temp_nozzle"), nozzle));
    EXPECT_DOUBLE_EQ(nozzle, 24.9);

    // An integer where a temperature is expected. §9.3 requires both notations to be
    // accepted, because different firmware sends different ones for the same field.
    double target = -1.0;
    ASSERT_TRUE(read_number(find(printer, "target_nozzle"), target));
    EXPECT_DOUBLE_EQ(target, 0.0);
}

TEST(JsonScanTests, UnknownMembersAreIgnored) {
    // Additive fields at both levels, including objects and arrays, plus a member whose
    // name is a prefix of one that is read.
    const std::string augmented =
        R"({"printer":{"chamber":{"temp":31.5},"state":"PRINTING","state_extra":"x",)"
        R"("fan_hotend":8000},"telemetry":[1,2,3],"future":{"a":{"b":{"c":true}}}})";

    const JsonObject root = parsed(augmented);

    JsonObject printer;
    ASSERT_TRUE(find_object(root, "printer", printer));
    EXPECT_EQ(read(find(printer, "state")), "PRINTING");
}

TEST(JsonScanTests, AbsentMembersReportAbsentRatherThanEmpty) {
    const JsonObject root = parsed(R"({"printer":{"state":"IDLE"}})");

    JsonObject printer;
    ASSERT_TRUE(find_object(root, "printer", printer));

    EXPECT_FALSE(find(printer, "temp_bed").present());
    EXPECT_EQ(find(printer, "temp_bed").type, JsonType::Absent);

    // Null is a value the printer chose to send and is not the same fact as absence.
    const JsonObject with_null = parsed(R"({"printer":{"state":"IDLE","temp_bed":null}})");
    JsonObject printer_null;
    ASSERT_TRUE(find_object(with_null, "printer", printer_null));
    EXPECT_TRUE(find(printer_null, "temp_bed").present());
    EXPECT_EQ(find(printer_null, "temp_bed").type, JsonType::Null);
}

TEST(JsonScanTests, MissingObjectIsNotAnEmptyObject) {
    const JsonObject root = parsed(R"({"job":{"id":7}})");

    JsonObject printer;
    EXPECT_FALSE(find_object(root, "printer", printer));

    // POSITIVE CONTROL: the same call succeeds for the object that is there, so the
    // failure above is about absence and not about find_object() never working.
    JsonObject job;
    EXPECT_TRUE(find_object(root, "job", job));
}

TEST(JsonScanTests, RejectsMalformedDocuments) {
    struct Case {
        const char* text;
        const char* why;
    };

    // Each entry is a mutation of a document that parses. The pair is the control.
    constexpr Case kCases[] = {
        {R"({"printer":{"state":"IDLE"})", "unclosed outer object"},
        {R"({"printer":{"state":"IDLE"}}})", "trailing brace"},
        {R"({"printer":{"state":"IDLE"},})", "trailing comma"},
        {R"({"printer":{"state":IDLE}})", "unquoted value"},
        {R"({printer:{"state":"IDLE"}})", "unquoted name"},
        {R"({"printer":{"state":"IDLE"}} garbage)", "content after the object"},
        {R"({"printer":{"state":"IDL)", "unterminated string"},
        {R"({"printer":{"state":"IDLE","t":01}})", "leading zero"},
        {R"({"printer":{"state":"IDLE","t":1.}})", "fraction with no digits"},
        {R"({"printer":{"state":"IDLE","t":+1}})", "leading plus"},
        {R"({"printer":{"state":"a\qb"}})", "invalid escape"},
        {R"({"printer":{"state":"IDLE"} "job":1})", "missing separator"},
        {R"([{"printer":{"state":"IDLE"}}])", "array at the top level"},
        {"", "empty body"},
        {"   ", "whitespace only"},
    };

    for (const Case& item : kCases) {
        JsonObject object;
        EXPECT_EQ(parse_object(item.text, object), ScanStatus::Malformed) << item.why;
    }

    // POSITIVE CONTROL for the whole table: the unmutated document parses.
    JsonObject control;
    EXPECT_EQ(parse_object(R"({"printer":{"state":"IDLE","t":1.0}})", control), ScanStatus::Ok);
}

TEST(JsonScanTests, RejectsAnUnescapedControlCharacter) {
    // The shape a truncated-then-concatenated response takes.
    JsonObject object;
    EXPECT_EQ(parse_object("{\"printer\":{\"state\":\"ID\nLE\"}}", object), ScanStatus::Malformed);

    JsonObject control;
    EXPECT_EQ(parse_object("{\"printer\":{\"state\":\"ID\\nLE\"}}", control), ScanStatus::Ok);
}

TEST(JsonScanTests, RejectsNestingBeyondTheDepthLimit) {
    // Depth is bounded rather than trusted: the recursion runs on the worker task's
    // stack, and a document is external input.
    const auto nested = [](const std::size_t depth) {
        std::string text;

        for (std::size_t level = 0; level < depth; ++level) {
            text += R"({"a":)";
        }

        text += "1";

        for (std::size_t level = 0; level < depth; ++level) {
            text += "}";
        }

        return text;
    };

    JsonObject object;
    EXPECT_EQ(parse_object(nested(kMaxDepth + 1), object), ScanStatus::TooDeep);

    // POSITIVE CONTROL at the boundary: one less nests to exactly the limit and passes,
    // so the rejection above is the limit and not a general failure on nesting.
    EXPECT_EQ(parse_object(nested(kMaxDepth), object), ScanStatus::Ok);
}

TEST(JsonScanTests, ResolvesEscapesAndRefusesWhatWillNotFit) {
    const JsonObject root =
        parsed(R"({"a":"x\"y\\z\/w\tv","b":"A\u00e9","c":"0123456789","d":"A\u0042"})");

    EXPECT_EQ(read(find(root, "a")), "x\"y\\z/w\tv");

    // A \u escape above ASCII becomes '?' rather than failing the document. Every value
    // this decoder reads is an ASCII enum name, so the substitution cannot change a
    // decision — and refusing valid JSON would be worse.
    EXPECT_EQ(read(find(root, "b")), "A?");

    // Below 0x80 the escape resolves to the character itself.
    EXPECT_EQ(read(find(root, "d")), "AB");

    // Raw UTF-8 in the body is not an escape and passes through as the bytes it is. The
    // distinction matters: a printer name with an accent must survive, and only the \u
    // form is lossy.
    const JsonObject utf8 = parsed("{\"n\":\"A\xc3\xa9\"}");
    EXPECT_EQ(read(find(utf8, "n")), "A\xc3\xa9");

    // Refuses rather than clips. A truncated state string must never become a state.
    char small[4] = {};
    std::size_t length = 99;
    EXPECT_FALSE(read_string(find(root, "c"), small, sizeof(small), length));
    EXPECT_EQ(length, 0U);

    // POSITIVE CONTROL: exactly-fitting capacity succeeds, so the refusal is the bound.
    char exact[10] = {};
    EXPECT_TRUE(read_string(find(root, "c"), exact, sizeof(exact), length));
    EXPECT_EQ(length, 10U);
}

TEST(JsonScanTests, TypedReadsRefuseTheWrongType) {
    const JsonObject root = parsed(R"({"n":42,"s":"42","b":true,"o":{},"a":[]})");

    double number = 0.0;
    EXPECT_TRUE(read_number(find(root, "n"), number));
    EXPECT_DOUBLE_EQ(number, 42.0);

    // A numeric string is not a number. Accepting it would make a printer's own type
    // error invisible.
    EXPECT_FALSE(read_number(find(root, "s"), number));
    EXPECT_FALSE(read_number(find(root, "b"), number));

    char buffer[8] = {};
    std::size_t length = 0;
    EXPECT_FALSE(read_string(find(root, "n"), buffer, sizeof(buffer), length));

    EXPECT_EQ(find(root, "o").type, JsonType::Object);
    EXPECT_EQ(find(root, "a").type, JsonType::Array);
    EXPECT_TRUE(find(root, "b").boolean);
}

TEST(JsonScanTests, EmptyObjectIsValidAndHasNoMembers) {
    JsonObject object;
    ASSERT_EQ(parse_object("{}", object), ScanStatus::Ok);
    EXPECT_FALSE(find(object, "anything").present());
}

} // namespace
} // namespace prd::prusalink
