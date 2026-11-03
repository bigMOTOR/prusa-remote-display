// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

// Forced into the pinned LVGL component only on the PSRAM board. LVGL keeps its
// builtin TLSF allocator and fixed 64 KB accounting, but receives a pool that
// the UI task has already reserved in external RAM before lv_init().
#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif
void* prd_lvgl_pool_alloc(size_t bytes);
#ifdef __cplusplus
}
#endif

#define LV_MEM_POOL_ALLOC(bytes) prd_lvgl_pool_alloc(bytes)
