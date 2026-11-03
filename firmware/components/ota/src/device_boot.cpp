// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include "ota/first_boot.hpp"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wundef"
#include "esp_ota_ops.h"
#pragma GCC diagnostic pop

namespace prd::ota {
namespace {
class DeviceBoot final : public BootPort {
  public:
    BootImage running_image() override {
        const auto* partition = esp_ota_get_running_partition();
        if (partition == nullptr) {
            return BootImage::Unknown;
        }
        esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
        const auto result = esp_ota_get_state_partition(partition, &state);
        if (result == ESP_ERR_NOT_FOUND) {
            // A direct USB installation need not have OTA state metadata.
            return BootImage::Ordinary;
        }
        if (result != ESP_OK) {
            return BootImage::Unknown;
        }
        if (state == ESP_OTA_IMG_PENDING_VERIFY) {
            return BootImage::Pending;
        }
        return state == ESP_OTA_IMG_VALID || state == ESP_OTA_IMG_UNDEFINED ? BootImage::Ordinary
                                                                            : BootImage::Unknown;
    }

    bool confirm_running_image() override {
        // Recheck the actual running partition immediately before the only write.
        return running_image() == BootImage::Pending &&
               esp_ota_mark_app_valid_cancel_rollback() == ESP_OK;
    }
};
} // namespace

BootPort& device_boot_port() {
    static DeviceBoot port;
    return port;
}
} // namespace prd::ota
