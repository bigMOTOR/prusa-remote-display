// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include "driver_credential.hpp"

#include <cstring>

namespace prd::network::driver_credential {
namespace {

// Copies `text` into the first bytes of `field` and zeroes the rest.
//
// memcpy with a null source is undefined even for zero length, and an empty string_view may
// legitimately carry one — the same guard BoundedString::assign() states for the same reason.
void fill(const std::string_view text, std::uint8_t* const field, const std::size_t field_size) {
    if (!text.empty()) {
        std::memcpy(field, text.data(), text.size());
    }

    std::memset(field + text.size(), 0, field_size - text.size());
}

} // namespace

bool copy_ssid(const std::string_view ssid, std::uint8_t* const field,
               const std::size_t field_size) {
    if (field == nullptr || ssid.empty() || ssid.size() > field_size) {
        return false;
    }

    // `>` and not `>=`, which is the entire correction. A name of exactly field_size fills the array
    // and is complete without a terminator; see the header on wifi_sta_config_t having no ssid_len.
    fill(ssid, field, field_size);
    return true;
}

bool copy_password(const std::string_view password, std::uint8_t* const field,
                   const std::size_t field_size) {
    if (field == nullptr || field_size == 0 || password.size() >= field_size) {
        return false;
    }

    // `>=`, so one byte is always left for the terminator that fill() writes as part of zeroing the
    // remainder. An empty passphrase is an open network and yields an all-zero field.
    fill(password, field, field_size);
    return true;
}

} // namespace prd::network::driver_credential
