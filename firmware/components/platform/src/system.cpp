// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include "platform/system.hpp"

// This TU is an ESP-IDF adapter boundary. The ESP-IDF C headers are not clean
// under the project's strict flags (undefined macros -> -Wundef; #include_next
// -> -Wpedantic, which no in-file pragma can silence). Both warnings are
// relaxed for this single file in CMakeLists.txt per architecture spec §12.3;
// all other project warnings still apply here.
#include "esp_system.h"

namespace prd::platform {

void restart() {
    esp_restart();
}

} // namespace prd::platform
