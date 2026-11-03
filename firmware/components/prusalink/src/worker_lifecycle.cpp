// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include "worker_lifecycle.hpp"

#include <limits>

namespace prd::prusalink::lifecycle {

Generation next_generation(const Generation current) {
    if (current == std::numeric_limits<Generation>::max()) {
        return 1;
    }

    return current + 1;
}

bool may_publish(const Generation mine, const Generation current) {
    return mine != kNoLease && mine == current;
}

bool should_exit(const Generation mine, const Generation current) {
    return !may_publish(mine, current);
}

bool may_start(const bool worker_alive) {
    return !worker_alive;
}

} // namespace prd::prusalink::lifecycle
