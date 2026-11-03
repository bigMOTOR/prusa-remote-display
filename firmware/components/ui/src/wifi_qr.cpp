// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include "wifi_qr.hpp"

#include <array>

namespace prd::ui::wifi_qr {
namespace {

constexpr std::string_view kPrefix = "WIFI:T:WPA;S:";
constexpr std::string_view kPasswordField = ";P:";
constexpr std::string_view kTerminator = ";;";

static_assert(kPrefix.size() + kPasswordField.size() + kTerminator.size() == kPayloadOverhead,
              "the overhead budget must match the literals the encoder actually emits");

// The four characters that are structural in the Wi-Fi QR format.
//
// `\` first, because it is the escape character itself: a value containing a backslash has
// to survive a round trip, and forgetting it is the classic way an escaper corrupts input
// it was written to protect.
[[nodiscard]] constexpr bool is_structural(const char character) {
    return character == '\\' || character == ';' || character == ',' || character == ':';
}

using Buffer = std::array<char, kMaxPayloadLength>;

[[nodiscard]] bool append(Buffer& buffer, std::size_t& used, const std::string_view text) {
    if (used + text.size() > buffer.size()) {
        return false;
    }

    for (const char character : text) {
        buffer[used] = character;
        ++used;
    }

    return true;
}

[[nodiscard]] bool append_escaped(Buffer& buffer, std::size_t& used, const std::string_view text) {
    for (const char character : text) {
        const std::size_t needed = is_structural(character) ? 2U : 1U;
        if (used + needed > buffer.size()) {
            return false;
        }

        if (is_structural(character)) {
            buffer[used] = '\\';
            ++used;
        }

        buffer[used] = character;
        ++used;
    }

    return true;
}

void wipe_bytes(void* const data, const std::size_t size) {
    auto* const bytes = static_cast<volatile unsigned char*>(data);
    for (std::size_t index = 0; index < size; ++index) {
        bytes[index] = 0;
    }
}

} // namespace

bool build(const std::string_view ssid, const std::string_view password, Payload& payload) {
    payload.clear();

    // An empty SSID names no network and an empty password produces a code that offers to
    // join an open network that does not exist. Both are refusals, not truncations.
    if (ssid.empty() || password.empty()) {
        return false;
    }

    if (ssid.size() > kMaxSsid || password.size() > kMaxPassword) {
        return false;
    }

    // The buffer holds the password in plaintext for the length of this function and is
    // wiped on every path out, including the failure paths.
    Buffer buffer{};
    std::size_t used = 0;

    const bool built = append(buffer, used, kPrefix) && append_escaped(buffer, used, ssid) &&
                       append(buffer, used, kPasswordField) &&
                       append_escaped(buffer, used, password) && append(buffer, used, kTerminator);

    if (built) {
        payload.assign(std::string_view{buffer.data(), used});
    }

    wipe_bytes(buffer.data(), buffer.size());

    // A truncated payload would encode into a QR code that scans and then fails to join,
    // which is worse than no code at all. The capacity arithmetic above makes this
    // unreachable; it is checked because "unreachable" is a claim about today's inputs.
    if (built && payload.truncated()) {
        wipe(payload);
        return false;
    }

    return built;
}

void wipe(Payload& payload) {
    // Overwrite the characters that were written, then drop the length.
    //
    // THE SAME IDIOM setup_ap::Session::clear() USES, deliberately, rather than a second
    // one: assign an equal-length run of NULs so BoundedString rewrites the bytes in place,
    // then clear() so the length goes too. clear() alone would leave the characters in the
    // buffer for whoever reads it next, which is the difference between a secret that is
    // gone and one that is merely unaddressed.
    const std::array<char, kMaxPayloadLength> zeros{};
    (void)payload.assign(std::string_view{zeros.data(), payload.view().size()});
    payload.clear();
}

} // namespace prd::ui::wifi_qr
