// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

#include "app_model/device_data.hpp"
#include "config/config.hpp"
#include "schema.hpp"
#include "storage/key_value.hpp"
#include "support/fake_key_value.hpp"

// The §13.3 staged write, the active-record selection and the projection into the model.
//
// TWO PROPERTIES CARRY THIS FILE. A failed save must never damage the record the device is
// currently running — that is what the candidate-into-the-inactive-slot design buys, and
// it is asserted by breaking each step in turn. And NOTHING may erase: ruling R10 says a
// record that cannot be read is reported and left where it is, and the last test in this
// file pins that across every failure path at once.

namespace {

using prd::app_model::AuthMethod;
using prd::config::ConfigV1;
using prd::config::LoadOutcome;
using prd::config::SaveOutcome;
using prd::storage::Namespace;
using prd::test::FakeKeyValue;

constexpr const char* kSlot0 = "cfg.0";
constexpr const char* kSlot1 = "cfg.1";
constexpr const char* kPointer = "cfg.gen";

[[nodiscard]] ConfigV1 configured(const char* host = "printer.local") {
    ConfigV1 value = prd::config::defaults();
    value.wifi_profile_id = 3;
    value.printer_host.assign(host);
    value.printer_port = 80;
    value.printer_alias.assign("Workshop");
    value.auth_method = AuthMethod::ApiKey;
    return value;
}

// --- First boot ---------------------------------------------------------------

// The vertical slice Milestone 3.2 exists to close: an empty store reports Missing, which
// the reducer turns into ProvisioningPhase::SetupRequired (§14.1).
TEST(ConfigRecordTests, AnEmptyStoreReportsAbsentAndBecomesMissing) {
    FakeKeyValue store;

    const auto report = prd::config::load(store);

    EXPECT_EQ(report.outcome, LoadOutcome::Absent);
    EXPECT_EQ(report.generation, 0U);
}

TEST(ConfigRecordTests, TheFirstSaveCommitsGenerationOneAndLoadsBack) {
    FakeKeyValue store;
    const ConfigV1 value = configured();

    ASSERT_EQ(prd::config::save(store, value), SaveOutcome::Committed);

    const auto report = prd::config::load(store);

    EXPECT_EQ(report.outcome, LoadOutcome::Loaded);
    EXPECT_EQ(report.generation, 1U);
    EXPECT_EQ(report.value, value);
}

TEST(ConfigRecordTests, SuccessiveSavesAlternateSlotsAndKeepThePrevious) {
    FakeKeyValue store;

    ASSERT_EQ(prd::config::save(store, configured("first.local")), SaveOutcome::Committed);
    EXPECT_TRUE(store.has(Namespace::Config, kSlot1));
    EXPECT_FALSE(store.has(Namespace::Config, kSlot0));

    ASSERT_EQ(prd::config::save(store, configured("second.local")), SaveOutcome::Committed);

    // Both slots now hold a record: the newest and the previous known-good.
    EXPECT_TRUE(store.has(Namespace::Config, kSlot0));
    EXPECT_TRUE(store.has(Namespace::Config, kSlot1));

    const auto report = prd::config::load(store);
    EXPECT_EQ(report.generation, 2U);
    EXPECT_EQ(report.value.printer_host.view(), "second.local");
}

// --- Validation gate ----------------------------------------------------------

TEST(ConfigRecordTests, AnInvalidValueIsRejectedAndNothingIsWritten) {
    FakeKeyValue store;

    ConfigV1 invalid = configured();
    invalid.brightness = 200;

    EXPECT_EQ(prd::config::save(store, invalid), SaveOutcome::Rejected);
    EXPECT_FALSE(store.has(Namespace::Config, kSlot0));
    EXPECT_FALSE(store.has(Namespace::Config, kSlot1));
    EXPECT_FALSE(store.has(Namespace::System, kPointer));
}

// --- A failed save never damages what is running ------------------------------

TEST(ConfigRecordTests, AFailedCandidateWriteLeavesTheActiveRecordRunning) {
    FakeKeyValue store;
    ASSERT_EQ(prd::config::save(store, configured("good.local")), SaveOutcome::Committed);

    store.fail_write(Namespace::Config, kSlot0);

    EXPECT_EQ(prd::config::save(store, configured("bad.local")), SaveOutcome::CandidateFailed);

    const auto report = prd::config::load(store);
    EXPECT_EQ(report.outcome, LoadOutcome::Loaded);
    EXPECT_EQ(report.generation, 1U);
    EXPECT_EQ(report.value.printer_host.view(), "good.local");
}

TEST(ConfigRecordTests, ACandidateThatDoesNotReadBackIsNotCommitted) {
    FakeKeyValue store;
    ASSERT_EQ(prd::config::save(store, configured("good.local")), SaveOutcome::Committed);

    store.fail_read(Namespace::Config, kSlot0);

    EXPECT_EQ(prd::config::save(store, configured("bad.local")), SaveOutcome::CandidateFailed);

    const auto report = prd::config::load(store);
    EXPECT_EQ(report.generation, 1U);
    EXPECT_EQ(report.value.printer_host.view(), "good.local");
}

// §13.3 step 6. The candidate is on the flash and verified, but the pointer never moved —
// so the device must go on running exactly what it was running.
TEST(ConfigRecordTests, AFailedCommitLeavesThePreviousGenerationActive) {
    FakeKeyValue store;
    ASSERT_EQ(prd::config::save(store, configured("good.local")), SaveOutcome::Committed);

    store.fail_write(Namespace::System, kPointer);

    EXPECT_EQ(prd::config::save(store, configured("newer.local")), SaveOutcome::CommitFailed);

    const auto report = prd::config::load(store);
    EXPECT_EQ(report.outcome, LoadOutcome::Loaded);
    EXPECT_EQ(report.generation, 1U);
    EXPECT_EQ(report.value.printer_host.view(), "good.local");
}

// The abandoned candidate carries a HIGHER generation than the pointer. Preferring it
// would resurrect a configuration the device never accepted, so the pointer wins.
TEST(ConfigRecordTests, AnAbandonedCandidateIsNotPreferredOverThePointer) {
    FakeKeyValue store;
    ASSERT_EQ(prd::config::save(store, configured("good.local")), SaveOutcome::Committed);

    store.fail_write(Namespace::System, kPointer);
    ASSERT_EQ(prd::config::save(store, configured("newer.local")), SaveOutcome::CommitFailed);
    ASSERT_TRUE(store.has(Namespace::Config, kSlot0));

    EXPECT_EQ(prd::config::load(store).value.printer_host.view(), "good.local");
}

// --- Previous-known-good ------------------------------------------------------

// §13.3's "previous-known-good generation for critical configuration".
TEST(ConfigRecordTests, ACorruptActiveRecordFallsBackToThePreviousGeneration) {
    FakeKeyValue store;
    ASSERT_EQ(prd::config::save(store, configured("first.local")), SaveOutcome::Committed);
    ASSERT_EQ(prd::config::save(store, configured("second.local")), SaveOutcome::Committed);

    // Generation 2 lives in slot 0. Rot one payload byte.
    auto corrupted = store.peek(Namespace::Config, kSlot0);
    ASSERT_GT(corrupted.size(), prd::config::kHeaderSize);
    corrupted[prd::config::kHeaderSize + 1] ^= 0x01U;
    store.poke(Namespace::Config, kSlot0, corrupted);

    const auto report = prd::config::load(store);

    EXPECT_EQ(report.outcome, LoadOutcome::LoadedFromPreviousGeneration);
    EXPECT_EQ(report.generation, 1U);
    EXPECT_EQ(report.value.printer_host.view(), "first.local");
}

// POWER LOST DURING THE CANDIDATE WRITE. The candidate goes to the slot the pointer does
// NOT name, so a half-written record there cannot reach the running configuration — the
// named slot is decoded first and returns before the damaged one is ever considered.
TEST(ConfigRecordTests, ATornCandidateSlotDoesNotDisturbTheActiveRecord) {
    FakeKeyValue store;
    ASSERT_EQ(prd::config::save(store, configured("first.local")), SaveOutcome::Committed);
    ASSERT_EQ(prd::config::save(store, configured("second.local")), SaveOutcome::Committed);

    // Generation 2 is active in slot 0; slot 1 holds the previous generation and is where
    // the next candidate would land. Truncate it the way an interrupted write would.
    auto torn = store.peek(Namespace::Config, kSlot1);
    torn.resize(torn.size() / 2);
    store.poke(Namespace::Config, kSlot1, torn);

    const auto report = prd::config::load(store);

    EXPECT_EQ(report.outcome, LoadOutcome::Loaded);
    EXPECT_EQ(report.generation, 2U);
    EXPECT_EQ(report.value.printer_host.view(), "second.local");
}

// POWER LOST WHILE THE POINTER IS BEING WRITTEN, in the one way NVS's own per-item
// atomicity does not already rule out: a generation for which no record exists. The named
// slot's generation will not match, so selection falls through to the highest valid record
// rather than to defaults.
TEST(ConfigRecordTests, APointerNamingAGenerationThatDoesNotExistFallsBackToARealRecord) {
    FakeKeyValue store;
    ASSERT_EQ(prd::config::save(store, configured("kept.local")), SaveOutcome::Committed);

    ASSERT_EQ(store.write_u32(Namespace::System, kPointer, 99), prd::storage::Result::Ok);

    const auto report = prd::config::load(store);

    EXPECT_EQ(report.outcome, LoadOutcome::LoadedFromPreviousGeneration);
    EXPECT_EQ(report.generation, 1U);
    EXPECT_EQ(report.value.printer_host.view(), "kept.local");
}

// NEVER ACCIDENTAL DEFAULTS. This is the property the finish() bug broke: a record that
// cannot be trusted must not arrive as a blank-but-valid configuration. The status says
// Invalid and every field is absent, so no consumer can read a default as a real setting.
TEST(ConfigRecordTests, AnUnusableRecordCarriesNoConfigurationValues) {
    FakeKeyValue store;
    ASSERT_EQ(prd::config::save(store, configured("real.local")), SaveOutcome::Committed);

    auto bytes = store.peek(Namespace::Config, kSlot1);
    bytes[prd::config::kHeaderSize + 1] ^= 0x01U;
    store.poke(Namespace::Config, kSlot1, bytes);

    const auto report = prd::config::load(store);
    ASSERT_EQ(report.outcome, LoadOutcome::Invalid);

    EXPECT_EQ(report.generation, 0U);
    EXPECT_EQ(report.value, prd::config::defaults())
        << "an Invalid report must carry nothing, not a plausible blank configuration";
}

// ⬅️ THIS TEST WAS REVERSED ON 2026-08-16, BY OWNER RULING, AND THE REVERSAL IS THE POINT.
//
// It used to assert that a device which lost only its pointer still found its record, on
// the reasoning that reporting Absent "would send a configured device back into
// provisioning". That reasoning had a hole: an absent pointer is not only what a lost
// pointer looks like, it is also what a FIRST provisioning looks like when the final
// activation write never landed. The old rule therefore promoted an abandoned candidate to
// active configuration on the next boot, and Milestone 3.8's power-loss table caught it.
//
// The ruling is: no active-generation marker, no active configuration. The pointer is
// §13.3's activation boundary, and bytes that survived a failed transaction are not an
// active record merely because they decode.
//
// WHAT IT COSTS, STATED HONESTLY: a device whose pointer was somehow lost while its record
// survived now returns to setup instead of recovering. That case is not distinguishable
// from the never-activated one from inside the loader, and of the two possible mistakes —
// re-provisioning a device that could have recovered, or silently activating a
// configuration the device never accepted — the ruling takes the first. A pointer that
// could not be READ is a different thing and still reports StorageFailure.
TEST(ConfigRecordTests, ALostPointerMeansThereIsNoActiveRecord) {
    FakeKeyValue store;
    ASSERT_EQ(prd::config::save(store, configured("kept.local")), SaveOutcome::Committed);

    ASSERT_EQ(store.erase_key(Namespace::System, kPointer), prd::storage::Result::Ok);

    const auto report = prd::config::load(store);

    EXPECT_EQ(report.outcome, LoadOutcome::Absent);
    EXPECT_EQ(report.generation, 0U);

    // Refused, not removed (R10): the record is still physically in the slot.
    EXPECT_TRUE(store.has(Namespace::Config, kSlot1));
}

TEST(ConfigRecordTests, BothSlotsCorruptReportsInvalidAndKeepsTheBytes) {
    FakeKeyValue store;
    ASSERT_EQ(prd::config::save(store, configured("first.local")), SaveOutcome::Committed);
    ASSERT_EQ(prd::config::save(store, configured("second.local")), SaveOutcome::Committed);

    for (const char* slot : {kSlot0, kSlot1}) {
        auto bytes = store.peek(Namespace::Config, slot);
        bytes[prd::config::kHeaderSize + 1] ^= 0x01U;
        store.poke(Namespace::Config, slot, bytes);
    }

    const auto report = prd::config::load(store);

    EXPECT_EQ(report.outcome, LoadOutcome::Invalid);

    // NOT ERASED — R10. The bytes are still there for Milestone 3.10's consented recovery.
    EXPECT_TRUE(store.has(Namespace::Config, kSlot0));
    EXPECT_TRUE(store.has(Namespace::Config, kSlot1));
}

TEST(ConfigRecordTests, AStoreFailureIsReportedAsStorageFailureNotAsMissing) {
    FakeKeyValue store;
    store.fail_read(Namespace::System, kPointer);

    const auto report = prd::config::load(store);

    EXPECT_EQ(report.outcome, LoadOutcome::StorageFailure);
}

// §13.4: a record from a newer firmware is reported, never rewritten. Reporting it Missing
// would send the device into provisioning and invite it to overwrite the newer record.
TEST(ConfigRecordTests, ARecordFromANewerSchemaReportsMigrationRequired) {
    FakeKeyValue store;
    ASSERT_EQ(prd::config::save(store, configured()), SaveOutcome::Committed);

    auto bytes = store.peek(Namespace::Config, kSlot1);
    bytes[6] = static_cast<std::uint8_t>(prd::config::kSchemaVersion + 1);

    // Re-seal over both spans, so the record is intact and only its schema version is
    // ahead — otherwise this would test corruption rather than a downgrade.
    const std::uint32_t checksum = prd::storage::crc32_update(
        prd::storage::crc32(bytes.data(), prd::config::kHeaderSize - 4),
        bytes.data() + prd::config::kHeaderSize, bytes.size() - prd::config::kHeaderSize);
    for (std::size_t index = 0; index < 4; ++index) {
        bytes[prd::config::kHeaderSize - 4 + index] =
            static_cast<std::uint8_t>((checksum >> (8U * index)) & 0xFFU);
    }
    store.poke(Namespace::Config, kSlot1, bytes);

    const auto report = prd::config::load(store);

    EXPECT_EQ(report.outcome, LoadOutcome::MigrationRequired);
    EXPECT_TRUE(store.has(Namespace::Config, kSlot1));
}

// R8's three-way distinction and the credential generation moved to
// test/host/app_core/configuration_observation_tests.cpp in Milestone 3.3: the observation
// is assembled by app_core from independent config and secrets reports, so it can no longer
// be asserted from this component alone. What is left here is the record layer's own
// behaviour, which is what this file is for.

// --- The rule that outranks the rest -----------------------------------------

// R10, pinned across every path this component has. `config` writes and replaces its own
// keys; it never removes one and has no partition operation at all. If a future change
// reaches for erase_key() to "clean up" a bad record, this fails.
TEST(ConfigRecordTests, NoLoadOrSavePathEverErasesAnything) {
    FakeKeyValue store;

    ASSERT_EQ(prd::config::save(store, configured("first.local")), SaveOutcome::Committed);
    ASSERT_EQ(prd::config::save(store, configured("second.local")), SaveOutcome::Committed);

    auto bytes = store.peek(Namespace::Config, kSlot0);
    bytes[prd::config::kHeaderSize + 1] ^= 0x01U;
    store.poke(Namespace::Config, kSlot0, bytes);

    (void)prd::config::load(store);

    ConfigV1 invalid = configured();
    invalid.brightness = 200;
    (void)prd::config::save(store, invalid);

    store.fail_write(Namespace::Config, kSlot0);
    (void)prd::config::save(store, configured("third.local"));

    store.fail_read(Namespace::System, kPointer);
    (void)prd::config::load(store);

    EXPECT_EQ(store.erase_key_calls(), 0U);
}

} // namespace
