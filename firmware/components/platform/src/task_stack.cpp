// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include "platform/task_stack.hpp"

// This TU is an ESP-IDF adapter boundary. The ESP-IDF C headers are not clean
// under the project's strict flags (undefined macros -> -Wundef; #include_next
// -> -Wpedantic, which no in-file pragma can silence). Both warnings are
// relaxed for this single file in CMakeLists.txt per architecture spec §12.3;
// all other project warnings still apply here.
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace prd::platform {

std::uint32_t task_stack_free_low_water_bytes() {
    // ESP-IDF deliberately deviates from upstream FreeRTOS here: its
    // uxTaskGetStackHighWaterMark returns BYTES, where standard FreeRTOS returns
    // a count of StackType_t words. Its own header calls the difference out. No
    // scaling belongs here — multiplying by sizeof(StackType_t) would report four
    // times the free stack that actually exists, and every stack sized from that
    // measurement would be too small.
    return static_cast<std::uint32_t>(uxTaskGetStackHighWaterMark(nullptr));
}

} // namespace prd::platform
