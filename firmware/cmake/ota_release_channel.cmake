# SPDX-FileCopyrightText: 2026 Mykola Fiantsev
# SPDX-License-Identifier: GPL-3.0-only

# A private LAN fixture is a hardware-validation instrument, never a release
# destination. Both inputs are required so TLS cannot silently fall back to a
# public trust store or a plaintext endpoint.
function(prd_select_ota_validation_lan profile origin certificate output_origin output_certificate)
    if("${origin}" STREQUAL "" AND "${certificate}" STREQUAL "")
        set(${output_origin} "" PARENT_SCOPE)
        set(${output_certificate} "" PARENT_SCOPE)
        return()
    endif()
    if(NOT "${profile}" STREQUAL "hardware-validation")
        message(FATAL_ERROR "Local OTA validation is permitted only in hardware-validation")
    endif()
    if("${origin}" STREQUAL "" OR "${certificate}" STREQUAL "")
        message(FATAL_ERROR "Local OTA validation requires both HTTPS origin and CA certificate")
    endif()
    if(NOT "${origin}" MATCHES
            "^https://([0-9]+)\\.([0-9]+)\\.([0-9]+)\\.([0-9]+):([0-9]+)$")
        message(FATAL_ERROR "Local OTA origin must be an exact HTTPS IPv4 address and port")
    endif()
    set(octet1 "${CMAKE_MATCH_1}")
    set(octet2 "${CMAKE_MATCH_2}")
    set(octet3 "${CMAKE_MATCH_3}")
    set(octet4 "${CMAKE_MATCH_4}")
    set(port "${CMAKE_MATCH_5}")
    foreach(octet IN ITEMS octet1 octet2 octet3 octet4)
        if("${${octet}}" MATCHES "^0[0-9]+$")
            message(FATAL_ERROR "Local OTA origin must use canonical IPv4 octets")
        endif()
        if(${${octet}} GREATER 255)
            message(FATAL_ERROR "Local OTA origin contains an invalid IPv4 octet")
        endif()
    endforeach()
    if("${port}" MATCHES "^0[0-9]+$")
        message(FATAL_ERROR "Local OTA origin must use a canonical port")
    endif()
    if(port LESS 1 OR port GREATER 65535)
        message(FATAL_ERROR "Local OTA origin contains an invalid port")
    endif()
    if(NOT (octet1 EQUAL 10 OR
            (octet1 EQUAL 172 AND octet2 GREATER_EQUAL 16 AND octet2 LESS_EQUAL 31) OR
            (octet1 EQUAL 192 AND octet2 EQUAL 168)))
        message(FATAL_ERROR "Local OTA origin must be an RFC1918 private IPv4 address")
    endif()
    if(NOT IS_ABSOLUTE "${certificate}" OR NOT EXISTS "${certificate}" OR
            IS_DIRECTORY "${certificate}")
        message(FATAL_ERROR "Local OTA CA certificate must be an existing absolute file")
    endif()
    file(SIZE "${certificate}" certificate_size)
    if(certificate_size LESS 100 OR certificate_size GREATER 8192)
        message(FATAL_ERROR "Local OTA CA certificate size is invalid")
    endif()
    file(READ "${certificate}" certificate_body)
    if(NOT certificate_body MATCHES "-----BEGIN CERTIFICATE-----" OR
            NOT certificate_body MATCHES "-----END CERTIFICATE-----" OR
            certificate_body MATCHES "PRIVATE KEY" OR
            certificate_body MATCHES "\\)PRD_OTA_CERT\"")
        message(FATAL_ERROR "Local OTA CA certificate is not a safe PEM certificate")
    endif()
    set(${output_origin} "${origin}" PARENT_SCOPE)
    set(${output_certificate} "${certificate_body}" PARENT_SCOPE)
endfunction()
