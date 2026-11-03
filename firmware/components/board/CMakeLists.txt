# SPDX-FileCopyrightText: 2026 Mykola Fiantsev
# SPDX-License-Identifier: GPL-3.0-only

# Board target selection. The compile-time choice lives in this component's
# Kconfig (Bootstrap Spec §6.4; architecture spec §12.3); with a single
# supported reference target the directory is set unconditionally, which also
# keeps ESP-IDF's early requirement-expansion pass (no Kconfig values yet)
# well-formed. Add CONFIG_PRD_BOARD_* branches when a second target appears.
set(PRD_BOARD_TARGET_DIR "targets/waveshare_esp32_s3_lcd_2_8")

idf_component_register(
    SRCS
        "src/board.cpp"
        "${PRD_BOARD_TARGET_DIR}/board_target.cpp"
    INCLUDE_DIRS
        "include"
    PRIV_INCLUDE_DIRS
        "${PRD_BOARD_TARGET_DIR}"
    REQUIRES
        app_model
    PRIV_REQUIRES
        esp_psram
        spi_flash
)

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)

include(${CMAKE_SOURCE_DIR}/cmake/compiler_policy.cmake)
prd_apply_compiler_policy(${COMPONENT_LIB})

# board.cpp is the ESP-IDF hardware-adapter TU. ESP-IDF C headers it includes
# are not -Wundef/-Wpedantic clean (undefined macros, anonymous structs, and
# #include_next -- the last cannot be silenced by an in-file diagnostic pragma).
# Relax only these two warnings for only this file, per architecture spec §12.3;
# every other project warning still applies. Applied after the policy so it wins.
# board_target.cpp includes no ESP-IDF headers and keeps the full strict flags.
set_source_files_properties(
    "src/board.cpp"
    PROPERTIES
        COMPILE_OPTIONS "-Wno-undef;-Wno-pedantic"
)
