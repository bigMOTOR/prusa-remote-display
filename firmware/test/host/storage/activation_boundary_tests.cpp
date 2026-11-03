// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include <gtest/gtest.h>

#include <string>

#include "config/config.hpp"
#include "secrets/secrets.hpp"
#include "support/fake_key_value.hpp"

// THE ACTIVATION BOUNDARY — owner ruling, 2026-08-16.
//
//     No active-generation marker → no active configuration.
//
// §13.3's staged update is candidate → commit and read back → mark active, and the
// active-generation pointer IS that final mark. A record that decodes perfectly is not
// active merely because its bytes survived: if the pointer was never written, the
// transaction that wrote those bytes did not finish, and a later boot must not promote it.
//
// WHY THIS FILE HOLDS BOTH COMPONENTS. `config` and `secrets` know nothing about each
// other and deliberately do not share an enum, so the only thing keeping their load
// semantics identical is that someone checks. Testing them side by side in one file is
// that check — a divergence shows up as an asymmetric pair here rather than as a device
// that treats its configuration and its credentials by different rules.
//
// The defect this closes was found by the Milestone 3.8 power-loss table: a first
// provisioning whose credential commit failed still booted as fully configured, because an
// absent pointer let previous-known-good recovery adopt the abandoned candidate.

namespace prd {
namespace {

constexpr std::string_view kConfigGeneration = "cfg.gen";
constexpr std::string_view kSecretGeneration = "sec.gen";

[[nodiscard]] config::ConfigV1 a_configuration(const std::uint32_t profile) {
    config::ConfigV1 value = config::defaults();
    value.wifi_profile_id = profile;
    value.printer_host.assign("192.168.1.50");
    value.printer_port = 80;
    return value;
}

[[nodiscard]] secrets::SecretConfigV1 a_secret(const std::uint32_t profile) {
    secrets::SecretConfigV1 value = secrets::defaults();
    value.profile_id = profile;
    value.wifi_ssid.assign("Home Network");
    value.wifi_password.assign("correct horse battery");
    return value;
}

// --- 1. A first-ever candidate with no pointer is not active ---------------
//
// Reached by writing a record and then removing the pointer, which is the same store state
// a device has when the pointer write never happened.

TEST(ActivationBoundaryTests, ConfigFirstEverCandidateWithNoPointerIsNotActive) {
    test::FakeKeyValue store;
    ASSERT_EQ(config::save(store, a_configuration(1)), config::SaveOutcome::Committed);
    ASSERT_EQ(config::load(store).outcome, config::LoadOutcome::Loaded);

    store.forget(storage::Namespace::System, std::string{kConfigGeneration});

    const config::LoadReport report = config::load(store);
    EXPECT_EQ(report.outcome, config::LoadOutcome::Absent);
    EXPECT_EQ(report.generation, 0U);
    EXPECT_EQ(report.value.wifi_profile_id, 0U);
}

TEST(ActivationBoundaryTests, SecretFirstEverCandidateWithNoPointerIsNotActive) {
    test::FakeKeyValue store;
    ASSERT_EQ(secrets::save(store, a_secret(1)), secrets::SaveOutcome::Committed);
    ASSERT_EQ(secrets::load(store).outcome, secrets::LoadOutcome::Loaded);

    store.forget(storage::Namespace::System, std::string{kSecretGeneration});

    const secrets::LoadReport report = secrets::load(store);
    EXPECT_EQ(report.outcome, secrets::LoadOutcome::Absent);
    EXPECT_EQ(report.generation, 0U);
    EXPECT_TRUE(report.value.wifi_ssid.empty());
}

// --- 2. Read back successfully, pointer commit fails → not active ----------
//
// The exact failure the Milestone 3.8 table calls row 6: the candidate is written AND
// verified, and only the final mark does not land.

TEST(ActivationBoundaryTests, ConfigPointerCommitFailureLeavesNothingActive) {
    test::FakeKeyValue store;
    store.fail_write(storage::Namespace::System, std::string{kConfigGeneration});

    EXPECT_EQ(config::save(store, a_configuration(1)), config::SaveOutcome::CommitFailed);
    EXPECT_EQ(config::load(store).outcome, config::LoadOutcome::Absent);
}

TEST(ActivationBoundaryTests, SecretPointerCommitFailureLeavesNothingActive) {
    test::FakeKeyValue store;
    store.fail_write(storage::Namespace::System, std::string{kSecretGeneration});

    EXPECT_EQ(secrets::save(store, a_secret(1)), secrets::SaveOutcome::CommitFailed);
    EXPECT_EQ(secrets::load(store).outcome, secrets::LoadOutcome::Absent);
}

// --- 3. A previously activated generation stays loadable -------------------

TEST(ActivationBoundaryTests, ConfigAnActivatedGenerationIsStillLoaded) {
    test::FakeKeyValue store;
    ASSERT_EQ(config::save(store, a_configuration(1)), config::SaveOutcome::Committed);
    ASSERT_EQ(config::save(store, a_configuration(2)), config::SaveOutcome::Committed);

    const config::LoadReport report = config::load(store);
    EXPECT_EQ(report.outcome, config::LoadOutcome::Loaded);
    EXPECT_EQ(report.value.wifi_profile_id, 2U);
    EXPECT_EQ(report.generation, 2U);
}

TEST(ActivationBoundaryTests, SecretAnActivatedGenerationIsStillLoaded) {
    test::FakeKeyValue store;
    ASSERT_EQ(secrets::save(store, a_secret(1)), secrets::SaveOutcome::Committed);
    ASSERT_EQ(secrets::save(store, a_secret(2)), secrets::SaveOutcome::Committed);

    const secrets::LoadReport report = secrets::load(store);
    EXPECT_EQ(report.outcome, secrets::LoadOutcome::Loaded);
    EXPECT_EQ(report.value.profile_id, 2U);
    EXPECT_EQ(report.generation, 2U);
}

// --- 4. Previous-known-good still recovers an activated older generation ---
//
// ⬅️ THE HALF THAT MUST NOT BE LOST. The correction removes recovery for records that were
// never activated; it must not remove recovery for records that were. Two generations are
// activated in turn, then the newest is corrupted — and the device must come up on the
// older one rather than on nothing.

TEST(ActivationBoundaryTests, ConfigPreviousKnownGoodStillRecoversAnActivatedGeneration) {
    test::FakeKeyValue store;
    ASSERT_EQ(config::save(store, a_configuration(1)), config::SaveOutcome::Committed);
    ASSERT_EQ(config::save(store, a_configuration(2)), config::SaveOutcome::Committed);

    // Generation 2 lives in the slot the pointer names. Corrupt it.
    store.poke(storage::Namespace::Config, std::string{"cfg.0"}, {0xDE, 0xAD, 0xBE, 0xEF});

    const config::LoadReport report = config::load(store);
    EXPECT_EQ(report.outcome, config::LoadOutcome::LoadedFromPreviousGeneration);
    EXPECT_EQ(report.value.wifi_profile_id, 1U);
    EXPECT_EQ(report.generation, 1U);
}

TEST(ActivationBoundaryTests, SecretPreviousKnownGoodStillRecoversAnActivatedGeneration) {
    test::FakeKeyValue store;
    ASSERT_EQ(secrets::save(store, a_secret(1)), secrets::SaveOutcome::Committed);
    ASSERT_EQ(secrets::save(store, a_secret(2)), secrets::SaveOutcome::Committed);

    store.poke(storage::Namespace::Secrets, std::string{"sec.0"}, {0xDE, 0xAD, 0xBE, 0xEF});

    const secrets::LoadReport report = secrets::load(store);
    EXPECT_EQ(report.outcome, secrets::LoadOutcome::LoadedFromPreviousGeneration);
    EXPECT_EQ(report.value.profile_id, 1U);
    EXPECT_EQ(report.generation, 1U);
}

// --- 5. An orphan higher generation is never preferred ---------------------
//
// Two ways it could win, and neither may. First: the pointer names a healthy record and an
// abandoned candidate sits beside it with a larger generation.

TEST(ActivationBoundaryTests, ConfigAnOrphanHigherGenerationDoesNotWinOverTheCommittedOne) {
    test::FakeKeyValue store;
    ASSERT_EQ(config::save(store, a_configuration(1)), config::SaveOutcome::Committed);

    // A second transaction whose pointer move fails: generation 2 is written and verified,
    // and the pointer still names generation 1.
    store.fail_write(storage::Namespace::System, std::string{kConfigGeneration});
    ASSERT_EQ(config::save(store, a_configuration(2)), config::SaveOutcome::CommitFailed);

    const config::LoadReport report = config::load(store);
    EXPECT_EQ(report.outcome, config::LoadOutcome::Loaded);
    EXPECT_EQ(report.value.wifi_profile_id, 1U);
    EXPECT_EQ(report.generation, 1U);
}

// Second, and this is the one the generation bound exists for: the committed record is ALSO
// unusable, so the loader falls back — and must still refuse the orphan above it.
TEST(ActivationBoundaryTests, ConfigAnOrphanIsNotAdoptedEvenWhenTheCommittedRecordIsCorrupt) {
    test::FakeKeyValue store;
    ASSERT_EQ(config::save(store, a_configuration(1)), config::SaveOutcome::Committed);

    store.fail_write(storage::Namespace::System, std::string{kConfigGeneration});
    ASSERT_EQ(config::save(store, a_configuration(2)), config::SaveOutcome::CommitFailed);

    // Generation 1 is in the slot the pointer names; destroy it. Generation 2 — the orphan —
    // is intact in the other slot and would be chosen by an unbounded fallback.
    store.poke(storage::Namespace::Config, std::string{"cfg.1"}, {0xDE, 0xAD, 0xBE, 0xEF});

    const config::LoadReport report = config::load(store);
    EXPECT_NE(report.outcome, config::LoadOutcome::Loaded);
    EXPECT_NE(report.outcome, config::LoadOutcome::LoadedFromPreviousGeneration);
    EXPECT_NE(report.value.wifi_profile_id, 2U);
}

TEST(ActivationBoundaryTests, SecretAnOrphanHigherGenerationDoesNotWinOverTheCommittedOne) {
    test::FakeKeyValue store;
    ASSERT_EQ(secrets::save(store, a_secret(1)), secrets::SaveOutcome::Committed);

    store.fail_write(storage::Namespace::System, std::string{kSecretGeneration});
    ASSERT_EQ(secrets::save(store, a_secret(2)), secrets::SaveOutcome::CommitFailed);

    const secrets::LoadReport report = secrets::load(store);
    EXPECT_EQ(report.outcome, secrets::LoadOutcome::Loaded);
    EXPECT_EQ(report.value.profile_id, 1U);
    EXPECT_EQ(report.generation, 1U);
}

TEST(ActivationBoundaryTests, SecretAnOrphanIsNotAdoptedEvenWhenTheCommittedRecordIsCorrupt) {
    test::FakeKeyValue store;
    ASSERT_EQ(secrets::save(store, a_secret(1)), secrets::SaveOutcome::Committed);

    store.fail_write(storage::Namespace::System, std::string{kSecretGeneration});
    ASSERT_EQ(secrets::save(store, a_secret(2)), secrets::SaveOutcome::CommitFailed);

    store.poke(storage::Namespace::Secrets, std::string{"sec.1"}, {0xDE, 0xAD, 0xBE, 0xEF});

    const secrets::LoadReport report = secrets::load(store);
    EXPECT_NE(report.outcome, secrets::LoadOutcome::Loaded);
    EXPECT_NE(report.outcome, secrets::LoadOutcome::LoadedFromPreviousGeneration);
    EXPECT_NE(report.value.profile_id, 2U);
}

// --- 6. The two components answer identically ------------------------------
//
// Stated as its own test rather than left to the reader of the pairs above: the same store
// state must produce the same verdict on both sides, because a device whose configuration
// and credentials disagree about what "active" means is a device with no coherent answer.

TEST(ActivationBoundaryTests, BothComponentsTreatAnOrphanCandidateTheSameWay) {
    test::FakeKeyValue store;
    ASSERT_EQ(config::save(store, a_configuration(1)), config::SaveOutcome::Committed);
    ASSERT_EQ(secrets::save(store, a_secret(1)), secrets::SaveOutcome::Committed);

    store.forget(storage::Namespace::System, std::string{kConfigGeneration});
    store.forget(storage::Namespace::System, std::string{kSecretGeneration});

    EXPECT_EQ(config::load(store).outcome, config::LoadOutcome::Absent);
    EXPECT_EQ(secrets::load(store).outcome, secrets::LoadOutcome::Absent);
}

// --- 7. No erase is introduced ---------------------------------------------
//
// R10, over every path this correction touches. An orphan is refused, not removed: the
// bytes stay where they are and the next successful staged write overwrites that slot in
// the ordinary course of things.

TEST(ActivationBoundaryTests, RefusingAnOrphanErasesNothing) {
    test::FakeKeyValue store;
    ASSERT_EQ(config::save(store, a_configuration(1)), config::SaveOutcome::Committed);
    ASSERT_EQ(secrets::save(store, a_secret(1)), secrets::SaveOutcome::Committed);

    store.forget(storage::Namespace::System, std::string{kConfigGeneration});
    store.forget(storage::Namespace::System, std::string{kSecretGeneration});

    ASSERT_EQ(config::load(store).outcome, config::LoadOutcome::Absent);
    ASSERT_EQ(secrets::load(store).outcome, secrets::LoadOutcome::Absent);

    EXPECT_EQ(store.erase_key_calls(), 0U);

    // And the refused bytes are still physically there, which is what "not considered
    // active" means as distinct from "removed".
    EXPECT_TRUE(store.has(storage::Namespace::Config, std::string{"cfg.1"}));
    EXPECT_TRUE(store.has(storage::Namespace::Secrets, std::string{"sec.1"}));
}

// A device that recovers from the orphan state by provisioning again must end up active.
TEST(ActivationBoundaryTests, AFreshTransactionAfterAnOrphanBecomesActiveNormally) {
    test::FakeKeyValue store;
    store.fail_write(storage::Namespace::System, std::string{kConfigGeneration});
    ASSERT_EQ(config::save(store, a_configuration(1)), config::SaveOutcome::CommitFailed);
    ASSERT_EQ(config::load(store).outcome, config::LoadOutcome::Absent);

    ASSERT_EQ(config::save(store, a_configuration(2)), config::SaveOutcome::Committed);

    const config::LoadReport report = config::load(store);
    EXPECT_EQ(report.outcome, config::LoadOutcome::Loaded);
    EXPECT_EQ(report.value.wifi_profile_id, 2U);
    EXPECT_EQ(store.erase_key_calls(), 0U);
}

} // namespace
} // namespace prd
