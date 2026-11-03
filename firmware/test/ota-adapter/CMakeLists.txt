# SPDX-FileCopyrightText: 2026 Mykola Fiantsev
# SPDX-License-Identifier: GPL-3.0-only
cmake_minimum_required(VERSION 3.20)
project(prd_ota_adapter_tests LANGUAGES C CXX)
find_package(Threads REQUIRED)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT DEFINED IDF_PATH)
    message(FATAL_ERROR "Pass the existing pinned ESP-IDF path; this target never downloads it")
endif()
set(MBEDTLS "${IDF_PATH}/components/mbedtls/mbedtls")
set(OTA "${CMAKE_CURRENT_SOURCE_DIR}/../../components/ota")
set(MODEL "${CMAKE_CURRENT_SOURCE_DIR}/../../components/app_model")
add_executable(ota_adapter_tests
    adapter_tests.cpp
    "${OTA}/src/device_ingestion.cpp" "${OTA}/src/ingestion.cpp"
    "${OTA}/src/image_identity.cpp"
    "${OTA}/src/installation.cpp" "${OTA}/src/manifest.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../components/json_reader/src/reader.cpp"
    "${OTA}/src/compatibility.cpp" "${MODEL}/src/build_info.cpp"
    "${MBEDTLS}/library/sha256.c" "${MBEDTLS}/library/platform_util.c")
target_include_directories(ota_adapter_tests PRIVATE
    support . "${OTA}/include" "${MODEL}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../components/json_reader/include"
    "${MBEDTLS}/include" "${MBEDTLS}/library")
target_compile_definitions(ota_adapter_tests PRIVATE MBEDTLS_CONFIG_FILE="sha_config.h")
# Redirect only the adapter's calls; wrappers still execute the real pinned SHA.
set_source_files_properties("${OTA}/src/device_ingestion.cpp" PROPERTIES COMPILE_DEFINITIONS
    "mbedtls_sha256_starts=prd_test_sha_starts;mbedtls_sha256_update=prd_test_sha_update;mbedtls_sha256_finish=prd_test_sha_finish")
target_compile_options(ota_adapter_tests PRIVATE -Wall -Wextra -Werror)
target_link_libraries(ota_adapter_tests PRIVATE Threads::Threads)
option(PRD_SANITIZE "Enable native address/undefined sanitizers" ON)
if(PRD_SANITIZE)
    target_compile_options(ota_adapter_tests PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(ota_adapter_tests PRIVATE -fsanitize=address,undefined)
endif()
enable_testing()
add_test(NAME ota_adapter_tests COMMAND ota_adapter_tests)
add_test(NAME ota_adapter_uncertain_activation COMMAND ota_adapter_tests --uncertain)
set_tests_properties(ota_adapter_tests ota_adapter_uncertain_activation PROPERTIES TIMEOUT 30
    ENVIRONMENT "ASAN_OPTIONS=halt_on_error=1:abort_on_error=1;UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1")
