// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include "storage/checksum.hpp"

namespace prd::storage {

std::uint32_t crc32_update(const std::uint32_t crc, const std::uint8_t* const data,
                           const std::size_t length) {
    std::uint32_t state = ~crc;

    for (std::size_t index = 0; index < length; ++index) {
        state ^= data[index];

        for (int bit = 0; bit < 8; ++bit) {
            const auto mask = static_cast<std::uint32_t>(-static_cast<std::int32_t>(state & 1U));
            state = (state >> 1U) ^ (0xEDB88320U & mask);
        }
    }

    return ~state;
}

std::uint32_t crc32(const std::uint8_t* const data, const std::size_t length) {
    return crc32_update(0, data, length);
}

} // namespace prd::storage
