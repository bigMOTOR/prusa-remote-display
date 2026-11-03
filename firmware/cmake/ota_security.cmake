# SPDX-FileCopyrightText: 2026 Mykola Fiantsev
# SPDX-License-Identifier: GPL-3.0-only

# Check effective Kconfig, not merely defaults: an old sdkconfig can override
# changed defaults. Never silently deliver a build without OTA verification or
# one that enables irreversible security provisioning/automatic key access.
foreach(option IN ITEMS
        CONFIG_SECURE_SIGNED_APPS_NO_SECURE_BOOT
        CONFIG_SECURE_SIGNED_APPS_RSA_SCHEME
        CONFIG_SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT
        CONFIG_SECURE_SIGNED_ON_UPDATE
        CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE)
    if(NOT ${option})
        message(FATAL_ERROR "OTA policy requires ${option}. Regenerate the build configuration from project defaults; do not bypass this guard.")
    endif()
endforeach()

foreach(option IN ITEMS
        CONFIG_SECURE_BOOT
        CONFIG_SECURE_BOOT_BUILD_SIGNED_BINARIES
        CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK
        CONFIG_SECURE_FLASH_ENC_ENABLED
        CONFIG_NVS_ENCRYPTION)
    if(${option})
        message(FATAL_ERROR "OTA policy prohibits ${option}: no hardware lock, automatic signing or eFuse provisioning.")
    endif()
endforeach()
