// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstdint>

// The two identities every printer request carries, and why both are needed.
//
// ⛔ WHAT WENT WRONG WITHOUT THEM. The 2026-08-23 audit found that a validation verdict was an
// anonymous fact: it said "the printer answered" without saying WHICH candidate it answered for, or
// which worker asked. Two defects followed directly. A new Wi-Fi submission arriving mid-validation
// left the old worker running, and its late verdict could promote or settle the transaction that had
// replaced it (B5). And a worker whose lease had been superseded could still publish, because the
// check and the callback could not be atomic (B4).
//
// Both are closed by making the two identities explicit, immutable for a worker's life, and
// re-checked by the receiver rather than trusted from the sender.

namespace prd::prusalink {

// A worker's lease. Monotonic within a boot, never reused, never zero for a live worker.
using Generation = std::uint32_t;

inline constexpr Generation kNoGeneration = 0;

// Which candidate configuration a worker was launched for.
//
// `profile` is the Wi-Fi profile id the transaction wrote, which is already the project's
// cross-namespace pairing handle — the same value `configuration_observation.cpp` uses to decide
// that a config record and a credential record belong together. Reusing it means the transaction
// identity and the storage identity cannot drift apart.
//
// `attempt` distinguishes two validations of the SAME profile — a retry after a failed probe writes
// no new profile id, so without it a stale verdict from the first attempt would look current to the
// second.
struct Transaction {
    std::uint32_t profile{0};
    std::uint32_t attempt{0};

    [[nodiscard]] bool operator==(const Transaction& other) const {
        return profile == other.profile && attempt == other.attempt;
    }

    [[nodiscard]] bool operator!=(const Transaction& other) const {
        return !(*this == other);
    }

    // A transaction that names no profile identifies nothing and must never match.
    [[nodiscard]] bool valid() const {
        return profile != 0;
    }
};

} // namespace prd::prusalink
