// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include "secret_schema.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "secrets/secrets.hpp"

// The persisted credential record.
//
// SAME CONTRACT AS THE CONFIGURATION RECORD, AND FOR A HIGHER STAKE. A credential that
// round-trips wrongly does not present as corruption — it presents as an authentication
// failure the user cannot explain. So the encoding is pinned field by field rather than
// assumed to survive.

namespace {

using prd::secrets::DecodeOutcome;
using prd::secrets::kHeaderSize;
using prd::secrets::kMaxRecordSize;
using prd::secrets::kSchemaVersion;
using prd::secrets::SecretConfigV1;

[[nodiscard]] SecretConfigV1 populated() {
    SecretConfigV1 value = prd::secrets::defaults();
    value.profile_id = 5;
    value.wifi_ssid.assign("Workshop-5G");
    value.wifi_password.assign("correct horse battery staple");
    value.printer_api_key.assign("abcdefghijklmnop");
    value.printer_username.assign("maker");
    value.printer_password.assign("hunter2");
    return value;
}

[[nodiscard]] std::vector<std::uint8_t> encode_to_vector(const SecretConfigV1& value,
                                                         const std::uint32_t generation) {
    std::array<std::uint8_t, kMaxRecordSize> buffer{};
    const std::size_t length =
        prd::secrets::encode(value, generation, buffer.data(), buffer.size());
    return std::vector<std::uint8_t>{buffer.begin(), buffer.begin() + static_cast<long>(length)};
}

void reseal(std::vector<std::uint8_t>& bytes) {
    const std::uint32_t checksum =
        prd::storage::crc32_update(prd::storage::crc32(bytes.data(), kHeaderSize - 4),
                                   bytes.data() + kHeaderSize, bytes.size() - kHeaderSize);

    for (std::size_t index = 0; index < 4; ++index) {
        bytes[kHeaderSize - 4 + index] =
            static_cast<std::uint8_t>((checksum >> (8U * index)) & 0xFFU);
    }
}

// --- Defaults and validation --------------------------------------------------

TEST(SecretsSchemaTests, TheDefaultsAreValidAndEmpty) {
    const SecretConfigV1 value = prd::secrets::defaults();

    EXPECT_TRUE(prd::secrets::validate(value));
    EXPECT_EQ(value.profile_id, 0U);
    EXPECT_TRUE(value.wifi_ssid.empty());
    EXPECT_TRUE(value.wifi_password.empty());
    EXPECT_TRUE(value.printer_api_key.empty());
}

// A truncated credential is worse than a missing one: it fails at the moment it is used,
// with no cause the user can see.
TEST(SecretsSchemaTests, ATruncatedCredentialIsRejected) {
    for (int field = 0; field < 5; ++field) {
        SecretConfigV1 value = populated();

        switch (field) {
        case 0:
            value.wifi_ssid.assign(std::string(64, 'x'));
            break;
        case 1:
            value.wifi_password.assign(std::string(128, 'x'));
            break;
        case 2:
            value.printer_api_key.assign(std::string(128, 'x'));
            break;
        case 3:
            value.printer_username.assign(std::string(64, 'x'));
            break;
        default:
            value.printer_password.assign(std::string(128, 'x'));
            break;
        }

        EXPECT_FALSE(prd::secrets::validate(value)) << "field " << field;
    }
}

// An open network legitimately has an SSID and no password, so only this direction is an
// inconsistency.
TEST(SecretsSchemaTests, APasswordWithoutAnSsidIsRejected) {
    SecretConfigV1 value = prd::secrets::defaults();
    value.profile_id = 1;
    value.wifi_password.assign("orphan");

    EXPECT_FALSE(prd::secrets::validate(value));
}

TEST(SecretsSchemaTests, AnOpenNetworkIsAccepted) {
    SecretConfigV1 value = prd::secrets::defaults();
    value.profile_id = 1;
    value.wifi_ssid.assign("Open-Guest");

    EXPECT_TRUE(prd::secrets::validate(value));
}

// The handle is how app_core decides whether these credentials still belong to the
// configured profile. Either half alone makes that question unanswerable.
TEST(SecretsSchemaTests, TheAssociationHandleMustAgreeWithTheStoredNetwork) {
    SecretConfigV1 unassociated = prd::secrets::defaults();
    unassociated.wifi_ssid.assign("Workshop");
    unassociated.profile_id = 0;
    EXPECT_FALSE(prd::secrets::validate(unassociated));

    SecretConfigV1 orphan_handle = prd::secrets::defaults();
    orphan_handle.profile_id = 7;
    EXPECT_FALSE(prd::secrets::validate(orphan_handle));
}

// --- Round trip ----------------------------------------------------------------

TEST(SecretsSchemaTests, EveryFieldSurvivesARoundTrip) {
    const SecretConfigV1 original = populated();
    const auto bytes = encode_to_vector(original, 9);
    ASSERT_FALSE(bytes.empty());

    const auto decoded = prd::secrets::decode(bytes.data(), bytes.size());

    ASSERT_EQ(decoded.outcome, DecodeOutcome::Ok);
    EXPECT_EQ(decoded.generation, 9U);
    EXPECT_EQ(decoded.schema_version, kSchemaVersion);
    EXPECT_EQ(decoded.value, original);
}

TEST(SecretsSchemaTests, TheEmptyRecordRoundTrips) {
    const SecretConfigV1 original = prd::secrets::defaults();
    const auto bytes = encode_to_vector(original, 1);

    const auto decoded = prd::secrets::decode(bytes.data(), bytes.size());

    ASSERT_EQ(decoded.outcome, DecodeOutcome::Ok);
    EXPECT_EQ(decoded.value, original);
}

TEST(SecretsSchemaTests, TheLargestRecordFitsTheDeclaredBuffer) {
    SecretConfigV1 value = prd::secrets::defaults();
    value.profile_id = 0xFFFFFFFFU;
    value.wifi_ssid.assign(std::string(32, 's'));
    value.wifi_password.assign(std::string(64, 'p'));
    value.printer_api_key.assign(std::string(64, 'k'));
    value.printer_username.assign(std::string(32, 'u'));
    value.printer_password.assign(std::string(64, 'w'));
    ASSERT_TRUE(prd::secrets::validate(value));

    const auto bytes = encode_to_vector(value, 0xFFFFFFFFU);

    ASSERT_FALSE(bytes.empty());
    EXPECT_LE(bytes.size(), kMaxRecordSize);
}

// A record written with the CONFIGURATION magic must not be parsed as a credential record,
// and the reverse. Distinct magics are what make a record written into the wrong namespace
// a rejection rather than a misreading.
TEST(SecretsSchemaTests, TheConfigurationMagicIsNotAccepted) {
    auto bytes = encode_to_vector(populated(), 1);

    // 'P','R','D','C' little-endian, in place of 'P','R','D','S'.
    bytes[0] = 0x43;
    reseal(bytes);

    EXPECT_EQ(prd::secrets::decode(bytes.data(), bytes.size()).outcome, DecodeOutcome::Malformed);
}

// --- Corruption ------------------------------------------------------------------

TEST(SecretsSchemaTests, AFlippedPayloadBitIsCaughtByTheChecksum) {
    auto bytes = encode_to_vector(populated(), 3);
    ASSERT_GT(bytes.size(), kHeaderSize);

    bytes[kHeaderSize + 8] ^= 0x01U;

    EXPECT_EQ(prd::secrets::decode(bytes.data(), bytes.size()).outcome,
              DecodeOutcome::ChecksumMismatch);
}

TEST(SecretsSchemaTests, AFlippedGenerationBitIsCaughtByTheChecksum) {
    auto bytes = encode_to_vector(populated(), 3);

    bytes[8] ^= 0x01U;

    EXPECT_EQ(prd::secrets::decode(bytes.data(), bytes.size()).outcome,
              DecodeOutcome::ChecksumMismatch);
}

TEST(SecretsSchemaTests, ForeignOrTruncatedBytesAreMalformed) {
    const auto bytes = encode_to_vector(populated(), 1);

    EXPECT_EQ(prd::secrets::decode(bytes.data(), kHeaderSize - 1).outcome,
              DecodeOutcome::Malformed);
    EXPECT_EQ(prd::secrets::decode(nullptr, 0).outcome, DecodeOutcome::Malformed);
    EXPECT_EQ(prd::secrets::decode(bytes.data(), bytes.size() - 1).outcome,
              DecodeOutcome::Malformed);
}

TEST(SecretsSchemaTests, ARecordFromANewerSchemaIsReportedAsSuchNotAsCorruption) {
    auto bytes = encode_to_vector(populated(), 1);

    bytes[6] = static_cast<std::uint8_t>(kSchemaVersion + 1);
    reseal(bytes);

    const auto decoded = prd::secrets::decode(bytes.data(), bytes.size());

    EXPECT_EQ(decoded.outcome, DecodeOutcome::SchemaTooNew);
    EXPECT_EQ(decoded.schema_version, kSchemaVersion + 1);
}

// --- Migration --------------------------------------------------------------------

TEST(SecretsSchemaTests, TheCurrentSchemaNeedsNoMigration) {
    SecretConfigV1 value = populated();
    const SecretConfigV1 before = value;

    EXPECT_TRUE(prd::secrets::migrate(value, kSchemaVersion));
    EXPECT_EQ(value, before);
}

// §13.4 names this case: "migration failure must not erase secrets automatically".
TEST(SecretsSchemaTests, AnUnknownSchemaVersionIsRefusedAndChangesNothing) {
    SecretConfigV1 value = populated();
    const SecretConfigV1 before = value;

    EXPECT_FALSE(prd::secrets::migrate(value, 0));
    EXPECT_FALSE(prd::secrets::migrate(value, kSchemaVersion + 1));
    EXPECT_EQ(value, before);
}

} // namespace
