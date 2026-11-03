# SPDX-FileCopyrightText: 2026 Mykola Fiantsev
# SPDX-License-Identifier: GPL-3.0-only

# Display hardware interface (Architecture Spec §5.5). Owns esp_lcd, panel
# initialisation and the panel transform. No other component may call esp_lcd.

set(PRD_DISPLAY_SRCS
    "src/display.cpp"
    "src/backlight.cpp"
    "src/brightness_curve.cpp"
    "src/transfer_tracker.cpp"
)

# The validation patterns are diagnostics, not product code: they are compiled
# only when CONFIG_PRD_DISPLAY_TEST_PATTERNS is set, which
# sdkconfig.defaults.hardware-validation does (Bootstrap Spec §23.3 —
# build-time profile differences, kept in one place rather than scattered
# #ifdefs).
#
# Driven by Kconfig rather than an environment variable on purpose: the
# setting is then visible in the generated sdkconfig and cannot silently
# vanish from a build invoked without the wrapper script.
if(CONFIG_PRD_DISPLAY_TEST_PATTERNS)
    list(APPEND PRD_DISPLAY_SRCS "src/test_patterns.cpp")
endif()

# Panel tuning tables are validation-time investigation material (Phase C1).
# They compile in every profile because display.cpp references the mode enum,
# but only the Generic path is reachable unless a tuning mode is selected.
list(APPEND PRD_DISPLAY_SRCS "src/panel_tuning.cpp")

idf_component_register(
    SRCS
        ${PRD_DISPLAY_SRCS}
    INCLUDE_DIRS
        "include"
    PRIV_REQUIRES
        board
        diagnostics
        esp_lcd
        esp_driver_spi
        esp_driver_ledc
)

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)

# CONFIG_* symbols are already visible to every component, so the orchestrating
# component guards its call site on CONFIG_PRD_DISPLAY_TEST_PATTERNS directly.

include(${CMAKE_SOURCE_DIR}/cmake/compiler_policy.cmake)
prd_apply_compiler_policy(${COMPONENT_LIB})

# display.cpp and backlight.cpp are the ESP-IDF adapter TUs (esp_lcd and LEDC
# respectively). ESP-IDF C headers they include are not -Wundef/-Wpedantic clean
# (undefined macros, anonymous structs, and #include_next -- the last cannot be
# silenced by an in-file diagnostic pragma). Relax only these two warnings for
# only these files, per architecture spec §12.3; every other project warning
# still applies. Applied after the policy so it wins.
#
# brightness_curve.cpp and test_patterns.cpp include no ESP-IDF headers and keep
# the full strict flags.
set_source_files_properties(
    "src/display.cpp"
    "src/backlight.cpp"
    PROPERTIES
        COMPILE_OPTIONS "-Wno-undef;-Wno-pedantic"
)
