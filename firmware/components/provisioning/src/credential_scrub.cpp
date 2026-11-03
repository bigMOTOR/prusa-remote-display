// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include "credential_scrub.hpp"

namespace prd::provisioning::scrub {

void bytes(void* const data, const std::size_t size) {
    if (data == nullptr) {
        return;
    }

    auto* const target = static_cast<volatile unsigned char*>(data);
    for (std::size_t index = 0; index < size; ++index) {
        target[index] = 0;
    }
}

void text(char* const value) {
    if (value == nullptr) {
        return;
    }

    // Walked rather than measured then wiped: strlen() followed by a loop reads the buffer twice and
    // gives a compiler two chances to decide the second read is unnecessary. Volatile stores through
    // the same pointer that reads the terminator keep both halves observable.
    auto* target = static_cast<volatile char*>(value);
    while (*target != '\0') {
        *target = '\0';
        ++target;
    }
}

} // namespace prd::provisioning::scrub
