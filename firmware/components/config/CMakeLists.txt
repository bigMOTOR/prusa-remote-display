# SPDX-FileCopyrightText: 2026 Mykola Fiantsev
# SPDX-License-Identifier: GPL-3.0-only

# Validated user configuration (Architecture Spec §5.9). Owns ConfigV1, its persisted
# record format, the §13.3 staged write and the §13.4 migration dispatch.
#
# NO ESP-IDF ADAPTER TU EXISTS HERE, and that is the design rather than an omission: the
# component talks only to storage::KeyValue, so every translation unit is standard C++17
# and the whole component is compiled and tested by the host suite. Nothing below needs
# the -Wno-undef relaxation the ESP-IDF adapter components require.

idf_component_register(
    SRCS
        "src/config.cpp"
        "src/schema.cpp"
    INCLUDE_DIRS
        "include"
    REQUIRES
        app_model
        storage
)

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)

include(${CMAKE_SOURCE_DIR}/cmake/compiler_policy.cmake)
prd_apply_compiler_policy(${COMPONENT_LIB})
