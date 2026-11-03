# SPDX-FileCopyrightText: 2026 Mykola Fiantsev
# SPDX-License-Identifier: GPL-3.0-only

cmake_minimum_required(VERSION 3.16)

if(DEFINED ENV{PRD_VERSION} AND NOT "$ENV{PRD_VERSION}" STREQUAL "")
    set(PROJECT_VER "$ENV{PRD_VERSION}")
else()
    file(READ "${CMAKE_CURRENT_LIST_DIR}/version.txt" PROJECT_VER)
    string(STRIP "${PROJECT_VER}" PROJECT_VER)
endif()

# Materialise the checksum-pinned backport before Component Manager resolves override_path.
# Generated sources live under ignored build/, never managed_components or a shared cache.
find_package(Python3 REQUIRED COMPONENTS Interpreter)
execute_process(
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/tools/prepare-lvgl.py"
    RESULT_VARIABLE PRD_LVGL_PREPARE_RESULT
)
if(NOT PRD_LVGL_PREPARE_RESULT EQUAL 0)
    message(FATAL_ERROR "Pinned LVGL backport preparation failed")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Component Manager serialises local overrides as absolute paths. Keep that resolved lock
# generated per build; the committed registry lock remains the portable upstream pin set.
file(SHA256 "${CMAKE_CURRENT_LIST_DIR}/dependencies.lock" PRD_UPSTREAM_LOCK_HASH)
set(PRD_RESOLVED_LOCK "${CMAKE_BINARY_DIR}/dependencies.lock")
set(PRD_LOCK_STAMP "${CMAKE_BINARY_DIR}/upstream-lock.sha256")
set(PRD_PREVIOUS_LOCK_HASH "")
if(EXISTS "${PRD_LOCK_STAMP}")
    file(READ "${PRD_LOCK_STAMP}" PRD_PREVIOUS_LOCK_HASH)
endif()
if(NOT EXISTS "${PRD_RESOLVED_LOCK}" OR NOT PRD_UPSTREAM_LOCK_HASH STREQUAL PRD_PREVIOUS_LOCK_HASH)
    configure_file("${CMAKE_CURRENT_LIST_DIR}/dependencies.lock" "${PRD_RESOLVED_LOCK}" COPYONLY)
    file(WRITE "${PRD_LOCK_STAMP}" "${PRD_UPSTREAM_LOCK_HASH}")
endif()
idf_build_set_property(DEPENDENCIES_LOCK "${PRD_RESOLVED_LOCK}")

project(prusa_remote_display)

# The pinned LVGL builtin allocator calls LV_MEM_POOL_ALLOC only while setting
# up its one fixed pool. On the PSRAM board the UI reserves that pool before
# lv_init(); no managed component source or global IDF BSS placement is changed.
if(CONFIG_PRD_BOARD_WAVESHARE_ESP32_S3_LCD_2_8)
    idf_component_get_property(PRD_LVGL_LIB lvgl__lvgl COMPONENT_LIB)
    target_compile_options(${PRD_LVGL_LIB} PRIVATE
        -include "${CMAKE_CURRENT_LIST_DIR}/cmake/lvgl_psram_pool_override.h")
endif()

include(${CMAKE_CURRENT_LIST_DIR}/cmake/ota_security.cmake)

idf_build_get_property(PRD_IDF_VERSION IDF_VER)

if(NOT PRD_IDF_VERSION STREQUAL "v5.5.4")
    message(FATAL_ERROR
        "Prusa Remote Display requires ESP-IDF v5.5.4; "
        "found ${PRD_IDF_VERSION}"
    )
endif()

message(STATUS "Prusa Remote Display ESP-IDF version: ${PRD_IDF_VERSION}")

include(${CMAKE_CURRENT_LIST_DIR}/cmake/build_metadata.cmake)
prd_collect_build_metadata("${PROJECT_VER}")

# The generated header needs a C++ bool literal; keep the CMake boolean intact.
if(PRD_GIT_DIRTY)
    set(PRD_GIT_DIRTY_CPP "true")
else()
    set(PRD_GIT_DIRTY_CPP "false")
endif()

configure_file(
    "${CMAKE_CURRENT_LIST_DIR}/components/app_model/include/app_model/generated/build_info_generated.hpp.in"
    "${CMAKE_BINARY_DIR}/prd_generated/app_model/generated/build_info_generated.hpp"
    @ONLY
)

message(STATUS
    "Prusa Remote Display build metadata: version=${PROJECT_VER} "
    "git=${PRD_GIT_COMMIT} describe=${PRD_GIT_DESCRIBE} dirty=${PRD_GIT_DIRTY} "
    "profile=${PRD_BUILD_PROFILE} board=${PRD_BOARD_TARGET} tag=${PRD_RELEASE_TAG}")
