// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include <cstdint>
#include <string>

#include <gtest/gtest.h>

#include "secret_schema.hpp"
#include "secrets/secrets.hpp"
#include "storage/key_value.hpp"
#include "support/fake_key_value.hpp"

// The credential record's staged write, its selection, and §13.5's handling rules.
//
// THE RULE THIS FILE EXISTS FOR: a credential must survive an interrupted write, and must
// never reach a log. The first is the same staged-write property the configuration record
// has, re-asserted here because a lost password is a worse outcome than a lost brightness
// setting. The second is §13.5, and it is pinned by driving describe() with every field
// populated and searching its output for the material.

namespace {

using prd::secrets::LoadOutcome;
using prd::secrets::SaveOutcome;
using prd::secrets::SecretConfigV1;
using prd::storage::Namespace;
using prd::test::FakeKeyValue;

constexpr const char* kSlot0 = "sec.0";
constexpr const char* kSlot1 = "sec.1";
constexpr const char* kPointer = "sec.gen";

// Distinctive enough that a substring search cannot match by accident.
constexpr const char* kSsid = "Workshop-5G-Zx";
constexpr const char* kWifiPassword = "correct-horse-battery-staple-42";
constexpr const char* kApiKey = "QQQ7f3a9c1e5b2d8a4";
constexpr const char* kUsername = "maker-jj";
constexpr const char* kPrinterPassword = "hunter2-vvv-9";

[[nodiscard]] SecretConfigV1 populated(const std::uint32_t profile = 5) {
    SecretConfigV1 value = prd::secrets::defaults();
    value.profile_id = profile;
    value.wifi_ssid.assign(kSsid);
    value.wifi_password.assign(kWifiPassword);
    value.printer_api_key.assign(kApiKey);
    value.printer_username.assign(kUsername);
    value.printer_password.assign(kPrinterPassword);
    return value;
}

// --- §13.5: nothing reaches a log --------------------------------------------

// The strongest form of "this line cannot contain a secret" is a builder that is never
// given one. describe() takes an outcome, a generation and five booleans; this drives it
// with a fully populated record and asserts none of the material appears.
TEST(SecretsRecordTests, TheDiagnosticLineContainsNoCredentialMaterial) {
    const SecretConfigV1 value = populated();
    const std::string line =
        prd::secrets::describe(LoadOutcome::Loaded, 4, prd::secrets::presence_of(value));

    for (const char* material : {kSsid, kWifiPassword, kApiKey, kUsername, kPrinterPassword}) {
        EXPECT_EQ(line.find(material), std::string::npos) << "leaked: " << material;
    }
}

TEST(SecretsRecordTests, TheDiagnosticLineStillReportsWhatIsPresent) {
    const std::string full =
        prd::secrets::describe(LoadOutcome::Loaded, 4, prd::secrets::presence_of(populated()));

    EXPECT_NE(full.find("ssid=yes"), std::string::npos);
    EXPECT_NE(full.find("wifi_password=yes"), std::string::npos);
    EXPECT_NE(full.find("api_key=yes"), std::string::npos);
    EXPECT_NE(full.find("generation=4"), std::string::npos);

    const std::string empty = prd::secrets::describe(
        LoadOutcome::Absent, 0, prd::secrets::presence_of(prd::secrets::defaults()));

    EXPECT_NE(empty.find("ssid=no"), std::string::npos);
    EXPECT_NE(empty.find("api_key=no"), std::string::npos);
}

TEST(SecretsRecordTests, PresenceReportsEachFieldIndependently) {
    SecretConfigV1 value = prd::secrets::defaults();
    value.profile_id = 1;
    value.wifi_ssid.assign(kSsid);
    value.printer_api_key.assign(kApiKey);

    const auto presence = prd::secrets::presence_of(value);

    EXPECT_TRUE(presence.wifi_ssid);
    EXPECT_FALSE(presence.wifi_password);
    EXPECT_TRUE(presence.printer_api_key);
    EXPECT_FALSE(presence.printer_username);
    EXPECT_FALSE(presence.printer_password);
}

// --- First boot and the happy path --------------------------------------------

TEST(SecretsRecordTests, AnEmptyStoreReportsAbsent) {
    FakeKeyValue store;

    const auto report = prd::secrets::load(store);

    EXPECT_EQ(report.outcome, LoadOutcome::Absent);
    EXPECT_EQ(report.generation, 0U);
}

TEST(SecretsRecordTests, TheFirstSaveCommitsGenerationOneAndLoadsBack) {
    FakeKeyValue store;
    const SecretConfigV1 value = populated();

    ASSERT_EQ(prd::secrets::save(store, value), SaveOutcome::Committed);

    const auto report = prd::secrets::load(store);

    EXPECT_EQ(report.outcome, LoadOutcome::Loaded);
    EXPECT_EQ(report.generation, 1U);
    EXPECT_EQ(report.value, value);
}

// The credential record must not collide with the configuration record's keys, or one
// would overwrite the other in a shared namespace.
TEST(SecretsRecordTests, TheRecordsLiveInTheSecretsNamespaceUnderTheirOwnKeys) {
    FakeKeyValue store;
    ASSERT_EQ(prd::secrets::save(store, populated()), SaveOutcome::Committed);

    EXPECT_TRUE(store.has(Namespace::Secrets, kSlot1));
    EXPECT_TRUE(store.has(Namespace::System, kPointer));

    EXPECT_FALSE(store.has(Namespace::Config, kSlot1));
    EXPECT_FALSE(store.has(Namespace::Secrets, "cfg.0"));
    EXPECT_FALSE(store.has(Namespace::System, "cfg.gen"));
}

TEST(SecretsRecordTests, AnInconsistentRecordIsRejectedAndNothingIsWritten) {
    FakeKeyValue store;

    SecretConfigV1 invalid = prd::secrets::defaults();
    invalid.wifi_password.assign("orphan");

    EXPECT_EQ(prd::secrets::save(store, invalid), SaveOutcome::Rejected);
    EXPECT_FALSE(store.has(Namespace::Secrets, kSlot0));
    EXPECT_FALSE(store.has(Namespace::Secrets, kSlot1));
    EXPECT_FALSE(store.has(Namespace::System, kPointer));
}

// --- A failed save never loses the working credentials -------------------------

TEST(SecretsRecordTests, AFailedCandidateWriteLeavesTheActiveCredentialsRunning) {
    FakeKeyValue store;
    ASSERT_EQ(prd::secrets::save(store, populated(1)), SaveOutcome::Committed);

    store.fail_write(Namespace::Secrets, kSlot0);

    EXPECT_EQ(prd::secrets::save(store, populated(2)), SaveOutcome::CandidateFailed);

    const auto report = prd::secrets::load(store);
    EXPECT_EQ(report.outcome, LoadOutcome::Loaded);
    EXPECT_EQ(report.generation, 1U);
    EXPECT_EQ(report.value.profile_id, 1U);
}

TEST(SecretsRecordTests, AFailedCommitLeavesThePreviousGenerationActive) {
    FakeKeyValue store;
    ASSERT_EQ(prd::secrets::save(store, populated(1)), SaveOutcome::Committed);

    store.fail_write(Namespace::System, kPointer);

    EXPECT_EQ(prd::secrets::save(store, populated(2)), SaveOutcome::CommitFailed);

    const auto report = prd::secrets::load(store);
    EXPECT_EQ(report.generation, 1U);
    EXPECT_EQ(report.value.profile_id, 1U);
}

TEST(SecretsRecordTests, AnAbandonedCandidateIsNotPreferredOverThePointer) {
    FakeKeyValue store;
    ASSERT_EQ(prd::secrets::save(store, populated(1)), SaveOutcome::Committed);

    store.fail_write(Namespace::System, kPointer);
    ASSERT_EQ(prd::secrets::save(store, populated(2)), SaveOutcome::CommitFailed);

    EXPECT_EQ(prd::secrets::load(store).value.profile_id, 1U);
}

TEST(SecretsRecordTests, ATornCandidateSlotDoesNotDisturbTheActiveRecord) {
    FakeKeyValue store;
    ASSERT_EQ(prd::secrets::save(store, populated(1)), SaveOutcome::Committed);
    ASSERT_EQ(prd::secrets::save(store, populated(2)), SaveOutcome::Committed);

    auto torn = store.peek(Namespace::Secrets, kSlot1);
    torn.resize(torn.size() / 2);
    store.poke(Namespace::Secrets, kSlot1, torn);

    const auto report = prd::secrets::load(store);

    EXPECT_EQ(report.outcome, LoadOutcome::Loaded);
    EXPECT_EQ(report.generation, 2U);
    EXPECT_EQ(report.value.profile_id, 2U);
}

// --- Previous-known-good --------------------------------------------------------

TEST(SecretsRecordTests, ACorruptActiveRecordFallsBackToThePreviousGeneration) {
    FakeKeyValue store;
    ASSERT_EQ(prd::secrets::save(store, populated(1)), SaveOutcome::Committed);
    ASSERT_EQ(prd::secrets::save(store, populated(2)), SaveOutcome::Committed);

    auto corrupted = store.peek(Namespace::Secrets, kSlot0);
    corrupted[prd::secrets::kHeaderSize + 8] ^= 0x01U;
    store.poke(Namespace::Secrets, kSlot0, corrupted);

    const auto report = prd::secrets::load(store);

    EXPECT_EQ(report.outcome, LoadOutcome::LoadedFromPreviousGeneration);
    EXPECT_EQ(report.generation, 1U);
    EXPECT_EQ(report.value.profile_id, 1U);
}

// The property config's finish() bug broke: a corrupt record must not arrive as an empty
// but valid credential set — which for secrets means silently forgetting the password.
TEST(SecretsRecordTests, ACorruptRecordNeverLoadsAsEmptyCredentials) {
    FakeKeyValue store;
    ASSERT_EQ(prd::secrets::save(store, populated()), SaveOutcome::Committed);

    auto corrupted = store.peek(Namespace::Secrets, kSlot1);
    corrupted[prd::secrets::kHeaderSize + 8] ^= 0x01U;
    store.poke(Namespace::Secrets, kSlot1, corrupted);

    const auto report = prd::secrets::load(store);

    EXPECT_EQ(report.outcome, LoadOutcome::Invalid);
    EXPECT_EQ(report.generation, 0U);
    EXPECT_EQ(report.value, prd::secrets::defaults())
        << "an Invalid report must carry nothing, not a plausible blank record";
}

TEST(SecretsRecordTests, AStoreFailureIsReportedAsStorageFailureNotAsAbsent) {
    FakeKeyValue store;
    store.fail_read(Namespace::System, kPointer);

    EXPECT_EQ(prd::secrets::load(store).outcome, LoadOutcome::StorageFailure);
}

TEST(SecretsRecordTests, ARecordFromANewerSchemaReportsMigrationRequired) {
    FakeKeyValue store;
    ASSERT_EQ(prd::secrets::save(store, populated()), SaveOutcome::Committed);

    auto bytes = store.peek(Namespace::Secrets, kSlot1);
    bytes[6] = static_cast<std::uint8_t>(prd::secrets::kSchemaVersion + 1);
    const std::uint32_t checksum = prd::storage::crc32_update(
        prd::storage::crc32(bytes.data(), prd::secrets::kHeaderSize - 4),
        bytes.data() + prd::secrets::kHeaderSize, bytes.size() - prd::secrets::kHeaderSize);
    for (std::size_t index = 0; index < 4; ++index) {
        bytes[prd::secrets::kHeaderSize - 4 + index] =
            static_cast<std::uint8_t>((checksum >> (8U * index)) & 0xFFU);
    }
    store.poke(Namespace::Secrets, kSlot1, bytes);

    EXPECT_EQ(prd::secrets::load(store).outcome, LoadOutcome::MigrationRequired);
    EXPECT_TRUE(store.has(Namespace::Secrets, kSlot1)) << "§13.4: never erased";
}

// --- R10, restated for the component that holds credentials ---------------------

TEST(SecretsRecordTests, NoLoadOrSavePathEverErasesAnything) {
    FakeKeyValue store;

    ASSERT_EQ(prd::secrets::save(store, populated(1)), SaveOutcome::Committed);
    ASSERT_EQ(prd::secrets::save(store, populated(2)), SaveOutcome::Committed);

    auto bytes = store.peek(Namespace::Secrets, kSlot0);
    bytes[prd::secrets::kHeaderSize + 8] ^= 0x01U;
    store.poke(Namespace::Secrets, kSlot0, bytes);

    (void)prd::secrets::load(store);

    SecretConfigV1 invalid = prd::secrets::defaults();
    invalid.wifi_password.assign("orphan");
    (void)prd::secrets::save(store, invalid);

    store.fail_write(Namespace::Secrets, kSlot0);
    (void)prd::secrets::save(store, populated(3));

    store.fail_read(Namespace::System, kPointer);
    (void)prd::secrets::load(store);

    EXPECT_EQ(store.erase_key_calls(), 0U);
}

} // namespace
