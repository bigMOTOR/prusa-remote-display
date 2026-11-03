// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <array>
#include <cstdint>
#include <string_view>

#include "network/setup_ap.hpp"

// Generation of a provisioning session's access-point credential — owner ruling R1.
//
// PURE, AND HOST-TESTED. No ESP-IDF header, no global state, no clock. The entropy source
// is injected exactly as backoff.cpp's Jitter is: the adapter passes esp_random, the host
// suite passes a known sequence, and the property that matters — an unbiased draw over the
// alphabet — is checkable without a board.

namespace prd::network {

// The password alphabet.
//
// AMBIGUOUS GLYPHS ARE ABSENT ON PURPOSE. From M3.6 this password is read off a 2.8-inch
// panel at arm's length and typed into a phone. `0`/`O` and `1`/`l`/`I` are the pairs that
// get misread there, and a user who cannot join the setup network has no second channel to
// check against — there is no serial output and no label on the device.
//
// 57 characters, so 12 of them carry just over 70 bits.
inline constexpr std::string_view kPasswordAlphabet =
    "23456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";

// The SSID's fixed part. §14.2 wants "a uniquely named WPA2-protected setup AP"; the
// device suffix below supplies the unique half.
inline constexpr std::string_view kSsidPrefix = "PRD-Setup-";

// Where a byte of entropy comes from. A function pointer and a context, matching
// ObservationSink rather than introducing a second convention for the same thing.
using RandomByte = std::uint8_t (*)(void* context);

// The device's stable four-character identity suffix: the last two bytes of `mac` as
// uppercase hex, e.g. "0C01".
//
// The last two bytes, not the first three: the first three are the OUI and identical
// across every ESP32 module, so a suffix taken from them would make every device's setup
// network look the same.
//
// ⬅️ ONE DERIVATION, TWO IDENTITIES. Milestone 3.5 used this for the setup SSID; Milestone
// 3.9 uses the same suffix for the persistent mDNS hostname, by owner ruling D. Deriving it
// twice would let the setup network and the LAN name disagree about which device they name —
// which is exactly the confusion a stable suffix exists to prevent.
[[nodiscard]] app_model::BoundedString<4> device_suffix(const std::array<std::uint8_t, 6>& mac);

// Builds the SSID from kSsidPrefix and device_suffix(mac).
[[nodiscard]] app_model::BoundedString<32> session_ssid(const std::array<std::uint8_t, 6>& mac);

// Draws one character from kPasswordAlphabet, uniformly.
//
// REJECTION SAMPLING, NOT MODULO. 256 is not a multiple of 57, so `byte % 57` would make
// the first 28 characters of the alphabet meaningfully likelier than the rest and cost
// real entropy from a password that already has to be short enough to read off a panel.
// Bytes at or above the largest multiple of the alphabet size are discarded and redrawn.
[[nodiscard]] char draw_password_character(RandomByte source, void* context);

// A full session: SSID from the MAC, password drawn character by character.
[[nodiscard]] setup_ap::Session make_session(const std::array<std::uint8_t, 6>& mac,
                                             RandomByte source, void* context);

} // namespace prd::network
