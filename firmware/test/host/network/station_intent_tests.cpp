// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include "station_intent.hpp"

#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

// The station intent machine — checkpoint 3c.
//
// ⛔ WHAT THESE TESTS ARE FOR. Every decision below used to live in a FreeRTOS task inside the
// driver adapter, where nothing could reach it: three separate rewrites of that task were rejected
// in review because each substituted a timer, a counter or a probe budget for the one thing the
// pinned SDK actually guarantees — that a connect or a connected-state disconnect is followed by a
// documented terminal event. These are the sequences that guarantee holds and does not hold in.
//
// ⬅️ AND WHAT THEY DO NOT COVER. wifi.cpp names ESP-IDF types and cannot be compiled on the host
// (Bootstrap Spec §11.3), so the adapter's obedience — that it really publishes the identity before
// it calls, really wipes its held configuration, and really calls service() from the application
// task — is established by reading the binding, not by these tests. That reading is reported
// separately.

namespace {

using prd::app_model::NetworkPhase;
using prd::network::delay_for;
using prd::network::DisconnectCause;
using prd::network::Jitter;
using prd::network::station::Driver;
using prd::network::station::Event;
using prd::network::station::EventKind;
using prd::network::station::Fault;
using prd::network::station::kNoTag;
using prd::network::station::Manager;
using prd::network::station::merge;
using prd::network::station::Moment;
using prd::network::station::Outcome;
using prd::network::station::Progress;
using prd::network::station::State;
using prd::network::station::Tag;
using prd::network::station::Truth;

enum class Op : std::uint8_t { Configure, Connect, Disconnect };

struct Call {
    Op op{Op::Configure};
    Tag tag{kNoTag};
};

[[nodiscard]] bool operator==(const Call& lhs, const Call& rhs) {
    return lhs.op == rhs.op && lhs.tag == rhs.tag;
}

// The driver, faked behind the same function pointers the adapter supplies. Each call is recorded
// with the identity it was issued under, so "which operation was charged to which candidate" is an
// assertion rather than a reading of the log.
class FakeDriver {
  public:
    std::vector<Call> calls;

    bool refuse_configure{false};
    bool refuse_connect{false};
    bool refuse_disconnect{false};

    [[nodiscard]] Driver handle() {
        return Driver{&configure_thunk, &connect_thunk, &disconnect_thunk, this};
    }

    [[nodiscard]] std::size_t count(const Op op) const {
        std::size_t total = 0;

        for (const Call& call : calls) {
            if (call.op == op) {
                ++total;
            }
        }

        return total;
    }

  private:
    static bool configure_thunk(void* const context, const Tag tag) {
        auto* const self = static_cast<FakeDriver*>(context);
        self->calls.push_back({Op::Configure, tag});
        return !self->refuse_configure;
    }

    static bool connect_thunk(void* const context, const Tag tag) {
        auto* const self = static_cast<FakeDriver*>(context);
        self->calls.push_back({Op::Connect, tag});
        return !self->refuse_connect;
    }

    static bool disconnect_thunk(void* const context, const Tag tag) {
        auto* const self = static_cast<FakeDriver*>(context);
        self->calls.push_back({Op::Disconnect, tag});
        return !self->refuse_disconnect;
    }
};

[[nodiscard]] Moment at(const std::uint32_t now_ms) {
    // A fixed jitter byte, so the §11.3 ladder is exact rather than a range. delay_for() is asked
    // for the expected value in the tests below rather than hard-coding one, so the ladder stays a
    // property of backoff.cpp.
    return Moment{now_ms, Jitter{0}};
}

[[nodiscard]] std::uint32_t ladder_ms(const std::uint32_t attempt) {
    return static_cast<std::uint32_t>(delay_for(attempt, Jitter{0}).count());
}

[[nodiscard]] Event event_of(const EventKind kind, const Tag tag) {
    Event event;
    event.kind = kind;
    event.tag = tag;
    return event;
}

[[nodiscard]] Event associated(const Tag tag) {
    return event_of(EventKind::Associated, tag);
}

[[nodiscard]] Event disassociated(const Tag tag, const std::uint8_t reason = 200) {
    Event event = event_of(EventKind::Disassociated, tag);
    event.reason = reason;
    return event;
}

[[nodiscard]] Event got_ip(const Tag tag, const std::uint32_t ipv4) {
    Event event = event_of(EventKind::GotIp, tag);
    event.ipv4 = ipv4;
    return event;
}

[[nodiscard]] Event lost_ip(const Tag tag) {
    return event_of(EventKind::LostIp, tag);
}

// Runs one submitted intent through to Connected with a lease, which is the precondition several
// sequences below start from. Returns the identity it was charged to.
Tag reach_connected(Manager& manager, FakeDriver& driver, const std::uint32_t ipv4,
                    const Tag tag = 1) {
    (void)manager.submit(tag, at(0), driver.handle());
    (void)manager.on(associated(tag), DisconnectCause::Transient, at(10), driver.handle());
    (void)manager.on(got_ip(tag, ipv4), DisconnectCause::Transient, at(20), driver.handle());
    return tag;
}

// --- 1. The ordinary path ------------------------------------------------------

TEST(StationIntentTests, AnIntentIsConfiguredConnectedAndThenLeased) {
    FakeDriver driver;
    Manager manager;

    const Progress submitted = manager.submit(1, at(0), driver.handle());

    EXPECT_EQ(driver.calls, (std::vector<Call>{{Op::Configure, 1}, {Op::Connect, 1}}));
    EXPECT_EQ(manager.state(), State::Attempting);
    EXPECT_TRUE(manager.completion_owed());
    EXPECT_TRUE(submitted.truth_changed);
    EXPECT_EQ(manager.truth().phase, NetworkPhase::Connecting);
    EXPECT_EQ(manager.truth().ipv4, 0U) << "association is not connection and carries no lease";

    const Progress link =
        manager.on(associated(1), DisconnectCause::Transient, at(10), driver.handle());

    EXPECT_EQ(manager.state(), State::Associated);
    EXPECT_FALSE(manager.completion_owed()) << "the documented completion arrived";
    EXPECT_FALSE(link.truth_changed) << "§7.4 carries an address, so the phase waits for the lease";
    EXPECT_FALSE(link.evidence) << "a link with no lease proves nothing about the candidate";

    const Progress lease =
        manager.on(got_ip(1, 0xC0A80164U), DisconnectCause::Transient, at(20), driver.handle());

    EXPECT_EQ(manager.state(), State::Connected);
    EXPECT_EQ(manager.truth().phase, NetworkPhase::Connected);
    EXPECT_EQ(manager.truth().ipv4, 0xC0A80164U);
    EXPECT_TRUE(lease.truth_changed);
    EXPECT_TRUE(lease.evidence);
    EXPECT_EQ(lease.tag, Tag{1});
    EXPECT_EQ(lease.outcome, Outcome::Leased);

    // No further driver operation: the station is where it should be.
    EXPECT_EQ(driver.calls.size(), 2U);
}

TEST(StationIntentTests, OneSubmissionProducesExactlyOneObservation) {
    // Test 24. The committed adapter published the same transition twice for one restart —
    // apply() published on phase_changed and restart() then published again — putting two identical
    // observations into a 12-slot queue per re-submission. Publication is now decided by whether
    // the truth actually differs, so a duplicate is unrepresentable rather than merely removed.
    //
    // ⬅️ AND IT IS ONE OBSERVATION, NOT TWO. NetworkPhase::Starting is set and superseded inside the
    // same call: by the time the machine runs, the adapter has already brought the driver up
    // synchronously, so "starting" is a phase that is already over and publishing it would be
    // publishing a transition the device is no longer in.
    FakeDriver driver;
    Manager manager;

    EXPECT_TRUE(manager.submit(1, at(0), driver.handle()).truth_changed);
    EXPECT_EQ(manager.truth().phase, NetworkPhase::Connecting);

    EXPECT_FALSE(manager.tick(at(0), driver.handle()).truth_changed);
    EXPECT_FALSE(manager.tick(at(100), driver.handle()).truth_changed);
    EXPECT_FALSE(manager.tick(at(200), driver.handle()).truth_changed);
}

TEST(StationIntentTests, AnAddressChangePublishesEvenThoughThePhaseDidNot) {
    // Test 23. A renewal to a different address is a change the product must not hide, and the
    // committed adapter hid it: it published only when the phase enum moved.
    FakeDriver driver;
    Manager manager;

    reach_connected(manager, driver, 0xC0A80164U);

    const Progress renewed =
        manager.on(got_ip(1, 0xC0A801C8U), DisconnectCause::Transient, at(60), driver.handle());

    EXPECT_EQ(manager.truth().phase, NetworkPhase::Connected) << "the phase name did not move";
    EXPECT_EQ(manager.truth().ipv4, 0xC0A801C8U);
    EXPECT_TRUE(renewed.truth_changed);
}

// --- 2. Failure and the accepted ladder ----------------------------------------

TEST(StationIntentTests, AFailedAttemptBacksOffOnTheAcceptedLadderAndRetriesWithoutReconfiguring) {
    FakeDriver driver;
    Manager manager;

    (void)manager.submit(1, at(0), driver.handle());

    const Progress failed =
        manager.on(disassociated(1), DisconnectCause::Transient, at(1000), driver.handle());

    EXPECT_EQ(manager.state(), State::Backoff);
    EXPECT_FALSE(manager.completion_owed());
    EXPECT_EQ(manager.truth().phase, NetworkPhase::Connecting);
    EXPECT_EQ(manager.truth().fault, Fault::LinkLost);
    EXPECT_TRUE(failed.truth_changed) << "the error is part of the published truth";
    EXPECT_TRUE(failed.evidence);
    EXPECT_EQ(failed.outcome, Outcome::LeaseInterrupted) << "not terminal: the station retries";

    // ⛔ NOTHING IS ISSUED WHILE THE DELAY RUNS, and the delay is §11.3's first step rather than a
    // number chosen here.
    EXPECT_FALSE(manager.tick(at(1000 + ladder_ms(1) - 1), driver.handle()).truth_changed);
    EXPECT_EQ(driver.calls.size(), 2U);

    (void)manager.tick(at(1000 + ladder_ms(1)), driver.handle());

    EXPECT_EQ(driver.calls,
              (std::vector<Call>{{Op::Configure, 1}, {Op::Connect, 1}, {Op::Connect, 1}}))
        << "a retry reuses the configuration the driver already holds";
    EXPECT_EQ(manager.state(), State::Attempting);
}

TEST(StationIntentTests, ARetryThatChangesNothingPublishesNothing) {
    // UI Behaviour Decision 001 §4: "Routine polling, retries and small telemetry changes shall not
    // become visible." The first failure moves the error and publishes; the second changes neither
    // the phase nor the error and must not.
    FakeDriver driver;
    Manager manager;

    (void)manager.submit(1, at(0), driver.handle());
    EXPECT_TRUE(manager.on(disassociated(1), DisconnectCause::Transient, at(1000), driver.handle())
                    .truth_changed);

    (void)manager.tick(at(1000 + ladder_ms(1)), driver.handle());

    EXPECT_FALSE(manager.on(disassociated(1), DisconnectCause::Transient, at(9000), driver.handle())
                     .truth_changed);
}

TEST(StationIntentTests, RepeatedCredentialRejectionGivesUpAndIsTerminalForTheCandidate) {
    FakeDriver driver;
    Manager manager;

    (void)manager.submit(1, at(0), driver.handle());

    std::uint32_t now = 0;
    Progress last;

    // The committed policy is three rejected attempts (a working baseline, owner ruling
    // 2026-08-15). The loop drives until the machine says it has stopped rather than assuming the
    // count.
    for (unsigned round = 0; round < 8 && manager.state() != State::GaveUp; ++round) {
        now += 1000;
        last = manager.on(disassociated(1, 202), DisconnectCause::CredentialsRejected, at(now),
                          driver.handle());

        if (manager.state() == State::Backoff) {
            now += ladder_ms(round + 1);
            (void)manager.tick(at(now), driver.handle());
        }
    }

    EXPECT_EQ(manager.state(), State::GaveUp);
    EXPECT_EQ(manager.truth().phase, NetworkPhase::Unavailable);
    EXPECT_EQ(manager.truth().fault, Fault::CredentialsRejected);
    EXPECT_TRUE(last.evidence);
    EXPECT_EQ(last.outcome, Outcome::Unreachable);

    // §16.2: nothing but a configuration change may re-arm it.
    const std::size_t settled = driver.calls.size();
    (void)manager.tick(at(now + 60000), driver.handle());
    EXPECT_EQ(driver.calls.size(), settled);
}

// --- 3-6. Supersession ---------------------------------------------------------

TEST(StationIntentTests, ANewIntentWhileAttemptingIsRetainedAndCancelsNothing) {
    // Test 3, and invariant 4. `wifi.rst:277-280` documents three WIFI_EVENT_STA_DISCONNECTED
    // scenarios and every one of them requires an already-connected station, a failed connect
    // setup, or a disrupted connection. There is no documented completion for a disconnect issued
    // at a station that is merely connecting, so calling one would leave a completion owed that may
    // never arrive.
    FakeDriver driver;
    Manager manager;

    (void)manager.submit(1, at(0), driver.handle());
    const std::vector<Call> before = driver.calls;

    const Progress superseded = manager.submit(2, at(500), driver.handle());

    EXPECT_EQ(driver.calls, before) << "no disconnect, no set_config, no connect";
    EXPECT_EQ(manager.active(), Tag{1});
    EXPECT_EQ(manager.retained(), Tag{2});
    EXPECT_TRUE(manager.completion_owed());
    EXPECT_FALSE(superseded.truth_changed);
    EXPECT_FALSE(superseded.evidence);
}

TEST(StationIntentTests, NothingIsIssuedWhileACompletionIsOwed) {
    // Invariants 1 and 2, driven for a long stretch of ticks so a stray path would show up.
    FakeDriver driver;
    Manager manager;

    (void)manager.submit(1, at(0), driver.handle());
    (void)manager.submit(2, at(10), driver.handle());

    const std::size_t issued = driver.calls.size();

    for (std::uint32_t now = 100; now <= 20000; now += 100) {
        (void)manager.tick(at(now), driver.handle());
    }

    EXPECT_EQ(driver.calls.size(), issued);
    EXPECT_TRUE(manager.completion_owed());
}

TEST(StationIntentTests, WhenTheOldAttemptFailsTheRetainedIntentBeginsFromTheNonAssociatedState) {
    // Tests 4 and 5's first half, and invariant 5.
    FakeDriver driver;
    Manager manager;

    (void)manager.submit(1, at(0), driver.handle());
    (void)manager.submit(2, at(500), driver.handle());

    const Progress failed =
        manager.on(disassociated(1), DisconnectCause::Transient, at(1000), driver.handle());

    EXPECT_EQ(driver.calls,
              (std::vector<Call>{
                  {Op::Configure, 1}, {Op::Connect, 1}, {Op::Configure, 2}, {Op::Connect, 2}}))
        << "no disconnect: there was no association to retire";

    EXPECT_EQ(manager.active(), Tag{2});
    EXPECT_EQ(manager.retained(), kNoTag);
    EXPECT_TRUE(manager.completion_owed());

    // ⛔ AND IT PUBLISHES NO EVIDENCE AT ALL — R2, and this assertion changed with the repair. It
    // used to expect the failure charged to identity 1: correct as far as it went, since a validator
    // waiting on 2 cannot be failed by an answer named 1 — but it also meant a TERMINAL outcome for
    // a dead transaction occupied the tick's single evidence result, where merge() keeps the first
    // terminal it is given and would have suppressed candidate 2's own outcome in the same tick.
    // A superseded attempt's failure is the step that permits adoption, not a verdict anybody reads.
    EXPECT_FALSE(failed.evidence);
}

TEST(StationIntentTests, WhenTheOldAttemptAssociatesItIsRetiredBeforeTheRetainedIntentIsApplied) {
    // Test 5, and invariant 6.
    FakeDriver driver;
    Manager manager;

    (void)manager.submit(1, at(0), driver.handle());
    (void)manager.submit(2, at(500), driver.handle());

    (void)manager.on(associated(1), DisconnectCause::Transient, at(600), driver.handle());

    EXPECT_EQ(driver.calls,
              (std::vector<Call>{{Op::Configure, 1}, {Op::Connect, 1}, {Op::Disconnect, 1}}))
        << "the association is retired under the identity that created it, and candidate 2 has not "
           "touched the driver yet";
    EXPECT_EQ(manager.state(), State::Retiring);
    EXPECT_TRUE(manager.completion_owed());

    const Progress retired =
        manager.on(disassociated(1), DisconnectCause::Transient, at(700), driver.handle());

    EXPECT_EQ(driver.calls, (std::vector<Call>{{Op::Configure, 1},
                                               {Op::Connect, 1},
                                               {Op::Disconnect, 1},
                                               {Op::Configure, 2},
                                               {Op::Connect, 2}}));
    EXPECT_EQ(manager.active(), Tag{2});

    // A deliberate retirement is not a failure, so it neither advances the ladder nor produces
    // candidate evidence: the intent that asked for it is what will be judged.
    EXPECT_FALSE(retired.evidence);
}

TEST(StationIntentTests, ReconfiguringAConnectedStationDisconnectsWaitsThenSetsAndConnects) {
    // Test 6, and invariant 7. esp_wifi_set_config() documents ESP_ERR_WIFI_STATE for a station
    // that is still connecting and esp_wifi_connect()'s @attention 2 requires an explicit
    // disconnect when connected, so this order is the SDK's rather than a preference. The committed
    // restart() did the opposite — disconnect, then set_config immediately, with no wait.
    FakeDriver driver;
    Manager manager;

    reach_connected(manager, driver, 0xC0A80164U);
    driver.calls.clear();

    (void)manager.submit(2, at(1000), driver.handle());

    EXPECT_EQ(driver.calls, (std::vector<Call>{{Op::Disconnect, 1}}));
    EXPECT_EQ(manager.state(), State::Retiring);

    // The wait: nothing else is issued until the matching terminal event arrives.
    for (std::uint32_t now = 1100; now <= 5000; now += 100) {
        (void)manager.tick(at(now), driver.handle());
    }

    EXPECT_EQ(driver.calls, (std::vector<Call>{{Op::Disconnect, 1}}));

    const Progress retired =
        manager.on(disassociated(1), DisconnectCause::Transient, at(5100), driver.handle());

    EXPECT_EQ(driver.calls,
              (std::vector<Call>{{Op::Disconnect, 1}, {Op::Configure, 2}, {Op::Connect, 2}}));
    EXPECT_EQ(manager.truth().ipv4, 0U) << "no stale address survives a disconnect";
    EXPECT_TRUE(retired.truth_changed);
}

// --- 9-13. DriverStalled -------------------------------------------------------

TEST(StationIntentTests, AMissingConnectCompletionQuarantinesTheStation) {
    // Tests 9 and 11.
    FakeDriver driver;
    Manager manager;

    (void)manager.submit(1, at(0), driver.handle());

    const Manager::Policy policy;
    EXPECT_FALSE(manager.tick(at(policy.completion_ms - 1), driver.handle()).truth_changed);
    EXPECT_EQ(manager.state(), State::Attempting);

    const Progress stalled = manager.tick(at(policy.completion_ms), driver.handle());

    EXPECT_EQ(manager.state(), State::Stalled);
    EXPECT_TRUE(manager.quarantined());
    EXPECT_EQ(manager.truth().phase, NetworkPhase::Unavailable);
    EXPECT_EQ(manager.truth().fault, Fault::DriverUnresponsive);
    EXPECT_TRUE(stalled.truth_changed);
    EXPECT_TRUE(stalled.evidence);
    EXPECT_EQ(stalled.tag, Tag{1});
    EXPECT_EQ(stalled.outcome, Outcome::DriverStalled);

    // ⛔ THE DEADLINE MOVED THE STATE AND CLEARED NOTHING — invariant 3. Only the matching
    // documented terminal event may clear an owed completion.
    EXPECT_TRUE(manager.completion_owed());

    // No further station driver operation, no reboot, no esp_wifi_stop()/start() recovery.
    const std::size_t settled = driver.calls.size();

    for (std::uint32_t now = policy.completion_ms; now <= policy.completion_ms + 60000;
         now += 100) {
        (void)manager.tick(at(now), driver.handle());
    }

    EXPECT_EQ(driver.calls.size(), settled);
}

TEST(StationIntentTests, AMissingDisconnectCompletionQuarantinesTheStation) {
    // Test 10.
    FakeDriver driver;
    Manager manager;

    reach_connected(manager, driver, 0xC0A80164U);
    (void)manager.submit(2, at(1000), driver.handle());
    ASSERT_EQ(manager.state(), State::Retiring);

    const Manager::Policy policy;
    const Progress stalled = manager.tick(at(1000 + policy.completion_ms), driver.handle());

    EXPECT_EQ(manager.state(), State::Stalled);
    EXPECT_TRUE(manager.completion_owed());
    EXPECT_EQ(stalled.outcome, Outcome::DriverStalled);

    // ⛔ CHARGED TO THE RETAINED INTENT, NOT TO THE OPERATION THAT OWED THE COMPLETION — R2. Before
    // the repair this asserted Tag{1}, which is the operation whose completion went missing; but the
    // transaction with a listener is candidate 2, because app_core re-begins its single
    // WifiValidation for every new submission and nothing is waiting on 1 any more. Reported under 1
    // it reached nobody, and the browser was told the generic WifiTimedOut thirty seconds later.
    EXPECT_EQ(stalled.tag, Tag{2})
        << "the newest retained candidate is the only transaction with a listener";
}

TEST(StationIntentTests, ALateCompletionInQuarantineClearsTheOwedFactAndNothingElse) {
    // Test 12.
    FakeDriver driver;
    Manager manager;

    (void)manager.submit(1, at(0), driver.handle());

    const Manager::Policy policy;
    (void)manager.tick(at(policy.completion_ms), driver.handle());
    ASSERT_EQ(manager.state(), State::Stalled);

    const auto before_truth = manager.truth();
    const std::size_t before_calls = driver.calls.size();

    const Progress late = manager.on(disassociated(1), DisconnectCause::Transient,
                                     at(policy.completion_ms + 5000), driver.handle());

    EXPECT_FALSE(manager.completion_owed()) << "the matching terminal event clears the owed fact";
    EXPECT_EQ(manager.state(), State::Stalled) << "and it may not resume";
    EXPECT_EQ(manager.truth(), before_truth);
    EXPECT_FALSE(late.truth_changed);
    EXPECT_FALSE(late.evidence) << "it must not validate or re-fail the transaction it belonged to";
    EXPECT_EQ(driver.calls.size(), before_calls);

    // And a fresh submission is refused rather than accepted into a station that will never answer.
    const Progress refused = manager.submit(2, at(policy.completion_ms + 6000), driver.handle());
    EXPECT_EQ(driver.calls.size(), before_calls);
    EXPECT_FALSE(refused.truth_changed);
    EXPECT_EQ(manager.state(), State::Stalled);
}

TEST(StationIntentTests, EvidenceLossQuarantinesUnderItsOwnTypedOutcome) {
    // Tests 16 and 17's consequence at this layer. Loss is a fault, not a case: the candidate
    // settles as WifiEvidenceLost rather than as Succeeded or CredentialsRejected.
    FakeDriver driver;
    Manager manager;

    (void)manager.submit(1, at(0), driver.handle());
    const std::size_t issued = driver.calls.size();

    const Progress lost = manager.evidence_lost();

    EXPECT_EQ(manager.state(), State::Stalled);
    EXPECT_EQ(manager.truth().phase, NetworkPhase::Unavailable);
    EXPECT_EQ(manager.truth().fault, Fault::DriverUnresponsive);
    EXPECT_TRUE(lost.truth_changed);
    EXPECT_TRUE(lost.evidence);
    EXPECT_EQ(lost.tag, Tag{1});
    EXPECT_EQ(lost.outcome, Outcome::EvidenceLost);

    (void)manager.tick(at(50000), driver.handle());
    EXPECT_EQ(driver.calls.size(), issued);
}

TEST(StationIntentTests, ARefusedSynchronousCallCreatesNoOwedCompletion) {
    // Test 13, and invariant 8, for each of the three calls.
    {
        FakeDriver driver;
        Manager manager;
        driver.refuse_configure = true;

        (void)manager.submit(1, at(0), driver.handle());

        EXPECT_EQ(driver.calls, (std::vector<Call>{{Op::Configure, 1}}));
        EXPECT_FALSE(manager.completion_owed());
        EXPECT_EQ(manager.state(), State::Stalled);
    }

    {
        FakeDriver driver;
        Manager manager;
        driver.refuse_connect = true;

        const Progress submitted = manager.submit(1, at(0), driver.handle());

        EXPECT_EQ(driver.calls, (std::vector<Call>{{Op::Configure, 1}, {Op::Connect, 1}}));
        EXPECT_FALSE(manager.completion_owed()) << "a refused call cannot owe a completion";
        EXPECT_EQ(manager.state(), State::Stalled);
        EXPECT_EQ(submitted.outcome, Outcome::DriverStalled);
    }

    {
        FakeDriver driver;
        Manager manager;

        reach_connected(manager, driver, 0xC0A80164U);
        driver.refuse_disconnect = true;
        driver.calls.clear();

        (void)manager.submit(2, at(1000), driver.handle());

        EXPECT_EQ(driver.calls, (std::vector<Call>{{Op::Disconnect, 1}}));
        EXPECT_FALSE(manager.completion_owed());
        EXPECT_EQ(manager.state(), State::Stalled);
    }
}

// --- 19-22. The lease ----------------------------------------------------------

TEST(StationIntentTests, LosingTheLeaseRetiresTheAssociationImmediatelyWithoutTouchingTheLadder) {
    // ⛔ PM DECISION, 2026-08-25: NO SECOND WAIT AFTER LostIp. ESP-IDF raises IP_EVENT_STA_LOST_IP
    // only after its own ~120 s lost-IP interval, by which time lwIP has already been rediscovering
    // for that whole interval. The earlier design then waited the 30 s no-lease bound on top, making
    // one outage roughly 150 s long before the association layer was asked to help. The documented
    // retirement begins at once instead.
    //
    // What did NOT change: the address is cleared immediately and truthfully, esp_wifi_connect() is
    // never called at an associated station, the DHCP client is never force-restarted, and §11.3's
    // ladder does not move — one physical outage is still counted once.
    FakeDriver driver;
    Manager manager;

    reach_connected(manager, driver, 0xC0A80164U);
    driver.calls.clear();

    const Progress lost =
        manager.on(lost_ip(1), DisconnectCause::Transient, at(1000), driver.handle());

    EXPECT_EQ(manager.truth().ipv4, 0U) << "the stale address goes immediately";
    EXPECT_EQ(manager.truth().phase, NetworkPhase::Reconnecting)
        << "a station with a link and no lease can reach nothing";
    EXPECT_TRUE(lost.truth_changed);
    EXPECT_EQ(lost.outcome, Outcome::LeaseInterrupted);

    // ⛔ THE DOCUMENTED RETIREMENT, AND NOTHING ELSE. One disconnect under the identity that owns
    // the association — never a connect at an associated station, and never a DHCP poke.
    EXPECT_EQ(driver.calls, (std::vector<Call>{{Op::Disconnect, 1}}));
    EXPECT_EQ(manager.state(), State::Retiring);
    EXPECT_TRUE(manager.completion_owed());

    // Its matching DISCONNECTED, then the reconnect. No reconfiguration: the driver still holds
    // that credential.
    (void)manager.on(disassociated(1), DisconnectCause::Transient, at(1100), driver.handle());

    EXPECT_EQ(driver.calls, (std::vector<Call>{{Op::Disconnect, 1}, {Op::Connect, 1}}));
    EXPECT_EQ(manager.state(), State::Attempting);

    // ⛔ THE LADDER IS UNTOUCHED, PROVEN BY WHAT THE NEXT REAL FAILURE COSTS. A disconnect now must
    // wait §11.3's FIRST step; if the lease loss or its retirement had counted, it would wait the
    // second.
    const std::size_t issued = driver.calls.size();
    (void)manager.on(disassociated(1), DisconnectCause::Transient, at(2000), driver.handle());
    ASSERT_EQ(manager.state(), State::Backoff);

    (void)manager.tick(at(2000 + ladder_ms(1) - 1), driver.handle());
    EXPECT_EQ(driver.calls.size(), issued);

    (void)manager.tick(at(2000 + ladder_ms(1)), driver.handle());
    EXPECT_EQ(driver.calls.size(), issued + 1);
}

TEST(StationIntentTests, ALeaseCancelsTheInitialNoLeaseEscalation) {
    // The no-lease bound survives for the case the PM accepted it for: an INITIAL association that
    // has not obtained its first lease. A lease arriving inside the window cancels it outright.
    FakeDriver driver;
    Manager manager;

    (void)manager.submit(1, at(0), driver.handle());
    (void)manager.on(associated(1), DisconnectCause::Transient, at(10), driver.handle());
    ASSERT_EQ(manager.state(), State::Associated);

    const Progress leased =
        manager.on(got_ip(1, 0xC0A80165U), DisconnectCause::Transient, at(2000), driver.handle());

    EXPECT_EQ(manager.state(), State::Connected);
    EXPECT_EQ(manager.truth().ipv4, 0xC0A80165U);
    EXPECT_TRUE(leased.truth_changed);
    EXPECT_EQ(leased.outcome, Outcome::Leased);

    const std::size_t issued = driver.calls.size();
    const Manager::Policy policy;

    for (std::uint32_t now = 2000; now <= 2000 + (policy.no_lease_ms * 2); now += 500) {
        (void)manager.tick(at(now), driver.handle());
    }

    EXPECT_EQ(driver.calls.size(), issued) << "the escalation was cancelled, not merely delayed";
    EXPECT_EQ(manager.state(), State::Connected);
}

TEST(StationIntentTests, AnInitialAssociationWithNoLeaseIsRetiredAtTheAcceptedBound) {
    // PM decision, 2026-08-25: thirty seconds is accepted for an initial association that has not
    // obtained its first lease. Escalation is the ordinary associated-state retirement, because that
    // disconnect has a documented completion and a forced DHCP restart does not.
    FakeDriver driver;
    Manager manager;

    (void)manager.submit(1, at(0), driver.handle());
    (void)manager.on(associated(1), DisconnectCause::Transient, at(10), driver.handle());
    ASSERT_EQ(manager.state(), State::Associated);

    driver.calls.clear();

    const Manager::Policy policy;

    (void)manager.tick(at(10 + policy.no_lease_ms - 1), driver.handle());
    EXPECT_TRUE(driver.calls.empty()) << "one millisecond short of the accepted bound";

    (void)manager.tick(at(10 + policy.no_lease_ms), driver.handle());

    EXPECT_EQ(driver.calls, (std::vector<Call>{{Op::Disconnect, 1}}));
    EXPECT_EQ(manager.state(), State::Retiring);

    (void)manager.on(disassociated(1), DisconnectCause::Transient, at(10 + policy.no_lease_ms + 50),
                     driver.handle());

    EXPECT_EQ(driver.calls, (std::vector<Call>{{Op::Disconnect, 1}, {Op::Connect, 1}}))
        << "reconnect only, under the same identity: the driver still holds that credential";
    EXPECT_EQ(manager.state(), State::Attempting);
}

TEST(StationIntentTests, AnOrdinaryDisconnectFollowedByADelayedLostIpCountsOneOutageOnce) {
    // Test 22. ESP-IDF raises IP_EVENT_STA_LOST_IP on a 120 s timer
    // (CONFIG_ESP_NETIF_IP_LOST_TIMER_INTERVAL=120), so it routinely arrives long after the
    // DISCONNECTED that already described the same physical outage.
    FakeDriver driver;
    Manager manager;

    reach_connected(manager, driver, 0xC0A80164U);
    const std::size_t issued = driver.calls.size();

    (void)manager.on(disassociated(1), DisconnectCause::Transient, at(1000), driver.handle());
    ASSERT_EQ(manager.state(), State::Backoff);
    EXPECT_EQ(manager.truth().ipv4, 0U);

    const Progress late =
        manager.on(lost_ip(1), DisconnectCause::Transient, at(1100), driver.handle());

    EXPECT_FALSE(late.truth_changed) << "the address was already gone and the phase already moved";
    EXPECT_FALSE(late.evidence) << "there is no second outage to report";
    EXPECT_EQ(manager.state(), State::Backoff);

    // The ladder is still at its first step, which is what "counted once" means.
    (void)manager.tick(at(1000 + ladder_ms(1) - 1), driver.handle());
    EXPECT_EQ(driver.calls.size(), issued);

    (void)manager.tick(at(1000 + ladder_ms(1)), driver.handle());
    EXPECT_EQ(driver.calls.size(), issued + 1);
}

// --- Withdrawal and the un-provisioned device ----------------------------------

TEST(StationIntentTests, TheAbsentCredentialTransitionPublishesDisabledAndAttemptsNothing) {
    FakeDriver driver;
    Manager manager;

    const Progress absent = manager.station_absent(at(0));

    EXPECT_TRUE(driver.calls.empty());
    EXPECT_EQ(manager.truth().phase, NetworkPhase::Disabled);
    EXPECT_FALSE(absent.truth_changed) << "Disabled is where it already was";
    EXPECT_EQ(manager.absent_refusals(), 0U);

    for (std::uint32_t now = 100; now <= 60000; now += 500) {
        (void)manager.tick(at(now), driver.handle());
    }

    EXPECT_TRUE(driver.calls.empty());
    EXPECT_EQ(manager.state(), State::Idle);
}

TEST(StationIntentTests, TheAbsentCredentialTransitionIsRefusedFromEveryLiveState) {
    // ⛔ THE REGRESSION FOR R6. The old contract said "withdraws any intent" and did not: while a
    // completion was owed it cleared only the retained intent and let the active one carry on
    // associating, and from Connected it published Disabled while leaving the station physically
    // joined. Both are now refused outright, because taking a live station down is a disconnect
    // transaction with a driver completion of its own — 3e's restoration work, not this.
    {
        FakeDriver driver;
        Manager manager;

        (void)manager.submit(1, at(0), driver.handle());
        ASSERT_TRUE(manager.completion_owed());

        const std::size_t issued = driver.calls.size();
        const Truth before = manager.truth();

        const Progress refused = manager.station_absent(at(100));

        EXPECT_FALSE(refused.truth_changed);
        EXPECT_EQ(manager.truth(), before) << "no published truth may change on a refusal";
        EXPECT_EQ(driver.calls.size(), issued) << "and no physical truth either";
        EXPECT_EQ(manager.active(), Tag{1}) << "the live intent is untouched";
        EXPECT_TRUE(manager.completion_owed());
        EXPECT_EQ(manager.absent_refusals(), 1U);
    }

    {
        FakeDriver driver;
        Manager manager;

        reach_connected(manager, driver, 0xC0A80164U);
        const Truth before = manager.truth();

        const Progress refused = manager.station_absent(at(1000));

        EXPECT_FALSE(refused.truth_changed);
        EXPECT_EQ(manager.truth(), before)
            << "publishing Disabled while still associated would be a lie about the radio";
        EXPECT_EQ(manager.truth().ipv4, 0xC0A80164U);
        EXPECT_EQ(manager.state(), State::Connected);
        EXPECT_EQ(manager.absent_refusals(), 1U);
    }

    {
        // And after a backoff, where an intent exists but nothing is owed: still a live intent.
        FakeDriver driver;
        Manager manager;

        (void)manager.submit(1, at(0), driver.handle());
        (void)manager.on(disassociated(1), DisconnectCause::Transient, at(100), driver.handle());
        ASSERT_EQ(manager.state(), State::Backoff);

        EXPECT_FALSE(manager.station_absent(at(200)).truth_changed);
        EXPECT_EQ(manager.active(), Tag{1});
        EXPECT_EQ(manager.absent_refusals(), 1U);
    }
}

// --- Runtime withdrawal (checkpoint 3e) ----------------------------------------
//
// ⛔ WHAT station_absent() REFUSED AND THIS DELIVERS. The test above asserts that taking a LIVE
// station down is refused, on the grounds that it is a disconnect transaction with a driver
// completion of its own. These are that transaction. Every invariant the rest of this file asserts
// for a submission holds for a withdrawal too, and each one is asserted again below rather than
// assumed: a withdrawal is the second thing in this machine that issues a driver operation, and the
// first one to do so with no successor intent behind it.

TEST(StationWithdrawalTests, AConnectedStationIsRetiredAndThenPublishesDisabled) {
    FakeDriver driver;
    Manager manager;

    reach_connected(manager, driver, 0xC0A80164U);
    const std::size_t issued = driver.calls.size();

    const Progress asked = manager.withdraw(at(1000), driver.handle());

    // The documented connected-state disconnect, charged to the identity whose association is being
    // taken down — §8.1's request identity, exactly as invariants 6 and 7 charge a retirement.
    EXPECT_EQ(driver.calls.size(), issued + 1);
    EXPECT_EQ(driver.calls.back(), (Call{Op::Disconnect, 1}));
    EXPECT_EQ(manager.state(), State::Retiring);
    EXPECT_TRUE(manager.completion_owed()) << "invariant 1: the disconnect owes its completion";
    EXPECT_TRUE(manager.withdrawing());
    EXPECT_FALSE(asked.evidence) << "a withdrawal is not a candidate anybody awaits a verdict on";

    // ⛔ NOT DISABLED YET, AND THAT IS THE WHOLE POINT. The station is still physically joined until
    // the driver says otherwise, and publishing Disabled here is precisely the lie R6 refused.
    EXPECT_EQ(manager.truth().phase, NetworkPhase::Connected);
    EXPECT_EQ(manager.truth().ipv4, 0xC0A80164U);

    const Progress done =
        manager.on(disassociated(1), DisconnectCause::Transient, at(1100), driver.handle());

    EXPECT_EQ(manager.truth().phase, NetworkPhase::Disabled);
    EXPECT_EQ(manager.truth().ipv4, 0U);
    EXPECT_EQ(manager.truth().fault, Fault::None) << "a deliberate withdrawal is not a Wi-Fi error";
    EXPECT_TRUE(done.truth_changed);
    EXPECT_FALSE(done.evidence);
    EXPECT_FALSE(manager.withdrawing());
    EXPECT_FALSE(manager.completion_owed());
    EXPECT_EQ(manager.state(), State::Idle);
    EXPECT_EQ(manager.active(), kNoTag);
    EXPECT_EQ(manager.retained(), kNoTag);
    EXPECT_FALSE(manager.credential_wanted())
        << "the adapter's wipe signal, so no passphrase stays";

    // Exactly one driver operation for the whole withdrawal, and nothing afterwards for ever.
    EXPECT_EQ(driver.calls.size(), issued + 1);

    for (std::uint32_t now = 1200; now <= 120000; now += 500) {
        (void)manager.tick(at(now), driver.handle());
    }

    EXPECT_EQ(driver.calls.size(), issued + 1);
    EXPECT_EQ(manager.truth().phase, NetworkPhase::Disabled);

    // ⬅️ AND THE MACHINE IS GENUINELY BACK IN THE UN-PROVISIONED SHAPE, which is why the boot-only
    // transition would now be representable again. Asserted so that this is a deliberate property
    // rather than something noticed later: withdraw() leaves no intent, owes nothing and is Idle.
    EXPECT_EQ(manager.absent_refusals(), 0U);
    (void)manager.station_absent(at(121000));
    EXPECT_EQ(manager.absent_refusals(), 0U) << "no refusal, because there is nothing live left";
}

TEST(StationWithdrawalTests, AnAssociatedStationIsRetiredThroughTheSameDisconnect) {
    // Associated is the other state that holds a link. Its no-lease deadline is running, and the
    // withdrawal must retire it through the documented disconnect rather than waiting the deadline
    // out or abandoning the link.
    FakeDriver driver;
    Manager manager;

    (void)manager.submit(1, at(0), driver.handle());
    (void)manager.on(associated(1), DisconnectCause::Transient, at(10), driver.handle());
    ASSERT_EQ(manager.state(), State::Associated);
    ASSERT_FALSE(manager.completion_owed());

    (void)manager.withdraw(at(20), driver.handle());

    EXPECT_EQ(driver.calls.back(), (Call{Op::Disconnect, 1}));
    EXPECT_EQ(manager.state(), State::Retiring);
    EXPECT_TRUE(manager.completion_owed());

    (void)manager.on(disassociated(1), DisconnectCause::Transient, at(30), driver.handle());

    EXPECT_EQ(manager.truth().phase, NetworkPhase::Disabled);
    EXPECT_EQ(manager.active(), kNoTag);
}

TEST(StationWithdrawalTests, ANonAssociatedStationIsWithdrawnWithNoDriverOperationAtAll) {
    // Three states that hold no link: a machine that never had an intent, one waiting out §11.3's
    // ladder, and §16.2's terminal give-up. None of them has anything to disconnect, so a
    // withdrawal settles immediately — and issuing a disconnect from any of them would be a call
    // the pinned SDK documents no completion for.
    {
        FakeDriver driver;
        Manager manager;

        const Progress withdrawn = manager.withdraw(at(0), driver.handle());

        EXPECT_TRUE(driver.calls.empty());
        EXPECT_EQ(manager.truth().phase, NetworkPhase::Disabled);
        EXPECT_FALSE(withdrawn.truth_changed) << "Disabled is where it already was";
        EXPECT_FALSE(manager.withdrawing());
    }

    {
        FakeDriver driver;
        Manager manager;

        (void)manager.submit(1, at(0), driver.handle());
        (void)manager.on(disassociated(1), DisconnectCause::Transient, at(100), driver.handle());
        ASSERT_EQ(manager.state(), State::Backoff);
        const std::size_t issued = driver.calls.size();

        const Progress withdrawn = manager.withdraw(at(200), driver.handle());

        EXPECT_EQ(driver.calls.size(), issued) << "a backoff holds no link to take down";
        EXPECT_EQ(manager.truth().phase, NetworkPhase::Disabled);
        EXPECT_TRUE(withdrawn.truth_changed);
        EXPECT_EQ(manager.active(), kNoTag);
        EXPECT_FALSE(manager.credential_wanted());

        // The ladder cannot fire afterwards: there is no intent left for it to retry.
        for (std::uint32_t now = 300; now <= 60000; now += 250) {
            (void)manager.tick(at(now), driver.handle());
        }

        EXPECT_EQ(driver.calls.size(), issued);
    }

    {
        FakeDriver driver;
        Manager manager;

        (void)manager.submit(1, at(0), driver.handle());

        // Rejected credentials, repeatedly, until §16.2 gives up.
        std::uint32_t now = 100;

        for (int attempt = 0; attempt < 12 && manager.state() != State::GaveUp; ++attempt) {
            (void)manager.on(disassociated(1, 2), DisconnectCause::CredentialsRejected, at(now),
                             driver.handle());
            now += 60000;
            (void)manager.tick(at(now), driver.handle());
            now += 1000;
        }

        ASSERT_EQ(manager.state(), State::GaveUp);
        const std::size_t issued = driver.calls.size();

        (void)manager.withdraw(at(now), driver.handle());

        EXPECT_EQ(driver.calls.size(), issued);
        EXPECT_EQ(manager.truth().phase, NetworkPhase::Disabled);
        EXPECT_EQ(manager.active(), kNoTag);
    }
}

TEST(StationWithdrawalTests, AWithdrawalRequestedWhileConnectingIsRetained) {
    // ⛔ INVARIANT 4's RULE, APPLIED TO A WITHDRAWAL. wifi.rst:277-280 documents three DISCONNECTED
    // scenarios and none of them is "disconnect while merely connecting", so calling disconnect()
    // here would leave a completion owed that may never arrive. The withdrawal waits.
    FakeDriver driver;
    Manager manager;

    (void)manager.submit(1, at(0), driver.handle());
    ASSERT_EQ(manager.state(), State::Attempting);
    ASSERT_TRUE(manager.completion_owed());
    const std::size_t issued = driver.calls.size();

    const Progress asked = manager.withdraw(at(100), driver.handle());

    EXPECT_EQ(driver.calls.size(), issued) << "invariant 2: nothing is issued while one is owed";
    EXPECT_TRUE(manager.withdrawing()) << "and it is retained rather than dropped";
    EXPECT_TRUE(manager.completion_owed());
    EXPECT_EQ(manager.active(), Tag{1}) << "the operation in flight keeps its identity";
    EXPECT_EQ(manager.state(), State::Attempting);
    EXPECT_FALSE(asked.truth_changed);

    // The attempt's own documented completion is what permits progress. It failed, so there is no
    // link to take down and the withdrawal finishes with no disconnect at all.
    (void)manager.on(disassociated(1), DisconnectCause::Transient, at(200), driver.handle());

    EXPECT_EQ(driver.calls.size(), issued);
    EXPECT_EQ(manager.truth().phase, NetworkPhase::Disabled);
    EXPECT_FALSE(manager.withdrawing());
    EXPECT_EQ(manager.active(), kNoTag);
}

TEST(StationWithdrawalTests, ARetainedWithdrawalRetiresAnAttemptThatSUCCEEDS) {
    // The other half of the retention rule, and the one a "clear the intent and publish Disabled"
    // shortcut would get wrong: the attempt the withdrawal was asked to cancel goes on to associate
    // and lease. The station is then genuinely joined, so the withdrawal issues the disconnect the
    // SDK does document — from the state that documents it.
    FakeDriver driver;
    Manager manager;

    (void)manager.submit(1, at(0), driver.handle());
    (void)manager.withdraw(at(50), driver.handle());
    ASSERT_TRUE(manager.withdrawing());

    const std::size_t issued = driver.calls.size();

    (void)manager.on(associated(1), DisconnectCause::Transient, at(100), driver.handle());

    // Associated holds a link, so the retained withdrawal is acted on as soon as the completion the
    // connect owed has been consumed.
    EXPECT_EQ(driver.calls.size(), issued + 1);
    EXPECT_EQ(driver.calls.back(), (Call{Op::Disconnect, 1}));
    EXPECT_EQ(manager.state(), State::Retiring);

    (void)manager.on(disassociated(1), DisconnectCause::Transient, at(200), driver.handle());

    EXPECT_EQ(manager.truth().phase, NetworkPhase::Disabled);
    EXPECT_EQ(manager.active(), kNoTag);
    EXPECT_EQ(driver.calls.size(), issued + 1) << "exactly one disconnect for one withdrawal";
}

TEST(StationWithdrawalTests, AWithdrawalDropsARetainedIntentAndASubmissionSupersedesAWithdrawal) {
    {
        // A submission that arrived while a completion was owed was never issued, so the session
        // that made it is over before the radio ever moved. Joining that network afterwards would
        // be acting on an instruction nobody is waiting for.
        FakeDriver driver;
        Manager manager;

        (void)manager.submit(1, at(0), driver.handle());
        (void)manager.submit(2, at(10), driver.handle());
        ASSERT_EQ(manager.retained(), Tag{2});

        (void)manager.withdraw(at(20), driver.handle());

        EXPECT_EQ(manager.retained(), kNoTag);
        EXPECT_TRUE(manager.withdrawing());

        (void)manager.on(disassociated(1), DisconnectCause::Transient, at(100), driver.handle());

        EXPECT_EQ(manager.truth().phase, NetworkPhase::Disabled);
        EXPECT_EQ(manager.active(), kNoTag) << "intent 2 was never adopted";
        EXPECT_EQ(driver.calls, (std::vector<Call>{{Op::Configure, 1}, {Op::Connect, 1}}));
    }

    {
        // ⬅️ AND THE OTHER DIRECTION, WHICH IS ALSO HOW A RESTORATION IS EXPRESSED. Rejoining a
        // previous pair IS a submit; if the withdrawal survived it, the station would be taken back
        // down the moment that intent settled.
        FakeDriver driver;
        Manager manager;

        reach_connected(manager, driver, 0xC0A80164U);
        (void)manager.withdraw(at(1000), driver.handle());
        ASSERT_EQ(manager.state(), State::Retiring);
        ASSERT_TRUE(manager.withdrawing());

        (void)manager.submit(2, at(1100), driver.handle());
        EXPECT_FALSE(manager.withdrawing()) << "the newer instruction wins";
        EXPECT_EQ(manager.retained(), Tag{2});

        // The disconnect already in flight is the retirement intent 2 needed anyway.
        (void)manager.on(disassociated(1), DisconnectCause::Transient, at(1200), driver.handle());

        EXPECT_EQ(manager.active(), Tag{2});
        EXPECT_EQ(manager.state(), State::Attempting);
        EXPECT_NE(manager.truth().phase, NetworkPhase::Disabled);
        EXPECT_EQ(driver.calls.back(), (Call{Op::Connect, 2}));
    }
}

TEST(StationWithdrawalTests, AStaleOrMismatchedCompletionCannotFinishAWithdrawal) {
    // §8.1's stale-answer rejection. A DISCONNECTED charged to another identity is not this
    // operation's completion, so it may neither clear the owed fact nor publish Disabled — the
    // station is still joined, and saying otherwise is the lie the whole transaction exists to
    // avoid.
    FakeDriver driver;
    Manager manager;

    reach_connected(manager, driver, 0xC0A80164U);
    (void)manager.withdraw(at(1000), driver.handle());
    ASSERT_EQ(manager.state(), State::Retiring);

    const std::size_t issued = driver.calls.size();

    const Progress stale =
        manager.on(disassociated(99), DisconnectCause::Transient, at(1100), driver.handle());

    EXPECT_TRUE(manager.completion_owed()) << "invariant 3: only the matching event clears it";
    EXPECT_EQ(manager.state(), State::Retiring);
    EXPECT_TRUE(manager.withdrawing());
    EXPECT_NE(manager.truth().phase, NetworkPhase::Disabled);
    EXPECT_FALSE(stale.truth_changed);
    EXPECT_EQ(driver.calls.size(), issued) << "and no second disconnect is issued";

    // The real one still works.
    (void)manager.on(disassociated(1), DisconnectCause::Transient, at(1200), driver.handle());
    EXPECT_EQ(manager.truth().phase, NetworkPhase::Disabled);
    EXPECT_FALSE(manager.completion_owed());
}

TEST(StationWithdrawalTests, ARefusedWithdrawalQuarantinesAndProducesExactlyOneTerminalOutcome) {
    // Invariant 8: a refused synchronous call creates no owed completion, and the owner's ruling for
    // a driver behaving outside its documentation is a truthful quarantine rather than a retry.
    FakeDriver driver;
    Manager manager;

    reach_connected(manager, driver, 0xC0A80164U);
    driver.refuse_disconnect = true;

    const Progress refused = manager.withdraw(at(1000), driver.handle());

    EXPECT_EQ(manager.state(), State::Stalled);
    EXPECT_FALSE(manager.completion_owed()) << "a refusal cannot create an owed completion";
    EXPECT_EQ(manager.truth().phase, NetworkPhase::Unavailable);
    EXPECT_EQ(manager.truth().fault, Fault::DriverUnresponsive);

    // ⛔ EXACTLY ONE TERMINAL OUTCOME, AND IT IS THE DRIVER'S. Not Disabled — the withdrawal did not
    // happen — and not two results merged into one tick.
    EXPECT_TRUE(refused.evidence);
    EXPECT_EQ(refused.outcome, Outcome::DriverStalled);
    EXPECT_EQ(refused.tag, Tag{1});
    EXPECT_TRUE(refused.truth_changed);

    // And no further driver operation, for ever.
    const std::size_t issued = driver.calls.size();
    driver.refuse_disconnect = false;

    EXPECT_FALSE(manager.withdraw(at(2000), driver.handle()).truth_changed);

    for (std::uint32_t now = 2000; now <= 120000; now += 1000) {
        (void)manager.tick(at(now), driver.handle());
    }

    EXPECT_EQ(driver.calls.size(), issued);
    EXPECT_EQ(manager.state(), State::Stalled);
}

TEST(StationWithdrawalTests, AWithdrawalWhoseCompletionNeverArrivesQuarantines) {
    FakeDriver driver;
    Manager manager;

    reach_connected(manager, driver, 0xC0A80164U);
    (void)manager.withdraw(at(1000), driver.handle());
    ASSERT_EQ(manager.state(), State::Retiring);

    const Manager::Policy policy{};

    // One millisecond inside the bound is still waiting: the consequence of this deadline is a
    // quarantine the user must power-cycle out of, so it must never fire on a merely slow driver.
    (void)manager.tick(at(1000 + policy.completion_ms - 1), driver.handle());
    EXPECT_EQ(manager.state(), State::Retiring);
    EXPECT_NE(manager.truth().phase, NetworkPhase::Unavailable);

    const Progress late = manager.tick(at(1000 + policy.completion_ms), driver.handle());

    EXPECT_EQ(manager.state(), State::Stalled);
    EXPECT_EQ(manager.truth().fault, Fault::DriverUnresponsive);
    EXPECT_TRUE(late.evidence);
    EXPECT_EQ(late.outcome, Outcome::DriverStalled);
    EXPECT_TRUE(manager.completion_owed())
        << "invariant 3: the deadline moves the state and clears nothing";

    // A late matching completion discharges the owed fact and revives nothing — least of all the
    // withdrawal, which must not quietly publish Disabled from inside a quarantine.
    (void)manager.on(disassociated(1), DisconnectCause::Transient, at(200000), driver.handle());
    EXPECT_FALSE(manager.completion_owed());
    EXPECT_EQ(manager.state(), State::Stalled);
    EXPECT_EQ(manager.truth().phase, NetworkPhase::Unavailable);
}

TEST(StationWithdrawalTests, LostEvidenceDuringAWithdrawalQuarantinesRatherThanConcluding) {
    FakeDriver driver;
    Manager manager;

    reach_connected(manager, driver, 0xC0A80164U);
    (void)manager.withdraw(at(1000), driver.handle());
    ASSERT_EQ(manager.state(), State::Retiring);

    const Progress lost = manager.evidence_lost();

    EXPECT_EQ(manager.state(), State::Stalled);
    EXPECT_EQ(manager.truth().fault, Fault::DriverUnresponsive);
    EXPECT_TRUE(lost.evidence);
    EXPECT_EQ(lost.outcome, Outcome::EvidenceLost);
    EXPECT_EQ(lost.tag, Tag{1}) << "R2's victim rule, unchanged by the withdrawal";
    EXPECT_NE(manager.truth().phase, NetworkPhase::Disabled)
        << "an incomplete record cannot conclude that the station came down";
}

TEST(StationWithdrawalTests, AWithdrawalIsRefusedWhileQuarantinedAndWhileStoodDown) {
    {
        // PM ruling 3 of checkpoint 3e: a quarantined station belongs to the typed Device Fault
        // path, and there is no honest way to take it down. Refused without a driver call and
        // without disturbing the published fault.
        FakeDriver driver;
        Manager manager;

        reach_connected(manager, driver, 0xC0A80164U);
        driver.refuse_connect = true;
        (void)manager.submit(2, at(1000), driver.handle());
        (void)manager.on(disassociated(1), DisconnectCause::Transient, at(1100), driver.handle());
        ASSERT_EQ(manager.state(), State::Stalled);

        const Truth before = manager.truth();
        const std::size_t issued = driver.calls.size();

        const Progress refused = manager.withdraw(at(2000), driver.handle());

        EXPECT_FALSE(refused.truth_changed);
        EXPECT_FALSE(refused.evidence) << "and it invents no second outcome for the same radio";
        EXPECT_EQ(manager.truth(), before);
        EXPECT_EQ(driver.calls.size(), issued);
        EXPECT_FALSE(manager.withdrawing());
    }

    {
        // R4: an exclusive operation owns the product state. A device erasing itself must not also
        // be taking its station down on somebody else's account.
        FakeDriver driver;
        Manager manager;

        reach_connected(manager, driver, 0xC0A80164U);
        (void)manager.quiesce(at(1000));
        ASSERT_TRUE(manager.quiesced());

        const Truth before = manager.truth();
        const std::size_t issued = driver.calls.size();

        const Progress refused = manager.withdraw(at(1100), driver.handle());

        EXPECT_FALSE(refused.truth_changed);
        EXPECT_EQ(manager.truth(), before);
        EXPECT_EQ(driver.calls.size(), issued);
        EXPECT_FALSE(manager.withdrawing());
        EXPECT_EQ(manager.state(), State::Connected) << "nothing about the radio changed";
    }
}

TEST(StationWithdrawalTests, AnExclusiveOperationForgetsAWithdrawalRatherThanParkingIt) {
    // A withdrawal is a request for a driver operation, and no driver operation is issued while an
    // exclusive operation holds the device. Resumption after a FAILED erase is the open
    // factory-reset item and is deliberately not modelled, so the request goes rather than waiting.
    FakeDriver driver;
    Manager manager;

    (void)manager.submit(1, at(0), driver.handle());
    (void)manager.withdraw(at(50), driver.handle());
    ASSERT_TRUE(manager.withdrawing());

    (void)manager.quiesce(at(100));

    EXPECT_FALSE(manager.withdrawing());

    const std::size_t issued = driver.calls.size();

    for (std::uint32_t now = 200; now <= 120000; now += 1000) {
        (void)manager.tick(at(now), driver.handle());
    }

    EXPECT_EQ(driver.calls.size(), issued);
}

TEST(StationWithdrawalTests, AWithdrawalProducesNoCandidateEvidenceOnItsSuccessfulPath) {
    // ⛔ THE TICK'S SINGLE EVIDENCE SLOT IS NOT SPENT ON A WITHDRAWAL. The session that submitted
    // the candidate is already over — that is why the withdrawal was asked for — so an outcome here
    // would be a verdict for a transaction with no listener, and being terminal it would survive
    // merge() and suppress whatever a successor actually reported.
    FakeDriver driver;
    Manager manager;

    reach_connected(manager, driver, 0xC0A80164U);

    Progress folded;
    merge(folded, manager.withdraw(at(1000), driver.handle()));
    merge(folded, manager.tick(at(1050), driver.handle()));
    merge(folded,
          manager.on(disassociated(1), DisconnectCause::Transient, at(1100), driver.handle()));
    merge(folded, manager.tick(at(1150), driver.handle()));

    EXPECT_FALSE(folded.evidence);
    EXPECT_TRUE(folded.truth_changed);
    EXPECT_EQ(manager.truth().phase, NetworkPhase::Disabled);
}

TEST(StationWithdrawalNegativeControlTests, TheRetentionAndTheCompletionAreBothLoadBearing) {
    // ⛔ TWO ASSERTIONS THAT WOULD PASS A BROKEN IMPLEMENTATION UNLESS THEY ARE MADE TOGETHER.
    // A withdrawal that published Disabled immediately would satisfy every "ends up Disabled" check
    // in this file, and one that never retained would satisfy every "issues a disconnect" check. The
    // two facts asserted here are the ones such an implementation cannot have at the same time: from
    // Connected the truth must NOT be Disabled before the completion arrives, and from Attempting a
    // disconnect must NOT be issued at all.
    {
        FakeDriver driver;
        Manager manager;

        reach_connected(manager, driver, 0xC0A80164U);
        (void)manager.withdraw(at(1000), driver.handle());

        ASSERT_EQ(manager.truth().phase, NetworkPhase::Connected);
        ASSERT_EQ(manager.state(), State::Retiring);
    }

    {
        FakeDriver driver;
        Manager manager;

        (void)manager.submit(1, at(0), driver.handle());
        const std::size_t issued = driver.calls.size();
        (void)manager.withdraw(at(50), driver.handle());

        ASSERT_EQ(driver.calls.size(), issued);
        ASSERT_EQ(driver.count(Op::Disconnect), 0U);
    }
}

TEST(StationIntentTests, AStationStopEndsTheIntentRatherThanRestartingIt) {
    FakeDriver driver;
    Manager manager;

    reach_connected(manager, driver, 0xC0A80164U);
    const std::size_t issued = driver.calls.size();

    const Progress stopped = manager.on(event_of(EventKind::Stopped, 1), DisconnectCause::Transient,
                                        at(1000), driver.handle());

    EXPECT_EQ(manager.truth().phase, NetworkPhase::Disabled);
    EXPECT_EQ(manager.truth().ipv4, 0U);
    EXPECT_EQ(manager.active(), kNoTag);
    EXPECT_TRUE(stopped.truth_changed);

    // ⬅️ AND IT IS NOT RE-ESTABLISHED. The adapter's copy of the credential is wiped as soon as
    // set_config consumes it, so there would be nothing left to reconfigure with. A new submission
    // is what starts a station again — the committed behaviour, preserved.
    for (std::uint32_t now = 1100; now <= 60000; now += 500) {
        (void)manager.tick(at(now), driver.handle());
    }

    EXPECT_EQ(driver.calls.size(), issued);
}

// --- R2: the newest live candidate is the one that gets settled -----------------
//
// ⛔ WHY THIS MATTERS AT ALL. app_core keeps a SINGLE WifiValidation and re-begins it for every new
// submission, so after a supersession nothing is listening on the attempt that was replaced. A
// station-fatal condition charged to the older identity therefore reaches nobody: the browser waits
// out the generic thirty-second budget and is told the network timed out, when the truth is that the
// driver stopped answering and a restart is required. Each shape below is a reachable production
// sequence.

TEST(StationIntentTests, ARefusedRetirementSettlesTheRetainedCandidateNotTheOldOne) {
    // Shape 1: A is Connected, B is submitted, and the disconnect that would retire A is refused.
    FakeDriver driver;
    Manager manager;

    reach_connected(manager, driver, 0xC0A80164U);
    driver.refuse_disconnect = true;

    const Progress superseded = manager.submit(2, at(1000), driver.handle());

    EXPECT_EQ(manager.state(), State::Stalled);
    EXPECT_FALSE(manager.completion_owed()) << "a refused call owes nothing";
    EXPECT_TRUE(superseded.evidence);
    EXPECT_EQ(superseded.outcome, Outcome::DriverStalled);
    EXPECT_EQ(superseded.tag, Tag{2}) << "candidate 2 is the transaction with a listener";
}

TEST(StationIntentTests, AMissingCompletionSettlesTheRetainedCandidate) {
    // Shape 2: A still owes a completion, B is retained, and A's deadline expires.
    FakeDriver driver;
    Manager manager;

    (void)manager.submit(1, at(0), driver.handle());
    (void)manager.submit(2, at(500), driver.handle());
    ASSERT_TRUE(manager.completion_owed());
    ASSERT_EQ(manager.retained(), Tag{2});

    const Manager::Policy policy;
    const Progress stalled = manager.tick(at(policy.completion_ms), driver.handle());

    EXPECT_EQ(manager.state(), State::Stalled);
    EXPECT_TRUE(manager.completion_owed()) << "invariant 3: only the matching event clears it";
    EXPECT_TRUE(stalled.evidence);
    EXPECT_EQ(stalled.outcome, Outcome::DriverStalled);
    EXPECT_EQ(stalled.tag, Tag{2});
}

TEST(StationIntentTests, LostEvidenceSettlesTheRetainedCandidate) {
    // Shape 3: the event channel loses a record while B is retained.
    FakeDriver driver;
    Manager manager;

    (void)manager.submit(1, at(0), driver.handle());
    (void)manager.submit(2, at(500), driver.handle());

    const Progress lost = manager.evidence_lost();

    EXPECT_EQ(manager.state(), State::Stalled);
    EXPECT_TRUE(lost.evidence);
    EXPECT_EQ(lost.outcome, Outcome::EvidenceLost);
    EXPECT_EQ(lost.tag, Tag{2});
}

TEST(StationIntentTests, AnUnexpectedStopSettlesTheRetainedCandidate) {
    // Shape 4 — R5's half of the same rule. An interface-wide STOP ends every in-flight operation,
    // so the retained candidate can never be executed and must be told so rather than left to reach
    // the browser as a generic timeout half a minute later.
    FakeDriver driver;
    Manager manager;

    (void)manager.submit(1, at(0), driver.handle());
    (void)manager.submit(2, at(500), driver.handle());

    const Progress stopped = manager.on(event_of(EventKind::Stopped, 1), DisconnectCause::Transient,
                                        at(600), driver.handle());

    EXPECT_EQ(manager.truth().phase, NetworkPhase::Disabled);
    EXPECT_FALSE(manager.completion_owed())
        << "an interface that no longer exists cannot complete anything";
    EXPECT_TRUE(stopped.evidence);
    EXPECT_EQ(stopped.outcome, Outcome::EvidenceLost);
    EXPECT_EQ(stopped.tag, Tag{2});
    EXPECT_EQ(manager.active(), kNoTag);
}

TEST(StationIntentTests, ASupersededFailureDoesNotOccupyTheEvidenceResult) {
    // Shape 5, and the merge() half of R2. In ONE tick: A's give-up (terminal, older identity)
    // followed by B's synchronous refusal (terminal, newest identity). merge() keeps the first
    // terminal outcome it is given, so if A's failure were published it would survive and suppress
    // B's — and B's browser would be told its network was unreachable rather than that the display
    // must be restarted. A superseded attempt's failure is the step that PERMITS adoption, not a
    // verdict, so it is not published at all.
    FakeDriver driver;
    Manager manager;

    // Drive candidate 1 to one rejection short of §16.2's give-up.
    (void)manager.submit(1, at(0), driver.handle());

    std::uint32_t now = 0;
    unsigned rounds = 0;

    while (manager.state() != State::GaveUp && rounds < 8) {
        ++rounds;
        now += 1000;
        (void)manager.on(disassociated(1, 202), DisconnectCause::CredentialsRejected, at(now),
                         driver.handle());

        if (manager.state() == State::Backoff) {
            now += ladder_ms(rounds);
            (void)manager.tick(at(now), driver.handle());
        }
    }

    ASSERT_EQ(manager.state(), State::GaveUp);

    // Now B arrives and its configure is refused: A's give-up already happened, so this tick's
    // single evidence result must belong to B.
    driver.refuse_configure = true;
    const Progress refused = manager.submit(2, at(now + 100), driver.handle());

    EXPECT_EQ(manager.state(), State::Stalled);
    EXPECT_TRUE(refused.evidence);
    EXPECT_EQ(refused.outcome, Outcome::DriverStalled);
    EXPECT_EQ(refused.tag, Tag{2});
}

TEST(StationIntentTests, ASupersededAttemptsOrdinaryFailurePublishesNoEvidenceAtAll) {
    // The narrow statement of the same rule: A fails transiently while B waits. That failure lets
    // advance() adopt B, and B is what will be judged — so the tick reports no candidate evidence.
    FakeDriver driver;
    Manager manager;

    (void)manager.submit(1, at(0), driver.handle());
    (void)manager.submit(2, at(500), driver.handle());

    const Progress failed =
        manager.on(disassociated(1), DisconnectCause::Transient, at(1000), driver.handle());

    EXPECT_FALSE(failed.evidence) << "a superseded attempt's failure is progress, not a verdict";
    EXPECT_EQ(manager.active(), Tag{2}) << "and it did permit the adoption";
    EXPECT_EQ(driver.calls,
              (std::vector<Call>{
                  {Op::Configure, 1}, {Op::Connect, 1}, {Op::Configure, 2}, {Op::Connect, 2}}));
}

TEST(StationIntentTests, NoStaleOutcomeCanSettleTheRetainedCandidate) {
    // The mirror guard: nothing in any of the shapes above may reach the retained candidate under
    // the OLD identity. Exhaustive over the four station-fatal entry points.
    {
        FakeDriver driver;
        Manager manager;
        driver.refuse_disconnect = true;
        reach_connected(manager, driver, 0xC0A80164U);
        const Progress p = manager.submit(2, at(1000), driver.handle());
        EXPECT_NE(p.tag, Tag{1});
    }
    {
        FakeDriver driver;
        Manager manager;
        (void)manager.submit(1, at(0), driver.handle());
        (void)manager.submit(2, at(10), driver.handle());
        const Manager::Policy policy;
        EXPECT_NE(manager.tick(at(policy.completion_ms), driver.handle()).tag, Tag{1});
    }
    {
        FakeDriver driver;
        Manager manager;
        (void)manager.submit(1, at(0), driver.handle());
        (void)manager.submit(2, at(10), driver.handle());
        EXPECT_NE(manager.evidence_lost().tag, Tag{1});
    }
    {
        FakeDriver driver;
        Manager manager;
        (void)manager.submit(1, at(0), driver.handle());
        (void)manager.submit(2, at(10), driver.handle());
        EXPECT_NE(manager
                      .on(event_of(EventKind::Stopped, 1), DisconnectCause::Transient, at(20),
                          driver.handle())
                      .tag,
                  Tag{1});
    }
}

TEST(StationIntentTests, ExactlyOneTerminalOutcomeReachesTheRetainedCandidate) {
    // Once quarantined the machine issues nothing and reports nothing further, so the retained
    // candidate receives its terminal outcome once and only once.
    FakeDriver driver;
    Manager manager;

    (void)manager.submit(1, at(0), driver.handle());
    (void)manager.submit(2, at(500), driver.handle());

    const Manager::Policy policy;
    const Progress first = manager.tick(at(policy.completion_ms), driver.handle());
    ASSERT_TRUE(first.evidence);
    ASSERT_EQ(first.tag, Tag{2});

    for (std::uint32_t now = policy.completion_ms + 100; now <= policy.completion_ms + 60000;
         now += 500) {
        const Progress later = manager.tick(at(now), driver.handle());
        EXPECT_FALSE(later.evidence) << "a settled quarantine reports nothing further";
    }

    // And the late matching completion clears the owed fact without producing a second verdict.
    const Progress late =
        manager.on(disassociated(1), DisconnectCause::Transient, at(200000), driver.handle());
    EXPECT_FALSE(manager.completion_owed());
    EXPECT_FALSE(late.evidence);
}

// --- R4: standing down for an exclusive operation --------------------------------

TEST(StationIntentTests, AQuiescedStationIssuesNothingAndForgetsTheRetainedIntent) {
    // ⛔ A DEVICE THAT IS ERASING ITSELF MUST NOT ALSO BE JOINING A NETWORK. Before this repair the
    // barrier ended the provisioning transaction's RAM but the machine went on advancing: a backoff
    // expiry issued connect() and a pending retirement issued disconnect() straight through it.
    FakeDriver driver;
    Manager manager;

    (void)manager.submit(1, at(0), driver.handle());
    (void)manager.on(disassociated(1), DisconnectCause::Transient, at(100), driver.handle());
    ASSERT_EQ(manager.state(), State::Backoff);

    (void)manager.submit(2, at(200), driver.handle());
    const std::size_t issued = driver.calls.size();

    (void)manager.quiesce(at(300));

    EXPECT_TRUE(manager.quiesced());
    EXPECT_EQ(manager.retained(), kNoTag) << "the unissued intent is forgotten";
    EXPECT_FALSE(manager.credential_wanted()) << "so the adapter may wipe its held configuration";

    // Every subsequent step is inert, including the backoff expiry that would otherwise connect.
    for (std::uint32_t now = 400; now <= 120000; now += 250) {
        const Progress step = manager.tick(at(now), driver.handle());
        EXPECT_FALSE(step.evidence);
    }

    EXPECT_EQ(driver.calls.size(), issued) << "no driver operation while the reset owns the device";
}

TEST(StationIntentTests, QuiescingLeavesAnOwedCompletionOwedAndPublishesNoContradiction) {
    // Invariant 3 still holds through the barrier: an operation in flight keeps owing its
    // completion, so a late terminal event is still consumed and still cannot revive anything. And
    // the published truth is untouched — the radio has not changed, and the reset owns the panel.
    FakeDriver driver;
    Manager manager;

    (void)manager.submit(1, at(0), driver.handle());
    ASSERT_TRUE(manager.completion_owed());

    const Truth before = manager.truth();
    const Progress quiesced = manager.quiesce(at(100));

    EXPECT_TRUE(manager.completion_owed());
    EXPECT_FALSE(quiesced.truth_changed);
    EXPECT_EQ(manager.truth(), before);

    (void)manager.on(disassociated(1), DisconnectCause::Transient, at(200), driver.handle());
    EXPECT_FALSE(manager.completion_owed()) << "the matching event still discharges it";
}

TEST(StationIntentTests, ANewSubmissionIsWhatLiftsTheStandDown) {
    // The barrier is asserted every tick while the operation runs, so only a submission accepted
    // after it releases the device can restart the station. app_core's guard is what guarantees the
    // ordering; this is the machine's half.
    FakeDriver driver;
    Manager manager;

    (void)manager.quiesce(at(0));
    ASSERT_TRUE(manager.quiesced());

    (void)manager.tick(at(100), driver.handle());
    EXPECT_TRUE(driver.calls.empty());

    (void)manager.submit(7, at(200), driver.handle());

    EXPECT_FALSE(manager.quiesced());
    EXPECT_EQ(driver.calls, (std::vector<Call>{{Op::Configure, 7}, {Op::Connect, 7}}));
}

// --- The adapter's wipe signal --------------------------------------------------

TEST(StationIntentTests, TheHeldCredentialIsWantedForExactlyAsLongAsAnIntentCanUseIt) {
    // ⛔ WHAT THIS GUARDS. The adapter holds ONE converted station configuration — the passphrase
    // included — for a submission the machine has not yet been able to apply, and wipes it when
    // this says nobody can ask for it any more. Getting it wrong in one direction leaves a
    // credential in RAM for the life of the device; in the other it wipes a configuration the very
    // next adopt() needs, and the machine then quarantines on a configure it cannot satisfy.
    {
        Manager manager;
        EXPECT_FALSE(manager.credential_wanted()) << "no intent, nothing to hold";
    }

    {
        FakeDriver driver;
        Manager manager;

        (void)manager.submit(1, at(0), driver.handle());
        EXPECT_FALSE(manager.credential_wanted()) << "set_config consumed it";
    }

    {
        // ⛔ THE CASE THAT WAS WRONG FIRST TIME. A submission arriving while a completion is owed
        // is retained, and its credential has NOT reached the driver — even though the predecessor's
        // has.
        FakeDriver driver;
        Manager manager;

        (void)manager.submit(1, at(0), driver.handle());
        (void)manager.submit(2, at(100), driver.handle());

        EXPECT_TRUE(manager.credential_wanted())
            << "the retained intent's credential is still needed by the adopt that follows";

        (void)manager.on(disassociated(1), DisconnectCause::Transient, at(200), driver.handle());

        EXPECT_EQ(driver.calls.back(), (Call{Op::Connect, 2})) << "the adopt really did consume it";
        EXPECT_FALSE(manager.credential_wanted());
    }

    {
        FakeDriver driver;
        Manager manager;
        driver.refuse_connect = true;

        (void)manager.submit(1, at(0), driver.handle());
        ASSERT_EQ(manager.state(), State::Stalled);
        EXPECT_FALSE(manager.credential_wanted()) << "a quarantined station asks for nothing again";
    }

    {
        FakeDriver driver;
        Manager manager;

        (void)manager.submit(1, at(0), driver.handle());
        (void)manager.submit(2, at(100), driver.handle());
        (void)manager.on(event_of(EventKind::Stopped, 1), DisconnectCause::Transient, at(200),
                         driver.handle());

        EXPECT_FALSE(manager.credential_wanted()) << "a stopped station forgot both intents";
    }

    {
        FakeDriver driver;
        Manager manager;

        (void)manager.station_absent(at(0));
        EXPECT_FALSE(manager.credential_wanted());
    }
}

// --- The tick's accumulation ---------------------------------------------------

TEST(StationProgressTests, TheFirstTerminalOutcomeOfATickIsTheOneThatStands) {
    Progress into;

    Progress leased;
    leased.evidence = true;
    leased.tag = 1;
    leased.outcome = Outcome::Leased;

    Progress stalled;
    stalled.evidence = true;
    stalled.tag = 1;
    stalled.outcome = Outcome::DriverStalled;

    Progress later;
    later.evidence = true;
    later.tag = 2;
    later.outcome = Outcome::Leased;

    merge(into, leased);
    EXPECT_EQ(into.outcome, Outcome::Leased);

    // A non-terminal outcome is replaced by whatever came after it: the dwell is a level, not an
    // edge.
    merge(into, stalled);
    EXPECT_EQ(into.outcome, Outcome::DriverStalled);

    // ⛔ AND A TERMINAL ONE IS NOT. A burst can contain a failure followed by the replacement
    // intent's ordinary progress, and collapsing to "the last one" would erase the failure a
    // browser is waiting on.
    merge(into, later);
    EXPECT_EQ(into.outcome, Outcome::DriverStalled);
    EXPECT_EQ(into.tag, Tag{1});

    Progress truth;
    Progress changed;
    changed.truth_changed = true;
    merge(truth, changed);
    EXPECT_TRUE(truth.truth_changed);
}

TEST(StationProgressNegativeControlTests, TheTerminalClassificationCanFail) {
    // A classification that called everything terminal, or nothing, would make the merge rule above
    // vacuous. Both directions are asserted.
    EXPECT_TRUE(prd::network::station::terminal(Outcome::Unreachable));
    EXPECT_TRUE(prd::network::station::terminal(Outcome::DriverStalled));
    EXPECT_TRUE(prd::network::station::terminal(Outcome::EvidenceLost));
    EXPECT_FALSE(prd::network::station::terminal(Outcome::Leased));
    EXPECT_FALSE(prd::network::station::terminal(Outcome::LeaseInterrupted));
}

} // namespace
