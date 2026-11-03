# SPDX-FileCopyrightText: 2026 Mykola Fiantsev
# SPDX-License-Identifier: GPL-3.0-only

# The PrusaLink integration (Architecture Spec §5.8), created at Milestone 4's first
# vertical slice.
#
# ⬅️ DEPENDENCY DIRECTION, §4.2: this component depends on `network` and on `app_model`,
# and on nothing above them. There is no dependency on `ui`, on `app_store` or on
# `app_core` — results leave through the Report struct and app_core turns them into
# events, which is what keeps the printer protocol from reaching the domain.
#
# ⛔ NO esp_http_client HERE. §5.7 assigns the HTTP client wrapper to `network`, and the
# only ESP-IDF headers this component includes are FreeRTOS's, for the worker task
# §10.4 requires. Everything else — the scanner, the decoder, the normalizer, the error
# taxonomy and the request builder — is free of ESP-IDF entirely and is compiled by the
# host test suite.
#
# ⛔ READ-ONLY BY CONSTRUCTION. Two GET paths, declared in request.hpp; no other method
# and no other endpoint exists in this component. API Research Report §19 places print
# control, uploads and temperature changes outside the MVP, and ADR-0010 defers Prusa
# Connect Local — so there is no WebSocket, MQTT or SSE code here either.

idf_component_register(
    SRCS
        "src/decode.cpp"
        "src/normalize.cpp"
        "src/outcome.cpp"
        "src/request.cpp"
        "src/worker.cpp"
        "src/worker_lifecycle.cpp"
    INCLUDE_DIRS
        "include"
    REQUIRES
        app_model
        network
    PRIV_REQUIRES
        diagnostics
        json_reader
)

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)

include(${CMAKE_SOURCE_DIR}/cmake/compiler_policy.cmake)
prd_apply_compiler_policy(${COMPONENT_LIB})

# worker.cpp is the only ESP-IDF adapter translation unit here: it includes FreeRTOS
# headers, which are not -Wundef/-Wpedantic clean. Relaxed for that file alone, per
# Architecture Spec §12.3 and exactly as the network component does for its radio
# adapters. Every other file keeps the full strict flag set, which is the point of
# keeping them ESP-IDF-free.
set_source_files_properties(
    "src/worker.cpp"
    PROPERTIES
        COMPILE_OPTIONS "-Wno-undef;-Wno-pedantic"
)
