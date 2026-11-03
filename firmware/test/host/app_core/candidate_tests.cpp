// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include <cstdint>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "candidate.hpp"
#include "configuration_observation.hpp"
#include "provisioning_flow.hpp"
#include "support/fake_key_value.hpp"

// The candidate lifecycle, its activation boundary, and every way a power loss can land inside it.
//
// ⛔ WHAT THESE PROTECT. §13.3 orders the pattern: stage, read back, TEST THE CONNECTION, activate,
// and retain the previous valid configuration on failure. Through Milestone 3 there was no applicable
// connection test, so writing and activating in one call was correct. Milestone 4 makes the test
// applicable, and the pointers must therefore move only after the printer has answered — which is two
// pointer writes in two namespaces, and a power loss between them leaves a MIXED pair that reads as
// un-provisioned.
//
// ⛔ AND WHAT THE PREVIOUS VERSION OF THIS FILE GOT WRONG. It host-tested a pure enum — decide() —
// and left the writes, the generation arithmetic, the rereads and the marker clear in app_core.cpp,
// which this suite cannot compile. All three of that mechanism's defects lived in the untested half:
// a credential pointer moved by arithmetic with no record behind it, a marker cleared after a FAILED
// repair, and no way to name a pair to fall back to. So the tests below drive the REAL executor
// against an injected store, and the matrix fails every individual write in turn.

namespace prd::app_core {
namespace {

using candidate::MarkerOutcome;
using candidate::Outcome;
using candidate::Recovery;
using provisioning_flow::Failure;

constexpr const char* kMarkerKey = "pair.rec";
// ⬅️ BOTH POINTERS LIVE IN `system`, NOT IN THEIR OWN NAMESPACES. That is the activation boundary's
// actual shape: two words in one namespace, which is exactly why a power loss between them needs a
// recovery record rather than a per-namespace guarantee.
constexpr const char* kConfigPointer = "cfg.gen";
constexpr const char* kSecretPointer = "sec.gen";

[[nodiscard]] const char* config_slot(const std::uint32_t generation) {
    return generation % 2 == 0 ? "cfg.0" : "cfg.1";
}

[[nodiscard]] const char* secret_slot(const std::uint32_t generation) {
    return generation % 2 == 0 ? "sec.0" : "sec.1";
}

[[nodiscard]] provisioning_flow::Submission submission_for(const char* host, const char* key) {
    provisioning_flow::Submission submission;
    submission.stage = provisioning_flow::Stage::Printer;
    (void)submission.wifi_ssid.assign("network");
    (void)submission.wifi_password.assign("password");
    (void)submission.printer_host.assign(host);
    (void)submission.printer_api_key.assign(key);
    submission.printer_port = 80;
    return submission;
}

[[nodiscard]] provisioning_flow::Records records_for(const char* host, const char* key,
                                                     const std::uint32_t profile) {
    provisioning_flow::Records records =
        provisioning_flow::compose(submission_for(host, key), config::defaults(), profile);

    // Milestone 4 stages a record that is validated by the time anything can observe it: the flag
    // describes the record once ACTIVE, and only the success path activates.
    records.configuration.printer_validated = true;
    return records;
}

// An OLD VALIDATED PROFILE: a device that already works. This is what every failure case below must
// leave usable.
provisioning_flow::StagedPair seed_working_device(test::FakeKeyValue& store) {
    const provisioning_flow::StagedPair staged =
        provisioning_flow::stage_pair(store, records_for("192.168.1.10", "old-key", 1));

    EXPECT_TRUE(staged.staged);
    EXPECT_TRUE(provisioning_flow::activate_pair(store, staged).activated);

    return staged;
}

// The candidate B every reconciliation test is trying to activate.
provisioning_flow::StagedPair stage_candidate(test::FakeKeyValue& store) {
    const provisioning_flow::StagedPair staged =
        provisioning_flow::stage_pair(store, records_for("192.168.1.99", "new-key", 2));

    EXPECT_TRUE(staged.staged);
    return staged;
}

struct PairView {
    std::uint32_t configuration_profile{0};
    std::uint32_t secret_profile{0};
    std::string host;
    bool whole{false};
};

[[nodiscard]] PairView active_pair(test::FakeKeyValue& store) {
    const config::LoadReport configuration = config::load(store);
    const secrets::LoadReport secret = secrets::load(store);

    PairView view;
    view.configuration_profile = configuration.value.wifi_profile_id;
    view.secret_profile = secret.value.profile_id;
    view.host = std::string{configuration.value.printer_host.view()};
    view.whole = configuration.outcome == config::LoadOutcome::Loaded &&
                 secret.outcome == secrets::LoadOutcome::Loaded &&
                 view.configuration_profile != 0 &&
                 view.configuration_profile == view.secret_profile;
    return view;
}

// --- The recovery record ---------------------------------------------------

TEST(RecoveryRecordTests, AnAbsentRecordIsAbsentAndNothingElse) {
    test::FakeKeyValue store;

    EXPECT_EQ(candidate::read_marker(store).outcome, MarkerOutcome::Absent);
    EXPECT_EQ(candidate::reconcile(store).outcome, Outcome::Nothing);
}

TEST(RecoveryRecordTests, ARecordRoundTripsWithEveryFieldIntact) {
    test::FakeKeyValue store;

    Recovery written;
    written.profile = 2;
    written.target_configuration = 9;
    written.target_secret = 11;
    written.previous_configuration = 8;
    written.previous_secret = 10;

    ASSERT_TRUE(candidate::write_marker(store, written));

    const candidate::MarkerRead read = candidate::read_marker(store);

    ASSERT_EQ(read.outcome, MarkerOutcome::Present);
    EXPECT_EQ(read.recovery.profile, 2U);
    EXPECT_EQ(read.recovery.target_configuration, 9U);
    EXPECT_EQ(read.recovery.target_secret, 11U);
    EXPECT_EQ(read.recovery.previous_configuration, 8U);
    EXPECT_EQ(read.recovery.previous_secret, 10U);

    ASSERT_TRUE(candidate::clear_marker(store));
    EXPECT_EQ(candidate::read_marker(store).outcome, MarkerOutcome::Absent);
}

TEST(RecoveryRecordTests, ARecordNamingNoProfileOrNoTargetIsRefused) {
    test::FakeKeyValue store;

    Recovery no_profile;
    no_profile.target_configuration = 9;
    no_profile.target_secret = 11;
    EXPECT_FALSE(candidate::write_marker(store, no_profile));

    Recovery no_target;
    no_target.profile = 2;
    EXPECT_FALSE(candidate::write_marker(store, no_target));

    // POSITIVE CONTROL.
    Recovery complete;
    complete.profile = 2;
    complete.target_configuration = 9;
    complete.target_secret = 11;
    EXPECT_TRUE(candidate::write_marker(store, complete));
}

TEST(RecoveryRecordTests, A_FAILED_READ_IS_UNREADABLE_AND_NEVER_AN_ABSENCE) {
    // ⛔ THE DEFECT THIS CLOSES. The previous read collapsed every non-Ok result into "no marker", so
    // one transient read failure skipped reconciliation entirely and a genuinely mixed pair was
    // presented as an un-provisioned device — the user's configuration apparently gone.
    test::FakeKeyValue store;

    Recovery recovery;
    recovery.profile = 2;
    recovery.target_configuration = 9;
    recovery.target_secret = 11;
    ASSERT_TRUE(candidate::write_marker(store, recovery));

    store.fail_read(storage::Namespace::System, kMarkerKey);
    EXPECT_EQ(candidate::read_marker(store).outcome, MarkerOutcome::Unreadable);

    // NEGATIVE CONTROL: and it is NOT treated as absent by the executor either — it retains the
    // evidence and refuses to let the store be read normally.
    store.fail_read(storage::Namespace::System, kMarkerKey);
    const candidate::Reconciliation reconciled = candidate::reconcile(store);

    EXPECT_EQ(reconciled.outcome, Outcome::Unresolved);
    EXPECT_FALSE(candidate::resolved(reconciled.outcome));
    EXPECT_TRUE(reconciled.marker_retained);
    EXPECT_EQ(candidate::read_marker(store).outcome, MarkerOutcome::Present);
}

TEST(RecoveryRecordTests, A_CORRUPT_RECORD_IS_EVIDENCE_AND_IS_NOT_CLEARED) {
    // A failed checksum means something wrote a record and it cannot be trusted. Clearing it would
    // destroy the only sign that an activation was in flight.
    test::FakeKeyValue store;

    Recovery recovery;
    recovery.profile = 2;
    recovery.target_configuration = 9;
    recovery.target_secret = 11;
    ASSERT_TRUE(candidate::write_marker(store, recovery));

    std::vector<std::uint8_t> bytes = store.peek(storage::Namespace::System, kMarkerKey);
    ASSERT_FALSE(bytes.empty());
    bytes[6] = static_cast<std::uint8_t>(bytes[6] ^ 0xFFU);
    store.poke(storage::Namespace::System, kMarkerKey, bytes);

    EXPECT_EQ(candidate::read_marker(store).outcome, MarkerOutcome::Corrupt);

    const candidate::Reconciliation reconciled = candidate::reconcile(store);
    EXPECT_EQ(reconciled.outcome, Outcome::Unresolved);
    EXPECT_TRUE(reconciled.marker_retained);
    EXPECT_TRUE(store.has(storage::Namespace::System, kMarkerKey));
}

TEST(RecoveryRecordTests, AShortOrForeignRecordIsCorruptRatherThanAbsent) {
    test::FakeKeyValue store;

    // Four bytes — exactly what the u32 marker this replaces would have left behind.
    store.poke(storage::Namespace::System, kMarkerKey, {2, 0, 0, 0});
    EXPECT_EQ(candidate::read_marker(store).outcome, MarkerOutcome::Corrupt);

    // Right length, wrong magic.
    store.poke(storage::Namespace::System, kMarkerKey, std::vector<std::uint8_t>(30, 0));
    EXPECT_EQ(candidate::read_marker(store).outcome, MarkerOutcome::Corrupt);
}

TEST(RecoveryRecordTests, EveryOutcomeAndEveryMarkerStateHasALogLabel) {
    for (const MarkerOutcome outcome : {MarkerOutcome::Absent, MarkerOutcome::Present,
                                        MarkerOutcome::Unreadable, MarkerOutcome::Corrupt}) {
        EXPECT_FALSE(candidate::log_label(outcome).empty());
        EXPECT_NE(candidate::log_label(outcome), "unknown");
    }

    for (const Outcome outcome : {Outcome::Nothing, Outcome::AlreadyComplete,
                                  Outcome::RolledForward, Outcome::Restored, Outcome::Unresolved}) {
        EXPECT_FALSE(candidate::log_label(outcome).empty());
        EXPECT_NE(candidate::log_label(outcome), "unknown");
    }

    EXPECT_TRUE(candidate::resolved(Outcome::Nothing));
    EXPECT_TRUE(candidate::resolved(Outcome::AlreadyComplete));
    EXPECT_TRUE(candidate::resolved(Outcome::RolledForward));
    EXPECT_TRUE(candidate::resolved(Outcome::Restored));
    EXPECT_FALSE(candidate::resolved(Outcome::Unresolved));
}

// --- Staging moves nothing -------------------------------------------------

TEST(StagedPairTests, StagingMovesNothingActive) {
    // ⬅️ THE PROPERTY THE WHOLE FIX RESTS ON. After staging, the device is still running the record
    // it was running before — so a probe that fails afterwards costs the user nothing.
    test::FakeKeyValue store;
    seed_working_device(store);

    const PairView before = active_pair(store);
    const provisioning_flow::StagedPair staged = stage_candidate(store);

    EXPECT_EQ(staged.profile, 2U);

    const PairView after = active_pair(store);
    EXPECT_EQ(after.host, before.host);
    EXPECT_EQ(after.configuration_profile, 1U);
    EXPECT_EQ(after.secret_profile, 1U);
    EXPECT_TRUE(after.whole);

    // And no recovery record was written: nothing is mid-activation.
    EXPECT_EQ(candidate::read_marker(store).outcome, MarkerOutcome::Absent);
}

TEST(StagedPairTests, StagingRecordsTheEXACTPreviousPairRatherThanInferringIt) {
    // ⛔ THE ARITHMETIC THIS REPLACES. The old design reconstructed the missing half as
    // `active + 1`, which is a pointer write with nothing behind it. The restore target is now read
    // from the store BEFORE anything is written, so it is a fact rather than a guess.
    test::FakeKeyValue store;
    const provisioning_flow::StagedPair first = seed_working_device(store);

    const provisioning_flow::StagedPair staged = stage_candidate(store);

    EXPECT_EQ(staged.previous_configuration_generation, first.configuration_generation);
    EXPECT_EQ(staged.previous_secret_generation, first.secret_generation);

    // NEGATIVE CONTROL: a first provisioning has no previous pair, so nothing is named — and
    // nothing is at risk.
    test::FakeKeyValue virgin;
    const provisioning_flow::StagedPair firstever = stage_candidate(virgin);
    EXPECT_EQ(firstever.previous_configuration_generation, config::kNoGeneration);
    EXPECT_EQ(firstever.previous_secret_generation, secrets::kNoGeneration);
}

TEST(StagedPairTests, TheStagedCandidateIsReadableWithoutBeingActive) {
    // §13.3's "the candidate must be readable for validation without becoming the operational
    // record" — the property that lets a probe run against it. Now required for BOTH namespaces,
    // because recovery proves both halves before activating either.
    test::FakeKeyValue store;
    seed_working_device(store);

    const provisioning_flow::StagedPair staged = stage_candidate(store);

    const config::LoadReport configuration =
        config::load_staged(store, staged.configuration_generation);
    EXPECT_EQ(configuration.outcome, config::LoadOutcome::Loaded);
    EXPECT_EQ(configuration.value.printer_host.view(), "192.168.1.99");
    EXPECT_EQ(configuration.value.wifi_profile_id, 2U);

    const secrets::LoadReport secret = secrets::load_staged(store, staged.secret_generation);
    EXPECT_EQ(secret.outcome, secrets::LoadOutcome::Loaded);
    EXPECT_EQ(secret.value.profile_id, 2U);

    // NEGATIVE CONTROL: the active records are still the old ones, so the two reads genuinely differ.
    EXPECT_EQ(config::load(store).value.printer_host.view(), "192.168.1.10");
    EXPECT_EQ(secrets::load(store).value.profile_id, 1U);

    // And a generation nothing occupies is Absent rather than "something plausible".
    EXPECT_EQ(secrets::load_staged(store, 99).outcome, secrets::LoadOutcome::Absent);
    EXPECT_EQ(secrets::load_staged(store, secrets::kNoGeneration).outcome,
              secrets::LoadOutcome::Absent);
}

TEST(StagedPairTests, OLD_A_SURVIVES_A_FAILED_CANDIDATE_B) {
    // ⛔ THE SCENARIO THE AUDIT NAMED. A working device, a new candidate, and a printer that never
    // answers. The user must still have their printer afterwards.
    test::FakeKeyValue store;
    seed_working_device(store);
    stage_candidate(store);

    // The probe fails, so activate_pair() is simply never called. That is the entire failure path.
    const auto observation = compose_configuration(config::load(store), secrets::load(store));

    EXPECT_TRUE(observation.has_printer_configuration);
    EXPECT_FALSE(observation.printer_configuration_pending);
    EXPECT_EQ(observation.printer_address.view(), "192.168.1.10");

    // POSITIVE CONTROL: activating the same staged pair DOES replace it, so the survival above is
    // the failure path and not an activation that never works.
    const provisioning_flow::StagedPair again = stage_candidate(store);
    ASSERT_TRUE(provisioning_flow::activate_pair(store, again).activated);
    EXPECT_EQ(config::load(store).value.printer_host.view(), "192.168.1.99");
}

TEST(StagedPairTests, ActivationCrossesOneBoundaryAndPairsBothNamespaces) {
    test::FakeKeyValue store;
    seed_working_device(store);

    const provisioning_flow::StagedPair staged = stage_candidate(store);
    const provisioning_flow::ActivatedPair activated =
        provisioning_flow::activate_pair(store, staged);

    ASSERT_TRUE(activated.activated);
    EXPECT_FALSE(activated.marker_retained);

    const PairView after = active_pair(store);
    EXPECT_EQ(after.configuration_profile, 2U);
    EXPECT_EQ(after.secret_profile, 2U);
    EXPECT_TRUE(after.whole);
    EXPECT_EQ(candidate::read_marker(store).outcome, MarkerOutcome::Absent);

    EXPECT_TRUE(
        compose_configuration(config::load(store), secrets::load(store)).has_wifi_credentials);
}

TEST(StagedPairTests, AFailedConfigurationCandidateNeverReachesTheCredential) {
    // The order is load-bearing for what a FAILED transaction leaves behind: stopping before the
    // credential means the user's Wi-Fi password never reached flash at all.
    test::FakeKeyValue store;

    store.fail_write(storage::Namespace::Config, "cfg.1");

    const provisioning_flow::StagedPair staged =
        provisioning_flow::stage_pair(store, records_for("192.168.1.99", "new-key", 1));

    EXPECT_FALSE(staged.staged);
    EXPECT_FALSE(staged.secret_attempted);
    EXPECT_EQ(staged.failure, Failure::StorageCandidateFailed);
}

TEST(StagedPairTests, FirstTimeSetupWithNoPreviousConfigurationStaysTruthful) {
    test::FakeKeyValue store;

    const provisioning_flow::StagedPair staged = stage_candidate(store);

    const auto before = compose_configuration(config::load(store), secrets::load(store));
    EXPECT_FALSE(before.has_printer_configuration);
    EXPECT_FALSE(before.has_wifi_credentials);

    ASSERT_TRUE(provisioning_flow::activate_pair(store, staged).activated);

    EXPECT_TRUE(
        compose_configuration(config::load(store), secrets::load(store)).has_wifi_credentials);
}

// --- The power-loss matrix -------------------------------------------------
//
// Every boundary inside activate_pair(), one failed write at a time, each followed by the boot that
// has to clean up after it. A power loss and a failed write leave the same store state, which is what
// makes this a proof about power loss rather than about error handling.

TEST(PowerLossMatrixTests, LOSS_BEFORE_THE_RECORD_IS_WRITTEN_ACTIVATES_NOTHING) {
    test::FakeKeyValue store;
    seed_working_device(store);
    const provisioning_flow::StagedPair staged = stage_candidate(store);

    store.fail_write(storage::Namespace::System, kMarkerKey);

    const provisioning_flow::ActivatedPair activated =
        provisioning_flow::activate_pair(store, staged);

    EXPECT_FALSE(activated.activated);
    EXPECT_EQ(activated.failure, Failure::PromotionFailed);

    // ⛔ NEITHER POINTER MOVED. Without a record in flash there would be nothing to repair a loss
    // between the two activations, so refusing to start is the only safe answer.
    const PairView after = active_pair(store);
    EXPECT_EQ(after.configuration_profile, 1U);
    EXPECT_EQ(after.secret_profile, 1U);
    EXPECT_TRUE(after.whole);

    // And the next boot has nothing to do.
    EXPECT_EQ(candidate::reconcile(store).outcome, Outcome::Nothing);
    EXPECT_TRUE(active_pair(store).whole);
}

TEST(PowerLossMatrixTests, LOSS_AFTER_THE_RECORD_WITH_NEITHER_POINTER_MOVED_ROLLS_FORWARD) {
    test::FakeKeyValue store;
    seed_working_device(store);
    const provisioning_flow::StagedPair staged = stage_candidate(store);

    store.fail_write(storage::Namespace::System, kConfigPointer);

    EXPECT_FALSE(provisioning_flow::activate_pair(store, staged).activated);

    // The old pair is still whole and operational, and the record is in flash.
    EXPECT_TRUE(active_pair(store).whole);
    EXPECT_EQ(active_pair(store).configuration_profile, 1U);
    ASSERT_EQ(candidate::read_marker(store).outcome, MarkerOutcome::Present);

    // ⬅️ THE NEXT BOOT FINISHES WHAT THE PROBE ALREADY EARNED. The record proves a validation
    // succeeded for profile 2 and both target records are provably whole, so it rolls forward.
    const candidate::Reconciliation reconciled = candidate::reconcile(store);

    EXPECT_EQ(reconciled.outcome, Outcome::RolledForward);
    EXPECT_EQ(reconciled.profile, 2U);
    EXPECT_FALSE(reconciled.marker_retained);

    const PairView after = active_pair(store);
    EXPECT_TRUE(after.whole);
    EXPECT_EQ(after.configuration_profile, 2U);
    EXPECT_EQ(after.secret_profile, 2U);
    EXPECT_EQ(after.host, "192.168.1.99");
    EXPECT_EQ(candidate::read_marker(store).outcome, MarkerOutcome::Absent);
}

TEST(PowerLossMatrixTests, LOSS_AFTER_THE_CONFIG_POINTER_ONLY_IS_THE_MIXED_PAIR_AND_ROLLS_FORWARD) {
    // ⛔ THE HAZARD THE WHOLE MECHANISM EXISTS FOR. Config names the new profile, credentials name
    // the old, so this boot reads as un-provisioned — the user's working configuration apparently
    // gone.
    test::FakeKeyValue store;
    seed_working_device(store);
    const provisioning_flow::StagedPair staged = stage_candidate(store);

    store.fail_write(storage::Namespace::System, kSecretPointer);

    EXPECT_FALSE(provisioning_flow::activate_pair(store, staged).activated);

    const PairView mixed = active_pair(store);
    EXPECT_FALSE(mixed.whole);
    EXPECT_EQ(mixed.configuration_profile, 2U);
    EXPECT_EQ(mixed.secret_profile, 1U);

    const candidate::Reconciliation reconciled = candidate::reconcile(store);

    EXPECT_EQ(reconciled.outcome, Outcome::RolledForward);
    EXPECT_FALSE(reconciled.marker_retained);

    const PairView after = active_pair(store);
    EXPECT_TRUE(after.whole);
    EXPECT_EQ(after.configuration_profile, 2U);
    EXPECT_EQ(after.secret_profile, 2U);
    EXPECT_EQ(candidate::read_marker(store).outcome, MarkerOutcome::Absent);
}

TEST(PowerLossMatrixTests, LOSS_AFTER_BOTH_POINTERS_BUT_BEFORE_THE_CLEAR_IS_IDEMPOTENT_TIDY_UP) {
    test::FakeKeyValue store;
    seed_working_device(store);
    const provisioning_flow::StagedPair staged = stage_candidate(store);

    store.fail_erase(storage::Namespace::System, kMarkerKey);

    const provisioning_flow::ActivatedPair activated =
        provisioning_flow::activate_pair(store, staged);

    // ⬅️ REPORTED AS ACTIVATED, BECAUSE IT WAS. A failed clear is not a failed activation, and the
    // retention is reported separately rather than turned into a false failure.
    EXPECT_TRUE(activated.activated);
    EXPECT_TRUE(activated.marker_retained);
    EXPECT_TRUE(active_pair(store).whole);
    ASSERT_EQ(candidate::read_marker(store).outcome, MarkerOutcome::Present);

    const candidate::Reconciliation reconciled = candidate::reconcile(store);

    EXPECT_EQ(reconciled.outcome, Outcome::AlreadyComplete);
    EXPECT_FALSE(reconciled.marker_retained);
    EXPECT_EQ(candidate::read_marker(store).outcome, MarkerOutcome::Absent);

    // And a second run of the same boot path changes nothing — the definition of idempotent.
    EXPECT_EQ(candidate::reconcile(store).outcome, Outcome::Nothing);
    EXPECT_TRUE(active_pair(store).whole);
    EXPECT_EQ(active_pair(store).configuration_profile, 2U);
}

TEST(PowerLossMatrixTests, A_CLEAR_THAT_KEEPS_FAILING_STAYS_SAFE_AND_KEEPS_RETRYING) {
    test::FakeKeyValue store;
    seed_working_device(store);
    const provisioning_flow::StagedPair staged = stage_candidate(store);

    store.fail_erase(storage::Namespace::System, kMarkerKey);
    ASSERT_TRUE(provisioning_flow::activate_pair(store, staged).activated);

    // Three boots in a row where the clear fails. Each one must reach the same safe conclusion and
    // must not damage the pair it has already established.
    for (int boot = 0; boot < 3; ++boot) {
        store.fail_erase(storage::Namespace::System, kMarkerKey);

        const candidate::Reconciliation reconciled = candidate::reconcile(store);

        EXPECT_EQ(reconciled.outcome, Outcome::AlreadyComplete) << boot;
        EXPECT_TRUE(reconciled.marker_retained) << boot;
        EXPECT_TRUE(candidate::resolved(reconciled.outcome)) << boot;
        EXPECT_TRUE(active_pair(store).whole) << boot;
        EXPECT_EQ(active_pair(store).configuration_profile, 2U) << boot;
    }

    // And the boot where it finally succeeds tidies up.
    EXPECT_EQ(candidate::reconcile(store).outcome, Outcome::AlreadyComplete);
    EXPECT_EQ(candidate::read_marker(store).outcome, MarkerOutcome::Absent);
}

TEST(PowerLossMatrixTests, EVERY_ACTIVATION_WRITE_FAILING_ONCE_IS_RESOLVED_ON_THE_NEXT_BOOT) {
    // The matrix as a loop: fail one pointer write, then let the next boot clean up. Whatever the
    // boundary, the device must end on a WHOLE pair.
    struct Victim {
        storage::Namespace space;
        const char* key;
    };

    for (const Victim victim : {Victim{storage::Namespace::System, kConfigPointer},
                                Victim{storage::Namespace::System, kSecretPointer}}) {
        test::FakeKeyValue store;
        seed_working_device(store);
        const provisioning_flow::StagedPair staged = stage_candidate(store);

        store.fail_write(victim.space, victim.key);

        EXPECT_FALSE(provisioning_flow::activate_pair(store, staged).activated) << victim.key;

        const candidate::Reconciliation reconciled = candidate::reconcile(store);

        EXPECT_EQ(reconciled.outcome, Outcome::RolledForward) << victim.key;
        EXPECT_TRUE(active_pair(store).whole) << victim.key;
        EXPECT_EQ(active_pair(store).configuration_profile, 2U) << victim.key;
        EXPECT_EQ(active_pair(store).secret_profile, 2U) << victim.key;
    }
}

// --- What "the pair is active" is allowed to mean --------------------------

TEST(ActivePairProofTests, A_FALLBACK_LOAD_IS_NOT_A_REPAIRED_POINTER) {
    // ⛔ THE WEAKER PROOF THIS REPLACES. pair_is_active() used to ask config::load() and
    // secrets::load() what generation they returned. Those fall back to a previous known-good record
    // when the record the pointer names is unusable, and they then report the FALLBACK's generation —
    // so a pointer naming a corrupt generation, with the fallback landing exactly on the requested
    // one, satisfied the check while the pointer still named something else. Every caller uses this
    // to decide whether the recovery record may be CLEARED, and clearing it there would discard the
    // evidence with the repair incomplete.
    test::FakeKeyValue store;

    // Two activations, so generation 1 and generation 2 both hold valid records for profile 1 and 2.
    const provisioning_flow::StagedPair first = seed_working_device(store);
    const provisioning_flow::StagedPair second = stage_candidate(store);
    ASSERT_TRUE(provisioning_flow::activate_pair(store, second).activated);

    ASSERT_EQ(config::active_generation(store).generation, second.configuration_generation);

    // Corrupt the record the CONFIG pointer names. load() now falls back to the previous generation
    // and reports ITS generation — which is exactly `first.configuration_generation`.
    std::vector<std::uint8_t> bytes =
        store.peek(storage::Namespace::Config, config_slot(second.configuration_generation));
    ASSERT_FALSE(bytes.empty());
    bytes[bytes.size() - 1] = static_cast<std::uint8_t>(bytes[bytes.size() - 1] ^ 0xFFU);
    store.poke(storage::Namespace::Config, config_slot(second.configuration_generation), bytes);

    const config::LoadReport fallback = config::load(store);
    ASSERT_EQ(fallback.outcome, config::LoadOutcome::LoadedFromPreviousGeneration);
    ASSERT_EQ(fallback.generation, first.configuration_generation);
    ASSERT_EQ(fallback.value.wifi_profile_id, 1U);

    // Put the secret pointer back on generation 1 so only the config half is the interesting term.
    ASSERT_EQ(secrets::activate(store, first.secret_generation), secrets::SaveOutcome::Committed);
    ASSERT_EQ(secrets::load(store).value.profile_id, 1U);

    // ⛔ THE DISCRIMINATOR. The old, fallback-tolerant proof would have said yes here: load() reports
    // generation 1 for both halves and profile 1 for both. The exact proof says no, because the
    // CONFIG POINTER still names generation 2.
    EXPECT_NE(config::active_generation(store).generation, first.configuration_generation);
    EXPECT_FALSE(candidate::pair_is_active(store, first.configuration_generation,
                                           first.secret_generation, 1));

    // POSITIVE CONTROL: move the pointer to where it is claimed to be, and the same call succeeds —
    // so the refusal above is the pointer identity and not a proof that never passes.
    ASSERT_EQ(config::activate(store, first.configuration_generation),
              config::SaveOutcome::Committed);
    EXPECT_TRUE(candidate::pair_is_active(store, first.configuration_generation,
                                          first.secret_generation, 1));
}

TEST(ActivePairProofTests, AnUnreadablePointerProvesNothing) {
    // A pointer the store will not report cannot be said to name anything, so the proof must fail
    // rather than fall back to a record it happens to find.
    test::FakeKeyValue store;
    const provisioning_flow::StagedPair only = seed_working_device(store);

    ASSERT_TRUE(
        candidate::pair_is_active(store, only.configuration_generation, only.secret_generation, 1));

    store.fail_read(storage::Namespace::System, kConfigPointer);
    EXPECT_FALSE(
        candidate::pair_is_active(store, only.configuration_generation, only.secret_generation, 1));

    store.fail_read(storage::Namespace::System, kSecretPointer);
    EXPECT_FALSE(
        candidate::pair_is_active(store, only.configuration_generation, only.secret_generation, 1));
}

TEST(ActivePairProofTests, AnAbsentPointerIsReadableAndNamesNothing) {
    // Never activated: the read succeeds and reports kNoGeneration, which matches no requested
    // generation. A store that has never been written must not satisfy a pair proof.
    test::FakeKeyValue store;

    EXPECT_TRUE(config::active_generation(store).readable);
    EXPECT_EQ(config::active_generation(store).generation, config::kNoGeneration);
    EXPECT_TRUE(secrets::active_generation(store).readable);
    EXPECT_EQ(secrets::active_generation(store).generation, secrets::kNoGeneration);

    EXPECT_FALSE(candidate::pair_is_active(store, 1, 1, 1));
}

TEST(ActivePairProofTests, TheRightPointerWithTheWrongProfileStillFails) {
    // Exact pointer identity is necessary, not sufficient: the record at that generation must also
    // carry the profile the recovery record names.
    test::FakeKeyValue store;
    const provisioning_flow::StagedPair only = seed_working_device(store);

    EXPECT_TRUE(
        candidate::pair_is_active(store, only.configuration_generation, only.secret_generation, 1));
    EXPECT_FALSE(
        candidate::pair_is_active(store, only.configuration_generation, only.secret_generation, 9));
}

// --- When rolling forward is impossible ------------------------------------

TEST(RestorationTests, A_MISSING_TARGET_CONFIG_RESTORES_THE_EXACT_PREVIOUS_PAIR) {
    // ⛔ WHAT THE OLD DESIGN COULD NOT DO. If the target could not be finished it cleared the marker
    // and left the pair mixed forever, because nothing recorded what to go back to.
    test::FakeKeyValue store;
    seed_working_device(store);
    const provisioning_flow::StagedPair staged = stage_candidate(store);

    store.fail_write(storage::Namespace::System, kSecretPointer);
    EXPECT_FALSE(provisioning_flow::activate_pair(store, staged).activated);
    ASSERT_FALSE(active_pair(store).whole);

    // The staged configuration is destroyed before the repair can use it.
    store.forget(storage::Namespace::Config, config_slot(staged.configuration_generation));

    const candidate::Reconciliation reconciled = candidate::reconcile(store);

    EXPECT_EQ(reconciled.outcome, Outcome::Restored);
    EXPECT_EQ(reconciled.profile, 1U);
    EXPECT_FALSE(reconciled.marker_retained);

    // ⬅️ THE DEVICE IS BACK ON THE PAIR IT WAS RUNNING, WHOLE AND USABLE. §13.3 step 7, performed.
    const PairView after = active_pair(store);
    EXPECT_TRUE(after.whole);
    EXPECT_EQ(after.configuration_profile, 1U);
    EXPECT_EQ(after.secret_profile, 1U);
    EXPECT_EQ(after.host, "192.168.1.10");
    EXPECT_TRUE(
        compose_configuration(config::load(store), secrets::load(store)).has_printer_configuration);
}

TEST(RestorationTests, A_MISSING_TARGET_SECRET_ALSO_RESTORES_THE_EXACT_PREVIOUS_PAIR) {
    // The credential half of the same case. The old design activated the secrets pointer by
    // arithmetic with no record behind it, which could move the pointer BELOW both real records and
    // leave the credential permanently unreachable.
    test::FakeKeyValue store;
    seed_working_device(store);
    const provisioning_flow::StagedPair staged = stage_candidate(store);

    store.fail_write(storage::Namespace::System, kConfigPointer);
    EXPECT_FALSE(provisioning_flow::activate_pair(store, staged).activated);

    store.forget(storage::Namespace::Secrets, secret_slot(staged.secret_generation));

    const candidate::Reconciliation reconciled = candidate::reconcile(store);

    EXPECT_EQ(reconciled.outcome, Outcome::Restored);
    EXPECT_EQ(reconciled.profile, 1U);

    const PairView after = active_pair(store);
    EXPECT_TRUE(after.whole);
    EXPECT_EQ(after.configuration_profile, 1U);
    EXPECT_EQ(after.secret_profile, 1U);
}

TEST(RestorationTests, A_CORRUPT_TARGET_RECORD_IS_NOT_ACTIVATED) {
    // A record that exists but fails its checksum is refused by exactly the standard load() applies
    // to the active record — so the repair falls back rather than activating rot.
    test::FakeKeyValue store;
    seed_working_device(store);
    const provisioning_flow::StagedPair staged = stage_candidate(store);

    store.fail_write(storage::Namespace::System, kSecretPointer);
    EXPECT_FALSE(provisioning_flow::activate_pair(store, staged).activated);

    std::vector<std::uint8_t> bytes =
        store.peek(storage::Namespace::Config, config_slot(staged.configuration_generation));
    ASSERT_FALSE(bytes.empty());
    bytes[bytes.size() - 1] = static_cast<std::uint8_t>(bytes[bytes.size() - 1] ^ 0xFFU);
    store.poke(storage::Namespace::Config, config_slot(staged.configuration_generation), bytes);

    const candidate::Reconciliation reconciled = candidate::reconcile(store);

    EXPECT_EQ(reconciled.outcome, Outcome::Restored);
    EXPECT_TRUE(active_pair(store).whole);
    EXPECT_EQ(active_pair(store).configuration_profile, 1U);
}

TEST(RestorationTests, A_TARGET_SECRET_FOR_THE_WRONG_PROFILE_IS_NOT_ACTIVATED) {
    // ⛔ THE UNGUARDED CREDENTIAL ACTIVATION THIS REPLACES. Here the record exists and decodes but
    // belongs to profile 5, so pairing it with profile 2's configuration would be a mixed pair with
    // no sign that anything was wrong.
    test::FakeKeyValue store;
    seed_working_device(store);
    const provisioning_flow::StagedPair staged = stage_candidate(store);

    store.fail_write(storage::Namespace::System, kSecretPointer);
    EXPECT_FALSE(provisioning_flow::activate_pair(store, staged).activated);

    // Build a valid credential record for a DIFFERENT profile at the same generation, in a scratch
    // store, then move its bytes into the slot the target occupies.
    secrets::SecretConfigV1 foreign = secrets::defaults();
    foreign.profile_id = 5;
    (void)foreign.wifi_ssid.assign("network");
    (void)foreign.wifi_password.assign("password");
    (void)foreign.printer_api_key.assign("new-key");

    test::FakeKeyValue scratch;
    for (std::uint32_t written = 0; written < staged.secret_generation; ++written) {
        const secrets::StageReport report = secrets::stage(scratch, foreign);
        ASSERT_EQ(report.outcome, secrets::SaveOutcome::Committed);
        ASSERT_EQ(secrets::activate(scratch, report.generation), secrets::SaveOutcome::Committed);
    }

    store.poke(storage::Namespace::Secrets, secret_slot(staged.secret_generation),
               scratch.peek(storage::Namespace::Secrets, secret_slot(staged.secret_generation)));

    // Precondition: the bytes now decode at the right generation but name the wrong profile.
    const secrets::LoadReport foreign_read = secrets::load_staged(store, staged.secret_generation);
    ASSERT_EQ(foreign_read.outcome, secrets::LoadOutcome::Loaded);
    ASSERT_EQ(foreign_read.value.profile_id, 5U);

    const candidate::Reconciliation reconciled = candidate::reconcile(store);

    // ⛔ ROLLING FORWARD IS REFUSED, AND PROFILE 5 NEVER BECOMES ACTIVE.
    EXPECT_NE(reconciled.outcome, Outcome::RolledForward);
    EXPECT_NE(active_pair(store).secret_profile, 5U);
    EXPECT_EQ(reconciled.outcome, Outcome::Restored);
    EXPECT_TRUE(active_pair(store).whole);
    EXPECT_EQ(active_pair(store).configuration_profile, 1U);
}

TEST(RestorationTests, A_WRONG_PROFILE_TARGET_IS_REFUSED_BEFORE_ANY_POINTER_MOVES) {
    // ⛔ WHY THE PRE-ACTIVATION GUARD EARNS ITS KEEP EVEN THOUGH THE REREAD ALSO CATCHES THIS. When
    // restoration is ALSO impossible, the two barriers give different outcomes: proving the pair
    // first means nothing moved and the store is exactly as the boot found it, while trusting the
    // writes and catching it afterwards leaves the config pointer on the target — a mixed pair, worse
    // than what it started from, with the record cleared or not.
    //
    // Reached with a first provisioning (no previous pair to restore) whose credential names another
    // profile.
    test::FakeKeyValue store;
    const provisioning_flow::StagedPair staged = stage_candidate(store);

    store.fail_write(storage::Namespace::System, kSecretPointer);
    EXPECT_FALSE(provisioning_flow::activate_pair(store, staged).activated);

    // Replace the target credential with a valid record for a DIFFERENT profile.
    secrets::SecretConfigV1 foreign = secrets::defaults();
    foreign.profile_id = 5;
    (void)foreign.wifi_ssid.assign("network");
    (void)foreign.wifi_password.assign("password");
    (void)foreign.printer_api_key.assign("new-key");

    test::FakeKeyValue scratch;
    for (std::uint32_t written = 0; written < staged.secret_generation; ++written) {
        const secrets::StageReport report = secrets::stage(scratch, foreign);
        ASSERT_EQ(report.outcome, secrets::SaveOutcome::Committed);
        ASSERT_EQ(secrets::activate(scratch, report.generation), secrets::SaveOutcome::Committed);
    }

    store.poke(storage::Namespace::Secrets, secret_slot(staged.secret_generation),
               scratch.peek(storage::Namespace::Secrets, secret_slot(staged.secret_generation)));

    // Both pointers as the boot finds them. The config one was already moved by activate_pair before
    // the secret write failed; that is the mixed state reconciliation inherits, not something it did.
    std::uint32_t config_pointer_before = 0;
    std::uint32_t secret_pointer_before = 0;
    (void)store.read_u32(storage::Namespace::System, kConfigPointer, config_pointer_before);
    const storage::Result secret_pointer_read =
        store.read_u32(storage::Namespace::System, kSecretPointer, secret_pointer_before);

    const candidate::Reconciliation reconciled = candidate::reconcile(store);

    EXPECT_EQ(reconciled.outcome, Outcome::Unresolved);
    EXPECT_TRUE(reconciled.marker_retained);

    // ⛔ THE DISCRIMINATOR. NEITHER pointer moved, because nothing was proved. Without the
    // pre-activation guard the credential pointer would have been written to the target generation
    // and only the reread would have noticed — leaving the store worse than the boot found it, with
    // profile 2's configuration paired against profile 5's credential.
    std::uint32_t config_pointer_after = 0;
    std::uint32_t secret_pointer_after = 0;
    (void)store.read_u32(storage::Namespace::System, kConfigPointer, config_pointer_after);
    const storage::Result secret_pointer_after_read =
        store.read_u32(storage::Namespace::System, kSecretPointer, secret_pointer_after);

    EXPECT_EQ(config_pointer_after, config_pointer_before);
    EXPECT_EQ(secret_pointer_after_read, secret_pointer_read);
    EXPECT_EQ(secret_pointer_after, secret_pointer_before);
    EXPECT_NE(secrets::load(store).value.profile_id, 5U);
}

TEST(RestorationTests, NEITHER_PAIR_PROVABLE_RETAINS_THE_EVIDENCE_AND_REFUSES_TO_RESOLVE) {
    // The worst case, and it must be reported as such rather than papered over. A first provisioning
    // has no previous pair, so a destroyed target leaves nothing to prove.
    test::FakeKeyValue store;
    const provisioning_flow::StagedPair staged = stage_candidate(store);

    store.fail_write(storage::Namespace::System, kSecretPointer);
    EXPECT_FALSE(provisioning_flow::activate_pair(store, staged).activated);

    store.forget(storage::Namespace::Secrets, secret_slot(staged.secret_generation));

    const candidate::Reconciliation reconciled = candidate::reconcile(store);

    EXPECT_EQ(reconciled.outcome, Outcome::Unresolved);
    EXPECT_FALSE(candidate::resolved(reconciled.outcome));
    EXPECT_TRUE(reconciled.marker_retained);

    // ⛔ THE EVIDENCE SURVIVES. Nothing is erased (R10), so a later boot — one where a transient
    // failure has passed — can still finish the job.
    EXPECT_EQ(candidate::read_marker(store).outcome, MarkerOutcome::Present);

    // POSITIVE CONTROL: with the record intact the same sequence DOES resolve, so the refusal above
    // is the missing record and not an executor that never succeeds.
    test::FakeKeyValue healthy;
    const provisioning_flow::StagedPair intact = stage_candidate(healthy);
    healthy.fail_write(storage::Namespace::System, kSecretPointer);
    EXPECT_FALSE(provisioning_flow::activate_pair(healthy, intact).activated);
    EXPECT_EQ(candidate::reconcile(healthy).outcome, Outcome::RolledForward);
}

TEST(RestorationTests, ARecordNamingGenerationsThatNoLongerExistResolvesNothingAndMovesNothing) {
    // A stale record pointing at generations that have since been overwritten must not activate
    // whatever now occupies those slots.
    test::FakeKeyValue store;
    seed_working_device(store);

    Recovery stale;
    stale.profile = 77;
    stale.target_configuration = 900;
    stale.target_secret = 900;
    stale.previous_configuration = 800;
    stale.previous_secret = 800;
    ASSERT_TRUE(candidate::write_marker(store, stale));

    const candidate::Reconciliation reconciled = candidate::reconcile(store);

    EXPECT_EQ(reconciled.outcome, Outcome::Unresolved);
    EXPECT_TRUE(reconciled.marker_retained);

    // ⛔ AND NOTHING MOVED. The working pair the device had is exactly where it was.
    const PairView after = active_pair(store);
    EXPECT_TRUE(after.whole);
    EXPECT_EQ(after.configuration_profile, 1U);
    EXPECT_EQ(after.host, "192.168.1.10");
}

TEST(RestorationTests, TheRecoveryPathNeverRevalidatesAndNeverProbes) {
    // ⛔ THE RECORD'S MEANING IS "VALIDATION ALREADY SUCCEEDED". §15.1 keeps a network dependency out
    // of the boot decision, so an activation reached from the boot path must rest on the record and on
    // the records it names — never on contacting a printer. The proof is structural: reconcile() takes
    // a storage::KeyValue and nothing else, so it has nothing to probe with.
    test::FakeKeyValue store;
    seed_working_device(store);
    const provisioning_flow::StagedPair staged = stage_candidate(store);

    store.fail_write(storage::Namespace::System, kSecretPointer);
    EXPECT_FALSE(provisioning_flow::activate_pair(store, staged).activated);

    ASSERT_EQ(candidate::reconcile(store).outcome, Outcome::RolledForward);

    // The rolled-forward record is validated, because it was validated before it was staged — not
    // because the boot path asked anyone.
    EXPECT_TRUE(config::load(store).value.printer_validated);
    EXPECT_TRUE(
        compose_configuration(config::load(store), secrets::load(store)).has_printer_configuration);
}

TEST(RestorationTests, AnOrphanedCandidateWithNoRecordIsNeverActivated) {
    // A staged candidate whose probe never succeeded has no recovery record, so nothing may promote
    // it — and the previous pair keeps running.
    test::FakeKeyValue store;
    seed_working_device(store);
    stage_candidate(store);

    EXPECT_EQ(candidate::reconcile(store).outcome, Outcome::Nothing);

    const PairView after = active_pair(store);
    EXPECT_TRUE(after.whole);
    EXPECT_EQ(after.configuration_profile, 1U);
    EXPECT_EQ(after.host, "192.168.1.10");
}

// --- Mapping a failed probe onto something showable ------------------------

TEST(PrinterFailureMappingTests, EachConnectionFailureBecomesADistinctActionableCause) {
    // Owner ruling D-6: each value names something the user can act on. Collapsing these would
    // tell someone with a mistyped API key to go and check their cables.
    EXPECT_EQ(provisioning_flow::failure_for_printer(app_model::ConnectionPhase::Unauthorized),
              Failure::PrinterUnauthorized);
    EXPECT_EQ(provisioning_flow::failure_for_printer(app_model::ConnectionPhase::UnsupportedApi),
              Failure::PrinterUnsupported);
    EXPECT_EQ(provisioning_flow::failure_for_printer(app_model::ConnectionPhase::InvalidResponse),
              Failure::PrinterUnusableResponse);
    EXPECT_EQ(
        provisioning_flow::failure_for_printer(app_model::ConnectionPhase::PrinterUnavailable),
        Failure::PrinterUnreachable);
}

TEST(PrinterFailureMappingTests, NoPhaseEverMapsToNoFailure) {
    // A caller asking why validation failed must never be handed "nothing failed" as the answer:
    // it would settle the transaction as a success with no printer behind it.
    constexpr app_model::ConnectionPhase kEveryPhase[] = {
        app_model::ConnectionPhase::Unconfigured,
        app_model::ConnectionPhase::WaitingForNetwork,
        app_model::ConnectionPhase::Resolving,
        app_model::ConnectionPhase::Probing,
        app_model::ConnectionPhase::Authenticating,
        app_model::ConnectionPhase::Connected,
        app_model::ConnectionPhase::Degraded,
        app_model::ConnectionPhase::Unauthorized,
        app_model::ConnectionPhase::PrinterUnavailable,
        app_model::ConnectionPhase::UnsupportedApi,
        app_model::ConnectionPhase::InvalidResponse,
    };

    for (const app_model::ConnectionPhase phase : kEveryPhase) {
        const Failure failure = provisioning_flow::failure_for_printer(phase);

        EXPECT_NE(failure, Failure::None) << static_cast<int>(phase);

        // And never a Wi-Fi or storage cause: a printer probe that failed says nothing about the
        // network join that already succeeded, or about a write that already committed.
        EXPECT_NE(failure, Failure::WifiUnreachable);
        EXPECT_NE(failure, Failure::WifiTimedOut);
        EXPECT_NE(failure, Failure::StorageRejected);
        EXPECT_NE(failure, Failure::StorageCandidateFailed);
        EXPECT_NE(failure, Failure::StorageCommitFailed);
    }
}

TEST(PrinterFailureMappingTests, EveryFailureHasItsOwnNonSecretLabel) {
    // The labels reach the browser and the log. They must be distinct — a shared label would make
    // two different problems indistinguishable — and none may echo anything the user submitted.
    constexpr Failure kPrinterFailures[] = {
        Failure::PrinterUnreachable,      Failure::PrinterUnauthorized, Failure::PrinterUnsupported,
        Failure::PrinterUnusableResponse, Failure::PromotionFailed,
    };

    for (const Failure lhs : kPrinterFailures) {
        EXPECT_FALSE(provisioning_flow::log_label(lhs).empty());
        EXPECT_NE(provisioning_flow::log_label(lhs), provisioning_flow::log_label(Failure::None));

        for (const Failure rhs : kPrinterFailures) {
            if (lhs == rhs) {
                continue;
            }

            EXPECT_NE(provisioning_flow::log_label(lhs), provisioning_flow::log_label(rhs));
        }
    }
}

} // namespace
} // namespace prd::app_core
