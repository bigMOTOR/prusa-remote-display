// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstdint>

// What a confirmed factory reset does on the tick it is observed — Milestone 3.11 T16.
//
// INTERNAL HEADER of the app_core component. Pure — no ESP-IDF, no clock, no store, no panel —
// so the rule below is host-tested against the same code the firmware runs.
//
// ⛔ WHY THIS IS A UNIT AND NOT THREE STATEMENTS IN THE TICK. A confirmed reset is observed on
// EVERY tick until the device restarts, because `factory_resetting` deliberately stays true so
// §9's progress presentation keeps drawing. That makes "which tick is this" the whole problem, and
// three distinct answers have to come out of one observation:
//
//   1. the first tick erases;
//   2. the ticks during the hold do nothing, and say nothing;
//   3. the tick after the hold elapses restarts.
//
// Milestone 3.11 T16 found the composition root answering (1) and (2) and never reaching (3). The
// tick returned early whenever `factory_resetting` was set, and the restart deadline it had itself
// scheduled was compared twenty-two lines below that return — so on a real board the erase
// completed in 31 ms and the device then sat in the progress presentation for 28 minutes until its
// power was pulled, having emitted the same warning 17,044 times at 10 Hz.
//
// ⬅️ BOTH SYMPTOMS WERE ONE DEFECT, AND THE SPAM WAS THE CHEAPER HALF. The success log sat above
// the idempotence guard rather than under the decision, so "this tick erases" and "this tick is
// waiting" produced the same line. Answering once, here, is what stops either recurring: a caller
// that re-derives the answer locally is the shape that failed.
//
// ⛔ THIS UNIT DECIDES NOTHING ABOUT WHETHER A RESET MAY HAPPEN. Consent, the confirmation window
// and the erase inventory are settled before a tick ever sees `resetting` — ADR-0008 decisions 7
// and 8, in the reducer and in factory_reset. This is only the sequencing that follows.

namespace prd::app_core::reset_progress {

enum class Step : std::uint8_t {
    // No reset is in progress. Nothing to do, nothing to log.
    Nothing,

    // The first tick of a confirmed reset: perform the erase. Reached exactly once per reset,
    // which is what makes the success log a single line rather than a stream.
    Erase,

    // The erase is done and §9's hold is still running. The panel is drawing; the tick is silent.
    Wait,

    // The hold has elapsed. Restart into first-boot behaviour — the honest way for a state that
    // has erased everything it described to end.
    Restart,

    // ⛔ THE RESET HAS ALREADY EXECUTED AND FAILED. Do nothing, say nothing, and above all DO NOT
    // ERASE AGAIN — audit item 5, 2026-08-28.
    //
    // `factory_resetting` deliberately stays true after a failure, because it is still true: a reset
    // owns this device and did not finish, and that is what keeps the exclusive barrier held. But
    // `restart_due_us` is still 0 on that path, so without this answer every subsequent tick would
    // read Step::Erase and re-run the executor at the loop's 10 Hz — the T16 shape again, now with a
    // destructive body. There is no automatic retry and no automatic reboot in this product; the
    // recovery is an explicit restart.
    Halted,
};

// `restart_due_us == 0` means the erase has not run yet; it is the only "no deadline" value the
// composition root can hold, because a scheduled deadline is a monotonic timestamp plus a
// positive hold.
//
// Signed microseconds throughout, matching esp_timer_get_time(). The comparison is `>=` so the
// deadline itself restarts rather than waiting one more tick.
// ⛔ `reset_failed` IS AppState::factory_reset_failed — A DEDICATED TERMINAL LIFECYCLE FACT, AND NOT
// A DEVICE-FAULT REASON. That distinction is audit item 5's fifth finding, and it is a safety
// property rather than a tidiness one.
//
// This parameter used to be a DeviceFaultReason, and the halt read `== FactoryResetIncomplete`. But
// `device_fault` is a single mutable slot that five reducer paths assign, and the application loop
// drains every queued event before the next reset tick — so a NetworkObserved already posted when the
// reset failed reached that drain, replaced the reason, and the very next tick answered Step::Erase.
// Measured against the real reducer: a DriverUnresponsive observation, a failed hardware probe, a
// StorageObserved(Failed) and a LowMemory DeviceFaultRaised each un-latched it.
//
// ⬅️ THE LATCH IS SET WHEN THE FAULT IS RAISED AND CLEARED ONLY BY BootPhase::Starting, so no later
// event of any kind can reach the destructive executor before the device actually reboots. The
// panel's truthfulness is a SEPARATE guarantee, carried by app_model::fault_holds_device()'s
// precedence rule. Deriving one from the other is exactly what failed.
[[nodiscard]] constexpr Step decide(const bool resetting, const bool reset_failed,
                                    const std::int64_t restart_due_us, const std::int64_t now_us) {
    if (!resetting) {
        return Step::Nothing;
    }

    // ⛔ CHECKED BEFORE THE DEADLINE, SO A HALTED RESET CAN NEITHER ERASE NOR REBOOT.
    if (reset_failed) {
        return Step::Halted;
    }

    if (restart_due_us == 0) {
        return Step::Erase;
    }

    return now_us >= restart_due_us ? Step::Restart : Step::Wait;
}

} // namespace prd::app_core::reset_progress
