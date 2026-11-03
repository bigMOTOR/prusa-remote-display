// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only
#include "ota/image_identity.hpp"
#include "app_model/generated/build_info_generated.hpp"

namespace {
// Preserve the generated string's compile-time extent to reject truncation.
template <std::size_t N>
// NOLINTNEXTLINE(modernize-avoid-c-arrays)
constexpr std::array<char, 32> identity_text(const char (&text)[N]) noexcept {
    static_assert(N > 1 && N <= 32, "OTA identity must fit without truncation");
    std::array<char, 32> result{};
    for (std::size_t i = 0; i < N; ++i) {
        result[i] = text[i];
    }
    return result;
}
} // namespace

extern "C" {
extern const prd::ota::ImageIdentity prd_image_identity
#ifdef ESP_PLATFORM
    __attribute__((section(".rodata_custom_desc"), used))
#endif
    = {prd::ota::kIdentityMagic,
       1,
       prd::ota::kMinimumBootloaderSchema,
       identity_text(prd::app_model::generated::kBoardTarget),
       identity_text(prd::app_model::generated::kProjectVersion),
       {}};
}
