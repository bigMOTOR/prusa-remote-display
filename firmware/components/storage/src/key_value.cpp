// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include "storage/key_value.hpp"

// The pure half of the key-value surface: the diagnostic labels and the key bound.
//
// PURE. No ESP-IDF header, so the host suite compiles this file directly and every test
// that exercises a fake store reports the same strings the firmware logs.

namespace prd::storage {

std::string_view log_label(const Result result) {
    switch (result) {
    case Result::Ok:
        return "ok";

    case Result::NotFound:
        return "not-found";

    case Result::TooLarge:
        return "too-large";

    case Result::InvalidKey:
        return "invalid-key";

    case Result::Failed:
        return "failed";
    }

    return "failed";
}

} // namespace prd::storage
