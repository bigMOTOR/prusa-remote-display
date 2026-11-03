// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include <gtest/gtest.h>

#include "request_budget.hpp"

// §11.4's phase budgets.
//
// ⛔ WHAT THIS PROVES AND WHAT IT DOES NOT. It proves the arithmetic: that connect is given §11.4's
// 2-3 s rather than a 400 ms read slice, that no phase allowance can outlive the absolute deadline,
// and that a spent connect allowance is distinguishable from a refusal. It does NOT prove transport
// behaviour — this suite links no ESP-IDF and opens no socket, so "a printer that answers in 900 ms
// now succeeds" remains target-verification debt. That limitation is stated in the checkpoint's
// verification record rather than papered over here.

namespace prd::network::budget {
namespace {

constexpr std::uint32_t kTotal = 4000;
constexpr std::uint32_t kSlice = 400;
constexpr std::uint32_t kConnect = 2500;

// The production defaults, named once.
constexpr Allowances kDefaults{kTotal, kSlice, kConnect};

[[nodiscard]] Allowances with_total(const std::uint32_t total) {
    return Allowances{total, kSlice, kConnect};
}

TEST(RequestBudgetTests, THE_CONNECT_PHASE_GETS_SECONDS_AND_NOT_A_READ_SLICE) {
    // ⛔ THE DEFECT, AS ONE COMPARISON. esp_http_client_open() is a single call that cannot be
    // re-entered, so whatever it is handed IS the connect timeout. It used to be handed this:
    EXPECT_EQ(slice_timeout_ms(kDefaults, 0), 400U);

    // And it is now handed this — §11.4's "approximately 2-3 s".
    EXPECT_EQ(connect_timeout_ms(kDefaults, 0), 2500U);

    EXPECT_GE(connect_timeout_ms(kDefaults, 0), 2000U);
    EXPECT_LE(connect_timeout_ms(kDefaults, 0), 3000U);
}

TEST(RequestBudgetTests, NO_PHASE_ALLOWANCE_CAN_OUTLIVE_THE_ABSOLUTE_DEADLINE) {
    // ⬅️ THE CLAMP IS THE WHOLE SAFETY ARGUMENT. A connect allowance that exceeded the remaining
    // total would let one phase carry a request past its deadline, which is the property the header
    // and body loops already had and connect did not.
    EXPECT_EQ(connect_timeout_ms(kDefaults, 3000), 1000U);
    EXPECT_EQ(connect_timeout_ms(kDefaults, 3900), 100U);

    // A total smaller than the allowance is bounded by the total, not by the allowance.
    EXPECT_EQ(connect_timeout_ms(with_total(1500), 0), 1500U);

    // And the sum can never exceed the total: whatever connect consumed, what is left for the rest
    // is exactly the remainder.
    for (const std::uint32_t elapsed : {0U, 1U, 399U, 400U, 2499U, 2500U, 3999U}) {
        EXPECT_LE(connect_timeout_ms(kDefaults, elapsed) + elapsed, kTotal) << elapsed;
        EXPECT_LE(slice_timeout_ms(kDefaults, elapsed) + elapsed, kTotal) << elapsed;
    }
}

TEST(RequestBudgetTests, NoTimeoutIsEverZeroBecauseZeroMeansBlockForever) {
    // ⛔ ESP-IDF READS A NON-POSITIVE TIMEOUT AS "BLOCK". A deadline that produced zero would hand
    // the transport an unbounded wait at the exact moment it was trying to stop.
    EXPECT_GE(slice_timeout_ms(kDefaults, kTotal), 1U);
    EXPECT_GE(slice_timeout_ms(kDefaults, kTotal + 5000), 1U);
    EXPECT_GE(connect_timeout_ms(kDefaults, kTotal), 1U);
    EXPECT_GE(connect_timeout_ms(kDefaults, kTotal + 5000), 1U);
    EXPECT_GE(connect_timeout_ms(Allowances{0, 0, 0}, 0), 1U);
}

TEST(RequestBudgetTests, TheDeadlineIsExhaustedAtItsInstantAndNotBefore) {
    EXPECT_FALSE(exhausted(kDefaults, 3999));
    EXPECT_TRUE(exhausted(kDefaults, kTotal));
    EXPECT_TRUE(exhausted(kDefaults, kTotal + 1));

    EXPECT_EQ(remaining_ms(kDefaults, 0), kTotal);
    EXPECT_EQ(remaining_ms(kDefaults, 1500), 2500U);
    EXPECT_EQ(remaining_ms(kDefaults, kTotal), 0U);
    EXPECT_EQ(remaining_ms(kDefaults, kTotal + 1), 0U);
}

TEST(RequestBudgetTests, ASliceShorterThanWhatRemainsIsUsedWholeAndOtherwiseTruncated) {
    EXPECT_EQ(slice_timeout_ms(kDefaults, 0), 400U);
    EXPECT_EQ(slice_timeout_ms(kDefaults, 3700), 300U);

    // A zero slice means "do not slice", so the whole remainder is used — still a bound.
    EXPECT_EQ(slice_timeout_ms(Allowances{kTotal, 0, kConnect}, 0), kTotal);
    EXPECT_EQ(slice_timeout_ms(Allowances{kTotal, 0, kConnect}, 1000), 3000U);
}

TEST(RequestBudgetTests, A_SPENT_CONNECT_ALLOWANCE_IS_A_TIMEOUT_AND_A_FAST_FAILURE_IS_A_REFUSAL) {
    // ⛔ ESP-IDF RETURNS ONE ERROR CODE FOR BOTH. §16.1 keeps "connection refused" and "timeout"
    // distinct, and the clock is the only thing that can separate them: a refusal comes back in
    // milliseconds; a connect that used its whole allowance ran out of time.
    EXPECT_FALSE(connect_spent(kDefaults, 3));
    EXPECT_FALSE(connect_spent(kDefaults, 2499));
    EXPECT_TRUE(connect_spent(kDefaults, 2500));
    EXPECT_TRUE(connect_spent(kDefaults, 2600));

    // ⬅️ AND THE ALLOWANCE IT IS MEASURED AGAINST IS THE CLAMPED ONE. A connect squeezed into 1500 ms
    // by a short total has spent its allowance at 1500 ms, not at 2500 — otherwise a genuine timeout
    // inside a short budget would be reported as a refusal.
    EXPECT_TRUE(connect_spent(with_total(1500), 1500));
    EXPECT_FALSE(connect_spent(with_total(1500), 1499));
}

} // namespace
} // namespace prd::network::budget
