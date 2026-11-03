// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "app_model/device_data.hpp"
#include "candidate.hpp"
#include "config/config.hpp"
#include "factory_reset.hpp"
#include "provisioning_flow.hpp"
#include "secrets/secrets.hpp"
#include "support/fake_key_value.hpp"

// User-consented factory reset — Milestone 3.10, owner Decision 4.
//
// ⬅️ WHY THIS FILE CARRIES THE WEIGHT IT DOES. This is the first operation in the project that
// removes anything, and the two ways it can be wrong are both invisible on a working device: it
// can leave a readable password behind, and it can boot a half-cleared store as though it were a
// configuration. Both are reachable here by breaking one step, and neither is reachable on a bench
// without cutting power at an exact instant.

namespace prd::app_core {
namespace {

// The SEVEN named keys the reset removes, plus its journal. Spelled out rather than imported, so a
// rename in the components under test is caught here instead of being followed silently.
constexpr std::string_view kConfigGeneration = "cfg.gen";
constexpr std::string_view kSecretGeneration = "sec.gen";

// ⬅️ ADDED TO THE INVENTORY BY THE OWNER-APPROVED ADR-0008 AMENDMENT, 2026-08-23. It holds no
// credential — five u32 payload fields: one profile id and four generation numbers — but
// candidate::reconcile() reads it before
// the configuration is loaded and will activate a pair from it, so leaving it behind would let a
// reset device boot into activation recovery instead of true first-boot behaviour.
constexpr std::string_view kRecoveryRecord = "pair.rec";

config::ConfigV1 configured() {
    config::ConfigV1 value = config::defaults();
    value.wifi_profile_id = 7;
    value.printer_host.assign("192.168.1.50");
    value.printer_port = 80;
    value.printer_alias.assign("Workshop");
    value.brightness = 42;
    return value;
}

secrets::SecretConfigV1 credentials() {
    secrets::SecretConfigV1 value;
    value.profile_id = 7;
    value.wifi_ssid.assign("HomeNetwork");
    value.wifi_password.assign("correct-horse-battery");
    value.printer_api_key.assign("abcdefghijklmnop");
    return value;
}

// A device that has been provisioned and then reconfigured once.
//
// ⬅️ SAVED TWICE ON PURPOSE. §13.3's staged write alternates slots, so ONE save populates only one
// of the two — and a reset tested against that state would never touch the other, which is exactly
// how a wipe that missed half the records could pass. Two saves put a record in every slot, which
// is also the state any device that has been reconfigured is actually in.
void provision(test::FakeKeyValue& store) {
    for (int pass = 0; pass < 2; ++pass) {
        ASSERT_EQ(config::save(store, configured()), config::SaveOutcome::Committed);
        ASSERT_EQ(secrets::save(store, credentials()), secrets::SaveOutcome::Committed);
    }

    ASSERT_EQ(config::load(store).outcome, config::LoadOutcome::Loaded);
    ASSERT_EQ(secrets::load(store).outcome, secrets::LoadOutcome::Loaded);

    // Both slots really are populated, so the assertions below are testing what they claim to.
    ASSERT_TRUE(store.has(storage::Namespace::Config, "cfg.0"));
    ASSERT_TRUE(store.has(storage::Namespace::Config, "cfg.1"));
    ASSERT_TRUE(store.has(storage::Namespace::Secrets, "sec.0"));
    ASSERT_TRUE(store.has(storage::Namespace::Secrets, "sec.1"));
}

// Whether any stored byte still contains the credential. The point of overwrite-before-erase.
[[nodiscard]] bool password_is_findable(const test::FakeKeyValue& store) {
    for (const std::string_view key : {"sec.0", "sec.1"}) {
        const auto blob = store.peek(storage::Namespace::Secrets, std::string{key});
        const std::string text{blob.begin(), blob.end()};

        if (text.find("correct-horse-battery") != std::string::npos) {
            return true;
        }
    }

    return false;
}

// Arms a real pair-activation recovery record over the provisioned store, exactly as a transaction
// interrupted between its two activations would leave it.
void arm_recovery_record(test::FakeKeyValue& store) {
    candidate::Recovery recovery;
    recovery.profile = 7;
    recovery.target_configuration = config::active_generation(store).generation;
    recovery.target_secret = secrets::active_generation(store).generation;
    recovery.previous_configuration = recovery.target_configuration - 1;
    recovery.previous_secret = recovery.target_secret - 1;

    ASSERT_TRUE(candidate::write_marker(store, recovery));
    ASSERT_EQ(candidate::read_marker(store).outcome, candidate::MarkerOutcome::Present);
}

} // namespace

TEST(FactoryResetTests, AProvisionedDeviceBecomesUnprovisioned) {
    test::FakeKeyValue store;
    provision(store);
    ASSERT_TRUE(password_is_findable(store));

    EXPECT_EQ(factory_reset::run(store), factory_reset::Outcome::Complete);

    // First-boot behaviour, which is the semantic the owner specified: not "invalid", not
    // "recovery", but a device that has never been configured.
    EXPECT_EQ(config::load(store).outcome, config::LoadOutcome::Absent);
    EXPECT_EQ(secrets::load(store).outcome, secrets::LoadOutcome::Absent);
}

TEST(FactoryResetTests, EVERY_SEVEN_NAMED_KEYS_AND_THE_JOURNAL_ARE_GONE) {
    test::FakeKeyValue store;
    provision(store);
    arm_recovery_record(store);

    EXPECT_EQ(factory_reset::run(store), factory_reset::Outcome::Complete);

    EXPECT_FALSE(store.has(storage::Namespace::System, std::string{kConfigGeneration}));
    EXPECT_FALSE(store.has(storage::Namespace::System, std::string{kSecretGeneration}));
    EXPECT_FALSE(store.has(storage::Namespace::System, std::string{kRecoveryRecord}));
    EXPECT_FALSE(store.has(storage::Namespace::Config, "cfg.0"));
    EXPECT_FALSE(store.has(storage::Namespace::Config, "cfg.1"));
    EXPECT_FALSE(store.has(storage::Namespace::Secrets, "sec.0"));
    EXPECT_FALSE(store.has(storage::Namespace::Secrets, "sec.1"));

    // And the journal itself.
    EXPECT_FALSE(store.has(storage::Namespace::System, std::string{factory_reset::kJournalKey}));
}

// ⛔ THE OWNER'S "remove credential material rather than merely abandoning pointers" REQUIREMENT.
// Clearing the activation markers alone would make the device un-provisioned while leaving the
// Wi-Fi password readable in flash — which is the outcome this asserts against.
TEST(FactoryResetTests, TheCredentialMaterialIsOverwrittenNotJustUnpointed) {
    test::FakeKeyValue store;
    provision(store);
    ASSERT_TRUE(password_is_findable(store));

    EXPECT_EQ(factory_reset::run(store), factory_reset::Outcome::Complete);

    EXPECT_FALSE(password_is_findable(store));
}

TEST(FactoryResetTests, ResettingAnUnconfiguredDeviceSucceedsAndChangesNothing) {
    // Idempotence, and the resumption path's precondition: every step must accept "already gone".
    test::FakeKeyValue store;

    EXPECT_EQ(factory_reset::run(store), factory_reset::Outcome::Complete);
    EXPECT_EQ(config::load(store).outcome, config::LoadOutcome::Absent);
    EXPECT_FALSE(store.has(storage::Namespace::System, std::string{factory_reset::kJournalKey}));
}

TEST(FactoryResetTests, RunningItTwiceIsSafe) {
    test::FakeKeyValue store;
    provision(store);

    EXPECT_EQ(factory_reset::run(store), factory_reset::Outcome::Complete);
    EXPECT_EQ(factory_reset::run(store), factory_reset::Outcome::Complete);
    EXPECT_EQ(config::load(store).outcome, config::LoadOutcome::Absent);
}

// --- The journal, and interruption at every step ----------------------------

TEST(FactoryResetTests, NothingIsPendingOnADeviceThatNeverReset) {
    test::FakeKeyValue store;
    provision(store);

    EXPECT_EQ(factory_reset::probe(store), factory_reset::Journal::Absent);
}

TEST(FactoryResetTests, TheJournalIsClearedOnlyAfterEverythingElse) {
    test::FakeKeyValue store;
    provision(store);

    EXPECT_EQ(factory_reset::run(store), factory_reset::Outcome::Complete);
    EXPECT_EQ(factory_reset::probe(store), factory_reset::Journal::Absent);
}

TEST(FactoryResetTests, AFailedStepLeavesTheJournalSoTheNextBootResumes) {
    test::FakeKeyValue store;
    provision(store);

    // The second configuration slot cannot be removed.
    store.fail_erase(storage::Namespace::Config, "cfg.1");

    EXPECT_EQ(factory_reset::run(store), factory_reset::Outcome::Failed);

    // ⛔ THE JOURNAL SURVIVES. A reset that half-finished must be finished, and forgetting it
    // would leave the device presenting as factory-fresh with material still in flash.
    EXPECT_EQ(factory_reset::probe(store), factory_reset::Journal::Armed);

    // And the next boot completes it.
    EXPECT_EQ(factory_reset::run(store), factory_reset::Outcome::Complete);
    EXPECT_EQ(factory_reset::probe(store), factory_reset::Journal::Absent);
    EXPECT_FALSE(store.has(storage::Namespace::Config, "cfg.1"));
}

// ⛔ THE POWER-LOSS INVARIANT THE OWNER STATED: "after power loss at any point, the device must
// never boot using partially erased credentials/configuration as an active configuration."
//
// Simulated by running the reset with one step broken and then asking what the NEXT BOOT would
// load — which is exactly what a device does after losing power partway.
TEST(FactoryResetTests, NoInterruptionEverLeavesAnActiveConfiguration) {
    struct Interruption {
        storage::Namespace space;
        std::string key;
    };

    const Interruption interruptions[] = {
        {storage::Namespace::System, std::string{kConfigGeneration}},
        {storage::Namespace::System, std::string{kSecretGeneration}},
        {storage::Namespace::Secrets, "sec.0"},
        {storage::Namespace::Secrets, "sec.1"},
        {storage::Namespace::Config, "cfg.0"},
        {storage::Namespace::Config, "cfg.1"},
    };

    for (const Interruption& broken : interruptions) {
        test::FakeKeyValue store;
        provision(store);

        store.fail_erase(broken.space, broken.key);
        (void)factory_reset::run(store);

        // Whatever failed, the device must not come back configured. Absent is first-boot;
        // anything else means a half-cleared store was presented as usable.
        const config::LoadOutcome config_outcome = config::load(store).outcome;
        const secrets::LoadOutcome secret_outcome = secrets::load(store).outcome;

        EXPECT_EQ(config_outcome, config::LoadOutcome::Absent) << "interrupted at " << broken.key;
        EXPECT_EQ(secret_outcome, secrets::LoadOutcome::Absent) << "interrupted at " << broken.key;

        // And the unfinished job is recorded, so the next boot finishes it.
        EXPECT_EQ(factory_reset::probe(store), factory_reset::Journal::Armed)
            << "interrupted at " << broken.key;
    }
}

TEST(FactoryResetTests, AJournalThatCannotBeWrittenAbortsBeforeRemovingAnything) {
    // Proceeding without a journal would mean an interruption could strand credential material on
    // a device that presents itself as factory-fresh, with nothing able to finish the job.
    test::FakeKeyValue store;
    provision(store);

    store.fail_write(storage::Namespace::System, std::string{factory_reset::kJournalKey});

    // ⬅️ ITS OWN OUTCOME SINCE AUDIT ITEM 5, AND NAMED FOR WHAT FAILED. "Failed" could not say
    // whether anything had been removed; this one can, and the log line depends on the difference.
    EXPECT_EQ(factory_reset::run(store), factory_reset::Outcome::JournalWriteFailed);

    // Untouched: the device is exactly as it was, still configured.
    EXPECT_EQ(config::load(store).outcome, config::LoadOutcome::Loaded);
    EXPECT_EQ(secrets::load(store).outcome, secrets::LoadOutcome::Loaded);
    EXPECT_TRUE(password_is_findable(store));
}

TEST(FactoryResetTests, AnUnreadableJournalAuthorisesNothingAndIsNotAnAbsence) {
    // ⛔ A FAILING STORE MUST NOT TRIGGER AN ERASE NOBODY ASKED FOR. Reporting the journal as armed
    // on an unreadable read would let ruling R10's automatic-erasure prohibition be defeated by a
    // read fault, which is the opposite of what the journal is for.
    //
    // ⬅️ AND IT IS NOT AN ABSENCE EITHER — audit item 5. This test used to assert only the first
    // half, against a boolean that had no way to say the second, and that gap is precisely what let
    // an armed journal hide behind one failed read. Both halves are asserted now.
    test::FakeKeyValue store;
    provision(store);

    store.fail_read(storage::Namespace::System, std::string{factory_reset::kJournalKey});

    const factory_reset::Journal state = factory_reset::probe(store);

    EXPECT_EQ(state, factory_reset::Journal::Unreadable);
    EXPECT_NE(state, factory_reset::Journal::Armed) << "it must not authorise an erase";
    EXPECT_NE(state, factory_reset::Journal::Absent) << "and it must not be read as no reset owed";
}

// --- What must NOT be touched ----------------------------------------------

TEST(FactoryResetTests, NothingOutsideTheSevenNamedKeysIsRemoved) {
    test::FakeKeyValue store;
    provision(store);
    arm_recovery_record(store);

    // Stand-ins for the device-owned facts that must survive: the System namespace holds more
    // than the two activation markers, and the runtime and diagnostics namespaces are declared
    // for state a reset has no business discarding.
    store.poke(storage::Namespace::System, "boot.count", {1, 2, 3, 4});
    store.poke(storage::Namespace::System, "ota.state", {9});
    store.poke(storage::Namespace::System, "factory.sn", {1, 1, 2, 3});
    store.poke(storage::Namespace::Runtime, "snapshot", {5, 6});
    store.poke(storage::Namespace::Diagnostics, "last.err", {7});

    EXPECT_EQ(factory_reset::run(store), factory_reset::Outcome::Complete);

    // ⬅️ NEGATIVE CONTROL FOR THE INVENTORY. A reset that enumerated, wildcarded or erased a
    // namespace would take these too — including the two System keys sitting beside the three the
    // reset does remove, which is the case a namespace-scoped erase would pass unnoticed.
    EXPECT_TRUE(store.has(storage::Namespace::System, "boot.count"));
    EXPECT_TRUE(store.has(storage::Namespace::System, "ota.state"));
    EXPECT_TRUE(store.has(storage::Namespace::System, "factory.sn"));
    EXPECT_TRUE(store.has(storage::Namespace::Runtime, "snapshot"));
    EXPECT_TRUE(store.has(storage::Namespace::Diagnostics, "last.err"));
}

TEST(FactoryResetTests, TheEraseCountIsBoundedByTheNamedKeys) {
    // A reset that enumerated or wildcarded would show up here as an erase count that does not
    // match the named inventory. Eight: seven records and markers, plus the journal.
    test::FakeKeyValue store;
    provision(store);
    arm_recovery_record(store);

    const std::size_t before = store.erase_key_calls();
    EXPECT_EQ(factory_reset::run(store), factory_reset::Outcome::Complete);

    EXPECT_EQ(store.erase_key_calls() - before, 8U);
}

// --- The recovery record in the inventory, and its failure matrix ---------
//
// ⛔ WHAT THESE PROTECT. candidate::reconcile() runs at boot BEFORE the configuration is loaded, and a
// surviving recovery record names generations it will activate. So a reset that removes the records
// but leaves the record behind does not produce a factory-fresh device: it produces one that boots
// into activation recovery.

TEST(RecoveryRecordResetTests, ADeviceWithNoRecoveryRecordResetsExactlyAsBefore) {
    test::FakeKeyValue store;
    provision(store);
    ASSERT_EQ(candidate::read_marker(store).outcome, candidate::MarkerOutcome::Absent);

    EXPECT_EQ(factory_reset::run(store), factory_reset::Outcome::Complete);

    EXPECT_FALSE(store.has(storage::Namespace::System, std::string{kRecoveryRecord}));
    EXPECT_EQ(config::load(store).outcome, config::LoadOutcome::Absent);
}

TEST(RecoveryRecordResetTests, A_VALID_RECORD_IS_REMOVED_AND_THE_NEXT_BOOT_RECONCILES_NOTHING) {
    test::FakeKeyValue store;
    provision(store);
    arm_recovery_record(store);

    EXPECT_EQ(factory_reset::run(store), factory_reset::Outcome::Complete);

    // ⛔ THE PROPERTY. Not merely "the key is gone" but "the next boot's reconciliation has nothing to
    // do" — asserted against the real executor, which is what actually runs.
    EXPECT_EQ(candidate::read_marker(store).outcome, candidate::MarkerOutcome::Absent);
    EXPECT_EQ(candidate::reconcile(store).outcome, candidate::Outcome::Nothing);
    EXPECT_EQ(config::load(store).outcome, config::LoadOutcome::Absent);
    EXPECT_EQ(secrets::load(store).outcome, secrets::LoadOutcome::Absent);
}

TEST(RecoveryRecordResetTests, ACorruptRecordIsAlsoRemoved) {
    // A record whose checksum fails is evidence reconciliation would refuse to act on — and would
    // report as Unresolved, sending a factory-fresh device to the storage/recovery path. It must go.
    test::FakeKeyValue store;
    provision(store);
    arm_recovery_record(store);

    std::vector<std::uint8_t> bytes =
        store.peek(storage::Namespace::System, std::string{kRecoveryRecord});
    ASSERT_FALSE(bytes.empty());
    bytes[6] = static_cast<std::uint8_t>(bytes[6] ^ 0xFFU);
    store.poke(storage::Namespace::System, std::string{kRecoveryRecord}, bytes);
    ASSERT_EQ(candidate::read_marker(store).outcome, candidate::MarkerOutcome::Corrupt);

    EXPECT_EQ(factory_reset::run(store), factory_reset::Outcome::Complete);

    EXPECT_FALSE(store.has(storage::Namespace::System, std::string{kRecoveryRecord}));
    EXPECT_EQ(candidate::reconcile(store).outcome, candidate::Outcome::Nothing);
}

TEST(RecoveryRecordResetTests, OversizedStoredBytesAreStillRemoved) {
    // Bytes longer than this firmware's record read as Corrupt, and erase does not care about length.
    // The removal must not depend on the content being parseable.
    test::FakeKeyValue store;
    provision(store);
    store.poke(storage::Namespace::System, std::string{kRecoveryRecord},
               std::vector<std::uint8_t>(512, 0xA5));
    ASSERT_EQ(candidate::read_marker(store).outcome, candidate::MarkerOutcome::Corrupt);

    EXPECT_EQ(factory_reset::run(store), factory_reset::Outcome::Complete);

    EXPECT_FALSE(store.has(storage::Namespace::System, std::string{kRecoveryRecord}));
}

TEST(RecoveryRecordResetTests, AN_UNREADABLE_RECORD_IS_STILL_REMOVED_BECAUSE_ERASE_DOES_NOT_READ) {
    // The reset must not need to understand the record to remove it. A failing read would make a
    // read-then-erase implementation give up on a key it could have deleted.
    test::FakeKeyValue store;
    provision(store);
    arm_recovery_record(store);
    store.fail_read(storage::Namespace::System, std::string{kRecoveryRecord});

    EXPECT_EQ(factory_reset::run(store), factory_reset::Outcome::Complete);

    EXPECT_FALSE(store.has(storage::Namespace::System, std::string{kRecoveryRecord}));
}

TEST(RecoveryRecordResetTests, A_FAILED_REMOVAL_LEAVES_THE_JOURNAL_ARMED) {
    // ⛔ THE INTERRUPTION-SAFETY RULE, FOR THIS STEP TOO. If the record cannot be removed the reset is
    // not complete, so the journal must stay and the next boot must resume.
    test::FakeKeyValue store;
    provision(store);
    arm_recovery_record(store);

    store.fail_erase(storage::Namespace::System, std::string{kRecoveryRecord});

    EXPECT_EQ(factory_reset::run(store), factory_reset::Outcome::Failed);

    EXPECT_EQ(factory_reset::probe(store), factory_reset::Journal::Armed);
    EXPECT_TRUE(store.has(storage::Namespace::System, std::string{kRecoveryRecord}));

    // And the resumption on the next boot finishes it.
    EXPECT_EQ(factory_reset::run(store), factory_reset::Outcome::Complete);
    EXPECT_EQ(factory_reset::probe(store), factory_reset::Journal::Absent);
    EXPECT_FALSE(store.has(storage::Namespace::System, std::string{kRecoveryRecord}));
    EXPECT_EQ(candidate::reconcile(store).outcome, candidate::Outcome::Nothing);
}

TEST(RecoveryRecordResetTests, THE_ROLL_FORWARD_WINDOW_EXISTS_AND_IS_WHY_THE_STEP_IS_WHERE_IT_IS) {
    // ⛔ THE WINDOW THE ORDERING CREATES, CONSTRUCTED EXACTLY. After step 2 the store holds no
    // pointers, BOTH record sets, and a recovery record whose targets still prove — which
    // reconcile() rolls FORWARD into an activated pair. That is the state a power loss between
    // steps 2 and 3 leaves, built here from the same deactivate() calls the reset uses.
    test::FakeKeyValue store;
    provision(store);
    arm_recovery_record(store);

    // Step 1 then step 2, as run() performs them: the journal records the user's consent, and the two
    // pointers go. This is the exact store state a power loss immediately afterwards leaves.
    ASSERT_EQ(store.write_u32(storage::Namespace::System, factory_reset::kJournalKey, 1),
              storage::Result::Ok);
    ASSERT_EQ(config::deactivate(store), config::ClearOutcome::Cleared);
    ASSERT_EQ(secrets::deactivate(store), secrets::ClearOutcome::Cleared);

    ASSERT_TRUE(store.has(storage::Namespace::System, std::string{kRecoveryRecord}));
    ASSERT_TRUE(store.has(storage::Namespace::Config, "cfg.0"));
    ASSERT_FALSE(store.has(storage::Namespace::System, std::string{kConfigGeneration}));

    // ⬅️ THE HAZARD, PROVED RATHER THAN ASSERTED. Reconciling from here re-activates the pair the
    // reset was in the middle of removing.
    {
        test::FakeKeyValue mirror;
        provision(mirror);
        arm_recovery_record(mirror);
        ASSERT_EQ(mirror.write_u32(storage::Namespace::System, factory_reset::kJournalKey, 1),
                  storage::Result::Ok);
        ASSERT_EQ(config::deactivate(mirror), config::ClearOutcome::Cleared);
        ASSERT_EQ(secrets::deactivate(mirror), secrets::ClearOutcome::Cleared);

        EXPECT_EQ(candidate::reconcile(mirror).outcome, candidate::Outcome::RolledForward);
        EXPECT_EQ(config::load(mirror).outcome, config::LoadOutcome::Loaded);
    }

    // ⛔ AND THE REASON IT IS SAFE. The journal was armed before step 2, so app_core resumes the reset
    // at boot BEFORE load_configuration() reaches reconciliation. Here that resumption runs and the
    // record is gone before anything can act on it.
    ASSERT_EQ(factory_reset::probe(store), factory_reset::Journal::Armed);
    EXPECT_EQ(factory_reset::run(store), factory_reset::Outcome::Complete);

    EXPECT_FALSE(store.has(storage::Namespace::System, std::string{kRecoveryRecord}));
    EXPECT_EQ(candidate::reconcile(store).outcome, candidate::Outcome::Nothing);
    EXPECT_EQ(config::load(store).outcome, config::LoadOutcome::Absent);
}

TEST(RecoveryRecordResetTests, A_SURVIVING_RECORD_WOULD_BOOT_INTO_UNRESOLVED_RECOVERY) {
    // ⛔ THE OTHER HALF OF THE HARM, AND THE ONE THE AMENDMENT NAMES. With the records removed but the
    // record left behind, reconciliation can prove neither the target pair nor a previous pair — so it
    // reports Unresolved and app_core refuses to read the store, sending a device that has just been
    // factory reset into the storage/recovery path instead of first-boot behaviour.
    test::FakeKeyValue store;
    provision(store);
    arm_recovery_record(store);

    store.fail_erase(storage::Namespace::System, std::string{kRecoveryRecord});
    ASSERT_EQ(factory_reset::run(store), factory_reset::Outcome::Failed);

    // The reset continued past the failed step, so the records are gone and only the record remains.
    ASSERT_TRUE(store.has(storage::Namespace::System, std::string{kRecoveryRecord}));
    ASSERT_FALSE(store.has(storage::Namespace::Config, "cfg.0"));
    ASSERT_FALSE(store.has(storage::Namespace::Secrets, "sec.0"));

    // ⬅️ NEGATIVE CONTROL: this is what a reset that omitted the step would leave for the next boot.
    const candidate::Reconciliation stranded = candidate::reconcile(store);
    EXPECT_EQ(stranded.outcome, candidate::Outcome::Unresolved);
    EXPECT_FALSE(candidate::resolved(stranded.outcome));
    EXPECT_TRUE(stranded.marker_retained);

    // And the journal is what saves it: resumption removes the record and first-boot behaviour is
    // restored.
    ASSERT_EQ(factory_reset::probe(store), factory_reset::Journal::Armed);
    EXPECT_EQ(factory_reset::run(store), factory_reset::Outcome::Complete);
    EXPECT_EQ(candidate::reconcile(store).outcome, candidate::Outcome::Nothing);
    EXPECT_EQ(config::load(store).outcome, config::LoadOutcome::Absent);
}

TEST(RecoveryRecordResetTests, POWER_LOSS_AFTER_THE_REMOVAL_RESUMES_AND_COMPLETES) {
    // The mirror case: the record is already gone and a later step failed. Resumption must accept
    // "already absent" for this step exactly as it does for the others.
    test::FakeKeyValue store;
    provision(store);
    arm_recovery_record(store);

    store.fail_erase(storage::Namespace::Config, "cfg.0");
    ASSERT_EQ(factory_reset::run(store), factory_reset::Outcome::Failed);
    ASSERT_FALSE(store.has(storage::Namespace::System, std::string{kRecoveryRecord}));
    ASSERT_EQ(factory_reset::probe(store), factory_reset::Journal::Armed);

    EXPECT_EQ(factory_reset::run(store), factory_reset::Outcome::Complete);
    EXPECT_EQ(factory_reset::probe(store), factory_reset::Journal::Absent);
}

TEST(RecoveryRecordResetTests, THE_FINAL_STORE_CONTAINS_NONE_OF_THE_EIGHT_KEYS) {
    // The whole inventory as one assertion, over a store that had every one of them.
    test::FakeKeyValue store;
    provision(store);
    arm_recovery_record(store);

    ASSERT_EQ(factory_reset::run(store), factory_reset::Outcome::Complete);

    EXPECT_FALSE(store.has(storage::Namespace::System, std::string{kConfigGeneration}));
    EXPECT_FALSE(store.has(storage::Namespace::System, std::string{kSecretGeneration}));
    EXPECT_FALSE(store.has(storage::Namespace::System, std::string{kRecoveryRecord}));
    EXPECT_FALSE(store.has(storage::Namespace::System, std::string{factory_reset::kJournalKey}));
    EXPECT_FALSE(store.has(storage::Namespace::Secrets, "sec.0"));
    EXPECT_FALSE(store.has(storage::Namespace::Secrets, "sec.1"));
    EXPECT_FALSE(store.has(storage::Namespace::Config, "cfg.0"));
    EXPECT_FALSE(store.has(storage::Namespace::Config, "cfg.1"));

    // POSITIVE CONTROL: the store really did hold them, so the eight assertions above are not
    // passing over an empty store.
    test::FakeKeyValue populated;
    provision(populated);
    arm_recovery_record(populated);
    EXPECT_TRUE(populated.has(storage::Namespace::System, std::string{kRecoveryRecord}));
    EXPECT_TRUE(populated.has(storage::Namespace::Config, "cfg.0"));
}

// --- The journal probe, and why a boolean was not enough ---------------------
//
// ⛔ WHAT THIS FILE'S NEW SECTION IS FOR — audit item 5, 2026-08-28. `pending()` returned false for
// a genuine absence AND for every read failure, so an ARMED journal whose read failed once was
// indistinguishable from a device that had never been reset. The boot skipped the resume, loaded a
// valid configuration, presented normally and permitted every mutation — and a later boot, whose
// read succeeded, erased whatever had been configured in between.
//
// ⬅️ candidate::MarkerOutcome HAD ALREADY SAID THIS: "Absent AND Unreadable ARE DIFFERENT ANSWERS,
// AND CONFLATING THEM WAS A DEFECT." It was applied to pair.rec and never to rst.pend, the one key
// that authorises destruction.

namespace {

// The value run() writes. Spelled out here rather than imported, so a change to the constant is
// caught by these tests instead of being followed silently.
constexpr std::uint32_t kArmedValue = 1;

[[nodiscard]] factory_reset::Journal probe_of(test::FakeKeyValue& store) {
    return factory_reset::probe(store);
}

} // namespace

TEST(JournalProbeTests, AnAbsentKeyIsAbsent) {
    test::FakeKeyValue store;
    provision(store);

    EXPECT_EQ(probe_of(store), factory_reset::Journal::Absent);
}

TEST(JournalProbeTests, TheExactArmedValueIsArmed) {
    test::FakeKeyValue store;
    provision(store);
    ASSERT_EQ(store.write_u32(storage::Namespace::System, factory_reset::kJournalKey, kArmedValue),
              storage::Result::Ok);

    EXPECT_EQ(probe_of(store), factory_reset::Journal::Armed);
}

TEST(JournalProbeTests, A_READ_FAILURE_IS_UNREADABLE_AND_NOT_ABSENCE) {
    // ⛔ THE DEFECT, AS ONE ASSERTION. The journal is armed and provable; one failed read must not
    // turn it into "no reset was ever authorised".
    test::FakeKeyValue store;
    provision(store);
    ASSERT_EQ(store.write_u32(storage::Namespace::System, factory_reset::kJournalKey, kArmedValue),
              storage::Result::Ok);
    ASSERT_EQ(probe_of(store), factory_reset::Journal::Armed) << "control: it really is armed";

    store.fail_read(storage::Namespace::System, std::string{factory_reset::kJournalKey});

    EXPECT_EQ(probe_of(store), factory_reset::Journal::Unreadable);
}

TEST(JournalProbeTests, AReadFailureOnADeviceWithNoJournalIsAlsoUnreadable) {
    // The store would not answer. That the key happens not to exist is exactly what could not be
    // established, so this must NOT report Absent either.
    test::FakeKeyValue store;
    provision(store);
    store.fail_read(storage::Namespace::System, std::string{factory_reset::kJournalKey});

    EXPECT_EQ(probe_of(store), factory_reset::Journal::Unreadable);
}

TEST(JournalProbeTests, ZeroIsInvalidRatherThanAbsent) {
    test::FakeKeyValue store;
    provision(store);
    ASSERT_EQ(store.write_u32(storage::Namespace::System, factory_reset::kJournalKey, 0),
              storage::Result::Ok);

    EXPECT_EQ(probe_of(store), factory_reset::Journal::Invalid);
}

TEST(JournalProbeTests, A_FOREIGN_NON_ZERO_VALUE_IS_INVALID_AND_AUTHORISES_NOTHING) {
    // ⛔ THE RULE THAT TIGHTENED. `pending()` accepted ANY non-zero value as consent. Only the value
    // this firmware writes proves this firmware armed it; R10 asks for proof before an erase, not
    // for a plausible-looking byte.
    for (const std::uint32_t foreign : {2U, 7U, 0xFFFFFFFFU, 0xDEADBEEFU}) {
        test::FakeKeyValue store;
        provision(store);
        ASSERT_EQ(store.write_u32(storage::Namespace::System, factory_reset::kJournalKey, foreign),
                  storage::Result::Ok);

        EXPECT_EQ(probe_of(store), factory_reset::Journal::Invalid) << "value " << foreign;
    }
}

// ⚠️ WHAT THE FAKE CAN AND CANNOT MODEL, STATED SO NOBODY READS MORE INTO THESE TESTS THAN IS THERE.
// FakeKeyValue::read_u32 consults its word map only, so a key stored as a BLOB reads back NotFound.
// The real adapter does not: nvs_key_value.cpp's classify_read() maps ESP_ERR_NVS_TYPE_MISMATCH —
// a journal key that is present but is not a u32 — into Result::Failed through its default branch.
//
// ⬅️ SO THE STRICT MAPPING IS TESTED AT THE storage::Result BOUNDARY, WHICH IS WHERE probe() ACTUALLY
// DECIDES. The test below covers Failed directly; the adapter's own mapping is read, not simulated,
// and this comment is the honest statement of that limit rather than a test pretending to cover it.
TEST(JournalProbeTests, TheFakeCannotModelNvsTypeMismatchAndDoesNotPretendTo) {
    test::FakeKeyValue store;
    store.poke(storage::Namespace::System, std::string{factory_reset::kJournalKey}, {1, 0, 0, 0});

    std::uint32_t value = 0;
    EXPECT_EQ(store.read_u32(storage::Namespace::System, factory_reset::kJournalKey, value),
              storage::Result::NotFound)
        << "the fake reports NotFound here; the real adapter reports Failed for a type mismatch";

    // And the branch the real adapter WOULD take is the one asserted above: Failed -> Unreadable.
    store.fail_read(storage::Namespace::System, std::string{factory_reset::kJournalKey});
    EXPECT_EQ(probe_of(store), factory_reset::Journal::Unreadable);
}

// --- The boot decision, exhaustive over the probe -----------------------------

TEST(BootResumeTests, AbsentLoadsConfiguration) {
    test::FakeKeyValue store;
    provision(store);

    EXPECT_EQ(factory_reset::resume_if_pending(store),
              factory_reset::BootAction::LoadConfiguration);

    // And nothing was touched.
    EXPECT_EQ(config::load(store).outcome, config::LoadOutcome::Loaded);
    EXPECT_TRUE(password_is_findable(store));
}

TEST(BootResumeTests, ArmedAndCompleteLoadsTheCleanFirstBootStore) {
    test::FakeKeyValue store;
    provision(store);
    arm_recovery_record(store);
    ASSERT_EQ(store.write_u32(storage::Namespace::System, factory_reset::kJournalKey, kArmedValue),
              storage::Result::Ok);

    EXPECT_EQ(factory_reset::resume_if_pending(store),
              factory_reset::BootAction::LoadConfiguration);

    // The store the configuration load then sees is a genuine first-boot store — which is the whole
    // reason this action is LoadConfiguration rather than a halt.
    EXPECT_EQ(config::load(store).outcome, config::LoadOutcome::Absent);
    EXPECT_EQ(secrets::load(store).outcome, secrets::LoadOutcome::Absent);
    EXPECT_EQ(candidate::reconcile(store).outcome, candidate::Outcome::Nothing);
    EXPECT_EQ(probe_of(store), factory_reset::Journal::Absent);
}

TEST(BootResumeTests, ARMED_AND_FAILED_HALTS_WITH_THE_RESET_FAULT) {
    test::FakeKeyValue store;
    provision(store);
    arm_recovery_record(store);
    ASSERT_EQ(store.write_u32(storage::Namespace::System, factory_reset::kJournalKey, kArmedValue),
              storage::Result::Ok);
    store.fail_erase(storage::Namespace::System, std::string{kRecoveryRecord});

    EXPECT_EQ(factory_reset::resume_if_pending(store),
              factory_reset::BootAction::HaltFactoryResetIncomplete);

    // ⛔ AND THIS IS WHAT THE HALT PREVENTS. The record survived and the records did not, so a boot
    // that went on to reconcile would report Unresolved and send a device that has just been reset
    // to the storage/recovery path — where the panel says "Nothing has been erased" over a store
    // whose credentials are gone. The halt is what stops that being reached.
    EXPECT_TRUE(store.has(storage::Namespace::System, std::string{kRecoveryRecord}));
    EXPECT_EQ(candidate::reconcile(store).outcome, candidate::Outcome::Unresolved);
    EXPECT_FALSE(password_is_findable(store));

    // The journal stays, so a later boot may try again. One attempt per boot, not per lifetime.
    EXPECT_EQ(probe_of(store), factory_reset::Journal::Armed);
}

TEST(BootResumeTests, ArmedAndJournalRewriteFailedHaltsWithTheResetFault) {
    // An already-proved armed journal is still owed even though the rewrite failed, so this is the
    // reset fault and not the storage one.
    test::FakeKeyValue store;
    provision(store);
    ASSERT_EQ(store.write_u32(storage::Namespace::System, factory_reset::kJournalKey, kArmedValue),
              storage::Result::Ok);
    store.fail_write(storage::Namespace::System, std::string{factory_reset::kJournalKey});

    EXPECT_EQ(factory_reset::resume_if_pending(store),
              factory_reset::BootAction::HaltFactoryResetIncomplete);

    // Nothing was removed: run() aborted at step 1.
    EXPECT_EQ(config::load(store).outcome, config::LoadOutcome::Loaded);
    EXPECT_TRUE(password_is_findable(store));
    EXPECT_EQ(probe_of(store), factory_reset::Journal::Armed) << "and it is still owed";
}

TEST(BootResumeTests, UNREADABLE_HALTS_WITH_STORAGE_FAILURE_AND_ERASES_NOTHING) {
    test::FakeKeyValue store;
    provision(store);
    ASSERT_EQ(store.write_u32(storage::Namespace::System, factory_reset::kJournalKey, kArmedValue),
              storage::Result::Ok);
    store.fail_read(storage::Namespace::System, std::string{factory_reset::kJournalKey});

    const std::size_t erases_before = store.erase_key_calls();

    EXPECT_EQ(factory_reset::resume_if_pending(store),
              factory_reset::BootAction::HaltStorageFailure);

    // ⛔ THE EXECUTOR WAS NOT CALLED. Not called and its result ignored — not called at all. An
    // erase that cannot prove a user asked for it must never be attempted, and a count is the only
    // way to say that rather than to hope it.
    EXPECT_EQ(store.erase_key_calls(), erases_before);
    EXPECT_EQ(config::load(store).outcome, config::LoadOutcome::Loaded);
    EXPECT_TRUE(password_is_findable(store));
}

TEST(BootResumeTests, INVALID_HALTS_WITH_STORAGE_FAILURE_AND_ERASES_NOTHING) {
    for (const std::uint32_t foreign : {0U, 2U, 0xDEADBEEFU}) {
        test::FakeKeyValue store;
        provision(store);
        ASSERT_EQ(store.write_u32(storage::Namespace::System, factory_reset::kJournalKey, foreign),
                  storage::Result::Ok);

        const std::size_t erases_before = store.erase_key_calls();

        EXPECT_EQ(factory_reset::resume_if_pending(store),
                  factory_reset::BootAction::HaltStorageFailure)
            << "value " << foreign;
        EXPECT_EQ(store.erase_key_calls(), erases_before) << "value " << foreign;
        EXPECT_EQ(config::load(store).outcome, config::LoadOutcome::Loaded) << "value " << foreign;
    }
}

TEST(BootResumeTests, EveryBootActionCarriesItsOwnTerminalFault) {
    // ⛔ THE COMPOSITION ROOT MUST NOT RE-DERIVE THE REASON. This mapping is the answer travelling
    // with the decision; app_core routes it and names nothing.
    EXPECT_EQ(factory_reset::fault_for(factory_reset::BootAction::LoadConfiguration),
              app_model::DeviceFaultReason::None);
    EXPECT_EQ(factory_reset::fault_for(factory_reset::BootAction::HaltFactoryResetIncomplete),
              app_model::DeviceFaultReason::FactoryResetIncomplete);
    EXPECT_EQ(factory_reset::fault_for(factory_reset::BootAction::HaltStorageFailure),
              app_model::DeviceFaultReason::StorageFailure);
}

// --- The halt notes are bounded to what THIS boot did ------------------------

TEST(BootResumeTests, NEITHER_HALT_NOTE_CLAIMS_ANYTHING_WAS_OR_WAS_NOT_ERASED) {
    // ⛔ THE FIRST VERSION OF THIS LOG SAID "nothing was erased on this path" FOR BOTH HALTS, and it
    // was false twice: on the reset route an authorised resume may have removed several records
    // before a later step failed, and on either route an EARLIER boot's reset may have erased data
    // before its journal became unreadable. Neither statement is available to this boot.
    for (const factory_reset::BootAction action :
         {factory_reset::BootAction::HaltFactoryResetIncomplete,
          factory_reset::BootAction::HaltStorageFailure}) {
        const std::string note{factory_reset::halt_note(action)};

        ASSERT_FALSE(note.empty());
        for (const auto* const forbidden :
             {"nothing was erased", "nothing is erased", "nothing was removed", "settings are safe",
              "unchanged", "nothing was lost"}) {
            EXPECT_EQ(note.find(forbidden), std::string::npos)
                << "the halt note must not say \"" << forbidden << "\": " << note;
        }

        // What each MAY say, and does: this boot loads nothing and starts nothing.
        EXPECT_NE(note.find("will not load or reconcile"), std::string::npos) << note;
        EXPECT_NE(note.find("setup or printer work"), std::string::npos) << note;
    }
}

TEST(BootResumeTests, THE_STORAGE_HALT_MAY_SAY_THIS_BOOT_ATTEMPTED_NO_ERASE) {
    // Provable, because the executor is not called — and the erase-count tests above are what prove
    // it. It is bounded to this boot and says nothing about any earlier one.
    const std::string note{factory_reset::halt_note(factory_reset::BootAction::HaltStorageFailure)};

    EXPECT_NE(note.find("this boot attempted no erase"), std::string::npos) << note;
    EXPECT_NE(note.find("authority could not be established"), std::string::npos) << note;
}

TEST(BootResumeTests, THE_RESET_HALT_DOES_NOT_CLAIM_NO_ERASE_HAPPENED) {
    // ⛔ THE DISCRIMINATING HALF. The resume RAN on this route, so "attempted no erase" would be
    // false — and that is exactly the claim the two notes must not share.
    const std::string note{
        factory_reset::halt_note(factory_reset::BootAction::HaltFactoryResetIncomplete)};

    EXPECT_EQ(note.find("attempted no erase"), std::string::npos) << note;
    EXPECT_NE(note.find("did not finish"), std::string::npos) << note;
}

TEST(BootResumeTests, LoadConfigurationHasNoHaltNote) {
    EXPECT_TRUE(factory_reset::halt_note(factory_reset::BootAction::LoadConfiguration).empty());
}

// --- The reproduction, and the control that makes it mean something ----------

TEST(HiddenJournalTests, AN_ARMED_JOURNAL_WHOSE_READ_FAILS_MUST_NOT_BOOT_NORMALLY) {
    // The exact sequence: power lost immediately after the journal was armed and before the first
    // destructive step, then one failed journal read on the next boot.
    test::FakeKeyValue store;
    provision(store);
    ASSERT_EQ(store.write_u32(storage::Namespace::System, factory_reset::kJournalKey, kArmedValue),
              storage::Result::Ok);
    ASSERT_EQ(probe_of(store), factory_reset::Journal::Armed) << "control: armed and provable";

    store.fail_read(storage::Namespace::System, std::string{factory_reset::kJournalKey});

    // ⛔ BEFORE THIS REPAIR THIS BOOT LOADED A VALID CONFIGURATION AND PRESENTED THE ORDINARY
    // OPERATIONAL SCREEN, with the journal armed and invisible. It must halt instead.
    EXPECT_EQ(factory_reset::resume_if_pending(store),
              factory_reset::BootAction::HaltStorageFailure);

    // Nothing was erased on the way, and the journal is still there to be tried again.
    EXPECT_TRUE(password_is_findable(store));
    EXPECT_EQ(probe_of(store), factory_reset::Journal::Armed);

    // The NEXT boot, whose read succeeds, is the one authorised to finish the job.
    EXPECT_EQ(factory_reset::resume_if_pending(store),
              factory_reset::BootAction::LoadConfiguration);
    EXPECT_EQ(config::load(store).outcome, config::LoadOutcome::Absent);
    EXPECT_FALSE(password_is_findable(store));
}

TEST(HiddenJournalTests,
     POSITIVE_CONTROL_AN_IDENTICAL_DEVICE_WITH_NO_JOURNAL_BOOTS_AND_KEEPS_EVERYTHING) {
    // ⬅️ THE SAME STORE, THE SAME TWO BOOTS, NO JOURNAL. If this also halted or erased, the test
    // above would be asserting nothing about the journal.
    test::FakeKeyValue store;
    provision(store);

    EXPECT_EQ(factory_reset::resume_if_pending(store),
              factory_reset::BootAction::LoadConfiguration);
    EXPECT_EQ(factory_reset::resume_if_pending(store),
              factory_reset::BootAction::LoadConfiguration);

    EXPECT_EQ(config::load(store).outcome, config::LoadOutcome::Loaded);
    EXPECT_EQ(secrets::load(store).outcome, secrets::LoadOutcome::Loaded);
    EXPECT_TRUE(password_is_findable(store));
}

// --- The two unsafe mappings, as explicit negative controls ------------------
//
// ⛔ NEITHER OF THESE ASSERTS PRODUCTION BEHAVIOUR. Each builds the store state a WRONG mapping
// would act on and shows what that mapping would do, so the two failure directions are recorded as
// harm rather than as preference. Both are what probe() refuses.

TEST(UnsafeMappingControls, TREATING_UNREADABLE_AS_ABSENT_HIDES_AN_ARMED_JOURNAL) {
    test::FakeKeyValue store;
    provision(store);
    ASSERT_EQ(store.write_u32(storage::Namespace::System, factory_reset::kJournalKey, kArmedValue),
              storage::Result::Ok);
    store.fail_read(storage::Namespace::System, std::string{factory_reset::kJournalKey});

    // What "Unreadable means Absent" would conclude, and what it would then do: carry on booting
    // with a valid configuration while a confirmed reset is still owed and unseen.
    ASSERT_EQ(probe_of(store), factory_reset::Journal::Unreadable);
    EXPECT_EQ(config::load(store).outcome, config::LoadOutcome::Loaded)
        << "a device that boots here looks perfectly healthy — that is the harm";
    EXPECT_EQ(probe_of(store), factory_reset::Journal::Armed)
        << "and the very next read proves the job was still owed the whole time";
}

TEST(UnsafeMappingControls, TREATING_UNREADABLE_AS_ARMED_WOULD_ERASE_WITHOUT_AUTHORISATION) {
    // A store that will not answer, on a device that never had a reset confirmed. "Unreadable means
    // Armed" would run the executor here — an erase nobody asked for, which is precisely ruling
    // R10's automatic erasure.
    test::FakeKeyValue store;
    provision(store);
    store.fail_read(storage::Namespace::System, std::string{factory_reset::kJournalKey});

    ASSERT_EQ(probe_of(store), factory_reset::Journal::Unreadable);

    // ⛔ RE-ARMED, AND THAT IS NOT BOOKKEEPING. FakeKeyValue's injectors fire once; the assertion
    // above consumed this one, so without this line resume_if_pending() below would read a HEALTHY
    // store and the test would pass while exercising nothing. A one-shot fault injector consumed by
    // an earlier assertion makes the later one vacuous — this test failed exactly that way first.
    store.fail_read(storage::Namespace::System, std::string{factory_reset::kJournalKey});

    const std::size_t erases_before = store.erase_key_calls();
    EXPECT_EQ(factory_reset::resume_if_pending(store),
              factory_reset::BootAction::HaltStorageFailure);

    EXPECT_EQ(store.erase_key_calls(), erases_before) << "no key may be erased on this route";
    EXPECT_TRUE(password_is_findable(store)) << "and the credential must still be there";
}

} // namespace prd::app_core
