// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include "reset_progress.hpp"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "app_model/app_state.hpp"
#include "app_model/device_data.hpp"
#include "app_model/events.hpp"
#include "app_model/reducer.hpp"

// What a confirmed factory reset does on each tick — Milestone 3.11 finding T16.
//
// ⛔ WHY THIS FILE EXISTS. The erase worked on hardware and the device never restarted. It sat in
// §9's progress presentation for 28 minutes, re-emitting one warning 17,044 times at 10 Hz, until
// its power was pulled. The cause was ordering in the composition root: the tick returned early on
// every tick of a reset in progress, and the restart deadline it had scheduled was compared below
// that return. Nothing about that depends on hardware, and nothing could reach it — app_core.cpp
// has no host build. reset_progress::decide() is that ordering, extracted, and this is the file
// that holds it.
//
// ⬅️ THESE TESTS CALL THE PRODUCTION DECISION. decide() is the same constexpr function
// run_factory_reset() switches on; there is no fake, no copy and no re-statement of the rule here.
// The tick simulation below drives it exactly as the composition root does — one observation per
// tick, the deadline threaded from the previous answer.

namespace {

using prd::app_core::reset_progress::decide;
using prd::app_core::reset_progress::Step;
using prd::app_model::DeviceFaultReason;

// AppState::factory_reset_failed, spelled out at every call so each test still calls the production
// decision directly rather than through a wrapper.
//
// ⛔ IT IS A DEDICATED LIFECYCLE FACT, NOT A DEVICE-FAULT REASON — audit item 5's fifth finding. The
// halt used to read `device_fault == FactoryResetIncomplete`, and that field is a single mutable slot
// several reducer paths assign; the sequence tests at the end of this file are what proved it.
constexpr bool kHealthy = false;
constexpr bool kResetFailed = true;

// The composition root's hold, mirrored so the sequences below run at the real spacing rather than
// at a convenient one. Not a second definition of anything: nothing asserts on its value, and the
// boundary tests below pin the rule to the deadline itself rather than to this number.
constexpr std::int64_t kHoldUs = 2'000'000;
constexpr std::int64_t kTickUs = 100'000; // the observed 10 Hz application tick

// Runs `ticks` observations the way the tick does: ask, and let the answer move the state on.
// Returns every Step in order, so a test can assert on the whole sequence rather than a sample.
std::vector<Step> run_ticks(const int ticks, const std::int64_t start_us = 1'000'000) {
    std::vector<Step> seen;
    std::int64_t now_us = start_us;
    std::int64_t due_us = 0;
    bool resetting = true;

    for (int i = 0; i < ticks; ++i) {
        const Step step = decide(resetting, kHealthy, due_us, now_us);
        seen.push_back(step);

        // Exactly what run_factory_reset() does with each answer.
        if (step == Step::Erase) {
            due_us = now_us + kHoldUs;
        }
        if (step == Step::Restart) {
            resetting = false; // the device restarts; nothing observes another tick
        }

        now_us += kTickUs;
    }

    return seen;
}

int count(const std::vector<Step>& steps, const Step wanted) {
    int n = 0;
    for (const Step step : steps) {
        if (step == wanted) {
            ++n;
        }
    }
    return n;
}

} // namespace

// --- The three answers, one per situation ------------------------------------

TEST(ResetProgressTests, NoResetInProgressDecidesNothing) {
    EXPECT_EQ(decide(false, kHealthy, 0, 5'000'000), Step::Nothing);
}

TEST(ResetProgressTests, TheFirstTickOfAConfirmedResetErases) {
    EXPECT_EQ(decide(true, kHealthy, 0, 5'000'000), Step::Erase);
}

TEST(ResetProgressTests, AHoldStillRunningWaits) {
    // Deadline in the future: the panel is drawing and the tick has nothing to add.
    EXPECT_EQ(decide(true, kHealthy, 7'000'000, 5'000'000), Step::Wait);
}

TEST(ResetProgressTests, AnElapsedHoldRestarts) {
    // ⛔ THE T16 REGRESSION. This is the answer the composition root could never reach.
    EXPECT_EQ(decide(true, kHealthy, 7'000'000, 9'000'000), Step::Restart);
}

// --- The boundary, in both directions ----------------------------------------

TEST(ResetProgressTests, TheExactDeadlineRestarts) {
    EXPECT_EQ(decide(true, kHealthy, 7'000'000, 7'000'000), Step::Restart)
        << "the deadline itself must restart, not wait one more tick";
}

TEST(ResetProgressTests, OneMicrosecondBeforeTheDeadlineStillWaits) {
    EXPECT_EQ(decide(true, kHealthy, 7'000'000, 6'999'999), Step::Wait);
}

// --- A reset that is no longer running ---------------------------------------

TEST(ResetProgressTests, AClearedResetNeverRestartsHoweverStaleTheDeadline) {
    // A deadline left behind must not restart a device whose reset is no longer running —
    // §14.1's "do not erase after a failure" has its counterpart here: do not reboot after one.
    //
    // ⬅️ THIS USED TO DESCRIBE factory_reset_completed CLEARING THE FLAG. That event was deleted in
    // audit item 5: clearing the flag on failure is exactly what released the device while its
    // journal was still armed. The flag now STAYS set and Step::Halted is what stops the executor;
    // see the Halted tests below. The property asserted here is unchanged and still worth having.
    EXPECT_EQ(decide(false, kHealthy, 7'000'000, 9'000'000), Step::Nothing);
    EXPECT_EQ(decide(false, kHealthy, 7'000'000, 7'000'000), Step::Nothing);
    EXPECT_EQ(decide(false, kHealthy, 1, 999'999'999), Step::Nothing);
}

// --- The sequences a real reset produces -------------------------------------

TEST(ResetProgressTests, AResetErasesExactlyOnceAndThenWaitsOutTheHold) {
    // ⛔ THE LOG-SPAM REGRESSION, STATED AS A COUNT. The success line is emitted under Step::Erase,
    // so "erased once" and "logged once" are the same assertion.
    const std::vector<Step> steps = run_ticks(20);

    EXPECT_EQ(count(steps, Step::Erase), 1) << "17,044 lines on hardware came from this being >1";
    EXPECT_EQ(steps.front(), Step::Erase) << "and it must be the first tick, not a later one";
}

TEST(ResetProgressTests, TheResetReachesARestartAndStopsThere) {
    const std::vector<Step> steps = run_ticks(40);

    ASSERT_EQ(count(steps, Step::Restart), 1);

    // Erase, then the hold, then exactly one Restart, then nothing further.
    const auto restart = std::find(steps.begin(), steps.end(), Step::Restart);
    ASSERT_NE(restart, steps.end()) << "a confirmed reset that never restarts is T16";

    for (auto it = steps.begin() + 1; it != restart; ++it) {
        EXPECT_EQ(*it, Step::Wait) << "the hold must be silent, and must not erase again";
    }
    for (auto it = restart + 1; it != steps.end(); ++it) {
        EXPECT_EQ(*it, Step::Nothing);
    }
}

TEST(ResetProgressTests, NoNumberOfTicksProducesASecondEraseOrASecondRestart) {
    // The property behind both halves of T16: however long the device is observed, the destructive
    // step happens once and the reboot happens once.
    for (const int ticks : {1, 2, 5, 21, 100, 1000}) {
        const std::vector<Step> steps = run_ticks(ticks);

        EXPECT_LE(count(steps, Step::Erase), 1) << "erased twice after " << ticks << " ticks";
        EXPECT_LE(count(steps, Step::Restart), 1) << "restarted twice after " << ticks << " ticks";
    }
}

TEST(ResetProgressTests, TheHoldLastsTheWholeHoldAndNotOneTickLess) {
    // 2,000,000 us at a 100,000 us tick: erase on tick 0, then Wait until the deadline is reached
    // on tick 20. Pins the sequence's shape, so a change to the comparison shows up here.
    const std::vector<Step> steps = run_ticks(25);

    EXPECT_EQ(steps.at(0), Step::Erase);
    EXPECT_EQ(steps.at(19), Step::Wait) << "one tick before the deadline";
    EXPECT_EQ(steps.at(20), Step::Restart) << "the tick the deadline is reached on";
    EXPECT_EQ(steps.at(21), Step::Nothing);
}

// --- The clock's own edges ----------------------------------------------------

TEST(ResetProgressTests, ADeadlineIsHonouredAcrossLargeMonotonicValues) {
    // esp_timer_get_time() is microseconds since boot in int64. A device that has been up for
    // weeks must behave identically — the comparison is on signed values with no subtraction, so
    // there is nothing to overflow, and this is the test that says so.
    constexpr std::int64_t weeks = 3LL * 7 * 24 * 60 * 60 * 1'000'000;

    EXPECT_EQ(decide(true, kHealthy, weeks + kHoldUs, weeks), Step::Wait);
    EXPECT_EQ(decide(true, kHealthy, weeks + kHoldUs, weeks + kHoldUs), Step::Restart);
}

// --- Step::Halted: a reset that failed must not erase again -------------------
//
// ⛔ WHAT THIS PROTECTS, AND WHY IT IS NOT COSMETIC. After a failed reset `factory_resetting` stays
// true — that is what holds the exclusive barrier — and `restart_due_us` is still 0, because no hold
// was ever scheduled. Those are exactly the inputs that mean Step::Erase. Without the fault term
// this rule would re-enter the destructive executor on every tick at 10 Hz, which is the T16 shape
// with a destructive body instead of a log line.

TEST(ResetProgressTests, AFailedResetHalts) {
    EXPECT_EQ(decide(true, kResetFailed, 0, 5'000'000), Step::Halted);
}

TEST(ResetProgressTests, A_HALTED_RESET_NEVER_ERASES_AGAIN_HOWEVER_LONG_IT_IS_OBSERVED) {
    // The exact post-failure inputs, driven for as long as anyone would look at the device.
    for (int tick = 0; tick < 2000; ++tick) {
        const std::int64_t now_us = 5'000'000 + (static_cast<std::int64_t>(tick) * kTickUs);

        ASSERT_EQ(decide(true, kResetFailed, 0, now_us), Step::Halted)
            << "tick " << tick << " would have re-run the erase";
    }
}

TEST(ResetProgressTests, NEGATIVE_CONTROL_WITHOUT_THE_FAULT_TERM_THOSE_SAME_INPUTS_ERASE) {
    // ⬅️ THE CONTROL THAT MAKES THE TEST ABOVE MEAN SOMETHING. Identical inputs, healthy fault: the
    // answer is Erase, every tick. That is precisely the 10 Hz loop Halted exists to prevent, and
    // it proves the assertion above is carried by the fault term rather than by anything else.
    for (int tick = 0; tick < 20; ++tick) {
        const std::int64_t now_us = 5'000'000 + (static_cast<std::int64_t>(tick) * kTickUs);

        ASSERT_EQ(decide(true, kHealthy, 0, now_us), Step::Erase);
    }
}

TEST(ResetProgressTests, HALTED_OUTRANKS_A_STALE_DEADLINE_SO_A_FAILED_RESET_NEVER_REBOOTS) {
    // A deadline cannot be left behind on the failure path today, because none is scheduled. Pinned
    // anyway: if one ever were, a halted reset must still not reboot — ADR-0008's no-automatic-
    // reboot rule, asserted rather than assumed.
    EXPECT_EQ(decide(true, kResetFailed, 7'000'000, 9'000'000), Step::Halted);
    EXPECT_EQ(decide(true, kResetFailed, 7'000'000, 7'000'000), Step::Halted);
    EXPECT_EQ(decide(true, kResetFailed, 7'000'000, 1'000'000), Step::Halted);
}

// --- The sequence that proved the latch has to exist -------------------------
//
// ⛔ THE FIFTH FINDING, AS THE EVENT ORDER THAT PRODUCES IT. `device_fault` is a single mutable field
// that five reducer paths assign. The application loop drains every queued event at the top of an
// iteration and only then runs the reset tick, so an observation posted BEFORE or DURING the
// synchronous reset pass is applied before the next tick asks what to do.
//
// ⬅️ AND STATION QUIESCING IS NOT A DEFENCE. quiesce() stops the station issuing NEW driver
// operations; it does not empty the application task's queue of what is already on it.

namespace {

using prd::app_model::AppState;
using prd::app_model::BootPhase;
using prd::app_model::ButtonGesture;
using prd::app_model::Event;
using prd::app_model::NetworkFault;
using prd::app_model::NetworkObservation;
using prd::app_model::NetworkPhase;
using prd::app_model::reduce;
using prd::app_model::Timestamp;

// A device whose confirmed reset has just executed and failed, exactly as app_core leaves it.
[[nodiscard]] AppState reset_failed_device() {
    AppState state;
    state = reduce(state, Event::boot_phase(Timestamp{}, BootPhase::Ready));
    state = reduce(state, Event::button_gesture(Timestamp{1000}, ButtonGesture::VeryLongPress));
    state = reduce(state, Event::factory_reset_confirmed(Timestamp{1500}));
    state = reduce(state,
                   Event::device_fault(Timestamp{2000}, DeviceFaultReason::FactoryResetIncomplete));
    return state;
}

[[nodiscard]] Event queued_driver_fault() {
    NetworkObservation quarantined;
    quarantined.phase = NetworkPhase::Unavailable;
    quarantined.fault = NetworkFault::DriverUnresponsive;
    return Event::network_observed(Timestamp{2100}, quarantined);
}

} // namespace

TEST(ResetHaltLatchTests, A_LATE_QUEUED_NETWORK_FAULT_CANNOT_RE_ENTER_THE_ERASE) {
    AppState state = reset_failed_device();

    ASSERT_TRUE(state.factory_resetting);
    ASSERT_TRUE(state.factory_reset_failed);
    ASSERT_EQ(decide(state.factory_resetting, state.factory_reset_failed, 0, 5'000'000),
              Step::Halted);

    // The event that was already on the queue when the reset failed.
    state = reduce(state, queued_driver_fault());

    EXPECT_TRUE(state.factory_reset_failed) << "the latch must survive any later event";
    EXPECT_EQ(decide(state.factory_resetting, state.factory_reset_failed, 0, 5'100'000),
              Step::Halted)
        << "a late observation re-entered the destructive executor";
}

TEST(ResetHaltLatchTests, THE_PANEL_KEEPS_THE_TRUTHFUL_REASON_TOO) {
    // ⛔ THE SECOND GUARANTEE, AND IT IS A SEPARATE ONE. The latch stops the executor; the precedence
    // rule stops the panel silently swapping "Reset could not finish" for "Wi-Fi unavailable".
    AppState state = reset_failed_device();
    state = reduce(state, queued_driver_fault());

    EXPECT_EQ(state.device_fault, DeviceFaultReason::FactoryResetIncomplete);
    EXPECT_EQ(state.device_operation, prd::app_model::DeviceOperation::DeviceFault);

    // The network observation itself was still applied; only the fault reason is protected.
    EXPECT_EQ(state.network.fault, NetworkFault::DriverUnresponsive);
}

TEST(ResetHaltLatchTests, NO_REAL_FAULT_PRODUCING_EVENT_UN_LATCHES_IT) {
    // Every reducer path that assigns device_fault, driven against a failed reset.
    prd::app_model::HardwareProbeObservation bad_board;
    bad_board.flash_validation = prd::app_model::ValidationState::Failed;

    const Event probes[] = {
        queued_driver_fault(),
        Event::hardware_probed(Timestamp{3000}, bad_board),
        Event::storage_observed(Timestamp{3100}, prd::app_model::StorageStatus::Failed),
        Event::device_fault(Timestamp{3200}, DeviceFaultReason::LowMemory),
        Event::device_fault(Timestamp{3300}, DeviceFaultReason::WifiUnavailable),
    };

    for (const Event& probe : probes) {
        AppState state = reset_failed_device();
        state = reduce(state, probe);

        EXPECT_TRUE(state.factory_reset_failed);
        EXPECT_EQ(decide(state.factory_resetting, state.factory_reset_failed, 0, 5'000'000),
                  Step::Halted)
            << "event " << std::string{prd::app_model::log_label(probe.type)}
            << " re-entered the erase";
        EXPECT_EQ(state.device_fault, DeviceFaultReason::FactoryResetIncomplete)
            << "event " << std::string{prd::app_model::log_label(probe.type)}
            << " replaced the truthful reason";
    }
}

TEST(ResetHaltLatchTests, NEGATIVE_CONTROL_THE_OLD_FAULT_DERIVED_HALT_WOULD_HAVE_ERASED) {
    // ⬅️ THE CONTROL THAT MAKES THE THREE ABOVE MEAN SOMETHING. It reconstructs the REJECTED design —
    // halt derived from `device_fault == FactoryResetIncomplete` — over the same state, and shows it
    // answers Erase. Without this the tests above could pass for a rule that never halted at all.
    AppState state = reset_failed_device();
    state = reduce(state, Event::device_fault(Timestamp{2100}, DeviceFaultReason::WifiUnavailable));

    // The precedence rule now protects the reason, so the rejected design is no longer even
    // reachable through the reducer — construct its input directly.
    const bool old_style_halt =
        (DeviceFaultReason::WifiUnavailable == DeviceFaultReason::FactoryResetIncomplete);
    EXPECT_FALSE(old_style_halt);
    EXPECT_EQ(decide(true, old_style_halt, 0, 5'000'000), Step::Erase)
        << "the rejected design really did re-enter the erase; that is why the latch exists";
}

TEST(ResetHaltLatchTests, AN_ACTUAL_BOOT_CLEARS_THE_LATCH) {
    // ⛔ AND ONLY A BOOT. The panel tells the user to restart, so the restart must be what ends it.
    AppState state = reset_failed_device();
    state = reduce(state, queued_driver_fault());
    ASSERT_TRUE(state.factory_reset_failed);

    state = reduce(state, Event::boot_phase(Timestamp{4000}, BootPhase::Starting));

    EXPECT_FALSE(state.factory_reset_failed);
    EXPECT_FALSE(state.factory_resetting);
    EXPECT_EQ(state.device_fault, DeviceFaultReason::None);
    EXPECT_EQ(decide(state.factory_resetting, state.factory_reset_failed, 0, 5'000'000),
              Step::Nothing);
}

TEST(ResetHaltLatchTests, AN_UNRELATED_FAULT_DOES_NOT_HALT_A_HEALTHY_RESET) {
    // ⛔ THE DISCRIMINATING CONTROL, MOVED TO WHERE IT NOW BELONGS. A Wi-Fi quarantine raised while a
    // confirmed reset is MID-FLIGHT must not silently abandon a destructive operation the user asked
    // for and is watching — the latch is set only by the reset's own failure.
    AppState state;
    state = reduce(state, Event::boot_phase(Timestamp{}, BootPhase::Ready));
    state = reduce(state, Event::button_gesture(Timestamp{1000}, ButtonGesture::VeryLongPress));
    state = reduce(state, Event::factory_reset_confirmed(Timestamp{1500}));
    state = reduce(state, queued_driver_fault());

    EXPECT_FALSE(state.factory_reset_failed);
    EXPECT_EQ(decide(state.factory_resetting, state.factory_reset_failed, 0, 5'000'000),
              Step::Erase);
}

TEST(ResetProgressTests, A_HALT_STILL_REQUIRES_A_RESET_TO_BE_RUNNING) {
    // The fault alone means nothing here: with no reset in progress there is no tick to halt, and
    // this function must keep answering Nothing rather than claiming an operation exists.
    EXPECT_EQ(decide(false, kResetFailed, 0, 5'000'000), Step::Nothing);
}
