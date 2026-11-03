# SPDX-FileCopyrightText: 2026 Mykola Fiantsev
# SPDX-License-Identifier: GPL-3.0-only

# Created here so the directory exists when include paths are validated;
# the root CMakeLists generates build_info_generated.hpp into it.
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/prd_generated")

idf_component_register(
    SRCS
        "src/board_identity.cpp"
        "src/build_info.cpp"
        "src/build_info_current.cpp"
        "src/hardware_check.cpp"
        "src/observation.cpp"
        "src/values.cpp"
        "src/state_layers.cpp"
        "src/printer_data.cpp"
        "src/device_data.cpp"
        "src/app_state.cpp"
        "src/events.cpp"
        "src/reducer.cpp"
        "src/presentation.cpp"
    "src/display_policy.cpp"
    INCLUDE_DIRS
        "include"
        "${CMAKE_BINARY_DIR}/prd_generated"
)

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)

include(${CMAKE_SOURCE_DIR}/cmake/compiler_policy.cmake)
prd_apply_compiler_policy(${COMPONENT_LIB})
