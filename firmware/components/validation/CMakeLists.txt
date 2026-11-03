# SPDX-FileCopyrightText: 2026 Mykola Fiantsev
# SPDX-License-Identifier: GPL-3.0-only

# Hardware-validation support flows, moved out of app_core in Milestone 2 Phase B.
#
# Not part of the production dependency graph: nothing product-facing depends on this,
# and every flow inside it is compiled out unless a validation flag selects it. It sits
# beside app_core rather than under it, and it must never depend on app_core — the
# composition root calls these, not the other way round.

set(PRD_OPTIONAL_REVIEW_SOURCES)
set(PRD_TIMELINE_SOURCE "src/printing_timeline_flow.cpp")
if(CONFIG_PRD_VALIDATION_ACCESS_REVIEW)
    set(PRD_TIMELINE_SOURCE "src/access_review_flow.cpp")
    if(CONFIG_PRD_VALIDATION_PRINTER_ERROR_REVIEW)
        list(APPEND PRD_OPTIONAL_REVIEW_SOURCES "src/printer_error_review.cpp")
    else()
        list(APPEND PRD_OPTIONAL_REVIEW_SOURCES "src/access_review.cpp")
    endif()
endif()
if(CONFIG_PRD_VALIDATION_PRINTING_LOSS_REVIEW)
    list(APPEND PRD_OPTIONAL_REVIEW_SOURCES "src/loss_review.cpp")
endif()

idf_component_register(
    SRCS
        ${PRD_OPTIONAL_REVIEW_SOURCES}
        "src/panel_flow.cpp"
        "src/display_patterns_flow.cpp"
        "src/render_report_flow.cpp"
        "src/button_log_flow.cpp"
        "src/scenario_flow.cpp"
        "src/operational_walk_flow.cpp"
        "src/brightness_harness_flow.cpp"
        "src/gesture_policy.cpp"
        ${PRD_TIMELINE_SOURCE}
        "src/status_timeline.cpp"
        "src/walk_time_base.cpp"
    INCLUDE_DIRS
        "include"
    REQUIRES
        app_model
        app_store
        button
    PRIV_REQUIRES
        diagnostics
        display
        fake_source
        platform
        prusalink
        ui
)

# The Printing timeline flow drives the PRODUCTION decoder, which lives in prusalink's private
# headers — decode.hpp, dto.hpp, normalize.hpp. A bench instrument whose whole purpose is to run
# the real decoder has to be able to name it, and the alternative is promoting three internal
# headers to a public interface so that one diagnostic flow can reach them. Same trade the host
# test suite already makes for the same three files.
target_include_directories(${COMPONENT_LIB} PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../prusalink/src")

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)

include(${CMAKE_SOURCE_DIR}/cmake/compiler_policy.cmake)
prd_apply_compiler_policy(${COMPONENT_LIB})

# The three flow TUs that reach ESP-IDF headers. Those headers are not -Wundef or
# -Wpedantic clean (undefined macros, anonymous structs, and #include_next -- the last
# cannot be silenced by an in-file diagnostic pragma). Relax only these two warnings
# for only these files, per architecture spec §12.3; every other project warning still
# applies. Applied after the policy so it wins.
set_source_files_properties(
    "src/display_patterns_flow.cpp"
    "src/render_report_flow.cpp"
    PROPERTIES
        COMPILE_OPTIONS "-Wno-undef;-Wno-pedantic"
)
