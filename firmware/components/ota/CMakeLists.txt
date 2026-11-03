# SPDX-FileCopyrightText: 2026 Mykola Fiantsev
# SPDX-License-Identifier: GPL-3.0-only

idf_component_register(SRCS "src/compatibility.cpp" "src/first_boot.cpp" "src/device_boot.cpp"
    "src/ingestion.cpp" "src/device_ingestion.cpp" "src/image_identity.cpp" "src/manifest.cpp"
    "src/installation.cpp"
    "src/release_discovery.cpp"
    "src/release_client.cpp"
    "src/device_release_client.cpp"
    "src/release_policy.cpp"
    "src/release_schedule.cpp"
    "src/release_bundle.cpp"
    "src/live_update.cpp"
    INCLUDE_DIRS "include" REQUIRES app_model network
    PRIV_REQUIRES app_update diagnostics mbedtls json_reader)

set(PRD_OTA_BUILD_PROFILE "$ENV{PRD_BUILD_PROFILE}")
if(PRD_OTA_BUILD_PROFILE STREQUAL "")
    set(PRD_OTA_BUILD_PROFILE "debug")
endif()
include(${CMAKE_SOURCE_DIR}/cmake/ota_release_channel.cmake)
prd_select_ota_validation_lan(
    "${PRD_OTA_BUILD_PROFILE}" "${CONFIG_PRD_OTA_LAN_ORIGIN}"
    "${CONFIG_PRD_OTA_LAN_CA_CERT_PATH}" PRD_OTA_LAN_ORIGIN PRD_OTA_LAN_CERTIFICATE)
if(NOT PRD_OTA_LAN_ORIGIN STREQUAL "")
    set(PRD_OTA_CERT_HEADER "${CMAKE_CURRENT_BINARY_DIR}/generated/ota_validation_ca.hpp")
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated")
    file(WRITE "${PRD_OTA_CERT_HEADER}"
        "#pragma once\ninline constexpr char kPrdOtaValidationCaPem[] = R\"PRD_OTA_CERT(${PRD_OTA_LAN_CERTIFICATE})PRD_OTA_CERT\";\n")
    target_include_directories(${COMPONENT_LIB} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE
        PRD_OTA_LAN_MODE=1 "PRD_OTA_LAN_ORIGIN=\"${PRD_OTA_LAN_ORIGIN}\"")
    message(STATUS "Prusa Remote Display OTA: private LAN validation origin ${PRD_OTA_LAN_ORIGIN}")
else()
    message(STATUS "Prusa Remote Display OTA: official GitHub release channel")
endif()

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
target_link_libraries(${COMPONENT_LIB} INTERFACE "-u prd_image_identity")
include(${CMAKE_SOURCE_DIR}/cmake/compiler_policy.cmake)
prd_apply_compiler_policy(${COMPONENT_LIB})
