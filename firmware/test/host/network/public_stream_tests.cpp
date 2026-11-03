// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include "public_stream.hpp"

#include <gtest/gtest.h>

#include <array>
#include <vector>

namespace prd::network {
namespace {

bool collect(void* context, const std::uint8_t* bytes, const std::size_t size) {
    auto& output = *static_cast<std::vector<std::uint8_t>*>(context);
    output.insert(output.end(), bytes, bytes + size);
    return true;
}

bool refuse(void*, const std::uint8_t*, std::size_t) {
    return false;
}

TEST(PublicStreamTests, StreamsChunksAtTheExactBound) {
    std::vector<std::uint8_t> output;
    PublicStream stream{4, collect, &output};
    const std::array<std::uint8_t, 4> bytes{1, 2, 3, 4};
    EXPECT_EQ(stream.append(bytes.data(), 2), PublicStreamResult::Accepted);
    EXPECT_EQ(stream.append(bytes.data() + 2, 2), PublicStreamResult::Accepted);
    EXPECT_EQ(stream.append(nullptr, 0), PublicStreamResult::Accepted);
    EXPECT_EQ(stream.length(), 4U);
    EXPECT_EQ(output, (std::vector<std::uint8_t>{1, 2, 3, 4}));
}

TEST(PublicStreamTests, OversizedChunkNeverReachesTheSinkOrChangesLength) {
    std::vector<std::uint8_t> output;
    PublicStream stream{3, collect, &output};
    const std::array<std::uint8_t, 4> bytes{1, 2, 3, 4};
    EXPECT_EQ(stream.append(bytes.data(), bytes.size()), PublicStreamResult::TooLarge);
    EXPECT_EQ(stream.length(), 0U);
    EXPECT_TRUE(output.empty());
}

TEST(PublicStreamTests, SinkRefusalAndNullDataAreFailuresWithoutAccounting) {
    const std::uint8_t value = 1;
    PublicStream refused{4, refuse, nullptr};
    EXPECT_EQ(refused.append(&value, 1), PublicStreamResult::SinkRejected);
    EXPECT_EQ(refused.length(), 0U);
    std::vector<std::uint8_t> output;
    PublicStream null_data{4, collect, &output};
    EXPECT_EQ(null_data.append(nullptr, 1), PublicStreamResult::SinkRejected);
    EXPECT_EQ(null_data.length(), 0U);
}

} // namespace
} // namespace prd::network
