# SPDX-FileCopyrightText: 2026 Mykola Fiantsev
# SPDX-License-Identifier: GPL-3.0-only

cmake_minimum_required(VERSION 3.24)

project(prd_host_tests LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# --- Host sanitizers (Bootstrap Spec §12.5) --------------------------------
#
# Off by default and confined to this project: this CMake tree is the host test
# build only and is never included by the ESP-IDF build, so no sanitizer flag can
# reach target firmware. Enable with -DPRD_ENABLE_SANITIZERS=ON, or through
# ./firmware/tools/test-host.sh --sanitize.
#
# GoogleTest itself is built with the same flags on purpose. Instrumenting the
# test code but not the framework leaves the allocator half-instrumented, and
# ASan reports that as a container-overflow false positive.
option(PRD_ENABLE_SANITIZERS "Build host tests with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)

if(PRD_ENABLE_SANITIZERS)
    # -fno-sanitize-recover=all makes a UBSan finding abort rather than print and
    # continue, which is what turns a finding into a failing test rather than a
    # line of output nobody reads (§12.5: "Sanitizer failures shall fail CI").
    #
    # The wrapper disables implicit Linux LeakSanitizer to preserve the declared
    # ASan/UBSan scope across Linux CI and local macOS. Leak coverage is not claimed.
    set(PRD_SANITIZER_FLAGS
        -fsanitize=address
        -fsanitize=undefined
        -fno-sanitize-recover=all
        -fno-omit-frame-pointer
        -g
    )

    add_compile_options(${PRD_SANITIZER_FLAGS})
    add_link_options(${PRD_SANITIZER_FLAGS})

    message(STATUS "prd: host sanitizers ENABLED (address, undefined; no leak-coverage claim)")
endif()

include(FetchContent)

FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.17.0.tar.gz
)

set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(googletest)

enable_testing()

add_executable(
    prd_host_tests

    app_model/build_info_tests.cpp
    ota/compatibility_tests.cpp
    ota/ingestion_tests.cpp
    ota/first_boot_tests.cpp
    ../../components/ota/src/compatibility.cpp
    ../../components/ota/src/ingestion.cpp
    ../../components/ota/src/manifest.cpp
    ../../components/ota/src/installation.cpp
    ota/installation_tests.cpp
    ota/manifest_tests.cpp
    ota/release_discovery_tests.cpp
    ota/release_client_tests.cpp
    ota/release_policy_tests.cpp
    ota/release_schedule_tests.cpp
    ota/release_bundle_tests.cpp
    ota/live_update_policy_tests.cpp
    ../../components/ota/src/release_discovery.cpp
    ../../components/ota/src/release_client.cpp
    ../../components/ota/src/release_policy.cpp
    ../../components/ota/src/release_schedule.cpp
    ../../components/ota/src/release_bundle.cpp
    ../../components/ota/src/first_boot.cpp
    app_model/build_metadata_tests.cpp
    app_model/hardware_check_tests.cpp
    app_model/bounded_string_tests.cpp
    app_model/observation_tests.cpp
    app_model/reducer_tests.cpp
    app_model/boot_recovery_tests.cpp
    app_model/projection_tests.cpp
    app_model/snapshot_ordering_tests.cpp
    app_model/network_credential_boundary_tests.cpp
    app_model/printer_identity_boundary_tests.cpp
    app_store/store_tests.cpp
    app_core/button_translation_tests.cpp
    fake_source/fixture_tests.cpp
    fake_source/scenario_tests.cpp
    ui/formatter_tests.cpp
    ui/text_tests.cpp
    ui/token_tests.cpp
    ui/scene_selection_tests.cpp
    app_model/display_policy_tests.cpp
    ui/operational_energy_tests.cpp
    ui/operational_motion_tests.cpp
    ui/operational_form_tests.cpp
    ui/form_motion_regression_tests.cpp
    ui/operational_accent_tests.cpp
    ui/operational_luminance_tests.cpp
    ui/operational_content_tests.cpp
    ui/unavailable_motion_tests.cpp
    ui/telemetry_presence_tests.cpp
    ui/walk_replay_tests.cpp
    ui/presentation_diff_tests.cpp
    ui/wifi_qr_tests.cpp
    diagnostics/redaction_tests.cpp
    display/brightness_curve_tests.cpp
    validation/brightness_harness_tests.cpp
    validation/printing_timeline_tests.cpp
    validation/printing_paced_tests.cpp
    validation/loss_review_tests.cpp
    validation/access_review_tests.cpp
    support/timeline/log_stub.cpp
    ../../components/validation/src/printing_timeline_flow.cpp
    display/transfer_tracker_tests.cpp
    button/press_classifier_tests.cpp
    storage/bring_up_tests.cpp
    storage/namespace_tests.cpp
    config/schema_tests.cpp
    config/record_tests.cpp
    secrets/schema_tests.cpp
    secrets/record_tests.cpp
    storage/checksum_tests.cpp
    storage/activation_boundary_tests.cpp
    app_core/configuration_observation_tests.cpp
    network/backoff_tests.cpp
    network/bringup_tests.cpp
    network/discovery_identity_tests.cpp
    network/lifecycle_tests.cpp
    network/station_events_tests.cpp
    network/station_events_interleaving_tests.cpp
    network/station_intent_tests.cpp
    network/session_credential_tests.cpp
    network/wifi_log_policy_tests.cpp
    network/http_body_tests.cpp
    network/request_budget_tests.cpp
    network/public_stream_tests.cpp
    network/driver_credential_tests.cpp
    network/lan_policy_tests.cpp
    prusalink/json_scan_tests.cpp
    prusalink/decode_tests.cpp
    prusalink/normalize_tests.cpp
    prusalink/outcome_tests.cpp
    prusalink/request_tests.cpp
    prusalink/worker_lifecycle_tests.cpp
    prusalink/redirect_tests.cpp
    prusalink/vertical_slice_tests.cpp
    app_core/factory_reset_tests.cpp
    app_core/provisioning_decision_tests.cpp
    app_core/provisioning_flow_tests.cpp
    app_core/candidate_tests.cpp
    app_core/printer_session_tests.cpp
    app_core/exclusive_operation_tests.cpp
    app_core/reentry_tests.cpp
    app_core/reset_progress_tests.cpp
    app_core/setup_entry_tests.cpp
    app_core/submission_handoff_tests.cpp
    app_core/wifi_restore_tests.cpp
    ui/setup_session_mailbox_tests.cpp
    provisioning/request_validation_tests.cpp
    provisioning/request_guard_tests.cpp
    provisioning/session_token_tests.cpp
    provisioning/served_page_tests.cpp
    provisioning/surface_tests.cpp
    provisioning/credential_scrub_tests.cpp
    provisioning/server_view_tests.cpp

    ../../components/app_model/src/board_identity.cpp
    ../../components/app_model/src/build_info.cpp
    ../../components/app_model/src/hardware_check.cpp
    ../../components/app_model/src/observation.cpp
    ../../components/app_model/src/values.cpp
    ../../components/app_model/src/state_layers.cpp
    ../../components/app_model/src/printer_data.cpp
    ../../components/app_model/src/device_data.cpp
    ../../components/app_model/src/app_state.cpp
    ../../components/app_model/src/events.cpp
    ../../components/app_model/src/reducer.cpp
    ../../components/app_model/src/presentation.cpp
    ../../components/app_model/src/display_policy.cpp
    ../../components/app_store/src/store.cpp
    ../../components/app_core/src/button_translation.cpp
    ../../components/ui/src/formatters.cpp
    ../../components/ui/src/scene_selection.cpp
    ../../components/ui/src/scenes/operational_energy.cpp
    ../../components/ui/src/scenes/operational_form.cpp
    ../../components/ui/src/scenes/operational_accent.cpp
    ../../components/ui/src/scenes/operational_luminance.cpp
    ../../components/ui/src/scenes/operational_content.cpp
    ../../components/ui/src/scenes/unavailable_motion.cpp
    ../../components/ui/src/presentation_diff.cpp
    ../../components/ui/src/wifi_qr.cpp
    ../../components/ui/src/theme/tokens.cpp
    ../../components/ui/src/text/text.cpp
    ../../components/validation/src/gesture_policy.cpp
    ../../components/validation/src/status_timeline.cpp
    ../../components/validation/src/loss_review.cpp
    ../../components/validation/src/access_review.cpp
    ../../components/validation/src/walk_time_base.cpp
    ../../components/fake_source/src/observations.cpp
    ../../components/fake_source/src/fixtures.cpp
    ../../components/fake_source/src/scenarios.cpp
    ../../components/fake_source/src/source.cpp
    ../../components/diagnostics/src/redaction.cpp
    ../../components/display/src/brightness_curve.cpp
    ../../components/display/src/transfer_tracker.cpp
    ../../components/button/src/press_classifier.cpp
    ../../components/button/src/button_event.cpp
    ../../components/storage/src/bring_up.cpp
    ../../components/storage/src/key_value.cpp
    ../../components/storage/src/namespaces.cpp
    ../../components/config/src/config.cpp
    ../../components/config/src/schema.cpp
    ../../components/network/src/http_body.cpp
    ../../components/network/src/lan_policy.cpp
    ../../components/json_reader/src/reader.cpp
    ../../components/prusalink/src/decode.cpp
    ../../components/prusalink/src/normalize.cpp
    ../../components/prusalink/src/outcome.cpp
    ../../components/prusalink/src/request.cpp
    ../../components/prusalink/src/worker_lifecycle.cpp
    ../../components/secrets/src/secrets.cpp
    ../../components/secrets/src/secret_schema.cpp
    ../../components/storage/src/checksum.cpp
    ../../components/app_core/src/configuration_observation.cpp
    ../../components/network/src/backoff.cpp
    ../../components/network/src/discovery_identity.cpp
    ../../components/network/src/lifecycle.cpp
    ../../components/network/src/station_events.cpp
    ../../components/network/src/station_intent.cpp
    ../../components/network/src/request_budget.cpp
    ../../components/network/src/public_stream.cpp
    ../../components/network/src/driver_credential.cpp
    ../../components/network/src/session_credential.cpp
    ../../components/app_core/src/factory_reset.cpp
    ../../components/app_core/src/provisioning_decision.cpp
    ../../components/app_core/src/provisioning_flow.cpp
    ../../components/app_core/src/candidate.cpp
    ../../components/app_core/src/exclusive_operation.cpp
    ../../components/app_core/src/printer_session.cpp
    ../../components/app_core/src/reentry.cpp
    ../../components/app_core/src/setup_entry.cpp
    ../../components/app_core/src/wifi_restore.cpp
    ../../components/provisioning/src/request_validation.cpp
    ../../components/provisioning/src/request_guard.cpp
    ../../components/provisioning/src/session_token.cpp
    ../../components/provisioning/src/escaping.cpp
    ../../components/provisioning/src/surface.cpp
    ../../components/provisioning/src/server_view.cpp
    ../../components/provisioning/src/credential_scrub.cpp
)

target_include_directories(
    prd_host_tests
    PRIVATE
        ../../components/app_model/include
        ../../components/ota/include
        ../../components/validation/include
        ../../components/app_store/include
        ../../components/fake_source/include
        ../../components/diagnostics/include
        # The brightness curve, the press classifier and the button translation are
        # internal headers of their components, deliberately kept free of ESP-IDF so
        # they can be tested here. display's PUBLIC header joins them because it is
        # equally free of ESP-IDF and declares percent_for_light_fraction() and its
        # LightFraction unit — the seam whose gamma domain this suite asserts. Adding
        # the path rather than duplicating the declaration keeps one declaration.
        ../../components/display/src
        ../../components/display/include
        # The brightness harness's step table: pure data, deliberately free of ESP-IDF and of
        # sdkconfig, so the sequence a person walks with the button is asserted here rather than
        # discovered on a panel.
        ../../components/validation/src
        ../../components/button/src
        ../../components/button/include
        ../../components/app_core/src
        # storage's public header, plus bring_up.hpp — an internal header kept free of
        # ESP-IDF precisely so the §13.4 erase rule can be tested here rather than only
        # on the board. The NVS adapter cannot be compiled in this build and is not.
        ../../components/storage/include
        ../../components/storage/src
        # config has no ESP-IDF adapter at all — it talks only to storage::KeyValue — so
        # the WHOLE component compiles here, internal header included. The record format,
        # the §13.3 staged write and the §13.4 migration dispatch are all exercised against
        # the same code the firmware runs.
        ../../components/config/include
        ../../components/config/src
        # secrets, on the same footing: no ESP-IDF adapter, so the whole component compiles
        # here and §13.5's "no credential reaches a log" is asserted rather than reviewed.
        ../../components/secrets/include
        ../../components/secrets/src
        # network, same footing again: the station phase machine and §11.3's backoff ladder
        # are pure, so both are tested here. The esp_wifi adapter cannot be compiled in this
        # build and is not.
        ../../components/network/include
        ../../components/network/src
        ../../components/provisioning/include
        ../../components/provisioning/src
        # prusalink, Milestone 4. Everything except the worker task is free of ESP-IDF by
        # design — the scanner, the decoder, the normalizer, the error taxonomy and the
        # request builder — so the layer that meets a real printer's output is exercised
        # here against the same code the firmware runs. The worker's own file includes
        # FreeRTOS and cannot be compiled in this build; it is not.
        ../../components/prusalink/include
        ../../components/prusalink/src
        ../../components/json_reader/include
        # The operational walk's fixture time base. The flow around it cannot be
        # compiled here — it is gated on sdkconfig and calls into ui — but the rule
        # deciding each load's base is pure, and the replay guard has to drive the
        # real one rather than a copy of its arithmetic.
        ../../components/validation/src
        # The ui component's LVGL-free logic: the §17.7 formatters, the scene
        # selector, the §17.4 diff and the Production UI token tables. Every one of
        # them is deliberately free of LVGL so it can be tested here; the LVGL side
        # of ui cannot be compiled in this build and is not.
        ../../components/ui/src
        # ui's public header. It names no LVGL type by contract, so the report structs
        # declared in it are as compilable here as anywhere — which is what lets the
        # diagnostic read-back be filled by a pure function and checked field by field.
        ../../components/ui/include
        # test/host/support/fake_clock.hpp is included as "support/fake_clock.hpp"
        .
)

# The station event ring is exercised by a real producer thread and a real consumer thread — see
# station_events_tests.cpp, which also records that a stress pass is not the release/acquire proof.
find_package(Threads REQUIRED)

target_link_libraries(
    prd_host_tests
    PRIVATE
        GTest::gtest_main
        Threads::Threads
)

# The served provisioning assets, read at run time by served_page_tests.cpp.
#
# A path rather than a generated header: these files are product surface served verbatim to
# the browser, and a test that read a copy of them would assert something other than what the
# device sends.
target_compile_definitions(
    prd_host_tests
    PRIVATE
        PRD_PROVISIONING_ASSETS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../../components/provisioning/assets"

        # Architecture Spec §20.3's captured-response tree. The decoder tests read the CORE One
        # payload from the file rather than restating it, so the fixture on disk is the single
        # authoritative copy and the two cannot drift apart.
        PRD_PRUSALINK_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../fixtures/prusalink"
        # The firmware tree, so the isolation test can read the profile defaults and the
        # validation Kconfig. A build-time property needs the build files themselves.
        PRD_FIRMWARE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../.."
)

target_compile_options(
    prd_host_tests
    PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Wshadow
        -Wconversion
        -Wsign-conversion
        -Wformat=2
        -Werror=return-type
        -Werror=format
)

include(GoogleTest)

# Compile the actual paced scheduler, not a replay of its fixture table. This test-only
# configuration applies to one source and does not alter any product model or other flow.
set_source_files_properties(../../components/validation/src/printing_timeline_flow.cpp PROPERTIES
    COMPILE_DEFINITIONS CONFIG_PRD_VALIDATION_PRINTING_TIMELINE=1
    INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/support/timeline")

gtest_discover_tests(prd_host_tests)

# A small dependency-free regression for the production public HTTPS limits.
add_executable(prd_public_https_buffer_tests network/public_https_buffer_tests.cpp)
target_include_directories(prd_public_https_buffer_tests PRIVATE ../../components/network/src)
target_compile_options(prd_public_https_buffer_tests PRIVATE -Wall -Wextra -Wpedantic -Werror=format)
add_test(NAME public_https_buffer COMMAND prd_public_https_buffer_tests)

# Compile the real OTA adapter with a narrowly scoped test-only IDF API boundary.
target_sources(prd_host_tests PRIVATE ota/device_boot_tests.cpp
    ../../components/ota/src/device_boot.cpp)
set_source_files_properties(../../components/ota/src/device_boot.cpp PROPERTIES
    INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/support/ota")

# Observe the actual acquire loads under a deterministic, bounded schedule. This
# seam adds no ring fields and is not part of any ESP-IDF target configuration.
set_source_files_properties(../../components/network/src/station_events.cpp PROPERTIES
    COMPILE_DEFINITIONS PRD_STATION_RING_TEST_INTERLEAVING=1)
