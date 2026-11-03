# SPDX-FileCopyrightText: 2026 Mykola Fiantsev
# SPDX-License-Identifier: GPL-3.0-only

# Stored credentials (Architecture Spec §13.2 SecretConfigV1, §13.5 credential handling).
#
# NO ESP-IDF ADAPTER TU, as with `config`: the component talks only to storage::KeyValue,
# so every translation unit is standard C++17 and the whole component is compiled and
# tested by the host suite.
#
# IT DOES NOT REQUIRE `config`, AND MUST NOT. Owner ruling, 2026-08-15: `secrets` knows
# nothing about non-secret configuration and `config` knows nothing about secrets;
# `app_core` is the composition root that sees both.

idf_component_register(
    SRCS
        "src/secret_schema.cpp"
        "src/secrets.cpp"
    INCLUDE_DIRS
        "include"
    REQUIRES
        app_model
        storage
)

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)

include(${CMAKE_SOURCE_DIR}/cmake/compiler_policy.cmake)
prd_apply_compiler_policy(${COMPONENT_LIB})
