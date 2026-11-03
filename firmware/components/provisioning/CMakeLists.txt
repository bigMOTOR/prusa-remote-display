# SPDX-FileCopyrightText: 2026 Mykola Fiantsev
# SPDX-License-Identifier: GPL-3.0-only

# The local provisioning experience (Architecture Spec §5.11, §10.5, §14.4).
#
# ⛔ THIS COMPONENT DOES NOT INCLUDE OR MANAGE esp_wifi — owner ruling D5, 2026-08-15.
# `network` owns the radio and the access point; this component is handed an address and
# serves on it. The dependency list below is the enforcement: esp_wifi is absent from it.
#
# Milestone 3.7 creates the component, per Bootstrap Spec §6.7's rule that a deferred
# component is added "in the milestone that first exercises them".

idf_component_register(
    SRCS
        "src/credential_scrub.cpp"
        "src/escaping.cpp"
        "src/request_guard.cpp"
        "src/request_validation.cpp"
        "src/server.cpp"
        "src/server_view.cpp"
        "src/session_token.cpp"
        "src/surface.cpp"
    INCLUDE_DIRS
        "include"
    PRIV_INCLUDE_DIRS
        "src"
    REQUIRES
        app_model
    PRIV_REQUIRES
        diagnostics
        esp_http_server
        esp_timer
        json
        # getsockname(), for the surface a request actually arrived on (owner ruling A of
        # Milestone 3.9). Reading it from the socket rather than a header is the whole point:
        # a Host or Origin header is written by the caller.
        lwip
    # The page, compiled into the image (owner ruling D8, ruling R5, Bootstrap §9.3).
    # Real files rather than a generated header: they stay lintable, diffable and
    # openable in a browser, and there is no generator to keep honest.
    EMBED_FILES
        "assets/index.html"
        "assets/style.css"
        "assets/app.js"
)

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)

include(${CMAKE_SOURCE_DIR}/cmake/compiler_policy.cmake)
prd_apply_compiler_policy(${COMPONENT_LIB})

# server.cpp is the ESP-IDF adapter TU. ESP-IDF C headers it includes are not
# -Wundef/-Wpedantic clean (undefined macros, anonymous structs, and #include_next -- the
# last cannot be silenced by an in-file diagnostic pragma). Relax only these two warnings
# for only this file, per architecture spec §12.3; every other project warning still
# applies. Applied after the policy so it wins.
#
# credential_scrub.cpp, escaping.cpp, request_guard.cpp, request_validation.cpp, server_view.cpp,
# session_token.cpp and surface.cpp include no ESP-IDF header and keep the full strict flags. All
# seven are compiled by the host test suite, which is the point of keeping them clean --
# credential_scrub.cpp in particular, because §13.5's wipe cannot be observed from the adapter TU at
# all, and server_view.cpp because the coherence of one request's answer is exactly what a review of
# the adapter cannot establish.
set_source_files_properties(
    "src/server.cpp"
    PROPERTIES
        COMPILE_OPTIONS "-Wno-undef;-Wno-pedantic"
)
