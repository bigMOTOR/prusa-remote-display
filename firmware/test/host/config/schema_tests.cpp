// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include "schema.hpp"

#include <array>
#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

#include "config/config.hpp"
#include "storage/checksum.hpp"

// The persisted record format, its validator and its migration dispatch.
//
// THE RECORD FORMAT IS A CONTRACT WITH DEVICES ALREADY IN THE FIELD. A record written by
// this firmware must be readable by the next one, so the encoding is pinned here field by
// field rather than assumed to round-trip. A change that breaks one of these is a schema
// migration, not an edit.

namespace {

using prd::app_model::AuthMethod;
using prd::config::ConfigV1;
using prd::config::DecodeOutcome;
using prd::config::kHeaderSize;
using prd::config::kMaxRecordSize;
using prd::config::kSchemaVersion;
using prd::config::UpdateChannel;

[[nodiscard]] ConfigV1 configured() {
    ConfigV1 value = prd::config::defaults();
    value.wifi_profile_id = 7;
    value.printer_host.assign("printer.local");
    value.printer_port = 80;
    value.printer_alias.assign("Workshop");
    value.auth_method = AuthMethod::ApiKey;
    value.brightness = 55;
    value.dim_delay_seconds = 120;
    value.sleep_delay_seconds = 900;
    return value;
}

[[nodiscard]] std::vector<std::uint8_t> encode_to_vector(const ConfigV1& value,
                                                         const std::uint32_t generation) {
    std::array<std::uint8_t, kMaxRecordSize> buffer{};
    const std::size_t length = prd::config::encode(value, generation, buffer.data(), buffer.size());
    return std::vector<std::uint8_t>{buffer.begin(), buffer.begin() + static_cast<long>(length)};
}

// Recomputes the record's checksum in place, so a test can change a header field and still
// present a record that is intact — which is the only way to reach the cases decode()
// reports after the checksum has passed.
void reseal(std::vector<std::uint8_t>& bytes) {
    const std::uint32_t checksum =
        prd::storage::crc32_update(prd::storage::crc32(bytes.data(), kHeaderSize - 4),
                                   bytes.data() + kHeaderSize, bytes.size() - kHeaderSize);

    for (std::size_t index = 0; index < 4; ++index) {
        bytes[kHeaderSize - 4 + index] =
            static_cast<std::uint8_t>((checksum >> (8U * index)) & 0xFFU);
    }
}

// --- Defaults and validation ------------------------------------------------

// A default that failed its own validator would make every factory-fresh device report
// Invalid instead of Missing, and would never reach provisioning.
TEST(ConfigSchemaTests, TheDefaultsAreValid) {
    EXPECT_TRUE(prd::config::validate(prd::config::defaults()));
}

TEST(ConfigSchemaTests, TheDefaultsDescribeNoPrinterAndNoWifi) {
    const ConfigV1 value = prd::config::defaults();

    EXPECT_TRUE(value.printer_host.empty());
    EXPECT_EQ(value.printer_port, 0U);
    EXPECT_FALSE(value.printer_validated);
    EXPECT_EQ(value.auth_method, AuthMethod::None);
    EXPECT_EQ(value.wifi_profile_id, 0U);
}

TEST(ConfigSchemaTests, BrightnessBeyondOneHundredIsRejected) {
    ConfigV1 value = prd::config::defaults();
    value.brightness = 101;

    EXPECT_FALSE(prd::config::validate(value));
}

TEST(ConfigSchemaTests, AnOverlongDelayIsRejected) {
    ConfigV1 dim = prd::config::defaults();
    dim.dim_delay_seconds = 24U * 60U * 60U + 1U;
    EXPECT_FALSE(prd::config::validate(dim));

    ConfigV1 sleep = prd::config::defaults();
    sleep.sleep_delay_seconds = 24U * 60U * 60U + 1U;
    EXPECT_FALSE(prd::config::validate(sleep));
}

TEST(ConfigSchemaTests, AnUnknownAuthMethodOrChannelIsRejected) {
    ConfigV1 auth = configured();
    auth.auth_method = static_cast<AuthMethod>(0x7F);
    EXPECT_FALSE(prd::config::validate(auth));

    ConfigV1 channel = prd::config::defaults();
    channel.update_channel = static_cast<UpdateChannel>(0x7F);
    EXPECT_FALSE(prd::config::validate(channel));
}

// Without a host there is no printer, so nothing about one may be asserted — otherwise
// R8's three-way distinction cannot be read back out of the record.
TEST(ConfigSchemaTests, PrinterFieldsWithoutAHostAreRejected) {
    ConfigV1 port = prd::config::defaults();
    port.printer_port = 80;
    EXPECT_FALSE(prd::config::validate(port));

    ConfigV1 validated = prd::config::defaults();
    validated.printer_validated = true;
    EXPECT_FALSE(prd::config::validate(validated));

    ConfigV1 auth = prd::config::defaults();
    auth.auth_method = AuthMethod::ApiKey;
    EXPECT_FALSE(prd::config::validate(auth));
}

// Owner ruling, 2026-08-15: printerPort is persisted and validated, and introduces no
// protocol behaviour. 0 stays legal because choosing a default port is a Milestone 4
// decision.
TEST(ConfigSchemaTests, AnUnsetPortIsLegalAlongsideAHost) {
    ConfigV1 value = configured();
    value.printer_port = 0;

    EXPECT_TRUE(prd::config::validate(value));
}

TEST(ConfigSchemaTests, ATruncatedStringIsRejected) {
    ConfigV1 value = configured();
    value.printer_alias.assign(std::string(64, 'x'));
    ASSERT_TRUE(value.printer_alias.truncated());

    EXPECT_FALSE(prd::config::validate(value));
}

// --- Round trip --------------------------------------------------------------

TEST(ConfigSchemaTests, EveryFieldSurvivesARoundTrip) {
    const ConfigV1 original = configured();
    const auto bytes = encode_to_vector(original, 42);
    ASSERT_FALSE(bytes.empty());

    const auto decoded = prd::config::decode(bytes.data(), bytes.size());

    ASSERT_EQ(decoded.outcome, DecodeOutcome::Ok);
    EXPECT_EQ(decoded.generation, 42U);
    EXPECT_EQ(decoded.schema_version, kSchemaVersion);
    EXPECT_EQ(decoded.value, original);
}

TEST(ConfigSchemaTests, ThePendingFlagSurvivesARoundTrip) {
    for (const bool validated : {false, true}) {
        ConfigV1 value = configured();
        value.printer_validated = validated;

        const auto bytes = encode_to_vector(value, 1);
        const auto decoded = prd::config::decode(bytes.data(), bytes.size());

        ASSERT_EQ(decoded.outcome, DecodeOutcome::Ok);
        EXPECT_EQ(decoded.value.printer_validated, validated);
    }
}

TEST(ConfigSchemaTests, TheEmptyConfigurationRoundTrips) {
    const ConfigV1 original = prd::config::defaults();
    const auto bytes = encode_to_vector(original, 1);

    const auto decoded = prd::config::decode(bytes.data(), bytes.size());

    ASSERT_EQ(decoded.outcome, DecodeOutcome::Ok);
    EXPECT_EQ(decoded.value, original);
}

// The buffer bound in schema.hpp is asserted against the real encoder rather than trusted:
// if a field is added and the constant is not raised, this fails instead of the encoder
// silently returning 0 on a device.
TEST(ConfigSchemaTests, TheLargestRecordFitsTheDeclaredBuffer) {
    ConfigV1 value = configured();
    value.printer_host.assign(std::string(64, 'h'));
    value.printer_alias.assign(std::string(32, 'a'));
    ASSERT_FALSE(value.printer_host.truncated());
    ASSERT_FALSE(value.printer_alias.truncated());

    const auto bytes = encode_to_vector(value, 0xFFFFFFFFU);

    ASSERT_FALSE(bytes.empty());
    EXPECT_LE(bytes.size(), kMaxRecordSize);
}

TEST(ConfigSchemaTests, EncodingIntoATooSmallBufferWritesNothing) {
    std::array<std::uint8_t, kHeaderSize> buffer{};

    EXPECT_EQ(prd::config::encode(configured(), 1, buffer.data(), buffer.size()), 0U);
}

// --- Corruption --------------------------------------------------------------

TEST(ConfigSchemaTests, AFlippedPayloadBitIsCaughtByTheChecksum) {
    auto bytes = encode_to_vector(configured(), 3);
    ASSERT_GT(bytes.size(), kHeaderSize);

    bytes[kHeaderSize + 1] ^= 0x01U;

    EXPECT_EQ(prd::config::decode(bytes.data(), bytes.size()).outcome,
              DecodeOutcome::ChecksumMismatch);
}

// The checksum covers the header too, so a torn write that leaves a stale generation
// beside a fresh payload is corruption rather than a plausible older record.
TEST(ConfigSchemaTests, AFlippedGenerationBitIsCaughtByTheChecksum) {
    auto bytes = encode_to_vector(configured(), 3);

    bytes[8] ^= 0x01U;

    EXPECT_EQ(prd::config::decode(bytes.data(), bytes.size()).outcome,
              DecodeOutcome::ChecksumMismatch);
}

TEST(ConfigSchemaTests, ForeignOrTruncatedBytesAreMalformed) {
    const auto bytes = encode_to_vector(configured(), 1);

    auto wrong_magic = bytes;
    wrong_magic[0] ^= 0xFFU;
    EXPECT_EQ(prd::config::decode(wrong_magic.data(), wrong_magic.size()).outcome,
              DecodeOutcome::Malformed);

    EXPECT_EQ(prd::config::decode(bytes.data(), kHeaderSize - 1).outcome, DecodeOutcome::Malformed);
    EXPECT_EQ(prd::config::decode(nullptr, 0).outcome, DecodeOutcome::Malformed);

    // A declared payload length that disagrees with the bytes present.
    EXPECT_EQ(prd::config::decode(bytes.data(), bytes.size() - 1).outcome,
              DecodeOutcome::Malformed);
}

// A record from a NEWER firmware is intact, not corrupt, and must be reported as its own
// case — §13.4 makes it a migration question and R10 forbids rewriting it.
TEST(ConfigSchemaTests, ARecordFromANewerSchemaIsReportedAsSuchNotAsCorruption) {
    auto bytes = encode_to_vector(configured(), 1);

    // schema_version sits at offset 6; re-seal so only the version differs.
    bytes[6] = static_cast<std::uint8_t>(kSchemaVersion + 1);
    reseal(bytes);

    const auto decoded = prd::config::decode(bytes.data(), bytes.size());

    EXPECT_EQ(decoded.outcome, DecodeOutcome::SchemaTooNew);
    EXPECT_EQ(decoded.schema_version, kSchemaVersion + 1);
}

// --- Migration ---------------------------------------------------------------

TEST(ConfigSchemaTests, TheCurrentSchemaNeedsNoMigration) {
    ConfigV1 value = configured();
    const ConfigV1 before = value;

    EXPECT_TRUE(prd::config::migrate(value, kSchemaVersion));
    EXPECT_EQ(value, before);
}

// §13.4: explicit and one-way. An unknown version is refused rather than guessed at, and
// the caller reports it — nothing is rewritten.
TEST(ConfigSchemaTests, AnUnknownSchemaVersionIsRefused) {
    ConfigV1 value = configured();

    EXPECT_FALSE(prd::config::migrate(value, 0));
    EXPECT_FALSE(prd::config::migrate(value, kSchemaVersion + 1));
    EXPECT_FALSE(prd::config::migrate(value, 0xFFFF));
}

} // namespace
