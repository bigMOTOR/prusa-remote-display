# SPDX-FileCopyrightText: 2026 Mykola Fiantsev
# SPDX-License-Identifier: GPL-3.0-only

idf_component_register(
    SRCS
        "src/app_core.cpp"
        "src/button_translation.cpp"
        "src/candidate.cpp"
        "src/exclusive_operation.cpp"
        "src/printer_session.cpp"
        "src/configuration_observation.cpp"
        "src/provisioning_decision.cpp"
        "src/factory_reset.cpp"
        "src/provisioning_flow.cpp"
        "src/reentry.cpp"
        "src/setup_entry.cpp"
        "src/wifi_restore.cpp"
    INCLUDE_DIRS
        "include"
    REQUIRES
        app_model
        app_store
        board
        diagnostics
    PRIV_REQUIRES
        button
        config
        esp_timer
        display
        network
        ota
        platform
        provisioning
        # Milestone 4's printer integration. PRIVATE: nothing above app_core may reach the
        # PrusaLink client, which is §4.2's rule that ui depends on app_model and not on
        # prusalink.
        prusalink
        secrets
        storage
        ui
        validation
)

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)

include(${CMAKE_SOURCE_DIR}/cmake/compiler_policy.cmake)
prd_apply_compiler_policy(${COMPONENT_LIB})
