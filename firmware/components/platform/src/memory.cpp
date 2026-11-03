// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include "platform/memory.hpp"

// ESP-IDF headers are not -Wundef clean. Suppress locally (per architecture spec
// §12.3) so the project -Wundef policy still applies to our own code.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wundef"
#include "esp_heap_caps.h"
#pragma GCC diagnostic pop

namespace prd::platform {

HeapReport heap_report() {
    HeapReport report{};

    // MALLOC_CAP_INTERNAL rather than the default heap: with
    // CONFIG_SPIRAM_USE_MALLOC enabled, a total-free figure mixes 8 MB of PSRAM
    // into the number and hides pressure on the region that actually runs out.
    report.free_internal_bytes =
        static_cast<std::uint32_t>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    report.minimum_free_internal_bytes =
        static_cast<std::uint32_t>(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
    report.largest_free_internal_block_bytes =
        static_cast<std::uint32_t>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));

    constexpr std::uint32_t kDmaInternalCaps = MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL;
    report.free_dma_internal_bytes =
        static_cast<std::uint32_t>(heap_caps_get_free_size(kDmaInternalCaps));
    report.minimum_free_dma_internal_bytes =
        static_cast<std::uint32_t>(heap_caps_get_minimum_free_size(kDmaInternalCaps));
    report.largest_free_dma_internal_block_bytes =
        static_cast<std::uint32_t>(heap_caps_get_largest_free_block(kDmaInternalCaps));

    report.free_psram_bytes =
        static_cast<std::uint32_t>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    report.largest_free_psram_block_bytes =
        static_cast<std::uint32_t>(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));

    return report;
}

} // namespace prd::platform
