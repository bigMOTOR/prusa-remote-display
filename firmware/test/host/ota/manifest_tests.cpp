// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only
#include "json_reader/reader.hpp"
#include "ota/ingestion.hpp"
#include "ota/manifest.hpp"
#include "producer_manifest.hpp"
#include <gtest/gtest.h>
#include <limits>
#include <string>
#include <vector>

namespace {
using namespace prd::ota;

TEST(OtaManifestTests, StrictAsciiFailureClearsLengthAndEntireOutputBuffer) {
    for (const std::string_view raw : {"abc\\u00e9", "abc\\", "abc\\q", "abcdefghijk"}) {
        std::array<char, 8> output;
        output.fill('x');
        std::size_t length = 99;
        const prd::json_reader::JsonField field{prd::json_reader::JsonType::String, raw, false};
        EXPECT_FALSE(prd::json_reader::read_ascii(field, output.data(), output.size(), length));
        EXPECT_EQ(length, 0U);
        EXPECT_EQ(output, (std::array<char, 8>{}));
    }
}

TEST(OtaManifestTests, StrictLookupStructuralFailureClearsEarlierMatch) {
    // Robustness-only: normal decoder constructs JsonObject through parse_object,
    // which rejects these documents before lookup can ever see them.
    for (const std::string_view text : {"{\"key\":1,\"broken\"}", "{\"key\":1,\"broken\":"}) {
        const prd::json_reader::JsonObject object{text, 0, text.size()};
        prd::json_reader::JsonField output{prd::json_reader::JsonType::Bool, "old", true};
        EXPECT_FALSE(prd::json_reader::find_unique(object, "key", output));
        EXPECT_EQ(output.type, prd::json_reader::JsonType::Absent);
        EXPECT_TRUE(output.raw.empty());
        EXPECT_FALSE(output.boolean);
    }
}

std::string replaced(std::string_view from, std::string_view to) {
    std::string body{kProducerManifest};
    const auto position = body.find(from);
    EXPECT_NE(position, std::string::npos);
    body.replace(position, from.size(), to);
    return body;
}

TEST(OtaManifestTests, ActualProducerFixtureDecodesOwnedFields) {
    DecodedManifest decoded;
    ASSERT_EQ(decode_manifest(kProducerManifest, decoded), ManifestStatus::Decoded);
    const auto value = decoded.candidate();
    EXPECT_EQ(value.board, "test-board");
    EXPECT_EQ(value.version, "0.1.0");
    EXPECT_EQ(value.chip, "esp32s3");
    EXPECT_EQ(value.manifest_schema, 1U);
    EXPECT_EQ(value.min_bootloader_schema, 1U);
    EXPECT_EQ(value.image_size, 1280000U);
    EXPECT_EQ(value.sha256, "f6145b86504aa0b54096c22071ba0b5b786fc735e21d853fbe25ead5db1cecc8");
}

TEST(OtaManifestTests, DestroyedInputAndCopiedOutputRetainNoBorrowedViews) {
    DecodedManifest copy;
    {
        std::string input{kProducerManifest};
        DecodedManifest first;
        ASSERT_EQ(decode_manifest(input, first), ManifestStatus::Decoded);
        copy = first;
        first = {};
        input.assign(input.size(), 'x');
    }
    EXPECT_EQ(copy.candidate().board, "test-board");
    EXPECT_EQ(copy.candidate().version, "0.1.0");
}

TEST(OtaManifestTests, FailuresClearPreviouslyDecodedOutput) {
    DecodedManifest output;
    ASSERT_EQ(decode_manifest(kProducerManifest, output), ManifestStatus::Decoded);
    EXPECT_EQ(decode_manifest("{}", output), ManifestStatus::InvalidField);
    EXPECT_TRUE(output.candidate().board.empty());
    EXPECT_EQ(output.candidate().image_size, 0U);
}

TEST(OtaManifestTests, RejectsMalformedTrailingDepthAndSize) {
    DecodedManifest output;
    for (const std::string input :
         {"", "[]", "{} garbage", "{\"x\":1,}", "{\"x\":[[[[[[[[[0]]]]]]]]] }"}) {
        EXPECT_EQ(decode_manifest(input, output), ManifestStatus::Malformed);
    }
    EXPECT_EQ(decode_manifest(std::string(kManifestBodyLimit + 1, ' '), output),
              ManifestStatus::TooLarge);
    std::string maximum{kProducerManifest};
    maximum.resize(kManifestBodyLimit, ' ');
    EXPECT_EQ(decode_manifest(maximum, output), ManifestStatus::Decoded);
}

TEST(OtaManifestTests, RelevantDuplicatesIncludingEscapedAliasesAreRejected) {
    for (const auto& body :
         {replaced("\"schemaVersion\": 1", "\"schemaVersion\":1,\"schemaVersion\":1"),
          replaced("\"schemaVersion\": 1", "\"schemaVersion\":1,\"schema\\u0056ersion\":1"),
          replaced("\"sizeBytes\": 1280000", "\"sizeBytes\":1280000,\"sizeBytes\":1280000"),
          replaced("\"image\": {", "\"im\\u0061ge\":{},\"image\": {"),
          replaced("\"minBootloaderSchema\": 1",
                   "\"minBootloaderSchema\":1,\"minBootloaderSchema\":1")}) {
        DecodedManifest output;
        EXPECT_EQ(decode_manifest(body, output), ManifestStatus::InvalidField);
    }
}

TEST(OtaManifestTests, EscapedAsciiWorksButNoNulUnicodeSubstitutionOrTruncation) {
    DecodedManifest output;
    EXPECT_EQ(decode_manifest(replaced("test-board", "test\\u002dboard"), output),
              ManifestStatus::Decoded);
    EXPECT_EQ(output.candidate().board, "test-board");
    EXPECT_EQ(decode_manifest(replaced("schemaVersion", "schema\\u0056ersion"), output),
              ManifestStatus::Decoded);
    for (const std::string& value :
         std::vector<std::string>{"test\\u0000board", "test\\u00e9board", "test\\ud800board",
                                  "test\\nboard", std::string(32, 'a')}) {
        EXPECT_EQ(decode_manifest(replaced("test-board", value), output),
                  ManifestStatus::InvalidField);
    }
}

TEST(OtaManifestTests, IntegerTokensAreExactAndOverflowIsRejected) {
    DecodedManifest output;
    for (const std::string value :
         {"1.0", "1e0", "-1", "true", "null", "\"1\"", "18446744073709551616"}) {
        EXPECT_EQ(decode_manifest(replaced("1280000", value), output),
                  ManifestStatus::InvalidField);
    }
    EXPECT_EQ(decode_manifest(replaced("1280000", "9007199254740993"), output),
              ManifestStatus::Decoded);
    EXPECT_EQ(output.image_size, UINT64_C(9007199254740993));
    EXPECT_EQ(decode_manifest(replaced("1280000", "18446744073709551615"), output),
              ManifestStatus::Decoded);
    EXPECT_EQ(output.image_size, std::numeric_limits<std::uint64_t>::max());
    EXPECT_EQ(
        decode_manifest(replaced("\"schemaVersion\": 1", "\"schemaVersion\":4294967296"), output),
        ManifestStatus::InvalidField);
}

TEST(OtaManifestTests, RequiredTypesAndLengthsAreNotCoerced) {
    DecodedManifest output;
    for (const auto& body :
         {replaced("\"boardTarget\"", "\"other\""), replaced("\"test-board\"", "null"),
          replaced("\"image\": {", "\"image\": null, \"ignored\": {"),
          replaced("f6145b86504aa0b54096c22071ba0b5b786fc735e21d853fbe25ead5db1cecc8", "abc")}) {
        EXPECT_EQ(decode_manifest(body, output), ManifestStatus::InvalidField);
    }
}

TEST(OtaManifestTests, DocumentedUnknownKeysCannotAliasRequiredAsciiNames) {
    for (const std::string& key :
         std::vector<std::string>{"boardTarget\\u0000extra", "\\u00e9", std::string(65, 'a')}) {
        DecodedManifest output;
        const auto body = replaced("\"boardTarget\":", "\"" + key + "\":null,\"boardTarget\":");
        ASSERT_EQ(decode_manifest(body, output), ManifestStatus::Decoded);
        EXPECT_EQ(output.candidate().board, "test-board");
    }
}

TEST(OtaManifestTests, FixedStringAndSchemaBoundariesRejectInsteadOfTruncate) {
    DecodedManifest output;
    EXPECT_EQ(decode_manifest(replaced("test-board", ""), output), ManifestStatus::InvalidField);
    EXPECT_EQ(decode_manifest(replaced("test-board", std::string(31, 'a')), output),
              ManifestStatus::Decoded);
    EXPECT_EQ(output.candidate().board.size(), 31U);
    constexpr std::string_view digest =
        "f6145b86504aa0b54096c22071ba0b5b786fc735e21d853fbe25ead5db1cecc8";
    EXPECT_EQ(decode_manifest(replaced(digest, std::string(64, 'a')), output),
              ManifestStatus::Decoded);
    EXPECT_EQ(output.candidate().sha256.size(), 64U);
    EXPECT_EQ(decode_manifest(replaced(digest, std::string(65, 'a')), output),
              ManifestStatus::InvalidField);
    EXPECT_EQ(
        decode_manifest(
            replaced("\"minBootloaderSchema\": 1", "\"minBootloaderSchema\":4294967296"), output),
        ManifestStatus::InvalidField);
}

class UntouchedPort final : public IngestionPort {
  public:
    IngestionBegin begin(std::uint64_t) override {
        ++calls;
        return IngestionBegin::Refused;
    }
    bool write(const std::uint8_t*, std::size_t) override {
        ++calls;
        return false;
    }
    bool finish_digest(ImageDigest&) override {
        ++calls;
        return false;
    }
    bool finalize(ImageIdentity&) override {
        ++calls;
        return false;
    }
    void abort() override {
        ++calls;
    }
    int calls{0};
};

TEST(OtaManifestTests, DecodingDoesNotGrantCompatibilityOrTouchStorage) {
    const DeviceCompatibility device{"test-board", "esp32s3", "0.1.0", 1, 2097152};
    for (const auto& body :
         {replaced("test-board", "wrong-board"), replaced("0.1.0", "0.0.1"),
          replaced("1280000", "2097153"), replaced("esp32s3", "esp32"),
          replaced("\"schemaVersion\": 1", "\"schemaVersion\":2"),
          replaced("f6145b86504aa0b54096c22071ba0b5b786fc735e21d853fbe25ead5db1cecc8",
                   std::string(64, 'z'))}) {
        DecodedManifest decoded;
        ASSERT_EQ(decode_manifest(body, decoded), ManifestStatus::Decoded);
        UntouchedPort port;
        ImageIngestion ingestion{port};
        EXPECT_FALSE(ingestion.begin(decoded.candidate(), device, DowngradePolicy::Reject));
        EXPECT_EQ(port.calls, 0);
    }
}
} // namespace
