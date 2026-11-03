// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include "sdkconfig.h"

#include "lvgl_pool.hpp"

#include <cstddef>
#include <cstdlib>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wundef"
#include "esp_heap_caps.h"
#include "esp_memory_utils.h"
#include "lvgl.h"
#pragma GCC diagnostic pop

namespace {

void* g_pool = nullptr;

} // namespace

namespace prd::ui::pool {

bool reserve() {
    if (g_pool != nullptr) {
        return true;
    }

    void* const candidate = heap_caps_malloc(LV_MEM_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (candidate == nullptr) {
        return false;
    }
    if (!esp_ptr_external_ram(candidate)) {
        heap_caps_free(candidate);
        return false;
    }
    g_pool = candidate;
    return true;
}

} // namespace prd::ui::pool

extern "C" void* prd_lvgl_pool_alloc(const std::size_t bytes) {
    // The override is used only by the pinned builtin allocator, once during
    // lv_init(). A null or mismatched pool must not reach LVGL's TLSF setup.
    if (g_pool == nullptr || bytes != LV_MEM_SIZE) {
        std::abort();
    }
    return g_pool;
}
