# SPDX-FileCopyrightText: 2026 Mykola Fiantsev
# SPDX-License-Identifier: GPL-3.0-only

# Multifunction button input (Architecture Spec §5.6). Owns the GPIO input, its
# interrupt, debouncing and press-duration measurement, and publishes typed
# gestures. Product meaning stays in app_store/app_core.

idf_component_register(
    SRCS
        "src/button.cpp"
        "src/button_event.cpp"
        "src/press_classifier.cpp"
    INCLUDE_DIRS
        "include"
    PRIV_REQUIRES
        board
        diagnostics
        platform
        esp_driver_gpio
)

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)

include(${CMAKE_SOURCE_DIR}/cmake/compiler_policy.cmake)
prd_apply_compiler_policy(${COMPONENT_LIB})

# button.cpp is the ESP-IDF input-adapter TU. ESP-IDF C headers it includes are
# not -Wundef/-Wpedantic clean (undefined macros, anonymous structs, and
# #include_next -- the last cannot be silenced by an in-file diagnostic pragma).
# Relax only these two warnings for only this file, per architecture spec §12.3;
# every other project warning still applies. Applied after the policy so it wins.
#
# press_classifier.cpp and button_event.cpp include no ESP-IDF headers and keep
# the full strict flags. press_classifier.cpp is also compiled by the host test
# suite, which is the point of keeping it clean.
set_source_files_properties(
    "src/button.cpp"
    PROPERTIES
        COMPILE_OPTIONS "-Wno-undef;-Wno-pedantic"
)
