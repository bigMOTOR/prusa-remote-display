// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include <gtest/gtest.h>

#include <string>

#include "configuration_observation.hpp"
#include "provisioning_decision.hpp"
#include "provisioning_flow.hpp"
#include "support/fake_key_value.hpp"

// The provisioning transaction — owner rulings D-1 to D-6, 2026-08-16.
//
// THE POWER-LOSS TABLE FROM THE M3.8 RESTATEMENT IS EXECUTABLE HERE. Each row breaks the
// transaction at one step and then asks the real boot decision — decide_provisioning() over
// the real config::load() and secrets::load() — what the device would come up as. That is
// the property the whole design rests on: a half-written transaction boots as
// un-provisioned, never as half-configured.

namespace prd::app_core {
namespace {

using provisioning_flow::Failure;
using provisioning_flow::Policy;
using provisioning_flow::Verdict;
using provisioning_flow::WifiValidation;

// The station request identities these tests judge candidates by. §8.1's "request generation or
// sequence IDs", spelled as constants so a test that means "the other candidate" says so.
using Outcome = prd::network::station::Outcome;
using Tag = prd::network::station::Tag;

constexpr Tag kCandidate = 4;
constexpr Tag kSuperseded = 3;
constexpr Tag kSuccessor = 5;

constexpr std::string_view kConfigSlot0 = "cfg.0";
constexpr std::string_view kConfigSlot1 = "cfg.1";
constexpr std::string_view kConfigGeneration = "cfg.gen";
constexpr std::string_view kSecretSlot0 = "sec.0";
constexpr std::string_view kSecretSlot1 = "sec.1";
constexpr std::string_view kSecretGeneration = "sec.gen";

[[nodiscard]] provisioning_flow::Submission a_submission() {
    provisioning_flow::Submission submission;
    submission.wifi_ssid.assign("Home Network");
    submission.wifi_password.assign("correct horse battery");
    submission.printer_host.assign("192.168.1.50");
    submission.printer_api_key.assign("Zx9QsT4bC1");
    submission.printer_port = 80;
    return submission;
}

// What the device would boot into, asked of the real code rather than asserted.
[[nodiscard]] ProvisioningDecision boot_decision(test::FakeKeyValue& store) {
    const config::LoadReport configuration = config::load(store);
    const secrets::LoadReport secret = secrets::load(store);
    const app_model::ConfigurationObservation observation =
        compose_configuration(configuration, secret);

    return decide_provisioning(configuration.outcome, secret.outcome,
                               observation.has_wifi_credentials);
}

// --- D-1: what counts as a validated Wi-Fi configuration -------------------
//
// ⛔ EVIDENCE IS NOW IDENTIFIED, NOT INSPECTED — checkpoint 3c. These tests used to hand the machine
// a NetworkPhase and rely on a guard that disregarded reports equal to the phase the attempt began
// with. That guard existed because the station published through a queue, so the first report after
// a restart still described the PREVIOUS transaction — and it could not tell "the old attempt's
// answer" from "the new attempt reached the same state". The station now charges every answer to the
// request identity that provoked it, and this machine judges by that identity alone.

TEST(WifiValidationTests, AnIdleMachineHasNoVerdict) {
    WifiValidation validation;
    EXPECT_EQ(validation.verdict(), Verdict::Idle);
    EXPECT_EQ(validation.failure(), Failure::None);
    EXPECT_FALSE(validation.dwelling());
}

TEST(WifiValidationTests, ALeaseAloneIsNotEnoughUntilTheDwellElapses) {
    WifiValidation validation;
    validation.begin(0, provisioning_flow::Candidate{kCandidate});

    EXPECT_EQ(validation.tick(100), Verdict::Pending);
    EXPECT_EQ(validation.observe(kCandidate, Outcome::Leased, 200), Verdict::Pending);
    EXPECT_TRUE(validation.dwelling());

    // One millisecond short of the two-second dwell.
    EXPECT_EQ(validation.tick(2199), Verdict::Pending);
    EXPECT_EQ(validation.tick(2200), Verdict::Validated);
    EXPECT_EQ(validation.failure(), Failure::None);
}

TEST(WifiValidationTests, LosingTheLeaseInsideTheDwellRestartsIt) {
    WifiValidation validation;
    validation.begin(0, provisioning_flow::Candidate{kCandidate});

    EXPECT_EQ(validation.observe(kCandidate, Outcome::Leased, 100), Verdict::Pending);
    EXPECT_EQ(validation.observe(kCandidate, Outcome::LeaseInterrupted, 1000), Verdict::Pending);
    EXPECT_FALSE(validation.dwelling());

    // The old dwell would have completed at 2100. It does not.
    EXPECT_EQ(validation.tick(2100), Verdict::Pending);

    EXPECT_EQ(validation.observe(kCandidate, Outcome::Leased, 3000), Verdict::Pending);
    EXPECT_EQ(validation.tick(4999), Verdict::Pending);
    EXPECT_EQ(validation.tick(5000), Verdict::Validated);
}

TEST(WifiValidationTests, ARepeatedLeaseDoesNotPushTheDwellOut) {
    WifiValidation validation;
    validation.begin(0, provisioning_flow::Candidate{kCandidate});

    EXPECT_EQ(validation.observe(kCandidate, Outcome::Leased, 100), Verdict::Pending);
    EXPECT_EQ(validation.observe(kCandidate, Outcome::Leased, 1500), Verdict::Pending);
    EXPECT_EQ(validation.tick(2100), Verdict::Validated);
}

TEST(WifiValidationTests, UnreachableFailsImmediatelyRatherThanWaitingOutTheTimeout) {
    WifiValidation validation;
    validation.begin(0, provisioning_flow::Candidate{kCandidate});

    EXPECT_EQ(validation.tick(100), Verdict::Pending);
    EXPECT_EQ(validation.observe(kCandidate, Outcome::Unreachable, 6000), Verdict::Failed);
    EXPECT_EQ(validation.failure(), Failure::WifiUnreachable);
}

TEST(WifiValidationTests, AStationThatNeverLeasesTimesOutAtThirtySeconds) {
    WifiValidation validation;
    validation.begin(0, provisioning_flow::Candidate{kCandidate});

    EXPECT_EQ(validation.tick(100), Verdict::Pending);
    EXPECT_EQ(validation.tick(29'999), Verdict::Pending);
    EXPECT_EQ(validation.tick(30'000), Verdict::Failed);
    EXPECT_EQ(validation.failure(), Failure::WifiTimedOut);
}

// An associated station with no DHCP lease reports nothing at all, so it reaches the user as a
// timeout. Pinned so the behaviour is a decision rather than a surprise. Checkpoint 3c bounds how
// long the station itself will tolerate that state before retiring the association, which is a
// separate mechanism from this budget and is tested in station_intent_tests.cpp.
TEST(WifiValidationTests, ADhcpFailureSurfacesAsATimeout) {
    WifiValidation validation;
    validation.begin(0, provisioning_flow::Candidate{kCandidate});

    for (std::uint32_t now = 1000; now < 30'000; now += 1000) {
        EXPECT_EQ(validation.tick(now), Verdict::Pending);
    }

    EXPECT_EQ(validation.tick(30'000), Verdict::Failed);
    EXPECT_EQ(validation.failure(), Failure::WifiTimedOut);
}

// A dwell that completes on the same millisecond the budget expires is a success.
TEST(WifiValidationTests, TheDwellIsCheckedBeforeTheTimeout) {
    WifiValidation validation;
    validation.begin(0, provisioning_flow::Candidate{kCandidate});

    EXPECT_EQ(validation.observe(kCandidate, Outcome::Leased, 28'000), Verdict::Pending);
    EXPECT_EQ(validation.tick(30'000), Verdict::Validated);
}

TEST(WifiValidationTests, ASettledVerdictIsNotRevisited) {
    WifiValidation validation;
    validation.begin(0, provisioning_flow::Candidate{kCandidate});

    EXPECT_EQ(validation.observe(kCandidate, Outcome::Unreachable, 100), Verdict::Failed);
    EXPECT_EQ(validation.observe(kCandidate, Outcome::Leased, 200), Verdict::Failed);
    EXPECT_EQ(validation.tick(10'000), Verdict::Failed);
    EXPECT_EQ(validation.failure(), Failure::WifiUnreachable);
}

// ⛔ R11: THE ARMING READING AND THE FIRST TICK'S READING ARE ONE OBSERVATION.
//
// provisioning_tick() reads the clock once per iteration and passes that reading into
// begin_transaction(), which uses it for BOTH network::station::submit() and begin(). So on the
// arming iteration the elapsed time is exactly zero and the attempt is Pending.
//
// Pinned at a boot-relative timestamp rather than 0 on purpose: 0 is the one base where a second,
// later clock reading would still have produced a plausible elapsed instead of an underflow. The
// 2026-08-26 board run armed at ~256,969 ms while the same iteration's tick carried ~256,966 ms,
// and unsigned `now - start` turned three milliseconds into roughly 4.29e9 — every submission was
// failed as WifiTimedOut one millisecond after being accepted, while the station went on to
// associate and lease an address perfectly.
//
// ⬅️ THIS IS NOT A TEST OF elapsed_since(), AND IT MUST NOT BECOME ONE. That function's unsigned
// subtraction is correct and is exactly what the wrap test below depends on: a guard against
// `start > now` would break a genuine 32-bit wrap while hiding this defect. What is pinned here is
// the binding's contract, which is the half a host test can reach.
TEST(WifiValidationTests, TheArmingReadingAndTheFirstTickAreOneObservation) {
    WifiValidation validation;
    const std::uint32_t armed_at = 256'969;
    validation.begin(armed_at, provisioning_flow::Candidate{kCandidate});

    EXPECT_EQ(validation.tick(armed_at), Verdict::Pending);
    EXPECT_EQ(validation.failure(), Failure::None);
}

TEST(WifiValidationTests, AMonotonicClockThatWrapsDoesNotFailTheAttempt) {
    WifiValidation validation;
    const std::uint32_t near_wrap = 0xFFFFFF00U;
    validation.begin(near_wrap, provisioning_flow::Candidate{kCandidate});

    EXPECT_EQ(validation.observe(kCandidate, Outcome::Leased, near_wrap + 100), Verdict::Pending);
    EXPECT_EQ(validation.tick(near_wrap + 2100), Verdict::Validated);
}

// --- §8.1: whose answer is this? -------------------------------------------

TEST(WifiValidationTests, StaleEvidenceFromASupersededAttemptCannotValidateThisCandidate) {
    // Test 7. The superseded attempt is still running inside the driver when a new credential is
    // submitted, and its lease — for the OLD network — arrives afterwards. Believing it would
    // persist a configuration naming a network nobody proved.
    WifiValidation validation;
    validation.begin(0, provisioning_flow::Candidate{kCandidate});

    EXPECT_EQ(validation.observe(kSuperseded, Outcome::Leased, 100), Verdict::Pending);
    EXPECT_FALSE(validation.dwelling()) << "no dwell was opened by another candidate's lease";

    // The dwell would have completed at 2100 had the stale lease counted.
    EXPECT_EQ(validation.tick(2100), Verdict::Pending);
    EXPECT_EQ(validation.tick(30'000), Verdict::Failed);
    EXPECT_EQ(validation.failure(), Failure::WifiTimedOut)
        << "the attempt timed out on its own evidence, not on somebody else's";
}

TEST(WifiValidationTests, StaleEvidenceFromASupersededAttemptCannotFailThisCandidate) {
    // The mirror, and the hardware failure of 2026-08-18: a terminal Unavailable left over from a
    // genuinely rejected password failed a corrected retry 6 ms after it was accepted, while the
    // station went on to associate and lease an address perfectly.
    WifiValidation validation;
    validation.begin(0, provisioning_flow::Candidate{kCandidate});

    EXPECT_EQ(validation.observe(kSuperseded, Outcome::Unreachable, 10), Verdict::Pending);
    EXPECT_EQ(validation.observe(kSuperseded, Outcome::DriverStalled, 20), Verdict::Pending);
    EXPECT_EQ(validation.observe(kSuperseded, Outcome::EvidenceLost, 30), Verdict::Pending);

    EXPECT_EQ(validation.observe(kCandidate, Outcome::Leased, 100), Verdict::Pending);
    EXPECT_EQ(validation.tick(2100), Verdict::Validated);
}

TEST(WifiValidationTests, EvidenceForALaterCandidateCannotSettleThisOne) {
    // Test 8. Candidate B's machine must not be settled by candidate C's evidence either: after
    // supersession the app begins a NEW attempt against a new identity, and the old machine — if
    // anything still held one — may not act on the successor's answers.
    WifiValidation validation;
    validation.begin(0, provisioning_flow::Candidate{kCandidate});

    EXPECT_EQ(validation.observe(kSuccessor, Outcome::Leased, 100), Verdict::Pending);
    EXPECT_EQ(validation.tick(2100), Verdict::Pending);
    EXPECT_EQ(validation.observe(kSuccessor, Outcome::Unreachable, 3000), Verdict::Pending);
    EXPECT_EQ(validation.verdict(), Verdict::Pending);
}

TEST(WifiValidationTests, EvidenceWithNoIdentityDecidesNothing) {
    // kNoTag means "no request". A station that reported without one would be reporting about
    // nothing, and it must not be able to settle a transaction.
    WifiValidation validation;
    validation.begin(0, provisioning_flow::Candidate{kCandidate});

    EXPECT_EQ(validation.observe(prd::network::station::kNoTag, Outcome::Leased, 100),
              Verdict::Pending);
    EXPECT_FALSE(validation.dwelling());
    EXPECT_EQ(validation.tick(2100), Verdict::Pending);
}

TEST(WifiValidationNegativeControlTests, TheIdentityCheckCanFail) {
    // The three tests above are only worth anything if the SAME evidence, charged to the right
    // identity, does settle the transaction. Both directions, side by side.
    WifiValidation ignored;
    ignored.begin(0, provisioning_flow::Candidate{kCandidate});
    EXPECT_EQ(ignored.observe(kSuperseded, Outcome::Unreachable, 10), Verdict::Pending);

    WifiValidation honoured;
    honoured.begin(0, provisioning_flow::Candidate{kCandidate});
    EXPECT_EQ(honoured.observe(kCandidate, Outcome::Unreachable, 10), Verdict::Failed);
    EXPECT_EQ(honoured.failure(), Failure::WifiUnreachable);
}

// --- The owner's DriverStalled ruling, as the candidate sees it -------------

TEST(WifiValidationTests, AStalledDriverIsItsOwnTypedFailure) {
    // ⛔ NOT WifiUnreachable. That reason sends the user to check the network name and the password;
    // a driver that never answered tested neither, and blaming their credential for it would be a
    // lie the browser then repeats.
    WifiValidation validation;
    validation.begin(0, provisioning_flow::Candidate{kCandidate});

    EXPECT_EQ(validation.observe(kCandidate, Outcome::DriverStalled, 500), Verdict::Failed);
    EXPECT_EQ(validation.failure(), Failure::WifiDriverStalled);
}

TEST(WifiValidationTests, LostEvidenceIsItsOwnTypedFailure) {
    // Not Succeeded (§14.3 step 9) and not CredentialsRejected: the record a verdict would rest on
    // is missing, so no verdict about the candidate can be honest.
    WifiValidation validation;
    validation.begin(0, provisioning_flow::Candidate{kCandidate});

    EXPECT_EQ(validation.observe(kCandidate, Outcome::EvidenceLost, 500), Verdict::Failed);
    EXPECT_EQ(validation.failure(), Failure::WifiEvidenceLost);
}

TEST(WifiValidationTests, EveryTerminalOutcomeSettlesTheTransactionExactlyOnce) {
    const Outcome terminal[] = {Outcome::Unreachable, Outcome::DriverStalled,
                                Outcome::EvidenceLost};

    for (const Outcome outcome : terminal) {
        WifiValidation validation;
        validation.begin(0, provisioning_flow::Candidate{kCandidate});

        EXPECT_EQ(validation.observe(kCandidate, outcome, 100), Verdict::Failed);
        EXPECT_NE(validation.failure(), Failure::None);

        const Failure settled = validation.failure();
        EXPECT_EQ(validation.observe(kCandidate, Outcome::Leased, 200), Verdict::Failed);
        EXPECT_EQ(validation.failure(), settled);
    }
}

// ⛔ EVERY FAILURE THE STATION CAN PRODUCE HAS A DISTINCT, NON-SECRET REASON LABEL. That label is
// the whole mechanism the browser and the panel read (§13.5: no submitted byte may appear), so a
// new failure that shared another's label would be reported as the wrong thing.
TEST(WifiValidationTests, EveryStationFailureHasItsOwnNonSecretLabel) {
    const Failure failures[] = {Failure::WifiUnreachable, Failure::WifiTimedOut,
                                Failure::WifiDriverStalled, Failure::WifiEvidenceLost};

    for (const Failure failure : failures) {
        const std::string label{provisioning_flow::log_label(failure)};

        EXPECT_FALSE(label.empty());
        EXPECT_NE(label, "none");

        // Bounded by ServerSnapshot::setup_reason's capacity, or the reason is truncated where the
        // reader most needs it.
        EXPECT_LE(label.size(), 32U);
    }

    EXPECT_NE(provisioning_flow::log_label(Failure::WifiDriverStalled),
              provisioning_flow::log_label(Failure::WifiUnreachable));
    EXPECT_NE(provisioning_flow::log_label(Failure::WifiDriverStalled),
              provisioning_flow::log_label(Failure::WifiEvidenceLost));
    EXPECT_NE(provisioning_flow::log_label(Failure::WifiEvidenceLost),
              provisioning_flow::log_label(Failure::WifiTimedOut));
}

TEST(WifiValidationTests, PhaseMapsOntoTheProvisioningPhase) {
    EXPECT_EQ(provisioning_flow::phase_for(Verdict::Pending),
              app_model::ProvisioningPhase::ValidatingConfiguration);
    EXPECT_EQ(provisioning_flow::phase_for(Verdict::Validated),
              app_model::ProvisioningPhase::Inactive);
    EXPECT_EQ(provisioning_flow::phase_for(Verdict::Failed),
              app_model::ProvisioningPhase::SetupFailed);
}

// --- Composition: R4 and R8 ------------------------------------------------

TEST(ProvisioningRecordsTests, TheProfileIdBindsTheTwoRecords) {
    const auto records = provisioning_flow::compose(a_submission(), config::defaults(), 7);
    EXPECT_EQ(records.configuration.wifi_profile_id, 7U);
    EXPECT_EQ(records.secret.profile_id, 7U);
}

TEST(ProvisioningRecordsTests, TheProfileIdAlwaysMoves) {
    EXPECT_EQ(provisioning_flow::next_profile_id(0), 1U);
    EXPECT_EQ(provisioning_flow::next_profile_id(1), 2U);
    EXPECT_EQ(provisioning_flow::next_profile_id(0xFFFFFFFFU), 1U);
}

// R4, and there is deliberately no way to ask for anything else.
TEST(ProvisioningRecordsTests, ThePrinterIsNeverWrittenAsValidated) {
    const auto records = provisioning_flow::compose(a_submission(), config::defaults(), 1);
    EXPECT_FALSE(records.configuration.printer_validated);
}

TEST(ProvisioningRecordsTests, APrinterWithAKeyRecordsTheApiKeyAuthMethod) {
    const auto records = provisioning_flow::compose(a_submission(), config::defaults(), 1);
    EXPECT_EQ(records.configuration.auth_method, app_model::AuthMethod::ApiKey);
    EXPECT_EQ(records.secret.printer_api_key.view(), "Zx9QsT4bC1");
}

TEST(ProvisioningRecordsTests, APrinterWithoutAKeyRecordsNoAuthMethod) {
    auto submission = a_submission();
    submission.printer_api_key.clear();

    const auto records = provisioning_flow::compose(submission, config::defaults(), 1);
    EXPECT_EQ(records.configuration.auth_method, app_model::AuthMethod::None);
}

// A submission configures a network and a printer. It must not reset display preferences.
TEST(ProvisioningRecordsTests, PreferencesTheBrowserDidNotSupplyAreCarriedForward) {
    config::ConfigV1 base = config::defaults();
    base.brightness = 42;
    base.dim_delay_seconds = 111;
    base.sleep_delay_seconds = 222;

    const auto records = provisioning_flow::compose(a_submission(), base, 1);
    EXPECT_EQ(records.configuration.brightness, 42);
    EXPECT_EQ(records.configuration.dim_delay_seconds, 111U);
    EXPECT_EQ(records.configuration.sleep_delay_seconds, 222U);
}

TEST(ProvisioningRecordsTests, TheCredentialGoesOnlyIntoTheSecretRecord) {
    const auto records = provisioning_flow::compose(a_submission(), config::defaults(), 1);

    EXPECT_EQ(records.secret.wifi_ssid.view(), "Home Network");
    EXPECT_EQ(records.secret.wifi_password.view(), "correct horse battery");

    // ConfigV1 has no field that could hold either, which is the structural half of §13.2.
    // What it does carry is the handle, and a handle is not a credential.
    EXPECT_NE(records.configuration.wifi_profile_id, 0U);
}

// --- The transaction, and the power-loss table -----------------------------
//
// ⛔ commit() IS GONE, AND THESE TESTS NO LONGER PRETEND TO COVER IT. It wrote both records AND
// activated both, which is the activate-before-validation design §13.3 step 5 rules out once a
// connection test is applicable. It had no production caller left, so a full test suite around it
// read as live coverage of a path production cannot take. Every row below runs the REAL sequence —
// stage_pair() then, only on the success path, activate_pair().

// The production sequence, as one call. `activated` is false when the pair never became active,
// whatever the reason.
struct Transaction {
    provisioning_flow::StagedPair staged;
    provisioning_flow::ActivatedPair activated;
};

[[nodiscard]] Transaction run_transaction(test::FakeKeyValue& store,
                                          const provisioning_flow::Records& records) {
    Transaction transaction;
    transaction.staged = provisioning_flow::stage_pair(store, records);

    if (!transaction.staged.staged) {
        return transaction;
    }

    transaction.activated = provisioning_flow::activate_pair(store, transaction.staged);
    return transaction;
}

// ⬅️ A LEGACY RECORD, WRITTEN AS AN M3 FIRMWARE WROTE IT. This is what R8's `pending` state actually
// describes now: an active configuration with a printer address that was never contacted, because
// the firmware that wrote it had no way to contact one (R4). It is seeded directly rather than
// through a production call, because no production path can produce it any more — which is precisely
// why it needs a fixture.
void seed_legacy_unvalidated_pair(test::FakeKeyValue& store) {
    provisioning_flow::Records records =
        provisioning_flow::compose(a_submission(), config::defaults(), 1);

    ASSERT_FALSE(records.configuration.printer_validated);

    ASSERT_EQ(config::save(store, records.configuration), config::SaveOutcome::Committed);
    ASSERT_EQ(secrets::save(store, records.secret), secrets::SaveOutcome::Committed);
}

TEST(ProvisioningTransactionTests, ACleanTransactionWritesBothRecordsAndBootsConfigured) {
    test::FakeKeyValue store;
    provisioning_flow::Records records =
        provisioning_flow::compose(a_submission(), config::defaults(), 1);
    records.configuration.printer_validated = true;

    const Transaction transaction = run_transaction(store, records);

    EXPECT_TRUE(transaction.staged.staged);
    EXPECT_TRUE(transaction.staged.secret_attempted);
    EXPECT_TRUE(transaction.activated.activated);
    EXPECT_EQ(transaction.activated.failure, Failure::None);
    EXPECT_EQ(boot_decision(store), ProvisioningDecision::NotRequired);
}

TEST(ProvisioningTransactionTests, A_STAGED_CANDIDATE_IS_NOT_A_CONFIGURED_DEVICE) {
    // ⬅️ WHAT REPLACED "commit writes a pending printer". A candidate that was staged and never
    // activated leaves the device exactly as it was, so on a virgin device it still boots into setup.
    test::FakeKeyValue store;
    provisioning_flow::Records records =
        provisioning_flow::compose(a_submission(), config::defaults(), 1);
    records.configuration.printer_validated = true;

    ASSERT_TRUE(provisioning_flow::stage_pair(store, records).staged);

    const auto observation = compose_configuration(config::load(store), secrets::load(store));
    EXPECT_FALSE(observation.has_wifi_credentials);
    EXPECT_FALSE(observation.has_printer_configuration);
    EXPECT_FALSE(observation.printer_configuration_pending);
    EXPECT_EQ(boot_decision(store), ProvisioningDecision::StartSetup);
}

TEST(ProvisioningTransactionTests, R8_THREE_WAY_DISTINCTION_OVER_REAL_ACTIVE_RECORDS) {
    // ⛔ R8's THREE STATES, EACH FROM AN ACTIVE PAIR A DEVICE CAN ACTUALLY BE IN. The middle one is
    // the legacy case and is the only thing that still produces it: an M3-written record carrying an
    // address the firmware could not validate. Its boot consumer and its user experience remain an
    // owner decision; this test only pins that the three states stay DISTINGUISHABLE, which is what
    // R8 requires and what an upgraded device needs.
    {
        // 1. No printer configured at all.
        test::FakeKeyValue store;
        provisioning_flow::Records records =
            provisioning_flow::compose(a_submission(), config::defaults(), 1);
        // §13.2, enforced by validate(): with no address, no port, no auth method and no validated
        // flag may be asserted — a record describing a printer that was never configured.
        records.configuration.printer_host.clear();
        records.configuration.printer_port = 0;
        records.configuration.auth_method = app_model::AuthMethod::None;
        records.configuration.printer_validated = false;
        ASSERT_EQ(config::save(store, records.configuration), config::SaveOutcome::Committed);
        ASSERT_EQ(secrets::save(store, records.secret), secrets::SaveOutcome::Committed);

        const auto observation = compose_configuration(config::load(store), secrets::load(store));
        EXPECT_FALSE(observation.has_printer_configuration);
        EXPECT_FALSE(observation.printer_configuration_pending);
    }

    {
        // 2. Configured and never validated — the legacy record.
        test::FakeKeyValue store;
        seed_legacy_unvalidated_pair(store);

        const auto observation = compose_configuration(config::load(store), secrets::load(store));
        EXPECT_TRUE(observation.has_wifi_credentials);
        EXPECT_FALSE(observation.has_printer_configuration);
        EXPECT_TRUE(observation.printer_configuration_pending);

        // ⬅️ AND IT DOES NOT SEND THE DEVICE BACK INTO SETUP. The Wi-Fi credential is usable, so the
        // device comes up normally — with a printer it will not poll. That is the state the owner
        // decision is about, recorded here rather than acted on.
        EXPECT_EQ(boot_decision(store), ProvisioningDecision::NotRequired);
    }

    {
        // 3. Active and validated.
        test::FakeKeyValue store;
        provisioning_flow::Records records =
            provisioning_flow::compose(a_submission(), config::defaults(), 1);
        records.configuration.printer_validated = true;
        ASSERT_TRUE(run_transaction(store, records).activated.activated);

        const auto observation = compose_configuration(config::load(store), secrets::load(store));
        EXPECT_TRUE(observation.has_printer_configuration);
        EXPECT_FALSE(observation.printer_configuration_pending);
    }
}

// --- Row 3: power loss after the join, before any write --------------------

TEST(PowerLossTests, Row3_NothingWrittenBootsAsUnprovisioned) {
    test::FakeKeyValue store;
    EXPECT_EQ(boot_decision(store), ProvisioningDecision::StartSetup);
}

// --- Row 4: the configuration candidate could not be written ---------------

TEST(PowerLossTests, Row4_ConfigurationCandidateFailedLeavesNothingAndBootsUnprovisioned) {
    test::FakeKeyValue store;
    store.fail_write(storage::Namespace::Config, std::string{kConfigSlot0});
    store.fail_write(storage::Namespace::Config, std::string{kConfigSlot1});

    const auto records = provisioning_flow::compose(a_submission(), config::defaults(), 1);
    const Transaction transaction = run_transaction(store, records);

    EXPECT_FALSE(transaction.staged.staged);
    EXPECT_EQ(transaction.staged.failure, Failure::StorageCandidateFailed);

    // THE CREDENTIAL WAS NEVER ATTEMPTED — the whole point of the staging order.
    EXPECT_FALSE(transaction.staged.secret_attempted);
    EXPECT_FALSE(store.has(storage::Namespace::Secrets, std::string{kSecretSlot0}));

    EXPECT_EQ(boot_decision(store), ProvisioningDecision::StartSetup);
}

// --- Row 5: the credential candidate could not be written ------------------
//
// ⬅️ THE ROW THAT USED TO BE THE DANGEROUS ONE, AND IS NOW STRUCTURALLY SAFE. Under commit() the
// configuration had ALREADY been activated by this point, so a failed credential left a live
// half-configured device. Under staging neither pointer has moved, so a failure here is
// indistinguishable from never having started.

TEST(PowerLossTests, Row5_CredentialCandidateFailedLeavesNothingActive) {
    test::FakeKeyValue store;
    store.fail_write(storage::Namespace::Secrets, std::string{kSecretSlot0});
    store.fail_write(storage::Namespace::Secrets, std::string{kSecretSlot1});

    const auto records = provisioning_flow::compose(a_submission(), config::defaults(), 1);
    const Transaction transaction = run_transaction(store, records);

    EXPECT_FALSE(transaction.staged.staged);
    EXPECT_TRUE(transaction.staged.secret_attempted);
    EXPECT_EQ(transaction.staged.configuration, config::SaveOutcome::Committed);

    // ⛔ AND NOTHING IS ACTIVE. The configuration candidate is in flash, but the pointer never moved,
    // so no reader can see it — which is the property commit() could not offer.
    EXPECT_EQ(config::load(store).outcome, config::LoadOutcome::Absent);
    EXPECT_EQ(boot_decision(store), ProvisioningDecision::StartSetup);
}

// ⬅️ ROW 6, AND IT IS WHY THE ACTIVE-GENERATION POINTER IS THE BOUNDARY.
//
// The credential's candidate is written and read back and only the pointer move fails. The device
// used to boot fully CONFIGURED, because an absent pointer let secrets::load() fall through to
// previous-known-good recovery and adopt the candidate the transaction had abandoned. The owner
// ruled the boundary rather than the symptom: the active-generation pointer IS the activation, and
// bytes that survived a failed transaction are not an active record merely because they decode.

TEST(PowerLossTests, Row6_SecretActivationFailedBootsAsUnprovisioned) {
    test::FakeKeyValue store;
    store.fail_write(storage::Namespace::System, std::string{kSecretGeneration});

    provisioning_flow::Records records =
        provisioning_flow::compose(a_submission(), config::defaults(), 1);
    records.configuration.printer_validated = true;

    const Transaction transaction = run_transaction(store, records);

    // The transaction is honest about what happened.
    EXPECT_TRUE(transaction.staged.staged);
    EXPECT_FALSE(transaction.activated.activated);
    EXPECT_EQ(transaction.activated.failure, Failure::PromotionFailed);

    // The pointer really did not move.
    std::uint32_t pointer = 0;
    EXPECT_NE(store.read_u32(storage::Namespace::System, kSecretGeneration, pointer),
              storage::Result::Ok);

    // So no credential is active, however complete the bytes in the slot are...
    EXPECT_EQ(secrets::load(store).outcome, secrets::LoadOutcome::Absent);

    // ...and the device returns to setup rather than coming up half-configured.
    EXPECT_EQ(boot_decision(store), ProvisioningDecision::StartSetup);

    // Refused, not removed (R10). The bytes are still in the slot.
    EXPECT_TRUE(store.has(storage::Namespace::Secrets, std::string{kSecretSlot1}));
}

// --- Row 7: both activated -------------------------------------------------

TEST(PowerLossTests, Row7_BothActivatedBootsConfigured) {
    test::FakeKeyValue store;
    provisioning_flow::Records records =
        provisioning_flow::compose(a_submission(), config::defaults(), 1);
    records.configuration.printer_validated = true;

    ASSERT_TRUE(run_transaction(store, records).activated.activated);

    EXPECT_EQ(boot_decision(store), ProvisioningDecision::NotRequired);
}

// --- The binding itself ----------------------------------------------------
//
// Written directly rather than through the transaction, because the property must hold for any
// pair of records however they came to be on the device.

TEST(PowerLossTests, MismatchedProfileIdsReadAsUnprovisionedInEitherDirection) {
    {
        test::FakeKeyValue store;
        const auto records = provisioning_flow::compose(a_submission(), config::defaults(), 1);
        ASSERT_EQ(config::save(store, records.configuration), config::SaveOutcome::Committed);

        auto stale = records.secret;
        stale.profile_id = 2; // a credential from a previous transaction
        ASSERT_EQ(secrets::save(store, stale), secrets::SaveOutcome::Committed);

        EXPECT_EQ(boot_decision(store), ProvisioningDecision::StartSetup);
    }

    {
        test::FakeKeyValue store;
        const auto records = provisioning_flow::compose(a_submission(), config::defaults(), 2);
        ASSERT_EQ(secrets::save(store, records.secret), secrets::SaveOutcome::Committed);

        auto stale = records.configuration;
        stale.wifi_profile_id = 1; // a configuration from a previous transaction
        ASSERT_EQ(config::save(store, stale), config::SaveOutcome::Committed);

        EXPECT_EQ(boot_decision(store), ProvisioningDecision::StartSetup);
    }
}

// A failed transaction must not damage what the device was already using.
TEST(PowerLossTests, AFailedTransactionLeavesThePreviousRecordsIntact) {
    test::FakeKeyValue store;
    provisioning_flow::Records first =
        provisioning_flow::compose(a_submission(), config::defaults(), 1);
    first.configuration.printer_validated = true;
    ASSERT_TRUE(run_transaction(store, first).activated.activated);
    ASSERT_EQ(boot_decision(store), ProvisioningDecision::NotRequired);

    auto second_submission = a_submission();
    second_submission.wifi_ssid.assign("Another Network");
    const auto second = provisioning_flow::compose(second_submission, config::load(store).value, 2);

    // Break the candidate write of the second transaction. The candidate goes to the
    // inactive slot, so the active one cannot be touched.
    store.fail_write(storage::Namespace::Config, std::string{kConfigSlot0});
    store.fail_write(storage::Namespace::Config, std::string{kConfigSlot1});
    EXPECT_FALSE(run_transaction(store, second).activated.activated);

    // The device still holds — and still boots into — the first configuration.
    EXPECT_EQ(config::load(store).value.wifi_profile_id, 1U);
    EXPECT_EQ(secrets::load(store).value.wifi_ssid.view(), "Home Network");
    EXPECT_EQ(boot_decision(store), ProvisioningDecision::NotRequired);
}

// R10, restated as a test rather than a promise. The recovery record's own clear is the ONE erase the
// transaction performs, and it touches a key that holds no user data.
TEST(PowerLossTests, NoPathThroughTheTransactionErasesAnyUserRecord) {
    test::FakeKeyValue store;
    provisioning_flow::Records records =
        provisioning_flow::compose(a_submission(), config::defaults(), 1);
    records.configuration.printer_validated = true;

    ASSERT_TRUE(run_transaction(store, records).activated.activated);

    // Every slot and both pointers are still there.
    EXPECT_TRUE(store.has(storage::Namespace::Config, std::string{kConfigSlot1}));
    EXPECT_TRUE(store.has(storage::Namespace::Secrets, std::string{kSecretSlot1}));
    EXPECT_TRUE(store.has(storage::Namespace::System, std::string{kConfigGeneration}));
    EXPECT_TRUE(store.has(storage::Namespace::System, std::string{kSecretGeneration}));

    store.fail_write(storage::Namespace::Secrets, std::string{kSecretSlot0});
    store.fail_write(storage::Namespace::Secrets, std::string{kSecretSlot1});
    (void)run_transaction(store,
                          provisioning_flow::compose(a_submission(), config::load(store).value, 2));

    EXPECT_TRUE(store.has(storage::Namespace::Config, std::string{kConfigSlot1}));
    EXPECT_TRUE(store.has(storage::Namespace::Secrets, std::string{kSecretSlot1}));
    EXPECT_EQ(config::load(store).value.wifi_profile_id, 1U);
    EXPECT_EQ(secrets::load(store).value.profile_id, 1U);
}

// --- The two-stage flow (owner ruling 1, 2026-08-16) -----------------------
//
// The browser flow is staged; the persistence transaction is not. These pin the half that
// matters for safety: that stage 1 writes nothing, and that a power loss between the stages
// is indistinguishable from never having started.

TEST(TwoStageFlowTests, TheWifiStageIsTheDefaultStage) {
    const provisioning_flow::Submission submission;
    EXPECT_EQ(submission.stage, provisioning_flow::Stage::Wifi);
}

// A validated Wi-Fi stage persists nothing at all — the store is untouched.
TEST(TwoStageFlowTests, AValidatedWifiStagePersistsNothing) {
    test::FakeKeyValue store;

    WifiValidation validation;
    validation.begin(0, provisioning_flow::Candidate{kCandidate});
    ASSERT_EQ(validation.observe(kCandidate, Outcome::Leased, 100), Verdict::Pending);
    ASSERT_EQ(validation.tick(2100), Verdict::Validated);

    // Stage 1 is complete and commit() has deliberately not been called.
    EXPECT_FALSE(store.has(storage::Namespace::Config, std::string{kConfigSlot0}));
    EXPECT_FALSE(store.has(storage::Namespace::Config, std::string{kConfigSlot1}));
    EXPECT_FALSE(store.has(storage::Namespace::Secrets, std::string{kSecretSlot0}));
    EXPECT_FALSE(store.has(storage::Namespace::Secrets, std::string{kSecretSlot1}));
    EXPECT_EQ(store.erase_key_calls(), 0U);
}

// ⬅️ POWER LOSS BETWEEN THE STAGES. The user proved their Wi-Fi and then the device lost
// power while they were typing the printer address. Nothing was written, so the device comes
// back wanting setup — identical to row 3, and the reason option B was chosen over a
// two-stage transaction.
TEST(TwoStageFlowTests, PowerLossBetweenTheStagesBootsBackIntoSetupRequired) {
    test::FakeKeyValue store;
    EXPECT_EQ(boot_decision(store), ProvisioningDecision::StartSetup);
}

// Only the printer stage runs the transaction, and it produces exactly the same records the
// single-stage flow did — config first, credential last, printer pending.
TEST(TwoStageFlowTests, ThePrinterStageRunsTheStagedTransaction) {
    // ⬅️ AND IT ACTIVATES A VALIDATED RECORD, WHICH IS THE MILESTONE 4 CHANGE. compose() still writes
    // printer_validated = false — it knows nothing about a probe — and app_core sets it true before
    // staging, because by the time anything can OBSERVE the record it will have been activated, and
    // only the success path activates.
    test::FakeKeyValue store;

    auto submission = a_submission();
    submission.stage = provisioning_flow::Stage::Printer;

    provisioning_flow::Records records =
        provisioning_flow::compose(submission, config::defaults(), 1);
    EXPECT_FALSE(records.configuration.printer_validated);
    records.configuration.printer_validated = true;

    const Transaction transaction = run_transaction(store, records);

    EXPECT_TRUE(transaction.staged.staged);
    EXPECT_TRUE(transaction.staged.secret_attempted);
    EXPECT_TRUE(transaction.activated.activated);

    const auto observation = compose_configuration(config::load(store), secrets::load(store));
    EXPECT_TRUE(observation.has_printer_configuration);
    EXPECT_FALSE(observation.printer_configuration_pending);
    EXPECT_EQ(boot_decision(store), ProvisioningDecision::NotRequired);
}

// The credential the printer stage pairs with is the one stage 1 validated, carried in RAM.
// It is never re-submitted, so a printer-stage submission carrying no credential must still
// produce a complete pair once merged.
TEST(TwoStageFlowTests, ThePrinterStageCarriesNoCredentialOfItsOwn) {
    provisioning_flow::Submission printer_stage;
    printer_stage.stage = provisioning_flow::Stage::Printer;
    (void)printer_stage.printer_host.assign("192.168.1.50");
    (void)printer_stage.printer_api_key.assign("Zx9QsT4bC1");
    printer_stage.printer_port = 80;

    EXPECT_TRUE(printer_stage.wifi_ssid.empty());
    EXPECT_TRUE(printer_stage.wifi_password.empty());

    // Merged onto the retained stage-1 credential, exactly as app_core does.
    provisioning_flow::Submission merged = a_submission();
    (void)merged.printer_host.assign(printer_stage.printer_host.view());
    (void)merged.printer_api_key.assign(printer_stage.printer_api_key.view());
    merged.printer_port = printer_stage.printer_port;

    const auto records = provisioning_flow::compose(merged, config::defaults(), 1);
    EXPECT_EQ(records.secret.wifi_ssid.view(), "Home Network");
    EXPECT_EQ(records.configuration.printer_host.view(), "192.168.1.50");
    EXPECT_FALSE(records.configuration.printer_validated);
}

} // namespace
// --- The terminal-outcome invariant ----------------------------------------
//
// OWNER REQUIREMENT, 2026-08-18: every accepted Wi-Fi validation transaction must eventually
// produce exactly ONE terminal application outcome within its bound, and the browser must be
// able to observe it. The hardware failure that prompted this was a device that crashed
// mid-attempt: no outcome was ever produced, and the page waited for ever. These tests pin the
// firmware half — that the machine cannot fail to terminate — over every input shape.

TEST(TerminalOutcomeTests, EveryEvidenceSequenceTerminatesWithinTheBound) {
    // Exhaustive over every outcome the station can report, held for the whole window, and over
    // both identities — the candidate's own and a superseded one. Whatever the station does, the
    // machine must reach Validated or Failed by the timeout and never remain Pending.
    const Outcome outcomes[] = {Outcome::Leased, Outcome::LeaseInterrupted, Outcome::Unreachable,
                                Outcome::DriverStalled, Outcome::EvidenceLost};

    const Tag identities[] = {kCandidate, kSuperseded, prd::network::station::kNoTag};

    const Policy policy;

    for (const Outcome outcome : outcomes) {
        for (const Tag tag : identities) {
            WifiValidation validation{policy};
            validation.begin(0, provisioning_flow::Candidate{kCandidate});

            Verdict verdict = Verdict::Pending;
            for (std::uint32_t now = 0; now <= policy.timeout_ms && verdict == Verdict::Pending;
                 now += 100) {
                verdict = validation.observe(tag, outcome, now);
            }

            EXPECT_NE(verdict, Verdict::Pending)
                << "a station repeating one answer must still reach a terminal outcome";
            EXPECT_NE(verdict, Verdict::Idle);
        }
    }
}

TEST(TerminalOutcomeTests, ASilentStationStillFailsAtTheBoundAndNotAfterIt) {
    // The worst case: the station says nothing at all. This is the shape a crashed or wedged
    // driver produces, and it must not hold the transaction open.
    const Policy policy;
    WifiValidation validation{policy};
    validation.begin(0, provisioning_flow::Candidate{kCandidate});

    EXPECT_EQ(validation.tick(policy.timeout_ms - 1), Verdict::Pending);
    EXPECT_EQ(validation.tick(policy.timeout_ms), Verdict::Failed);
    EXPECT_EQ(validation.failure(), Failure::WifiTimedOut);
}

TEST(TerminalOutcomeTests, ExactlyOneTerminalOutcomeIsProduced) {
    // Once terminal, further observation must not produce a second outcome — the browser would
    // otherwise be told two different things about one transaction.
    const Policy policy;
    WifiValidation validation{policy};
    validation.begin(0, provisioning_flow::Candidate{kCandidate});

    EXPECT_EQ(validation.observe(kCandidate, Outcome::Unreachable, 10), Verdict::Failed);

    const Verdict settled = validation.verdict();
    for (std::uint32_t now = 20; now < policy.timeout_ms * 2; now += 500) {
        EXPECT_EQ(validation.observe(kCandidate, Outcome::Leased, now), settled)
            << "a settled transaction must not change its answer";
    }

    EXPECT_EQ(validation.verdict(), Verdict::Failed);
}

TEST(TerminalOutcomeTests, EveryTerminalVerdictMapsToABrowserObservablePhase) {
    // The browser observes the outcome through the provisioning phase, so every terminal verdict
    // must map to a phase the page can act on. UIAD 001 §5.
    EXPECT_EQ(phase_for(Verdict::Validated), app_model::ProvisioningPhase::Inactive);
    EXPECT_EQ(phase_for(Verdict::Failed), app_model::ProvisioningPhase::SetupFailed);

    // And the non-terminal ones are distinct from both, so "still working" is never mistaken for
    // an outcome.
    EXPECT_EQ(phase_for(Verdict::Pending), app_model::ProvisioningPhase::ValidatingConfiguration);
    EXPECT_EQ(phase_for(Verdict::Idle), app_model::ProvisioningPhase::SetupActive);
}

TEST(TerminalOutcomeTests, EveryFailureCarriesANonSecretReasonTheBrowserCanShow) {
    // A terminal outcome the page cannot explain is how "That did not work" became the only
    // message a user ever saw. Each failure names something actionable and echoes nothing.
    const Failure failures[] = {
        Failure::WifiUnreachable,        Failure::WifiTimedOut,        Failure::StorageRejected,
        Failure::StorageCandidateFailed, Failure::StorageCommitFailed,
    };

    for (const auto failure : failures) {
        const std::string label{provisioning_flow::log_label(failure)};
        EXPECT_FALSE(label.empty());
        EXPECT_NE(label, std::string{provisioning_flow::log_label(Failure::None)});
    }
}

// --- The 2026-08-18 failure->corrected retry defect -------------------------
//
// CAPTURED ON THE BOARD, NOT SUPPOSED. Attempt 1 (wrong password) ran five real attempts over
// 28.7 s — reasons 15, 205, 15, 205, 15 — and correctly ended Unavailable/WifiUnreachable at
// 79719 ms. The corrected submission was then accepted at 102721 ms and FAILED AT 102728 ms,
// six milliseconds later, before the station had been asked to connect — and the station then
// associated at 103220 and leased an address at 104262. The Wi-Fi worked; the verdict was
// already decided.
//
// ⛔ WHAT CHANGED AT CHECKPOINT 3c, AND WHY THESE TESTS LOOK DIFFERENT. The cause was that the
// station's phase reached this module through the application STORE, one or more ticks late, so the
// first report after a restart described the PREVIOUS transaction. The repair at the time was a
// guard that disregarded reports equal to the phase the attempt began with — which works, but only
// by inspecting content, and content cannot distinguish "the old attempt's answer" from "the new
// attempt genuinely reached the same state". The station now returns its answers directly to the
// application task and charges each one to the request identity that provoked it, so the lag is
// gone AND the ambiguity with it. These tests drive the same recorded sequence against identities.

TEST(RetryAfterFailureTests, TheWholeSequenceSucceedsWrongThenCorrected) {
    // wrong credentials -> terminal failure -> replaced credentials -> fresh attempt -> success,
    // at the timings the board recorded.
    const Policy policy;
    WifiValidation validation{policy};

    constexpr Tag kWrong = 1;
    constexpr Tag kCorrected = 2;

    // --- attempt 1, wrong password -----------------------------------------
    std::uint32_t now = 1000;
    validation.begin(now, provisioning_flow::Candidate{kWrong});

    Verdict verdict = Verdict::Pending;
    for (int step = 0; step < 60 && verdict == Verdict::Pending; ++step) {
        now += 100;
        verdict = validation.tick(now);
    }
    EXPECT_EQ(verdict, Verdict::Pending) << "a station still trying is not yet a failure";

    // The station exhausts §16.2's rejected-credential budget and stops trying.
    now += 100;
    EXPECT_EQ(validation.observe(kWrong, Outcome::Unreachable, now), Verdict::Failed);
    EXPECT_EQ(validation.failure(), Failure::WifiUnreachable);

    // --- the user corrects the password and submits again -------------------
    now += 20000;
    validation.reset();
    validation.begin(now, provisioning_flow::Candidate{kCorrected});

    // ⛔ THE SIX MILLISECONDS THAT USED TO DECIDE IT. The previous attempt's terminal answer is
    // still in flight and arrives now. It names kWrong, so it decides nothing.
    now += 6;
    EXPECT_EQ(validation.observe(kWrong, Outcome::Unreachable, now), Verdict::Pending)
        << "the previous transaction's answer must not fail this one";

    // And the corrected attempt proceeds normally: association at 103220, lease at 104262.
    now += 500;
    EXPECT_EQ(validation.tick(now), Verdict::Pending);

    now += 1000;
    EXPECT_EQ(validation.observe(kCorrected, Outcome::Leased, now), Verdict::Pending)
        << "the dwell is running";

    now += policy.dwell_ms;
    EXPECT_EQ(validation.tick(now), Verdict::Validated);
    EXPECT_EQ(validation.failure(), Failure::None);
}

TEST(RetryAfterFailureTests, AStaleTerminalAnswerCannotFailTheNextTransaction) {
    // The minimal statement of the defect, at the exact timing the board produced: 6 ms.
    const Policy policy;
    WifiValidation validation{policy};

    validation.begin(102721, provisioning_flow::Candidate{2});

    EXPECT_EQ(validation.observe(1, Outcome::Unreachable, 102728), Verdict::Pending);
    EXPECT_EQ(validation.verdict(), Verdict::Pending);
    EXPECT_EQ(validation.failure(), Failure::None);
}

TEST(RetryAfterFailureTests, AGenuineTerminalAnswerInTheNewAttemptStillFails) {
    // ⛔ THE GUARD MUST NOT SWALLOW A REAL FAILURE, AND THIS IS WHERE AN IDENTITY BEATS A PHASE.
    // The content-based guard had to see the phase CHANGE before it would honour a repeat; a new
    // attempt that failed for the same reason as its predecessor, with no visible phase change in
    // between, had to wait out the whole timeout. An identity has no such blind spot: the very
    // first answer charged to this attempt is honoured.
    const Policy policy;
    WifiValidation validation{policy};

    validation.begin(0, provisioning_flow::Candidate{2});

    EXPECT_EQ(validation.observe(1, Outcome::Unreachable, 5), Verdict::Pending);
    EXPECT_EQ(validation.observe(2, Outcome::Unreachable, 6), Verdict::Failed);
    EXPECT_EQ(validation.failure(), Failure::WifiUnreachable);
}

TEST(RetryAfterFailureTests, AStaleLeaseCannotValidateTheNextTransactionEither) {
    // The mirror hazard, and the reason the rule is not special-cased to failures: an attempt must
    // not validate on a lease its predecessor obtained for a different network.
    const Policy policy;
    WifiValidation validation{policy};

    validation.begin(0, provisioning_flow::Candidate{2});

    for (std::uint32_t now = 100; now <= policy.dwell_ms * 2; now += 100) {
        EXPECT_EQ(validation.observe(1, Outcome::Leased, now), Verdict::Pending)
            << "a dwell may not run on a lease this transaction did not cause";
    }
}

TEST(RetryAfterFailureTests, AStationThatNeverReportsStillFailsAtTheBound) {
    // The rule may not become a way to hang. If nothing charged to this attempt is ever reported,
    // the overall timeout is still the backstop.
    const Policy policy;
    WifiValidation validation{policy};

    validation.begin(0, provisioning_flow::Candidate{2});

    EXPECT_EQ(validation.observe(1, Outcome::Unreachable, policy.timeout_ms - 1), Verdict::Pending);
    EXPECT_EQ(validation.observe(1, Outcome::Unreachable, policy.timeout_ms), Verdict::Failed);
    EXPECT_EQ(validation.failure(), Failure::WifiTimedOut);
}

TEST(RetryAfterFailureTests, EveryIdentityIsSafeToBeginFrom) {
    // Exhaustive: whatever identity the previous transaction used, and whatever it left in flight,
    // beginning a new one must not decide it immediately.
    const Tag identities[] = {prd::network::station::kNoTag, 1, 2, 7, 0xFFFFFFFFU};

    for (const Tag previous : identities) {
        for (const Tag fresh : identities) {
            WifiValidation validation;
            validation.begin(0, provisioning_flow::Candidate{fresh});

            const Verdict verdict = validation.observe(previous, Outcome::Unreachable, 1);

            // The one case that MUST settle is the one where the answer genuinely belongs to this
            // attempt — and kNoTag never does, because it is not a live identity.
            const bool own = previous == fresh && fresh != prd::network::station::kNoTag;

            EXPECT_EQ(verdict, own ? Verdict::Failed : Verdict::Pending)
                << "previous=" << previous << " fresh=" << fresh;
        }
    }
}

} // namespace prd::app_core
