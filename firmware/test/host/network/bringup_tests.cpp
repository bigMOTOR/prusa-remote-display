// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include "bringup.hpp"

#include "lifecycle.hpp"

#include <string>
#include <vector>

#include <gtest/gtest.h>

// The station bring-up ordering rule — Milestone 3.11.
//
// ⛔ WHAT THESE TESTS ARE FOR. A configured device stopped reaching its network after its first
// reboot, and the cause was an ordering one: event handlers were registered before the shared
// stack that carries the event loop existed. It was invisible on the only path anyone had
// physically exercised, because first-time provisioning raises a setup access point, and that
// brings the stack up before any station starts. Both boot shapes are therefore asserted here,
// separately, because passing one proves nothing about the other — which is exactly how this
// reached hardware.
//
// ⬅️ WHAT THEY DO AND DO NOT COVER, STATED RATHER THAN IMPLIED. wifi.cpp and radio.cpp name
// ESP-IDF types and cannot be compiled on the host (Bootstrap Spec §11.3), so what is under test
// is the pure rule the adapter delegates to — bringup::ensure_events_once — together with the
// sequence the adapter must express. That the adapter really calls them in this order is what
// Milestone 3.11's physical T1 establishes; these tests are what stop the order being rewritten
// afterwards.
//
// ⬅️ AND THE ATTEMPT IS NO LONGER ARMED — checkpoint 3c. StartupArming existed because
// `Trigger::Started` had two producers and either could arrive first. The intent machine issues the
// attempt because an INTENT exists rather than because an event arrived, so the last step below is
// unconditional: a successful start is followed by an attempt, full stop.

namespace {

using prd::network::bringup::ensure_events_once;

// Records what the sequence touched, in the order it touched it, so a reordering fails a test
// rather than passing one.
struct Recorder {
    std::vector<std::string> calls;

    // Whether the shared stack already exists. True is the first-time-provisioning shape: the
    // setup access point brought it up before any station existed.
    bool stack_exists{false};

    bool stack_fails{false};
    bool registration_fails{false};

    int stack_creations{0};
    int registrations{0};

    bool ensure_stack() {
        calls.emplace_back("ensure_stack");

        if (stack_fails) {
            return false;
        }

        // Idempotent, exactly as radio::ensure_stack() is: consulted every time, creating
        // only when there is nothing there yet.
        if (!stack_exists) {
            stack_exists = true;
            ++stack_creations;
        }

        return true;
    }

    bool register_handlers() {
        calls.emplace_back("register_handlers");

        if (registration_fails) {
            return false;
        }

        ++registrations;
        return true;
    }
};

// The order network::start() must express, with the driver calls stubbed. Returns whether a
// station attempt was reached.
bool run_station_bring_up(Recorder& recorder, bool& events_latched,
                          const bool prepare_succeeds = true, const bool start_succeeds = true) {
    if (!ensure_events_once(
            events_latched, [&recorder] { return recorder.ensure_stack(); },
            [&recorder] { return recorder.register_handlers(); })) {
        return false;
    }

    // radio::prepare() — esp_wifi_set_mode(), which is what can post STA_START from inside the
    // call. It must never run before the registration above.
    recorder.calls.emplace_back("prepare");
    if (!prepare_succeeds) {
        return false;
    }

    recorder.calls.emplace_back("start");
    if (!start_succeeds) {
        return false;
    }

    // The first attempt, which must not depend on catching an event.
    recorder.calls.emplace_back("attempt");
    return true;
}

// --- Shape 1: configured cold boot, no access point ever raised ---------------

TEST(StationBringUpTests, ConfiguredColdBootCreatesTheStackBeforeRegisteringHandlers) {
    // The exact shape that failed on hardware: nothing has run before this, so the stack does
    // not exist and the event loop it carries does not either.
    Recorder recorder;
    bool latched = false;

    EXPECT_TRUE(ensure_events_once(
        latched, [&recorder] { return recorder.ensure_stack(); },
        [&recorder] { return recorder.register_handlers(); }));

    EXPECT_EQ(recorder.calls, (std::vector<std::string>{"ensure_stack", "register_handlers"}))
        << "the stack must exist before anything registers on its event loop";
    EXPECT_EQ(recorder.stack_creations, 1);
    EXPECT_EQ(recorder.registrations, 1);
    EXPECT_TRUE(latched);
}

TEST(StationBringUpTests, ConfiguredColdBootReachesAStationAttempt) {
    // §15.1 steps 13 and 18 end in an attempt, and this is the boot that produced none: the
    // registration failed, start() returned false, and the device published
    // `wifi station did not start` with no driver ever initialised.
    Recorder recorder;
    bool latched = false;
    EXPECT_TRUE(run_station_bring_up(recorder, latched));

    EXPECT_EQ(recorder.calls, (std::vector<std::string>{"ensure_stack", "register_handlers",
                                                        "prepare", "start", "attempt"}));
}

// --- Shape 2: first-time provisioning, the access point got there first -------

TEST(StationBringUpTests, FirstTimeProvisioningReusesTheAccessPointsStack) {
    // setup_ap::start() called radio::prepare() first, so the stack and its event loop already
    // exist. The station must still consult the stack step and must still register BEFORE the
    // mode change — this is the shape the Milestone 3.8 fix was written for, and it must not
    // regress now that the stack step precedes it.
    Recorder recorder;
    recorder.stack_exists = true;
    bool latched = false;
    EXPECT_TRUE(run_station_bring_up(recorder, latched));

    EXPECT_EQ(recorder.calls, (std::vector<std::string>{"ensure_stack", "register_handlers",
                                                        "prepare", "start", "attempt"}));
    EXPECT_EQ(recorder.stack_creations, 0) << "nothing may be created a second time";
    EXPECT_EQ(recorder.registrations, 1);
}

TEST(StationBringUpTests, ASecondStationStartRegistersNothingFurther) {
    // Idempotence, and it is not cosmetic: esp_event_handler_instance_register creates an
    // INSTANCE per call, so a second registration would deliver every event twice and each
    // WIFI_EVENT_STA_DISCONNECTED would be counted twice against the retry limit.
    Recorder recorder;
    bool latched = false;

    EXPECT_TRUE(ensure_events_once(
        latched, [&recorder] { return recorder.ensure_stack(); },
        [&recorder] { return recorder.register_handlers(); }));
    EXPECT_TRUE(ensure_events_once(
        latched, [&recorder] { return recorder.ensure_stack(); },
        [&recorder] { return recorder.register_handlers(); }));

    EXPECT_EQ(recorder.registrations, 1);
    EXPECT_EQ(recorder.calls, (std::vector<std::string>{"ensure_stack", "register_handlers"}))
        << "a latched sequence touches neither operation again";
}

TEST(StationBringUpTests, AnAccessPointStillRunningDoesNotChangeTheOrder) {
    // The APSTA case: prepare() will start the station interface immediately and post STA_START
    // from inside itself. Registration still precedes it, so the event cannot be missed.
    //
    // ⬅️ AND CATCHING IT NO LONGER MATTERS TO THE ATTEMPT — checkpoint 3c. STA_START is
    // informational: the attempt is issued because an intent exists. What still matters, and is
    // asserted here, is that the handler is registered before anything can post to it, because a
    // record posted while the handler is absent is a record the event ring never sees and therefore
    // a sequence hole the consumer would report as lost evidence.
    Recorder recorder;
    recorder.stack_exists = true;
    bool latched = false;

    EXPECT_TRUE(ensure_events_once(
        latched, [&recorder] { return recorder.ensure_stack(); },
        [&recorder] { return recorder.register_handlers(); }));

    EXPECT_EQ(recorder.calls.front(), "ensure_stack");
    EXPECT_EQ(recorder.calls.back(), "register_handlers");
    EXPECT_EQ(recorder.registrations, 1);
}

// --- The defect itself, as a regression ---------------------------------------

TEST(StationBringUpTests, AFailedStackBringUpNeverRegistersHandlers) {
    // ⛔ THE REGRESSION FOR MILESTONE 3.11 T1. Registration on a stack that is not there is the
    // failure, and it must not be attempted at all — not attempted and reported, not attempted
    // and retried.
    Recorder recorder;
    recorder.stack_fails = true;
    bool latched = false;

    EXPECT_FALSE(ensure_events_once(
        latched, [&recorder] { return recorder.ensure_stack(); },
        [&recorder] { return recorder.register_handlers(); }));

    EXPECT_EQ(recorder.calls, (std::vector<std::string>{"ensure_stack"}));
    EXPECT_EQ(recorder.registrations, 0);
    EXPECT_FALSE(latched) << "a failure must leave the sequence retryable, not recorded as done";
}

TEST(StationBringUpTests, AFailedRegistrationLeavesTheSequenceRetryable) {
    // Nothing latches on a partial run, so a later attempt runs the whole sequence again —
    // and the stack, being idempotent, is not created twice on the way through.
    Recorder recorder;
    recorder.registration_fails = true;
    bool latched = false;

    EXPECT_FALSE(ensure_events_once(
        latched, [&recorder] { return recorder.ensure_stack(); },
        [&recorder] { return recorder.register_handlers(); }));
    EXPECT_FALSE(latched);

    recorder.registration_fails = false;

    EXPECT_TRUE(ensure_events_once(
        latched, [&recorder] { return recorder.ensure_stack(); },
        [&recorder] { return recorder.register_handlers(); }));

    EXPECT_TRUE(latched);
    EXPECT_EQ(recorder.registrations, 1);
    EXPECT_EQ(recorder.stack_creations, 1) << "the retry must not create a second stack";
    EXPECT_EQ(recorder.calls, (std::vector<std::string>{"ensure_stack", "register_handlers",
                                                        "ensure_stack", "register_handlers"}));
}

} // namespace
