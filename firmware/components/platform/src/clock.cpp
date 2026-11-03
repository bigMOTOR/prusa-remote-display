// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include "platform/clock.hpp"

// This TU is an ESP-IDF adapter boundary. The ESP-IDF C headers are not clean
// under the project's strict flags (undefined macros -> -Wundef; #include_next
// -> -Wpedantic, which no in-file pragma can silence). Both warnings are
// relaxed for this single file in CMakeLists.txt per architecture spec §12.3;
// all other project warnings still apply here.
#include "esp_attr.h"
#include "esp_timer.h"

namespace prd::platform {

// IRAM-resident, because the button's edge interrupt calls it and that interrupt is
// allocated with ESP_INTR_FLAG_IRAM so it keeps firing while the flash cache is
// disabled by an NVS or OTA write. A flash-resident function called from that
// context faults, and the fault would present as an unexplained crash during a
// write rather than as anything to do with the button.
//
// esp_timer_get_time() is itself IRAM-resident by default (CONFIG_ESP_TIMER_IN_IRAM
// defaults to y), so the whole path is covered. Cost: a few dozen bytes of IRAM.
IRAM_ATTR std::chrono::microseconds monotonic_time() {
    return std::chrono::microseconds{esp_timer_get_time()};
}

} // namespace prd::platform
