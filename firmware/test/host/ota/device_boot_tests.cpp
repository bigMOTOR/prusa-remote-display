// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include "ota/first_boot.hpp"
#include "support/ota/esp_ota_ops.h"
#include <gtest/gtest.h>

namespace {
esp_partition_t partition;
bool partition_present = true;
esp_err_t read_result = ESP_OK;
esp_err_t write_result = ESP_OK;
esp_ota_img_states_t image_state = ESP_OTA_IMG_PENDING_VERIFY;
int writes = 0;
int reads = 0;
class OtaDeviceBoot : public testing::Test {
    void SetUp() override {
        partition_present = true;
        read_result = ESP_OK;
        write_result = ESP_OK;
        image_state = ESP_OTA_IMG_PENDING_VERIFY;
        writes = 0;
        reads = 0;
    }
};
} // namespace

const esp_partition_t* esp_ota_get_running_partition() {
    return partition_present ? &partition : nullptr;
}
esp_err_t esp_ota_get_state_partition(const esp_partition_t* part, esp_ota_img_states_t* state) {
    EXPECT_EQ(part, &partition);
    ++reads;
    *state = image_state;
    return read_result;
}
esp_err_t esp_ota_mark_app_valid_cancel_rollback() {
    ++writes;
    return write_result;
}

namespace {
using namespace prd::ota;
TEST_F(OtaDeviceBoot, OnlyPendingStateMayBeConfirmed) {
    auto& port = device_boot_port();
    for (auto state : {ESP_OTA_IMG_NEW, ESP_OTA_IMG_VALID, ESP_OTA_IMG_INVALID, ESP_OTA_IMG_ABORTED,
                       ESP_OTA_IMG_UNDEFINED}) {
        image_state = state;
        EXPECT_NE(port.running_image(), BootImage::Pending);
        EXPECT_FALSE(port.confirm_running_image());
    }
    EXPECT_EQ(writes, 0);
    image_state = ESP_OTA_IMG_PENDING_VERIFY;
    EXPECT_EQ(port.running_image(), BootImage::Pending);
    EXPECT_TRUE(port.confirm_running_image());
    EXPECT_EQ(writes, 1);
}
TEST_F(OtaDeviceBoot, MissingPartitionOrFailedStateReadNeverWrites) {
    auto& port = device_boot_port();
    partition_present = false;
    EXPECT_EQ(port.running_image(), BootImage::Unknown);
    EXPECT_FALSE(port.confirm_running_image());
    EXPECT_EQ(reads, 0);
    partition_present = true;
    for (auto result : {ESP_FAIL, ESP_ERR_NOT_SUPPORTED, ESP_ERR_NOT_FOUND}) {
        read_result = result;
        EXPECT_NE(port.running_image(), BootImage::Pending);
        EXPECT_FALSE(port.confirm_running_image());
    }
    EXPECT_EQ(writes, 0);
}
TEST_F(OtaDeviceBoot, StateIsRecheckedAtConfirmationAndWriteFailurePropagates) {
    auto& port = device_boot_port();
    EXPECT_EQ(port.running_image(), BootImage::Pending);
    image_state = ESP_OTA_IMG_VALID;
    EXPECT_FALSE(port.confirm_running_image());
    EXPECT_EQ(reads, 2);
    EXPECT_EQ(writes, 0);
    image_state = ESP_OTA_IMG_PENDING_VERIFY;
    write_result = ESP_FAIL;
    EXPECT_FALSE(port.confirm_running_image());
    EXPECT_EQ(writes, 1);
}
TEST_F(OtaDeviceBoot, RealAdapterAndTimerRequireTheFullWindow) {
    FirstBoot check;
    auto& port = device_boot_port();
    check.begin(port);
    check.tick(port, 1000, {true, true, true, false});
    check.tick(port, 30999, {true, true, true, false});
    EXPECT_EQ(writes, 0);
    check.tick(port, 31000, {true, true, true, false});
    EXPECT_EQ(writes, 1);
    EXPECT_EQ(reads, 2);
    EXPECT_EQ(check.health(), BootHealth::Confirmed);
}
} // namespace
