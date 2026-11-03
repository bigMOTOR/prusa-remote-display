# SPDX-FileCopyrightText: 2026 Mykola Fiantsev
# SPDX-License-Identifier: GPL-3.0-only

function(prd_collect_build_metadata product_version)
    if(DEFINED ENV{PRD_BUILD_PROFILE} AND NOT "$ENV{PRD_BUILD_PROFILE}" STREQUAL "")
        set(PRD_BUILD_PROFILE "$ENV{PRD_BUILD_PROFILE}")
    else()
        set(PRD_BUILD_PROFILE "debug")
    endif()

    # Board target identity for build metadata (Bootstrap Spec §14.2). The
    # compile-time Kconfig board selection (components/board/Kconfig) is
    # authoritative so the reported boardTarget cannot diverge from the
    # compiled target. The environment override and the static default remain
    # only for contexts where no configured sdkconfig is available.
    if(DEFINED CONFIG_PRD_BOARD_WAVESHARE_ESP32_S3_LCD_2_8 AND
       CONFIG_PRD_BOARD_WAVESHARE_ESP32_S3_LCD_2_8)
        set(PRD_BOARD_TARGET "waveshare_esp32_s3_lcd_2_8")
    elseif(DEFINED ENV{PRD_BOARD_TARGET} AND NOT "$ENV{PRD_BOARD_TARGET}" STREQUAL "")
        set(PRD_BOARD_TARGET "$ENV{PRD_BOARD_TARGET}")
    else()
        # Reference board target as named by Bootstrap Specification v0.3
        # (board/targets/ directory and sdkconfig.defaults profile suffix).
        set(PRD_BOARD_TARGET "waveshare_esp32_s3_lcd_2_8")
    endif()

    set(PRD_GIT_COMMIT "unknown")
    set(PRD_GIT_DESCRIBE "unknown")
    set(PRD_GIT_DIRTY FALSE)
    set(PRD_BUILD_EPOCH "0")
    set(PRD_RELEASE_TAG "")

    find_package(Git QUIET)

    if(GIT_FOUND)
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" rev-parse --short=12 HEAD
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/.."
            RESULT_VARIABLE git_commit_result
            OUTPUT_VARIABLE git_commit
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
        )

        if(git_commit_result EQUAL 0 AND NOT git_commit STREQUAL "")
            set(PRD_GIT_COMMIT "${git_commit}")
        endif()

        # gitDescribe, one of the build-identity fields required by §14.2.
        # --always keeps this usable on a repository with no tags yet, which is
        # the state every project starts in.
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" describe --tags --always --dirty=-dirty
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/.."
            RESULT_VARIABLE git_describe_result
            OUTPUT_VARIABLE git_describe
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
        )

        if(git_describe_result EQUAL 0 AND NOT git_describe STREQUAL "")
            set(PRD_GIT_DESCRIBE "${git_describe}")
        endif()

        # Version tags pointing at HEAD. `--points-at` is exact: it reports only
        # tags on this commit, so a release built from an untagged commit sees an
        # empty list and is left alone.
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" tag --points-at HEAD
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/.."
            RESULT_VARIABLE git_tags_result
            OUTPUT_VARIABLE git_tags_raw
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
        )

        if(git_tags_result EQUAL 0 AND NOT git_tags_raw STREQUAL "")
            string(REPLACE "\n" ";" git_tags "${git_tags_raw}")

            foreach(candidate IN LISTS git_tags)
                string(STRIP "${candidate}" candidate)
                if(candidate MATCHES "^v[0-9]")
                    list(APPEND PRD_VERSION_TAGS "${candidate}")
                endif()
            endforeach()
        endif()

        execute_process(
            COMMAND "${GIT_EXECUTABLE}" status --porcelain
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/.."
            RESULT_VARIABLE git_status_result
            OUTPUT_VARIABLE git_status
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
        )

        if(git_status_result EQUAL 0 AND NOT git_status STREQUAL "")
            set(PRD_GIT_DIRTY TRUE)
        endif()

        if(NOT DEFINED ENV{SOURCE_DATE_EPOCH} OR "$ENV{SOURCE_DATE_EPOCH}" STREQUAL "")
            execute_process(
                COMMAND "${GIT_EXECUTABLE}" show -s --format=%ct HEAD
                WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/.."
                RESULT_VARIABLE git_epoch_result
                OUTPUT_VARIABLE git_epoch
                OUTPUT_STRIP_TRAILING_WHITESPACE
                ERROR_QUIET
            )

            if(git_epoch_result EQUAL 0 AND git_epoch MATCHES "^[0-9]+$")
                set(PRD_BUILD_EPOCH "${git_epoch}")
            endif()
        endif()
    endif()

    if(DEFINED ENV{SOURCE_DATE_EPOCH} AND
       NOT "$ENV{SOURCE_DATE_EPOCH}" STREQUAL "")
        if(NOT "$ENV{SOURCE_DATE_EPOCH}" MATCHES "^[0-9]+$")
            message(FATAL_ERROR "SOURCE_DATE_EPOCH must contain an integer Unix timestamp")
        endif()

        set(PRD_BUILD_EPOCH "$ENV{SOURCE_DATE_EPOCH}")
    endif()

    if(PRD_BUILD_PROFILE STREQUAL "release" AND PRD_GIT_DIRTY)
        message(FATAL_ERROR
            "Release builds require a clean Git working tree.\n"
            "  Commit or stash your changes, then rebuild.\n"
            "  Debug and hardware-validation builds may run from a dirty tree.")
    endif()

    # Tag/version agreement (§14.1: "The Git tag shall match firmware/version.txt
    # without the leading v"; §14.4: "Tagged release builds shall fail when tag and
    # version mismatch").
    #
    # Only release builds are gated, and only when HEAD actually carries a version
    # tag. A release built from an untagged commit is a legitimate local operation
    # and stays usable; what must never happen is a v0.2.0 tag producing an image
    # that reports 0.1.0-dev.
    set(PRD_EXPECTED_TAG "v${product_version}")

    if(PRD_VERSION_TAGS)
        list(FIND PRD_VERSION_TAGS "${PRD_EXPECTED_TAG}" matching_index)

        if(matching_index EQUAL -1)
            string(REPLACE ";" ", " found_tags "${PRD_VERSION_TAGS}")

            if(PRD_BUILD_PROFILE STREQUAL "release")
                message(FATAL_ERROR
                    "Release tag does not match firmware/version.txt.\n"
                    "  firmware/version.txt : ${product_version}\n"
                    "  expected tag         : ${PRD_EXPECTED_TAG}\n"
                    "  tag(s) on HEAD       : ${found_tags}\n"
                    "Update firmware/version.txt, or retag this commit. "
                    "version.txt is the canonical version (§14.1).")
            endif()

            # Debug and hardware-validation only warn: a mismatched tag on a
            # development commit is worth knowing about, but it must not stop
            # someone from building and flashing.
            message(WARNING
                "Tag on HEAD (${found_tags}) does not match expected "
                "${PRD_EXPECTED_TAG} from firmware/version.txt")
        else()
            set(PRD_RELEASE_TAG "${PRD_EXPECTED_TAG}")
        endif()
    endif()

    set(PRD_BUILD_PROFILE "${PRD_BUILD_PROFILE}" PARENT_SCOPE)
    set(PRD_BOARD_TARGET "${PRD_BOARD_TARGET}" PARENT_SCOPE)
    set(PRD_GIT_COMMIT "${PRD_GIT_COMMIT}" PARENT_SCOPE)
    set(PRD_GIT_DESCRIBE "${PRD_GIT_DESCRIBE}" PARENT_SCOPE)
    set(PRD_RELEASE_TAG "${PRD_RELEASE_TAG}" PARENT_SCOPE)
    set(PRD_GIT_DIRTY "${PRD_GIT_DIRTY}" PARENT_SCOPE)
    set(PRD_BUILD_EPOCH "${PRD_BUILD_EPOCH}" PARENT_SCOPE)
endfunction()
