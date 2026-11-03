# SPDX-FileCopyrightText: 2026 Mykola Fiantsev
# SPDX-License-Identifier: GPL-3.0-only

# Deterministic fixture and scenario source (Architecture Spec Milestone 2's "fake
# data source"). Pure: no ESP-IDF header, no clock, no networking, no PrusaLink
# concept. It drives the real store through real typed events and has no other way
# to affect it, which is what lets the PrusaLink adapter replace it in Milestone 4
# without the store, the reducer or any scene noticing.

idf_component_register(
    SRCS
        "src/observations.cpp"
        "src/fixtures.cpp"
        "src/scenarios.cpp"
        "src/source.cpp"
    INCLUDE_DIRS
        "include"
    REQUIRES
        app_model
        app_store
)

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)

include(${CMAKE_SOURCE_DIR}/cmake/compiler_policy.cmake)
prd_apply_compiler_policy(${COMPONENT_LIB})
