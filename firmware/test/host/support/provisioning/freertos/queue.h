// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#pragma once
#include <cstddef>
using QueueHandle_t = void*;
QueueHandle_t xQueueCreate(std::size_t, std::size_t);
int xQueuePeek(QueueHandle_t, void*, unsigned);
int xQueueOverwrite(QueueHandle_t, const void*);
