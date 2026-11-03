# SPDX-FileCopyrightText: 2026 Mykola Fiantsev
# SPDX-License-Identifier: GPL-3.0-only

idf_component_register(
    SRCS
        "src/log.cpp"
        "src/redaction.cpp"
        "src/boot_summary.cpp"
        "src/task_report.cpp"
    INCLUDE_DIRS
        "include"
    REQUIRES
        app_model
    PRIV_REQUIRES
        # task_report asks platform for the stack high-water mark. Private, so no
        # consumer of diagnostics inherits a platform dependency.
        platform
)

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)

include(${CMAKE_SOURCE_DIR}/cmake/compiler_policy.cmake)
prd_apply_compiler_policy(${COMPONENT_LIB})
