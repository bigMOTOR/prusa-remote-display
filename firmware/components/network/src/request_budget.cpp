// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include "request_budget.hpp"

namespace prd::network::budget {
namespace {

// The smallest timeout ESP-IDF still treats as bounded. Zero or negative means "block", which is
// the one thing a deadline cannot allow.
constexpr std::uint32_t kMinimumTimeoutMs = 1;

// One phase allowance, clamped to what is left of the absolute deadline. The clamp is the whole
// safety argument: a phase can be made shorter than its allowance, never longer than the request.
[[nodiscard]] std::uint32_t bounded(const std::uint32_t remaining, const std::uint32_t allowance) {
    if (remaining == 0) {
        return kMinimumTimeoutMs;
    }

    // A zero allowance means "no separate allowance for this phase", so what remains is used — still
    // a bound, and still shorter than blocking.
    const std::uint32_t wanted = allowance == 0 ? remaining : allowance;

    return wanted < remaining ? wanted : remaining;
}

} // namespace

bool exhausted(const Allowances& allowances, const std::uint32_t elapsed_ms) {
    return elapsed_ms >= allowances.total_ms;
}

std::uint32_t remaining_ms(const Allowances& allowances, const std::uint32_t elapsed_ms) {
    return exhausted(allowances, elapsed_ms) ? 0U : allowances.total_ms - elapsed_ms;
}

std::uint32_t slice_timeout_ms(const Allowances& allowances, const std::uint32_t elapsed_ms) {
    return bounded(remaining_ms(allowances, elapsed_ms), allowances.slice_ms);
}

std::uint32_t connect_timeout_ms(const Allowances& allowances, const std::uint32_t elapsed_ms) {
    return bounded(remaining_ms(allowances, elapsed_ms), allowances.connect_ms);
}

bool connect_spent(const Allowances& allowances, const std::uint32_t elapsed_ms) {
    // Compared against the allowance the connect was ACTUALLY given, clamp included: a connect
    // squeezed into 200 ms by an almost-spent total has spent its allowance at 200 ms, not at 2500.
    return elapsed_ms >= connect_timeout_ms(allowances, 0);
}

} // namespace prd::network::budget
