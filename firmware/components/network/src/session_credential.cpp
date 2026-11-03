// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include "session_credential.hpp"

#include <cstddef>
#include <string>

namespace prd::network {
namespace {

constexpr std::string_view kHexDigits = "0123456789ABCDEF";

void append_hex(std::string& text, const std::uint8_t value) {
    text.push_back(kHexDigits[(value >> 4U) & 0x0FU]);
    text.push_back(kHexDigits[value & 0x0FU]);
}

// Overwrites through a volatile pointer so the store cannot be optimised away. The same
// job radio::wipe() does for the driver's copy, repeated here because this translation
// unit is host-compiled and must not reach an ESP-IDF-adjacent header (§11.3).
void wipe(char* const data, const std::size_t size) {
    auto* const bytes = static_cast<volatile char*>(data);
    for (std::size_t index = 0; index < size; ++index) {
        bytes[index] = '\0';
    }
}

} // namespace

// Defined here rather than beside the adapter so the host suite can both call it and check
// what it did — a wipe nobody can test is a comment.
void setup_ap::Session::clear() {
    // BoundedString::clear() only resets the stored length, which leaves the characters in
    // the buffer for whoever reads it next. Overwriting them first is the difference
    // between a secret that is gone and one that is merely unaddressed.
    const std::array<char, 64> zeros{};
    password.assign(std::string_view{zeros.data(), password.view().size()});

    password.clear();
    ssid.clear();
}

app_model::BoundedString<4> device_suffix(const std::array<std::uint8_t, 6>& mac) {
    std::string text;
    append_hex(text, mac[4]);
    append_hex(text, mac[5]);

    app_model::BoundedString<4> suffix;
    suffix.assign(text);
    return suffix;
}

app_model::BoundedString<32> session_ssid(const std::array<std::uint8_t, 6>& mac) {
    std::string text{kSsidPrefix};
    text += device_suffix(mac).view();

    app_model::BoundedString<32> ssid;
    ssid.assign(text);
    return ssid;
}

char draw_password_character(const RandomByte source, void* const context) {
    const auto size = static_cast<std::uint32_t>(kPasswordAlphabet.size());

    // The largest multiple of `size` that fits in a byte. Draws at or above it would wrap
    // unevenly, so they are discarded.
    const std::uint32_t limit = 256U - (256U % size);

    for (;;) {
        const std::uint32_t byte = source(context);
        if (byte < limit) {
            return kPasswordAlphabet[byte % size];
        }
    }
}

setup_ap::Session make_session(const std::array<std::uint8_t, 6>& mac, const RandomByte source,
                               void* const context) {
    setup_ap::Session session;
    session.ssid = session_ssid(mac);

    // A fixed buffer rather than a std::string: a string that outgrew its small-buffer
    // capacity would put the password on the heap, where nothing here could reliably wipe
    // it. This one is a stack local and is overwritten before it goes out of scope.
    std::array<char, setup_ap::kPasswordLength> password{};

    for (char& character : password) {
        character = draw_password_character(source, context);
    }

    session.password.assign(std::string_view{password.data(), password.size()});
    wipe(password.data(), password.size());

    return session;
}

} // namespace prd::network
