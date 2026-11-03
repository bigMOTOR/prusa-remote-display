// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstddef>
#include <string_view>

#include "app_model/bounded_string.hpp"

// The Wi-Fi QR payload for the setup access point.
//
// PURE, AND HOST-TESTED. No ESP-IDF header, no LVGL header, no global state. The renderer
// hands it an SSID and a password and receives bytes; turning those bytes into modules is
// the adapter's job and is the only part that needs a panel.
//
// WHY THIS LIVES IN `ui` RATHER THAN `network`. `ui` already holds the session password —
// it must, because rasterising characters is the whole reason Milestone 3.6 exists — so
// encoding that same value for display is a presentation act. The alternative would have
// `ui` call into `network`, which architecture spec §4.2's dependency direction forbids and
// which ui.hpp's own "push, not pull" rule already rules out.
//
// ⛔ NOT UNDER src/scenes/. tools/check-tokens.sh applies its "user-visible literal cached
// in a constant" rule to scenes/ and components/, and this file's format literals would
// fail it — correctly, since that rule exists to keep English out of scenes. The payload is
// not English; it is a wire format, and it belongs outside that boundary.
//
// THE FORMAT is the de-facto Wi-Fi QR convention that phone cameras already understand:
//
//     WIFI:T:WPA;S:<ssid>;P:<password>;;
//
// ⛔ ESCAPING IS NOT OPTIONAL, AND IS NOT WRITTEN AGAINST THE CURRENT ALPHABET. `\`, `;`,
// `,` and `:` are structural in that format and are backslash-escaped inside both values.
// Milestone 3.5's generated password happens to contain none of them and its SSID happens
// to contain none either — but that alphabet is a recorded WORKING BASELINE, not product
// policy, and a naive concatenation would be a correctness bug the day it changes. The
// encoder is written against arbitrary input and the host tests exercise the separators
// directly.

namespace prd::ui::wifi_qr {

// The widest values the session channel can carry — SetupSession's own capacities.
inline constexpr std::size_t kMaxSsid = 32;
inline constexpr std::size_t kMaxPassword = 63;

// Every character of both values may need an escape, so both budget double.
inline constexpr std::size_t kPayloadOverhead = 18; // "WIFI:T:WPA;S:" + ";P:" + ";;"
inline constexpr std::size_t kMaxPayloadLength =
    kPayloadOverhead + (2 * kMaxSsid) + (2 * kMaxPassword);

using Payload = app_model::BoundedString<kMaxPayloadLength>;

// Builds the payload for one session.
//
// Returns false — leaving `payload` empty — when the session cannot describe a joinable
// network: either value empty, or either value longer than the channel can carry. A
// caller that gets false must draw no code rather than draw one that fails to join.
[[nodiscard]] bool build(std::string_view ssid, std::string_view password, Payload& payload);

// Overwrites a payload's characters and then empties it.
//
// The payload contains the session password in plaintext. This is the same obligation
// `network`'s radio::wipe() carries for the driver's copy and setup_ap::Session::clear()
// carries for the generated one, and it is deliberately the same idiom — it is spelled
// again here only because `ui` cannot reach either of those (§4.2).
void wipe(Payload& payload);

} // namespace prd::ui::wifi_qr
