# SPDX-FileCopyrightText: 2026 Mykola Fiantsev
# SPDX-License-Identifier: GPL-3.0-only

# Persistent storage primitives (Architecture Spec §5.10). Owns NVS bring-up and the
# namespace boundary. No other component may include nvs.h.

idf_component_register(
    SRCS
        "src/bring_up.cpp"
        "src/checksum.cpp"
        "src/key_value.cpp"
        "src/namespaces.cpp"
        "src/nvs_key_value.cpp"
        "src/storage.cpp"
    INCLUDE_DIRS
        "include"
    REQUIRES
        app_model
    PRIV_REQUIRES
        diagnostics
        nvs_flash
)

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)

include(${CMAKE_SOURCE_DIR}/cmake/compiler_policy.cmake)
prd_apply_compiler_policy(${COMPONENT_LIB})

# storage.cpp and nvs_key_value.cpp are the ESP-IDF adapter TUs. ESP-IDF C headers they
# include are not -Wundef/-Wpedantic clean (undefined macros, anonymous structs, and
# #include_next -- the last cannot be silenced by an in-file diagnostic pragma). Relax
# only these two warnings for only these files, per architecture spec §12.3; every other
# project warning still applies. Applied after the policy so it wins.
#
# bring_up.cpp, key_value.cpp and namespaces.cpp include no ESP-IDF headers and keep the
# full strict flags. All three are also compiled by the host test suite, which is the
# point of keeping them clean.
set_source_files_properties(
    "src/storage.cpp"
    "src/nvs_key_value.cpp"
    PROPERTIES
        COMPILE_OPTIONS "-Wno-undef;-Wno-pedantic"
)
