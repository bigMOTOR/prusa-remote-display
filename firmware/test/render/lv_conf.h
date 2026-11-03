// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only
// Host renderer configuration: target64KiB pool plus a CLIB/redzone comparison.
#pragma once
#define LV_CONF_H /* the guard lv_conf_internal.h checks for */

#define LV_COLOR_DEPTH 16

#if PRD_ASAN_MODE == 1
#define LV_USE_STDLIB_MALLOC LV_STDLIB_BUILTIN
#define LV_MEM_SIZE (64 * 1024U)  /* CONFIG_LV_MEM_SIZE_KILOBYTES=64 */
#define LV_MEM_POOL_EXPAND_SIZE 0 /* CONFIG_LV_MEM_POOL_EXPAND_SIZE_KILOBYTES=0 */
#define LV_MEM_ADR 0              /* CONFIG_LV_MEM_ADR=0x0 -> the static work_mem_int array */
#else
#define LV_USE_STDLIB_MALLOC LV_STDLIB_CLIB
#endif

#define LV_USE_STDLIB_STRING LV_STDLIB_BUILTIN  /* CONFIG_LV_USE_BUILTIN_STRING=y */
#define LV_USE_STDLIB_SPRINTF LV_STDLIB_BUILTIN /* CONFIG_LV_USE_BUILTIN_SPRINTF=y */

#define LV_USE_OS LV_OS_NONE  /* CONFIG_LV_OS_NONE=y */
#define LV_DEF_REFR_PERIOD 33 /* CONFIG_LV_DEF_REFR_PERIOD=33 */

/* Draw-buffer geometry */
#define LV_DRAW_BUF_STRIDE_ALIGN 1
#define LV_DRAW_BUF_ALIGN 4
#define LV_DRAW_LAYER_SIMPLE_BUF_SIZE (24 * 1024)
#define LV_DRAW_LAYER_MAX_MEMORY 0

/* Software renderer — the mask/cache configuration the fault is suspected in */
#define LV_USE_DRAW_SW 1
#define LV_DRAW_SW_SUPPORT_RGB565 1
#define LV_DRAW_SW_SUPPORT_RGB565_SWAPPED 1
#define LV_DRAW_SW_SUPPORT_RGB565A8 1
#define LV_DRAW_SW_SUPPORT_RGB888 1
#define LV_DRAW_SW_SUPPORT_XRGB8888 1
#define LV_DRAW_SW_SUPPORT_ARGB8888 1
#define LV_DRAW_SW_SUPPORT_ARGB8888_PREMULTIPLIED 1
#define LV_DRAW_SW_SUPPORT_L8 1
#define LV_DRAW_SW_SUPPORT_AL88 1
#define LV_DRAW_SW_SUPPORT_A8 1
#define LV_DRAW_SW_SUPPORT_I1 1
#define LV_DRAW_SW_I1_LUM_THRESHOLD 127
#define LV_DRAW_SW_DRAW_UNIT_CNT 1
#define LV_DRAW_SW_COMPLEX 1
#define LV_DRAW_SW_SHADOW_CACHE_SIZE 0
#define LV_DRAW_SW_CIRCLE_CACHE_SIZE 4
#define LV_DRAW_SW_ASM LV_DRAW_SW_ASM_NONE
#define LV_COLOR_MIX_ROUND_OFS 128

#define LV_CACHE_DEF_SIZE 0
#define LV_IMAGE_HEADER_CACHE_DEF_CNT 0

/* Diagnostics ON, exactly as the flashed image had them: an allocation failure or an
   internal complaint must be visible rather than swallowed. */
#define LV_USE_LOG 1
#define LV_LOG_LEVEL LV_LOG_LEVEL_WARN
#define LV_LOG_USE_TIMESTAMP 1
#define LV_LOG_USE_FILE_LINE 1
#define LV_USE_ASSERT_NULL 1
#define LV_USE_ASSERT_MALLOC 1
/* HARNESS-ONLY INSTRUMENTATION, and it is a deliberate difference from the product.
   The product configuration sets no CONFIG_LV_ASSERT_HANDLER, so LVGL's own default —
   `while(1);` (lv_conf_internal.h:1495) — applies on the target: an LVGL allocation failure
   HALTS the calling task. A diagnostic harness must be able to attribute that instead of
   hanging, so here the handler aborts and the log is routed to stderr. */
#define LV_ASSERT_HANDLER_INCLUDE <stdlib.h>
#define LV_ASSERT_HANDLER abort();
#define LV_LOG_PRINTF 1

/* Widgets the acknowledgement and the operational composition use */
#define LV_USE_ARC 1
#define LV_USE_LABEL 1
#define LV_USE_IMAGE 1
#define LV_USE_LINE 1
#define LV_USE_CANVAS 1
#define LV_USE_QRCODE 1

/* A default font is required by lv_conf_internal; the product's own faces are linked
   separately and are what every label actually uses. */
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_DEFAULT &lv_font_montserrat_14
