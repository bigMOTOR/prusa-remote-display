// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include "storage/checksum.hpp"

#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

// The integrity checksum shared by every persisted record.
//
// IT MOVED HERE IN MILESTONE 3.3, out of `config`, so the secret record could use the same
// function instead of a second hand-written copy. These tests moved with it — a checksum
// with two callers and no test of its own is how two records quietly stop agreeing about
// what "intact" means.

namespace {

// Pinned against the published CRC-32/ISO-HDLC check value, so a future rewrite of the
// bitwise loop cannot quietly change the polynomial and invalidate every record already on
// a device in the field.
TEST(StorageChecksumTests, TheChecksumIsStandardCrc32) {
    const std::uint8_t check[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};

    EXPECT_EQ(prd::storage::crc32(check, sizeof(check)), 0xCBF43926U);
    EXPECT_EQ(prd::storage::crc32(nullptr, 0), 0U);
}

// The property both record formats depend on: a checksum whose field sits in the middle of
// what it covers has to be computed over two spans, and chaining must equal the one-shot
// call over the concatenation.
TEST(StorageChecksumTests, ChainingTwoSpansEqualsOneCallOverTheConcatenation) {
    const std::vector<std::uint8_t> whole{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};

    for (std::size_t split = 0; split <= whole.size(); ++split) {
        const std::uint32_t chained = prd::storage::crc32_update(
            prd::storage::crc32(whole.data(), split), whole.data() + split, whole.size() - split);

        EXPECT_EQ(chained, prd::storage::crc32(whole.data(), whole.size())) << "split at " << split;
    }
}

TEST(StorageChecksumTests, ASingleFlippedBitChangesTheChecksum) {
    std::vector<std::uint8_t> data{0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x11, 0x22, 0x33};
    const std::uint32_t original = prd::storage::crc32(data.data(), data.size());

    for (std::size_t index = 0; index < data.size(); ++index) {
        for (int bit = 0; bit < 8; ++bit) {
            data[index] ^= static_cast<std::uint8_t>(1U << bit);
            EXPECT_NE(prd::storage::crc32(data.data(), data.size()), original)
                << "byte " << index << " bit " << bit;
            data[index] ^= static_cast<std::uint8_t>(1U << bit);
        }
    }
}

} // namespace
