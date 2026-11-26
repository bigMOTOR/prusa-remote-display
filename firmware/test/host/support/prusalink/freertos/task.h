// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "FreeRTOS.h"

inline BaseType_t xTaskCreate(void (*task)(void*), const char*, std::uint32_t, void* context,
                              UBaseType_t, void*) {
    task(context);
    return pdPASS;
}

inline void vTaskDelete(void*) {}
inline void vTaskDelay(TickType_t) {}
