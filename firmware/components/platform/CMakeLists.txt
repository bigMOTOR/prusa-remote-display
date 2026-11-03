# SPDX-FileCopyrightText: 2026 Mykola Fiantsev
# SPDX-License-Identifier: GPL-3.0-only

# Narrow framework-service wrappers (Bootstrap Spec §6.6): monotonic clock,
# system restart, and runtime task-stack and heap measurement. Runtime hardware
# reports (chip, flash, PSRAM capacity, reset reason) stay in the board component
# — this reports allocator state, not installed hardware. Not a generic RTOS
# abstraction layer.

idf_component_register(
    SRCS
        "src/clock.cpp"
        "src/memory.cpp"
        "src/system.cpp"
        "src/task_stack.cpp"
    INCLUDE_DIRS
        "include"
    PRIV_REQUIRES
        esp_timer
        esp_system
)

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)

include(${CMAKE_SOURCE_DIR}/cmake/compiler_policy.cmake)
prd_apply_compiler_policy(${COMPONENT_LIB})

# Both TUs are ESP-IDF adapter boundaries. ESP-IDF C headers they include are
# not -Wundef/-Wpedantic clean (undefined macros and #include_next -- the
# latter cannot be silenced by an in-file diagnostic pragma). Relax only these
# two warnings for only these files, per architecture spec §12.3; every other
# project warning still applies. Applied after the policy so it wins.
set_source_files_properties(
    "src/clock.cpp"
    "src/memory.cpp"
    "src/system.cpp"
    "src/task_stack.cpp"
    PROPERTIES
        COMPILE_OPTIONS "-Wno-undef;-Wno-pedantic"
)
