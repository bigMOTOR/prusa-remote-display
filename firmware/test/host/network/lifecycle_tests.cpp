// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include "lifecycle.hpp"

#include "backoff.hpp"

#include <string>

#include <gtest/gtest.h>

#include "app_model/device_data.hpp"

// The station phase machine.
//
// TWO RULES CARRY THIS FILE, and both come from accepted sources rather than preference.
// §16.2: transient Wi-Fi loss is retried automatically, invalid configuration is not
// retried at high frequency — so a wrong password must stop the station, and a router
// reboot must not. And UI Behaviour Decision 001 §4: "Routine polling, retries and small
// telemetry changes shall not become visible" — so a retry that changes no phase must
// publish nothing.

namespace {

using prd::app_model::NetworkPhase;
using prd::network::DisconnectCause;
using prd::network::Lifecycle;
using prd::network::NextAction;
using prd::network::Trigger;

// Drives a station from cold to Connected, which is the precondition for most of the
// interesting cases below.
void bring_up(Lifecycle& lifecycle) {
    (void)lifecycle.on(Trigger::Started);
    (void)lifecycle.on(Trigger::Associating);
    (void)lifecycle.on(Trigger::Associated);
    (void)lifecycle.on(Trigger::GotIp);
}

// --- The un-provisioned device -------------------------------------------------

TEST(NetworkLifecycleTests, WithoutCredentialsTheStationIsDisabledAndAttemptsNothing) {
    Lifecycle lifecycle;

    const auto step = lifecycle.on(Trigger::CredentialsAbsent);

    EXPECT_EQ(step.phase, NetworkPhase::Disabled);
    EXPECT_EQ(step.action, NextAction::None);
}

// --- Association is not connection ---------------------------------------------

TEST(NetworkLifecycleTests, StartingAttemptsImmediately) {
    Lifecycle lifecycle;

    const auto step = lifecycle.on(Trigger::Started);

    EXPECT_EQ(step.phase, NetworkPhase::Starting);
    EXPECT_EQ(step.action, NextAction::Attempt);
    EXPECT_TRUE(step.phase_changed);
}

TEST(NetworkLifecycleTests, AFirstAttemptIsConnectingNotReconnecting) {
    Lifecycle lifecycle;
    (void)lifecycle.on(Trigger::Started);

    EXPECT_EQ(lifecycle.on(Trigger::Associating).phase, NetworkPhase::Connecting);
}

// Owner ruling, 2026-08-15: Connected requires successful IP acquisition, not association
// alone. A station with a link and no lease cannot reach anything.
TEST(NetworkLifecycleTests, AssociationAloneIsNotConnected) {
    Lifecycle lifecycle;
    (void)lifecycle.on(Trigger::Started);
    (void)lifecycle.on(Trigger::Associating);

    const auto step = lifecycle.on(Trigger::Associated);

    EXPECT_EQ(step.phase, NetworkPhase::Connecting);
    EXPECT_FALSE(step.phase_changed);
    EXPECT_NE(step.phase, NetworkPhase::Connected);
}

TEST(NetworkLifecycleTests, AnAddressCompletesTheConnection) {
    Lifecycle lifecycle;
    (void)lifecycle.on(Trigger::Started);
    (void)lifecycle.on(Trigger::Associating);
    (void)lifecycle.on(Trigger::Associated);

    const auto step = lifecycle.on(Trigger::GotIp);

    EXPECT_EQ(step.phase, NetworkPhase::Connected);
    EXPECT_TRUE(step.phase_changed);
    EXPECT_EQ(step.action, NextAction::None);
    EXPECT_TRUE(lifecycle.has_connected());
}

// --- Transient loss: retried automatically (§16.2) ------------------------------

TEST(NetworkLifecycleTests, ALostConnectionIsReconnectingAndRetries) {
    Lifecycle lifecycle;
    bring_up(lifecycle);

    const auto step = lifecycle.on(Trigger::Disconnected, DisconnectCause::Transient);

    EXPECT_EQ(step.phase, NetworkPhase::Reconnecting);
    EXPECT_EQ(step.action, NextAction::WaitBackoff);
    EXPECT_EQ(step.retry_attempt, 1U);
}

TEST(NetworkLifecycleTests, ALostLeaseIsNotAnAssociationFailure) {
    // ⛔ THE CHECKPOINT 3c CORRECTION. IP_EVENT_STA_LOST_IP used to fall into the failure path:
    // it climbed §11.3's ladder for a DHCP problem and told the adapter to reconnect a station
    // that was still associated. §16.2 retries by ASSOCIATION and there is nothing to
    // re-associate — the link is up. The phase becomes attempting again, truthfully, and the
    // counters do not move, so one physical outage is counted once.
    Lifecycle lifecycle;
    bring_up(lifecycle);

    const std::uint32_t before = lifecycle.transient_failures();

    const auto step = lifecycle.on(Trigger::LeaseLost);

    EXPECT_EQ(step.phase, NetworkPhase::Reconnecting);
    EXPECT_EQ(step.action, NextAction::None) << "no retry is scheduled at an associated station";
    EXPECT_EQ(lifecycle.transient_failures(), before) << "the association ladder is untouched";

    // And the next REAL failure is still the ladder's first step, which is what "untouched" means.
    const auto failed = lifecycle.on(Trigger::Disconnected, DisconnectCause::Transient);
    EXPECT_EQ(failed.action, NextAction::WaitBackoff);
    EXPECT_EQ(failed.retry_attempt, 1U);
}

TEST(NetworkLifecycleTests, EachTransientFailureClimbsTheLadder) {
    Lifecycle lifecycle;
    bring_up(lifecycle);

    for (std::uint32_t expected = 1; expected <= 4; ++expected) {
        const auto step = lifecycle.on(Trigger::Disconnected, DisconnectCause::Transient);
        EXPECT_EQ(step.retry_attempt, expected);
        EXPECT_EQ(step.action, NextAction::WaitBackoff);
    }
}

// Once the ladder has saturated the device has been failing for the whole schedule, and
// "reconnecting" would stop being true. Retries continue — this changes what is reported,
// not what is done.
TEST(NetworkLifecycleTests, ASaturatedLadderReportsUnavailableAndKeepsRetrying) {
    Lifecycle lifecycle;
    bring_up(lifecycle);

    prd::network::Step step{};
    for (std::uint32_t attempt = 0; attempt < 5; ++attempt) {
        step = lifecycle.on(Trigger::Disconnected, DisconnectCause::Transient);
    }

    EXPECT_EQ(step.phase, NetworkPhase::Unavailable);
    EXPECT_EQ(step.action, NextAction::WaitBackoff) << "§16.2 keeps retrying transient loss";
}

TEST(NetworkLifecycleTests, ARecoveredConnectionResetsTheLadder) {
    Lifecycle lifecycle;
    bring_up(lifecycle);

    for (std::uint32_t attempt = 0; attempt < 6; ++attempt) {
        (void)lifecycle.on(Trigger::Disconnected, DisconnectCause::Transient);
    }
    ASSERT_GT(lifecycle.transient_failures(), 0U);

    (void)lifecycle.on(Trigger::Associating);
    (void)lifecycle.on(Trigger::GotIp);

    EXPECT_EQ(lifecycle.transient_failures(), 0U);
    EXPECT_EQ(lifecycle.on(Trigger::Disconnected, DisconnectCause::Transient).retry_attempt, 1U);
}

// A device that had the network and lost it is reconnecting, not connecting — §7.4 lists
// both phases and this is the difference between them.
TEST(NetworkLifecycleTests, AfterAConnectionEveryAttemptIsReconnecting) {
    Lifecycle lifecycle;
    bring_up(lifecycle);
    (void)lifecycle.on(Trigger::Disconnected, DisconnectCause::Transient);

    EXPECT_EQ(lifecycle.on(Trigger::Associating).phase, NetworkPhase::Reconnecting);
}

// --- Rejected credentials: NOT retried at high frequency (§16.2) -----------------

TEST(NetworkLifecycleTests, RejectedCredentialsGiveUpAfterTheBoundedCount) {
    Lifecycle lifecycle;
    (void)lifecycle.on(Trigger::Started);

    const Lifecycle::Policy policy;

    for (std::uint32_t attempt = 1; attempt < policy.rejected_attempt_limit; ++attempt) {
        const auto step = lifecycle.on(Trigger::Disconnected, DisconnectCause::CredentialsRejected);
        EXPECT_EQ(step.action, NextAction::WaitBackoff) << "attempt " << attempt;
    }

    const auto final_step =
        lifecycle.on(Trigger::Disconnected, DisconnectCause::CredentialsRejected);

    EXPECT_EQ(final_step.phase, NetworkPhase::Unavailable);
    EXPECT_EQ(final_step.action, NextAction::GiveUp)
        << "§16.2 forbids retrying invalid configuration at high frequency";
}

// The owner's constraint: provisioning and recovery must not be hidden behind a long retry
// sequence. Three attempts run 2 s + 4 s of backoff, so a wrong password reports Unavailable
// in roughly six seconds rather than climbing the whole ladder.
TEST(NetworkLifecycleTests, TheRejectedPathIsShortEnoughToStayResponsive) {
    const Lifecycle::Policy policy;

    EXPECT_LE(policy.rejected_attempt_limit, 3U);

    std::int64_t worst_case_ms = 0;
    for (std::uint32_t attempt = 1; attempt < policy.rejected_attempt_limit; ++attempt) {
        worst_case_ms += prd::network::delay_for(attempt, prd::network::Jitter{255}).count();
    }

    EXPECT_LE(worst_case_ms, 10000) << "a wrong password must not hide provisioning";
}

// A rejected credential must not be reported as a network that merely went away, and the
// reverse: the two causes drive different actions and must stay distinguishable.
TEST(NetworkLifecycleTests, TheTwoCausesAreNotConflated) {
    Lifecycle transient;
    (void)transient.on(Trigger::Started);

    Lifecycle rejected;
    (void)rejected.on(Trigger::Started);

    for (std::uint32_t attempt = 0; attempt < 3; ++attempt) {
        (void)transient.on(Trigger::Disconnected, DisconnectCause::Transient);
        (void)rejected.on(Trigger::Disconnected, DisconnectCause::CredentialsRejected);
    }

    EXPECT_EQ(transient.transient_failures(), 3U);
    EXPECT_EQ(transient.rejected_attempts(), 0U);
    EXPECT_EQ(rejected.rejected_attempts(), 3U);
    EXPECT_EQ(rejected.transient_failures(), 0U);
}

TEST(NetworkLifecycleTests, ASuccessfulConnectionClearsTheRejectedCount) {
    Lifecycle lifecycle;
    (void)lifecycle.on(Trigger::Started);
    (void)lifecycle.on(Trigger::Disconnected, DisconnectCause::CredentialsRejected);
    ASSERT_EQ(lifecycle.rejected_attempts(), 1U);

    bring_up(lifecycle);

    EXPECT_EQ(lifecycle.rejected_attempts(), 0U);
}

// --- UIBD 001 §4: a retry that changes nothing is invisible -----------------------

TEST(NetworkLifecycleTests, RepeatedFailuresInTheSamePhasePublishNothing) {
    Lifecycle lifecycle;
    bring_up(lifecycle);

    EXPECT_TRUE(lifecycle.on(Trigger::Disconnected, DisconnectCause::Transient).phase_changed)
        << "Connected -> Reconnecting is a real change";

    for (std::uint32_t attempt = 0; attempt < 3; ++attempt) {
        EXPECT_FALSE(lifecycle.on(Trigger::Disconnected, DisconnectCause::Transient).phase_changed)
            << "a retry inside Reconnecting must not wake the store";
    }
}

TEST(NetworkLifecycleTests, StoppingReportsDisabled) {
    Lifecycle lifecycle;
    bring_up(lifecycle);

    const auto step = lifecycle.on(Trigger::Stopped);

    EXPECT_EQ(step.phase, NetworkPhase::Disabled);
    EXPECT_TRUE(step.phase_changed);
    EXPECT_EQ(step.action, NextAction::None);
}

// The §13.5 error text moved to station::Fault at checkpoint 3c — see station_intent_tests.cpp.
// DisconnectCause answers "why did the LINK go down", which has no honest wording for a driver that
// never answered at all, and a stall must not borrow a disconnect's words.

// --- reset(): a new credential set is a new attempt ------------------------
//
// The hardware-acceptance failure this closes: a second Milestone 3.8 Wi-Fi submission was
// tried against a machine still in §16.2's terminal give-up, so the corrected password was
// never attempted at all.

TEST(LifecycleTests, ResetClearsTheTerminalGiveUpSoANewCredentialIsTried) {
    Lifecycle lifecycle;

    ASSERT_EQ(lifecycle.on(Trigger::Started).action, NextAction::Attempt);
    for (int attempt = 0; attempt < 3; ++attempt) {
        (void)lifecycle.on(Trigger::Disconnected, DisconnectCause::CredentialsRejected);
    }

    // Terminal: the station has stopped trying, which is correct for a rejected credential.
    ASSERT_EQ(lifecycle.phase(), NetworkPhase::Unavailable);
    ASSERT_EQ(lifecycle.on(Trigger::Disconnected, DisconnectCause::CredentialsRejected).action,
              NextAction::GiveUp);

    lifecycle.reset();

    // And now a fresh attempt is possible, because the configuration changed.
    EXPECT_EQ(lifecycle.phase(), NetworkPhase::Disabled);
    EXPECT_EQ(lifecycle.rejected_attempts(), 0U);
    EXPECT_EQ(lifecycle.transient_failures(), 0U);
    EXPECT_FALSE(lifecycle.has_connected());
    EXPECT_EQ(lifecycle.on(Trigger::Started).action, NextAction::Attempt);
}

// Wrong password A, then wrong password B: the second must genuinely be attempted rather
// than refused on the first credential's exhausted budget.
TEST(LifecycleTests, ResetGivesASecondWrongCredentialItsOwnFullBudget) {
    Lifecycle lifecycle;

    (void)lifecycle.on(Trigger::Started);
    for (int attempt = 0; attempt < 3; ++attempt) {
        (void)lifecycle.on(Trigger::Disconnected, DisconnectCause::CredentialsRejected);
    }

    lifecycle.reset();
    ASSERT_EQ(lifecycle.on(Trigger::Started).action, NextAction::Attempt);

    // The first rejection of the NEW credential must retry, not give up.
    const prd::network::Step step =
        lifecycle.on(Trigger::Disconnected, DisconnectCause::CredentialsRejected);
    EXPECT_EQ(step.action, NextAction::WaitBackoff);
    EXPECT_EQ(step.retry_attempt, 1U);
}

// No backoff or reconnect history carries over: a reset machine reports Connecting rather
// than Reconnecting, because it has never reached the network it is about to try.
TEST(LifecycleTests, ResetDropsReconnectHistoryAndBackoffState) {
    Lifecycle lifecycle;

    (void)lifecycle.on(Trigger::Started);
    (void)lifecycle.on(Trigger::Associated);
    ASSERT_EQ(lifecycle.on(Trigger::GotIp).phase, NetworkPhase::Connected);
    (void)lifecycle.on(Trigger::Disconnected, DisconnectCause::Transient);
    ASSERT_TRUE(lifecycle.has_connected());

    lifecycle.reset();

    (void)lifecycle.on(Trigger::Started);
    EXPECT_EQ(lifecycle.on(Trigger::Associating).phase, NetworkPhase::Connecting);
}

// --- The first-attempt defect of 2026-08-17 ---------------------------------
//
// MEASURED ON THE BOARD, NOT SUPPOSED. A first provisioning submission with correct credentials
// switched the radio AP → APSTA in 7 ms and then did nothing for the full 30 s timeout:
// esp_wifi_connect() was never called and not one disconnect reason code was produced, because
// WIFI_EVENT_STA_START had been posted from inside radio::prepare() before the handler was
// registered, and esp_wifi_start() — the only other producer of that event — is skipped when the
// access point has already started the driver. These tests pin both halves of the correction.

TEST(FirstAttemptTests, StartedIsWhatRequestsARealConnectionAttempt) {
    Lifecycle lifecycle;

    const auto step = lifecycle.on(Trigger::Started);

    // NextAction::Attempt is what makes the adapter call esp_wifi_connect(). Nothing else does.
    EXPECT_EQ(step.action, NextAction::Attempt);
    EXPECT_EQ(step.phase, NetworkPhase::Starting);
}

TEST(FirstAttemptTests, WithoutStartedNothingIsEverAttempted) {
    // The failure mode itself: a station that is never told it has started requests no attempt,
    // schedules no backoff and reports nothing — which is precisely the 30 s of silence the
    // board recorded. This is why the kick-off may not depend on receiving an event.
    Lifecycle lifecycle;

    EXPECT_EQ(lifecycle.phase(), NetworkPhase::Disabled);
    EXPECT_EQ(lifecycle.transient_failures(), 0U);
}

// ⬅️ StartupArming AND ITS FOUR TESTS ARE GONE — checkpoint 3c. It existed because
// `Trigger::Started` had TWO producers: WIFI_EVENT_STA_START, and network::start()'s own explicit
// kick-off. Both were needed, because in APSTA the event fires from inside esp_wifi_set_mode() and
// esp_wifi_start() then never posts a second one, while in station-only mode both fire for one
// bring-up. The intent machine removes the problem instead of arming around it: the attempt is
// issued because an INTENT exists, not because an event arrived, and STA_START is informational.
// The bring-up ORDER those tests also covered is still asserted, in bringup_tests.cpp.

TEST(FirstAttemptTests, ARestartStillRequestsAnAttemptForTheNewCredential) {
    // The already-correct retry path, unchanged: even from the terminal give-up, reset() plus
    // Started produces a real attempt.
    Lifecycle lifecycle;
    (void)lifecycle.on(Trigger::Started);

    for (int attempt = 0; attempt < 3; ++attempt) {
        (void)lifecycle.on(Trigger::Disconnected, DisconnectCause::CredentialsRejected);
    }
    EXPECT_EQ(lifecycle.phase(), NetworkPhase::Unavailable);

    lifecycle.reset();
    const auto step = lifecycle.on(Trigger::Started);

    EXPECT_EQ(step.action, NextAction::Attempt);
}

TEST(FirstAttemptTests, ClassificationAndBackoffPolicyAreUnchangedByThisFix) {
    // The correction is ordering only. These are the values the fix must not have touched.
    const Lifecycle::Policy policy;
    EXPECT_EQ(policy.rejected_attempt_limit, 3U) << "§16.2's rejected-credential limit";

    // A wrong password still gives up after exactly three rejections, and no sooner.
    Lifecycle lifecycle;
    (void)lifecycle.on(Trigger::Started);
    EXPECT_EQ(lifecycle.on(Trigger::Disconnected, DisconnectCause::CredentialsRejected).action,
              NextAction::WaitBackoff);
    EXPECT_EQ(lifecycle.on(Trigger::Disconnected, DisconnectCause::CredentialsRejected).action,
              NextAction::WaitBackoff);
    EXPECT_EQ(lifecycle.on(Trigger::Disconnected, DisconnectCause::CredentialsRejected).action,
              NextAction::GiveUp);

    // And a transient loss is still retried rather than given up on.
    Lifecycle other;
    (void)other.on(Trigger::Started);
    EXPECT_EQ(other.on(Trigger::Disconnected, DisconnectCause::Transient).action,
              NextAction::WaitBackoff);
}

} // namespace
