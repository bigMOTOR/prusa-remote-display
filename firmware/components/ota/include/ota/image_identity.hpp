// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace prd::ota {

// Version 1 wire layout, little endian, at image offset 24 + 8 + 256.
// All character arrays are NUL terminated and zero padded. No native pointers.
inline constexpr std::array<char, 8> kIdentityMagic{'P', 'R', 'D', 'O', 'T', 'A', '0', '1'};
inline constexpr std::uint32_t kMinimumBootloaderSchema = 1;
struct ImageIdentity {
    std::array<char, 8> magic{kIdentityMagic};
    std::uint32_t format{1};
    std::uint32_t min_bootloader_schema{kMinimumBootloaderSchema};
    std::array<char, 32> board{};
    std::array<char, 32> version{};
    std::array<std::uint8_t, 8> reserved{};
};
static_assert(sizeof(ImageIdentity) == 88);
static_assert(offsetof(ImageIdentity, board) == 16);
static_assert(offsetof(ImageIdentity, version) == 48);

} // namespace prd::ota
