// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include <cstdint>

#include <gtest/gtest.h>

#include "reentry.hpp"

// The privileged setup session's bounded idle lifetime — Milestone 3.10, owner rulings 1 and C.
//
// ⬅️ THE TEST THIS FILE EXISTS FOR IS THE POLLING ONE. The served page fetches /api/status once a
// second for as long as a browser is open, and the first version of this design counted a
// successful poll as activity — which would have let a tab forgotten on a desk hold the privileged
// access point open indefinitely, defeating the bound rather than implementing it. There is no way
// to observe that on hardware short of leaving a browser open for hours; here it is a loop.

namespace prd::app_core::reentry {
namespace {

constexpr std::uint32_t kMinute = 60U * 1000U;

} // namespace

TEST(ReentryTests, NoSessionNeverExpires) {
    const PrivilegedSession session;

    EXPECT_FALSE(session.active());
    EXPECT_FALSE(session.expired(0));
    EXPECT_FALSE(session.expired(kIdleLifetimeMs * 100));
}

TEST(ReentryTests, ASessionExpiresAfterItsIdleLifetime) {
    PrivilegedSession session;
    session.begin(0);

    EXPECT_TRUE(session.active());
    EXPECT_FALSE(session.expired(0));
    EXPECT_FALSE(session.expired(kIdleLifetimeMs - 1));
    EXPECT_TRUE(session.expired(kIdleLifetimeMs));
}

// ⛔ THE ONE THAT MATTERS. An abandoned browser polling for an hour touches nothing here, because
// read-only status polling never reaches note_activity() at all — and the session must expire on
// schedule regardless of how long that goes on.
TEST(ReentryTests, AnAbandonedBrowserCannotHoldTheSessionOpen) {
    PrivilegedSession session;
    session.begin(0);

    // Nothing is called for an hour: this is what a page polling once a second amounts to from
    // this object's point of view, and it is the whole point.
    EXPECT_TRUE(session.expired(60U * kMinute));

    // And the expiry is not a one-shot that a later read clears.
    EXPECT_TRUE(session.expired(120U * kMinute));
}

TEST(ReentryTests, AnAcceptedActionRefreshesTheDeadline) {
    PrivilegedSession session;
    session.begin(0);

    // Nine minutes in, the user submits something the device accepts.
    session.note_activity(9U * kMinute);

    // The original deadline passes and the session survives it.
    EXPECT_FALSE(session.expired(kIdleLifetimeMs));

    // A full lifetime after the ACTION, not after the start.
    EXPECT_FALSE(session.expired(9U * kMinute + kIdleLifetimeMs - 1));
    EXPECT_TRUE(session.expired(9U * kMinute + kIdleLifetimeMs));
}

TEST(ReentryTests, ActivityOnAnEndedSessionDoesNothing) {
    PrivilegedSession session;
    session.begin(0);
    session.end();

    session.note_activity(kMinute);

    EXPECT_FALSE(session.active());
    EXPECT_FALSE(session.expired(100U * kMinute));
}

// --- A running transaction cannot expire ------------------------------------

TEST(ReentryTests, ARunningTransactionSuspendsExpiry) {
    PrivilegedSession session;
    session.begin(0);

    session.set_transaction_running(true, kMinute);

    EXPECT_TRUE(session.transaction_running());

    // Well past the deadline, and the access point must not be taken away from a browser
    // mid-validation.
    EXPECT_FALSE(session.expired(kIdleLifetimeMs));
    EXPECT_FALSE(session.expired(60U * kMinute));
}

TEST(ReentryTests, AccountingResumesWhenTheTransactionSettles) {
    PrivilegedSession session;
    session.begin(0);
    session.set_transaction_running(true, kMinute);

    // It settles at t=30min, long after the original deadline.
    session.set_transaction_running(false, 30U * kMinute);

    EXPECT_FALSE(session.transaction_running());

    // Owner ruling 1: "once that transaction settles, idle accounting resumes" — from the
    // settlement, so the user who just watched it settle gets the whole window.
    EXPECT_FALSE(session.expired(30U * kMinute + kIdleLifetimeMs - 1));
    EXPECT_TRUE(session.expired(30U * kMinute + kIdleLifetimeMs));
}

TEST(ReentryTests, SettlingWithoutHavingRunChangesNothing) {
    // A redundant "not running" must not be usable as a free refresh: only the true→false
    // transition is a settlement.
    PrivilegedSession session;
    session.begin(0);

    session.set_transaction_running(false, 9U * kMinute);

    EXPECT_TRUE(session.expired(kIdleLifetimeMs));
}

TEST(ReentryTests, ANewSessionStartsWithNoTransactionAndAFreshClock) {
    PrivilegedSession session;
    session.begin(0);
    session.set_transaction_running(true, kMinute);

    // A fresh deliberate re-entry.
    session.begin(50U * kMinute);

    EXPECT_TRUE(session.active());
    EXPECT_FALSE(session.transaction_running());
    EXPECT_FALSE(session.expired(50U * kMinute));
    EXPECT_TRUE(session.expired(50U * kMinute + kIdleLifetimeMs));
}

TEST(ReentryTests, TheMillisecondCounterMayWrap) {
    // The counter wraps after 49 days. A session that appeared to have been idle for a month at
    // that moment would be stopped mid-use.
    PrivilegedSession session;

    const std::uint32_t nearly_wrapped = 0xFFFFFFFFU - (kMinute / 2);
    session.begin(nearly_wrapped);

    // One minute later, having wrapped through zero.
    const std::uint32_t after = nearly_wrapped + kMinute;

    EXPECT_LT(after, nearly_wrapped) << "the test's own arithmetic must have wrapped";
    EXPECT_EQ(session.idle_for(after), kMinute);
    EXPECT_FALSE(session.expired(after));
    EXPECT_TRUE(session.expired(nearly_wrapped + kIdleLifetimeMs));
}

TEST(ReentryTests, EndingTheSessionStopsEverything) {
    PrivilegedSession session;
    session.begin(0);
    session.set_transaction_running(true, kMinute);

    session.end();

    EXPECT_FALSE(session.active());
    EXPECT_FALSE(session.transaction_running());
    EXPECT_FALSE(session.expired(100U * kMinute));
}

TEST(ReentryTests, TheIdleLifetimeIsTheReportedWorkingBaseline) {
    // Ten minutes, reported to the owner as a working baseline rather than product policy. The
    // assertion exists so a change to the value is a deliberate edit to a test that names it.
    EXPECT_EQ(kIdleLifetimeMs, 10U * 60U * 1000U);
}

// --- Milestone 3.11 T4: one time observation per tick ------------------------

TEST(ReentryTests, ASessionBegunAtTheTicksOwnTimeCannotExpireInThatTick) {
    // ⛔ THE T4 REGRESSION. local_interface_tick() raises the access point and then, a few lines
    // later in the SAME tick, asks whether the session it just began has expired. With one shared
    // observation that question has exactly one right answer, and this is it.
    PrivilegedSession session;

    // A realistic uptime rather than zero: the board was 4,397,973 ms up when this defect fired,
    // and a value near zero would hide an underflow behind small numbers.
    const std::uint32_t tick = 4'397'973U;

    session.begin(tick);

    EXPECT_EQ(session.idle_for(tick), 0U);
    EXPECT_FALSE(session.expired(tick)) << "a session cannot be idle at the instant it began";
}

TEST(ReentryTests, ATimestampPrecedingTheSessionIsNotAnElapsedInterval) {
    // The defensive half, and the exact arithmetic observed on hardware: the tick read the clock,
    // start_setup_ap() read it ~120 ms later and recorded THAT as the activity moment, and the
    // tick's older value was then subtracted from the newer one.
    //
    // Unsigned, that is 4,294,967,176 ms — reported as "idle for 4294967s", which is UINT32_MAX
    // divided by a thousand. The access point was torn down 8 ms after it came up.
    PrivilegedSession session;

    const std::uint32_t later = 4'398'093U;
    const std::uint32_t earlier = later - 120U;

    session.begin(later);

    EXPECT_EQ(session.idle_for(earlier), 0U)
        << "a backwards reading is no elapsed time, not almost the whole 32-bit range";
    EXPECT_FALSE(session.expired(earlier));

    // What the unguarded subtraction produced, so the number in the hardware log is explained by
    // this test rather than by a comment somewhere else.
    EXPECT_EQ(static_cast<std::uint32_t>(earlier - later), 4'294'967'176U);
}

TEST(ReentryTests, TheGuardDoesNotSwallowARealIdleWindow) {
    // The guard must not have bought correctness for the backwards case by making expiry
    // unreachable. Forward intervals of every relevant size still behave.
    PrivilegedSession session;

    const std::uint32_t tick = 4'397'973U;
    session.begin(tick);

    EXPECT_FALSE(session.expired(tick + 1U));
    EXPECT_FALSE(session.expired(tick + kIdleLifetimeMs - 1U));
    EXPECT_TRUE(session.expired(tick + kIdleLifetimeMs));
    EXPECT_EQ(session.idle_for(tick + kIdleLifetimeMs), kIdleLifetimeMs);
}

// --- Milestone 3.11 T4 finding B: the phase an expiry leaves behind ----------

// --- The printer-stage boundary (checkpoint 3e, PM ruling 2) -------------------
//
// ⛔ WHAT RULING 2 SETTLED AND WHAT IT DID NOT CHANGE. Reaching the printer step grants NO automatic
// idle window of its own: only an allowed and accepted submission refreshes activity, exactly as
// ADR-0008 decision 5 already said. What it added is the requirement that an accepted printer
// submission become transaction-running before the same iteration can expire it — until checkpoint
// 3e only the WI-FI stage ever set that flag, so a printer commit ran its staging and its
// twenty-second probe with the idle clock still running against it. It was saved from firing only by
// note_activity() having just reset the clock, which is a coincidence of call order rather than the
// rule the ADR states.

TEST(ReentryTests, AnAcceptedSubmissionAtTheEXPIRYINSTANTCannotBeExpiredByThatIteration) {
    // ⛔ THE EXACT BOUNDARY. The submission arrives on the tick where the window would otherwise
    // close, and app_core's order is note_activity() → the transaction begins → local_interface_tick()
    // asks whether the session expired, all against ONE clock observation. Both halves are asserted:
    // the refresh alone, and the refresh plus the suspension.
    for (const bool becomes_transaction : {false, true}) {
        PrivilegedSession session;
        session.begin(0);

        ASSERT_TRUE(session.expired(kIdleLifetimeMs)) << "the window really was about to close";

        session.note_activity(kIdleLifetimeMs);

        if (becomes_transaction) {
            session.set_transaction_running(true, kIdleLifetimeMs);
        }

        EXPECT_FALSE(session.expired(kIdleLifetimeMs))
            << "an accepted submission is activity, whichever order the flags are set in";
        EXPECT_EQ(session.transaction_running(), becomes_transaction);
    }
}

TEST(ReentryTests, APrinterCommitCannotBeExpiredWhileItRuns) {
    // The whole point of the one assignment ruling 2 asks for. A printer submission stages two
    // records and then waits on a printer that may be slow or silent; the access point must not
    // vanish underneath the browser while that happens, on exactly the rule that already protects a
    // Wi-Fi validation.
    PrivilegedSession session;
    session.begin(0);

    // The Wi-Fi stage: submitted, validated, settled.
    session.note_activity(kMinute);
    session.set_transaction_running(true, kMinute);
    session.set_transaction_running(false, kMinute + 5000);

    // The user fills in the printer step and submits it well inside the window.
    session.note_activity(kMinute + 120000);
    session.set_transaction_running(true, kMinute + 120000);

    // A printer that never answers. The validation budget is twenty seconds, but the assertion is
    // stronger than that: no amount of elapsed time may expire a running commit.
    for (std::uint32_t elapsed = 0; elapsed <= kIdleLifetimeMs * 3; elapsed += 5000) {
        ASSERT_FALSE(session.expired(kMinute + 120000 + elapsed))
            << "expiry must stay suspended for the whole commit, elapsed=" << elapsed;
    }

    // And when it settles, the ordinary window starts from THAT moment — the person who has just
    // watched a commit fail is mid-task.
    const std::uint32_t settled = kMinute + 120000 + 20000;
    session.set_transaction_running(false, settled);

    EXPECT_FALSE(session.expired(settled + kIdleLifetimeMs - 1));
    EXPECT_TRUE(session.expired(settled + kIdleLifetimeMs));
}

TEST(ReentryTests, ReachingThePrinterStepGrantsNoWindowOfItsOwn) {
    // ⛔ ADR-0008 DECISION 5 IS UNCHANGED, AND THIS IS THE ASSERTION THAT SAYS SO. A user who
    // validates Wi-Fi and then walks away is expired by the ordinary rule, measured from the moment
    // that validation settled — not given a second lifetime for having advanced a stage. Getting
    // this wrong in the other direction is what "without a second timer" forbids.
    PrivilegedSession session;
    session.begin(0);

    session.note_activity(1000);
    session.set_transaction_running(true, 1000);

    // Stage 1 succeeds after three seconds. complete_wifi_stage() clears the transaction flag.
    const std::uint32_t validated = 4000;
    session.set_transaction_running(false, validated);

    // The browser now polls once a second, for ever, on the printer step. Polling is not activity.
    for (std::uint32_t now = validated; now < validated + kIdleLifetimeMs; now += 1000) {
        ASSERT_FALSE(session.expired(now)) << "still inside the one window, now=" << now;
    }

    EXPECT_TRUE(session.expired(validated + kIdleLifetimeMs))
        << "the window closes exactly one lifetime after the stage settled";

    // NEGATIVE CONTROL: had the printer stage been granted its own window, this would still be open
    // a lifetime later. Asserted so a future "be generous on the printer step" change fails here.
    EXPECT_TRUE(session.expired(validated + (kIdleLifetimeMs * 2)));
}

TEST(ReentryTests, AnIdleUserBetweenStagesExpiresAndTheDeviceIsLeftToRestoreOrWithdraw) {
    // The state ruling 2 describes in full: Step 1 validated, nothing persisted, the printer step
    // never submitted. This unit owns only the decision that the session is over — what happens to
    // the RADIO afterwards is wifi_restore's, driven from the same branch. The two facts asserted
    // here are the ones app_core's expiry branch reads: that it expired, and what the panel is told.
    PrivilegedSession session;
    session.begin(0);
    session.note_activity(1000);
    session.set_transaction_running(true, 1000);
    session.set_transaction_running(false, 4000);

    ASSERT_TRUE(session.expired(4000 + kIdleLifetimeMs));

    // Nothing was persisted, so this device is still un-provisioned and goes back to asking.
    const AfterExpiry unprovisioned = after_expiry(false, ProvisioningDecision::StartSetup);
    EXPECT_EQ(unprovisioned.phase, app_model::ProvisioningPhase::SetupRequired);
    EXPECT_FALSE(unprovisioned.return_to_recovery);

    // A device that was already configured before the abandoned re-entry has nothing left to say —
    // and it is exactly the device with a previous pair for the radio to be restored to.
    const AfterExpiry provisioned = after_expiry(true, ProvisioningDecision::NotRequired);
    EXPECT_EQ(provisioned.phase, app_model::ProvisioningPhase::Inactive);
    EXPECT_FALSE(provisioned.return_to_recovery);
}

TEST(ReentryTests, AProvisionedExpiryLeavesNothingOnThePanel) {
    // The device is configured and the setup surface is gone, so there is nothing to say. Inactive
    // is the only phase that takes the setup composition off the panel rather than drawing it with
    // its rows missing — which is what "Join this network" over an empty screen was.
    const AfterExpiry next = after_expiry(true, ProvisioningDecision::NotRequired);

    EXPECT_EQ(next.phase, app_model::ProvisioningPhase::Inactive);
    EXPECT_FALSE(next.return_to_recovery);
}

TEST(ReentryTests, AnUnprovisionedExpiryGoesBackToAskingToBeSetUp) {
    // Still unprovisioned: it keeps a screen, because it genuinely does need setting up, but the
    // screen must be the one that ASKS rather than the one naming a network to join.
    const AfterExpiry next = after_expiry(false, ProvisioningDecision::StartSetup);

    EXPECT_EQ(next.phase, app_model::ProvisioningPhase::SetupRequired);
    EXPECT_FALSE(next.return_to_recovery);
}

TEST(ReentryTests, NoExpiryPathCanLeaveTheJoinThisNetworkPhase) {
    // ⛔ THE T4 REGRESSION, STATED AS THE PROPERTY RATHER THAN AS A LIST OF VALUES. Whatever any
    // branch returns, none may be SetupActive: that phase means "an access point is up and here is
    // how to join it", and by the time this runs the access point has stopped.
    for (const bool provisioned : {false, true}) {
        for (const ProvisioningDecision decision :
             {ProvisioningDecision::NotRequired, ProvisioningDecision::StartSetup,
              ProvisioningDecision::DeferredToRecovery}) {
            const AfterExpiry next = after_expiry(provisioned, decision);

            EXPECT_NE(next.phase, app_model::ProvisioningPhase::SetupActive);
            EXPECT_NE(next.phase, app_model::ProvisioningPhase::ValidatingConfiguration);
            EXPECT_NE(next.phase, app_model::ProvisioningPhase::SetupFailed);
        }
    }
}

// --- Milestone 3.11 T18: a session opened out of recovery goes back to recovery ---------

TEST(ReentryTests, ARecoveryOriginatedExpiryReturnsToRecovery) {
    // Owner ruling 2026-08-21. The long press left recovery, the session ended without replacing
    // the faulted record, and the fault is exactly as real as it was before the press. Recovery is
    // therefore the truthful resting state — SetupRequired would drop the reason the user is there.
    const AfterExpiry next = after_expiry(false, ProvisioningDecision::DeferredToRecovery);

    EXPECT_TRUE(next.return_to_recovery);

    // Inactive, not SetupRequired: recovery has no session, and the scene draws §8's Status Message
    // in the space the session rows would occupy. Any session phase here would put the two back
    // together, which is the defect this rule exists to prevent.
    EXPECT_EQ(next.phase, app_model::ProvisioningPhase::Inactive);
}

TEST(ReentryTests, AFaultBesideAValidConfigurationStillReturnsToRecovery) {
    // ⛔ THE ORDERING THAT MATTERS. decide_provisioning() weighs BOTH records, so a faulted secret
    // beside a valid configuration record reaches DeferredToRecovery. Testing the configuration
    // first would send exactly that device to Inactive and leave it presenting as configured while
    // it cannot connect at all.
    const AfterExpiry next = after_expiry(true, ProvisioningDecision::DeferredToRecovery);

    EXPECT_TRUE(next.return_to_recovery);
    EXPECT_EQ(next.phase, app_model::ProvisioningPhase::Inactive);
}

TEST(ReentryTests, OnlyTheRecoveryDecisionEverReturnsToRecovery) {
    // The negative half, and the reason it is a property rather than two cases: an ordinary
    // first-boot session and a re-entry on a healthy device must both keep the behaviour they had
    // before this rule existed. Neither may acquire recovery it never had.
    for (const bool provisioned : {false, true}) {
        EXPECT_FALSE(
            after_expiry(provisioned, ProvisioningDecision::NotRequired).return_to_recovery);
        EXPECT_FALSE(
            after_expiry(provisioned, ProvisioningDecision::StartSetup).return_to_recovery);
    }
}

TEST(ReentryTests, ASuccessfulSetupCannotFallBackIntoRecovery) {
    // Requirement 3, at the level this unit owns it. A commit that replaces the faulted record is
    // followed by a re-observation, and app_core mirrors that load's decision — so by the time any
    // later expiry runs, the decision reaching here is NotRequired and recovery is unreachable.
    // What this pins is that no combination of "configured" answers ever asks for recovery.
    EXPECT_FALSE(after_expiry(true, ProvisioningDecision::NotRequired).return_to_recovery);
    EXPECT_EQ(after_expiry(true, ProvisioningDecision::NotRequired).phase,
              app_model::ProvisioningPhase::Inactive);
}

} // namespace prd::app_core::reentry
