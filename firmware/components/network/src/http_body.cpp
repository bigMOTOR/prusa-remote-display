// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include <cstring>

#include "network/http.hpp"

// The bounded body accumulator, kept in its own translation unit so it carries no
// ESP-IDF include and is compiled by the host suite under the full strict flag set.

namespace prd::network {

bool BodyBuffer::append(const std::string_view chunk) {
    if (overflowed_) {
        return false;
    }

    if (storage_ == nullptr) {
        overflowed_ = true;
        return false;
    }

    if (chunk.empty()) {
        return true;
    }

    // Refuses the whole chunk rather than copying the part that fits. A body that
    // arrives one byte over the limit is not a body this device may decode (§9.3), so
    // keeping a prefix would only make the failure harder to see.
    if (chunk.size() > capacity_ - length_) {
        overflowed_ = true;
        return false;
    }

    std::memcpy(storage_ + length_, chunk.data(), chunk.size());
    length_ += chunk.size();
    return true;
}

} // namespace prd::network
