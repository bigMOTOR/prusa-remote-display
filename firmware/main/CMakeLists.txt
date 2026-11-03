# SPDX-FileCopyrightText: 2026 Mykola Fiantsev
# SPDX-License-Identifier: GPL-3.0-only

idf_component_register(
    SRCS
        "app_main.cpp"
    INCLUDE_DIRS
        "."
    REQUIRES
        app_core
        diagnostics
)

target_compile_features(${COMPONENT_LIB} PRIVATE cxx_std_17)
