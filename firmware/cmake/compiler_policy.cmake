# SPDX-FileCopyrightText: 2026 Mykola Fiantsev
# SPDX-License-Identifier: GPL-3.0-only

function(prd_apply_compiler_policy target)
    target_compile_options(${target} PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Wshadow
        -Wconversion
        -Wsign-conversion
        -Wformat=2
        -Wundef
        -Wdouble-promotion
        -Wnull-dereference
        -Werror=return-type
        -Werror=format

        $<$<COMPILE_LANGUAGE:C>:-Werror=implicit-function-declaration>
    )
endfunction()
