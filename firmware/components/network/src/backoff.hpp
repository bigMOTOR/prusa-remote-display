// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <array>
#include <chrono>
#include <cstdint>

// The reconnect schedule — Architecture Spec §11.3.
//
// INTERNAL HEADER of the network component. Pure — no ESP-IDF, no clock, no randomness of
// its own — so the whole schedule is host-tested against the same code the firmware runs.
//
// THE NUMBERS ARE THE SOURCE'S, NOT A CHOICE. §11.3 gives the ladder verbatim: "bounded
// exponential backoff with jitter ... 2 s → 4 s → 8 s → 15 s → 30 s maximum". The one
// figure it does not give is the jitter magnitude, which is recorded below as an
// implementation value rather than a product decision.

namespace prd::network {

// §11.3's ladder. Attempt numbers are 1-based; anything past the end holds at the maximum,
// which is what "30 s maximum" means.
inline constexpr std::array<std::chrono::milliseconds, 5> kSchedule{
    std::chrono::milliseconds{2000},  std::chrono::milliseconds{4000},
    std::chrono::milliseconds{8000},  std::chrono::milliseconds{15000},
    std::chrono::milliseconds{30000},
};

// ±12.5 % of the step, and this is an IMPLEMENTATION VALUE — owner ruling, 2026-08-15 —
// not an accepted product figure.
//
// Jitter exists so that several devices on one network do not retry in lockstep after a
// router restart. An eighth either way is wide enough to break that synchronisation and
// narrow enough that the schedule still reads as 2/4/8/15/30 in a log, which matters when
// somebody is holding the ladder next to a serial capture.
inline constexpr std::uint32_t kJitterFractionDenominator = 8;

// The random byte the jitter is drawn from.
//
// A TYPE RATHER THAN A BARE INTEGER, so it cannot be swapped with the attempt number: both
// are small unsigned values, and delay_for(jitter, attempt) would compile and be wrong in a
// way no test would obviously catch. It also names the one thing about the parameter that
// matters — it is entropy, not a duration.
struct Jitter {
    std::uint8_t value{0};
};

// The delay before `attempt`, jittered.
//
// The jitter comes from the caller, not from a generator here: this file stays pure, and a
// test that could not choose the jitter could only assert a range. The adapter supplies a
// real random byte.
[[nodiscard]] std::chrono::milliseconds delay_for(std::uint32_t attempt, Jitter jitter);

// Whether the ladder has reached its maximum step.
//
// This is what decides when a network stops being "reconnecting" and starts being
// "unavailable": retries continue at the capped delay either way (§16.2 retries transient
// Wi-Fi loss automatically), but a device that has been failing for the whole ladder should
// say so rather than claim it is still in the middle of connecting.
[[nodiscard]] bool schedule_saturated(std::uint32_t attempt);

} // namespace prd::network
