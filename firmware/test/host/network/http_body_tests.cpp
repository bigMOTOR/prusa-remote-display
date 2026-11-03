// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include <array>
#include <string>

#include <gtest/gtest.h>

#include "network/http.hpp"

// The bounded response body — Milestone 4's first vertical slice.
//
// ⛔ WHAT THIS PROTECTS. §16.4: "every network body and queue must be bounded", on a
// device with no virtual memory and a printer on the other end of the socket. The bound
// lives in this small type precisely so it can be proved without a network — the
// transport around it is a thin adapter, and an adapter cannot be made to deliver a
// 64 kB body on demand from a healthy bench printer.

namespace prd::network {
namespace {

TEST(BodyBufferTests, AccumulatesChunksInOrder) {
    std::array<char, 16> storage{};
    BodyBuffer buffer{storage.data(), storage.size()};

    EXPECT_TRUE(buffer.append("{\"a\":"));
    EXPECT_TRUE(buffer.append("1}"));

    EXPECT_EQ(buffer.view(), "{\"a\":1}");
    EXPECT_EQ(buffer.length(), 7U);
    EXPECT_FALSE(buffer.overflowed());
}

TEST(BodyBufferTests, FillsExactlyToCapacity) {
    std::array<char, 8> storage{};
    BodyBuffer buffer{storage.data(), storage.size()};

    EXPECT_TRUE(buffer.append("12345678"));
    EXPECT_EQ(buffer.length(), 8U);
    EXPECT_FALSE(buffer.overflowed());
    EXPECT_EQ(buffer.view(), "12345678");
}

TEST(BodyBufferTests, RefusesOneByteBeyondCapacity) {
    // The boundary is the whole point: an off-by-one here is either a refused valid
    // response or a one-byte overrun.
    std::array<char, 8> storage{};
    BodyBuffer buffer{storage.data(), storage.size()};

    EXPECT_FALSE(buffer.append("123456789"));
    EXPECT_TRUE(buffer.overflowed());

    // Nothing was written from the refused chunk. A partial body must never reach a
    // decoder (§9.3), so refusing the chunk whole is what keeps a truncated response from
    // looking like a short one.
    EXPECT_EQ(buffer.length(), 0U);
    EXPECT_TRUE(buffer.view().empty());
}

TEST(BodyBufferTests, RefusesTheChunkThatWouldStraddleTheLimit) {
    std::array<char, 8> storage{};
    BodyBuffer buffer{storage.data(), storage.size()};

    ASSERT_TRUE(buffer.append("12345"));
    EXPECT_FALSE(buffer.append("6789"));

    // The accepted prefix survives; the straddling chunk contributes nothing.
    EXPECT_EQ(buffer.length(), 5U);
    EXPECT_EQ(buffer.view(), "12345");
    EXPECT_TRUE(buffer.overflowed());
}

TEST(BodyBufferTests, OverflowLatches) {
    // A body that was too large stays too large. Without the latch, a large chunk followed
    // by a small one would end with overflowed() false and a body silently missing its
    // middle — which would decode, and be wrong.
    std::array<char, 8> storage{};
    BodyBuffer buffer{storage.data(), storage.size()};

    ASSERT_FALSE(buffer.append("123456789"));
    EXPECT_FALSE(buffer.append("x"));
    EXPECT_TRUE(buffer.overflowed());
    EXPECT_EQ(buffer.length(), 0U);
}

TEST(BodyBufferTests, ResetClearsTheOverflowAndTheContent) {
    // The worker reuses one buffer for every cycle, so a previous oversized response must
    // not fail the next one.
    std::array<char, 4> storage{};
    BodyBuffer buffer{storage.data(), storage.size()};

    ASSERT_FALSE(buffer.append("toolong"));
    ASSERT_TRUE(buffer.overflowed());

    buffer.reset();

    EXPECT_FALSE(buffer.overflowed());
    EXPECT_EQ(buffer.length(), 0U);
    EXPECT_TRUE(buffer.append("ok"));
    EXPECT_EQ(buffer.view(), "ok");
}

TEST(BodyBufferTests, AnEmptyChunkIsNotAnOverflow) {
    // esp_http_client can hand back a zero-length read; treating that as a failure would
    // turn every complete response into a transport error.
    std::array<char, 4> storage{};
    BodyBuffer buffer{storage.data(), storage.size()};

    EXPECT_TRUE(buffer.append(""));
    EXPECT_FALSE(buffer.overflowed());

    ASSERT_TRUE(buffer.append("abcd"));
    EXPECT_TRUE(buffer.append(""));
    EXPECT_FALSE(buffer.overflowed());
    EXPECT_EQ(buffer.length(), 4U);
}

TEST(BodyBufferTests, ARefusedBufferWithNoStorageFailsSafely) {
    BodyBuffer buffer{nullptr, 0};

    EXPECT_FALSE(buffer.append("x"));
    EXPECT_TRUE(buffer.overflowed());
    EXPECT_EQ(buffer.length(), 0U);
}

TEST(BodyBufferTests, TheProductionBoundHoldsARealisticResponseAndRefusesAFlood) {
    // The worker's own bound. A CORE One status response is a few hundred bytes; the
    // margin is for optional objects a future firmware adds. This pins that the bound is
    // generous enough to be usable and small enough to be a bound.
    constexpr std::size_t kWorkerBound = 2048;
    std::array<char, kWorkerBound> storage{};
    BodyBuffer buffer{storage.data(), storage.size()};

    const std::string realistic =
        R"({"printer":{"state":"PRINTING","temp_nozzle":215.0,"target_nozzle":215.0,)"
        R"("temp_bed":60.0,"target_bed":60.0,"axis_z":10.5,"flow":100,"speed":100},)"
        R"("job":{"id":42,"progress":37.5,"time_remaining":1800,"time_printing":1200}})";

    EXPECT_TRUE(buffer.append(realistic));
    EXPECT_LT(buffer.length(), kWorkerBound / 2);

    buffer.reset();
    EXPECT_FALSE(buffer.append(std::string(kWorkerBound + 1, 'x')));
    EXPECT_TRUE(buffer.overflowed());
}

} // namespace
} // namespace prd::network
