// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <cstddef>
#include <cstdint>

using esp_err_t = int;
using esp_ota_handle_t = std::uint32_t;
inline constexpr esp_err_t ESP_OK = 0;
inline constexpr esp_err_t ESP_ERR_OTA_VALIDATE_FAILED = 0x1503;
inline constexpr unsigned ESP_PARTITION_TYPE_APP = 0;
inline constexpr unsigned ESP_PARTITION_SUBTYPE_APP_OTA_0 = 0x10;
inline constexpr unsigned ESP_PARTITION_SUBTYPE_APP_OTA_15 = 0x1f;
inline constexpr std::size_t OTA_WITH_SEQUENTIAL_WRITES = 0xfffffffe;
struct esp_partition_t {
    unsigned type;
    unsigned subtype;
    std::uint32_t address;
    std::uint32_t size;
};
const esp_partition_t* esp_ota_get_running_partition();
const esp_partition_t* esp_ota_get_next_update_partition(const esp_partition_t*);
esp_err_t esp_ota_begin(const esp_partition_t*, std::size_t, esp_ota_handle_t*);
esp_err_t esp_ota_write(esp_ota_handle_t, const void*, std::size_t);
esp_err_t esp_ota_abort(esp_ota_handle_t);
esp_err_t esp_ota_end(esp_ota_handle_t);
esp_err_t esp_ota_set_boot_partition(const esp_partition_t*);
esp_err_t esp_partition_read(const esp_partition_t*, std::size_t, void*, std::size_t);
