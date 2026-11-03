# SPDX-FileCopyrightText: 2026 Mykola Fiantsev
# SPDX-License-Identifier: GPL-3.0-only

# Generic local-network services (Architecture Spec §5.7). Milestone 3.4 built the station
# lifecycle — §5.7's WifiManager — Milestone 3.5 added the provisioning access point, and
# Milestone 3.9 added the mDNS advertisement owner ruling R6 asks for. Milestone 4 adds §5.7's
# HTTP client wrapper — generic, printer-agnostic, and the only place in the firmware that
# opens an outbound HTTP request — together with §5.7's HostResolver (src/resolve.cpp) and the
# §13.5 destination policy that decides what may be spoken to (src/lan_policy.cpp). §16.1's
# DNS-versus-refused distinction is what network::lan_policy::ResolveOutcome carries.
#
# ⬅️ mDNS HERE ADVERTISES; IT DOES NOT RESOLVE. §5.7 lists "mDNS-assisted hostname resolution"
# for finding the printer, and owner ruling R4 keeps every printer protocol in Milestone 4.
# What Milestone 3.9 adds is the other direction — making this device findable on the LAN it
# has just joined. The dependency is pinned in idf_component.yml per Architecture Spec §22.
#
# ⛔ NO OTHER COMPONENT MAY INCLUDE esp_wifi.h. Owner ruling D5 (2026-08-15) settles the
# §5.7/§5.11 boundary in this component's favour: `network` owns the ESP-IDF radio and
# access-point mechanics, and the `provisioning` component — created at M3.7, when there is
# server behaviour to own — shall not include or independently manage esp_wifi.

idf_component_register(
    SRCS
        "src/backoff.cpp"
        "src/discovery.cpp"
        "src/discovery_identity.cpp"
        "src/driver_credential.cpp"
        "src/http.cpp"
        "src/http_body.cpp"
        "src/lan_policy.cpp"
        "src/request_budget.cpp"
        "src/public_https.cpp"
        "src/public_stream.cpp"
        "src/resolve.cpp"
        "src/vendor_log_policy.cpp"
        "src/lifecycle.cpp"
        "src/radio.cpp"
        "src/station_events.cpp"
        "src/station_intent.cpp"
        "src/session_credential.cpp"
        "src/setup_ap.cpp"
        "src/wifi.cpp"
    INCLUDE_DIRS
        "include"
    REQUIRES
        app_model
    PRIV_REQUIRES
        diagnostics
        # §5.7's HTTP client wrapper, added at Milestone 4. PRIVATE, so no other
        # component acquires esp_http_client.h transitively — the same containment rule
        # that keeps esp_wifi.h and mdns.h inside this component.
        esp_http_client
        # Certificate verification for the separate credential-free public HTTPS stream.
        # A build without the bundle is rejected below; no insecure fallback exists.
        mbedtls
        # §5.7's resolver, added at Milestone 4. PRIVATE: getaddrinfo stays inside this component,
        # so no other component acquires lwip headers transitively.
        lwip
        esp_wifi
        esp_netif
        esp_event
        esp_timer
        # Pinned exactly in idf_component.yml (Architecture Spec §22). PRIVATE, so no other
        # component acquires mdns.h through a transitive requirement on this one — the same
        # rule that keeps LVGL inside the ui component.
        espressif__mdns
)

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)

include(${CMAKE_SOURCE_DIR}/cmake/compiler_policy.cmake)
prd_apply_compiler_policy(${COMPONENT_LIB})

# The six ESP-IDF adapter TUs are discovery.cpp, http.cpp, radio.cpp, resolve.cpp, setup_ap.cpp and
# wifi.cpp — exactly the list below. The C headers they include are not -Wundef/-Wpedantic clean
# (undefined macros, anonymous structs, and #include_next -- the last cannot be silenced by an
# in-file diagnostic pragma). Relax only these two warnings for only these files, per architecture
# spec §12.3; every other project warning still applies. Applied after the policy so it wins.
#
# The other ten -- backoff.cpp, discovery_identity.cpp, driver_credential.cpp, http_body.cpp,
# lan_policy.cpp, lifecycle.cpp, request_budget.cpp, session_credential.cpp, station_events.cpp and
# station_intent.cpp -- include no ESP-IDF headers, keep the full strict flags, and are ALL TEN
# compiled by the host test suite. That is the point of keeping them clean: lan_policy.cpp carries
# §13.5's destination policy, http_body.cpp carries §16.4's response bound, request_budget.cpp
# carries §11.4's phase budgets, driver_credential.cpp carries the station field bounds §13.5 and
# the 32-byte SSID depend on, and station_events.cpp plus station_intent.cpp carry the whole Wi-Fi
# station intent machine and its event channel -- so each is proved without a network.
set_source_files_properties(
    "src/discovery.cpp"
    "src/http.cpp"
    "src/public_https.cpp"
    "src/resolve.cpp"
    "src/radio.cpp"
    "src/setup_ap.cpp"
    "src/wifi.cpp"
    PROPERTIES
        COMPILE_OPTIONS "-Wno-undef;-Wno-pedantic"
)

if(NOT CONFIG_MBEDTLS_CERTIFICATE_BUNDLE)
    message(FATAL_ERROR "prd: public HTTPS requires CONFIG_MBEDTLS_CERTIFICATE_BUNDLE")
endif()
