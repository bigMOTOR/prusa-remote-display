// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstdint>

// Runtime heap measurement (Bootstrap Spec §6.6 — narrow framework-service
// wrappers; Architecture Spec §21 memory budget). No ESP-IDF type appears here.
//
// This reports the state of the allocator, not the capacity of the hardware:
// installed flash and PSRAM size remain board's to report (decision D3).

namespace prd::platform {

struct HeapReport {
    // Internal SRAM. This is the scarce region: task stacks, DMA buffers and
    // ESP-IDF itself all come from it, so it is the number a memory budget is
    // actually spent against.
    std::uint32_t free_internal_bytes{0};

    // Lowest free_internal_bytes has ever been since boot. A single instantaneous
    // reading cannot distinguish a comfortable margin from one that has already
    // been spent and released.
    std::uint32_t minimum_free_internal_bytes{0};

    // Largest single internal allocation that would currently succeed. Free total
    // minus this is fragmentation, which is what actually fails an allocation.
    std::uint32_t largest_free_internal_block_bytes{0};

    // DMA-capable internal SRAM is a narrower pool than all internal SRAM.
    // Wi-Fi fixed buffers may fail here even when the totals above look healthy.
    std::uint32_t free_dma_internal_bytes{0};
    std::uint32_t minimum_free_dma_internal_bytes{0};
    std::uint32_t largest_free_dma_internal_block_bytes{0};

    std::uint32_t free_psram_bytes{0};
    std::uint32_t largest_free_psram_block_bytes{0};
};

[[nodiscard]] HeapReport heap_report();

} // namespace prd::platform
