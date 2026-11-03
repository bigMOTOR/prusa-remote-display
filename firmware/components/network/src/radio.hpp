// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

// Ownership of the ESP-IDF Wi-Fi driver, shared by every radio role.
//
// WHY THIS FILE EXISTS. Until Milestone 3.5 the station adapter initialised the whole stack
// itself — esp_netif_init, the default event loop, the STA interface, esp_wifi_init and the
// driver mode — inside network::start(). That was correct while the station was the only
// role, but it had one consequence that only became visible when the access point arrived:
// an un-provisioned device never calls network::start() past its empty-credential guard, so
// NOTHING was initialised, and the setup AP could not assume any of it existed.
//
// So bring-up moved here, in the same order and idempotent, and both roles call it.
//
// ⬅️ MODE OWNERSHIP IS CENTRAL, BY OWNER RULING D4 (2026-08-15). Milestone 3.5 is AP-only:
// the two roles are mutually exclusive, because a device with a usable credential does not
// provision and a device without one has nothing to connect to. Milestone 3.8 breaks that —
// §14.3 has the device join the user's network while the browser is still on the setup AP,
// and dropping the AP to join would kill the page mid-flow. When that lands it is one
// enumerator here and one case in driver_mode(), not a restructuring of either adapter.
//
// THIS HEADER IS INTERNAL to the network component. It names no ESP-IDF type, so a caller
// does not acquire esp_wifi.h by including it, but it is not host-compilable and no
// host-tested translation unit may include it (Bootstrap Spec §11.3).

namespace prd::network::radio {

// Which role the driver is configured for.
enum class Mode : std::uint8_t {
    Station,
    AccessPoint,

    // Both at once — Milestone 3.8, and the case this header predicted.
    //
    // §14.3 has the device join the user's network while the browser is still on the setup
    // access point. Dropping the access point to join would kill the page mid-flow, so the
    // radio carries both roles for the length of the transaction and returns to Station
    // once the browser has seen the result (owner ruling D-2).
    AccessPointStation,
};

// Everything that must happen before a role can be configured, in one call because the
// order is not ours to choose: ESP-IDF's own examples create the default interface for a
// role BEFORE esp_wifi_init, and splitting the sequence across two entry points is how a
// caller gets it wrong. Safe to call repeatedly and from either role.
//
// Performs, each step at most once: esp_netif_init, the default event loop, the default
// interface for `mode`, esp_wifi_init and esp_wifi_set_storage(WIFI_STORAGE_RAM). Then
// selects the driver mode, which is the one part that runs on every call.
//
// ⛔ THE STORAGE SETTING IS NOT INCIDENTAL. ESP-IDF defaults to persisting every
// esp_wifi_set_config() into NVS, which would put both the station's password and the
// setup access point's per-session password into flash in plaintext. radio.cpp explains it
// at the call; it is named here because it is the reason this function must be the only
// route to a configured driver.
[[nodiscard]] bool prepare(Mode mode);

// The ESP-IDF event-handler signature, spelled out rather than included.
//
// ⬅️ WHY NOT esp_event_handler_t. This header deliberately names no ESP-IDF type (see the note
// at the top), so a caller does not acquire esp_event.h by including it. radio.cpp asserts this
// spelling against the pinned SDK's own typedef, so a signature change upstream fails the build
// at the assertion rather than silently at a call site.
using EventHandler = void (*)(void* handler_arg, const char* base, std::int32_t id, void* data);

// Brings the shared stack up if it is not up yet, then registers `handler` for every
// WIFI_EVENT and IP_EVENT. Idempotent: the handler is registered once, however many callers ask.
//
// ⛔ CALL THIS INSTEAD OF REGISTERING DIRECTLY, AND CALL IT BEFORE prepare(). It fuses the two
// halves of a defect this project has now hit from both ends — the stack must exist before
// registration (Milestone 3.11 T1), and registration must precede the mode change that can post
// STA_START (Milestone 3.8 defect 2). Because one call does both, in order, neither half can be
// satisfied while the other is lost. bringup.hpp carries the reasoning and the host-tested rule.
//
// A failed call leaves nothing latched, so a later attempt runs the whole sequence again.
[[nodiscard]] bool ensure_events(EventHandler handler, void* context);

// esp_wifi_start(). Idempotent: the driver is started once regardless of how many roles
// asked, which is what makes the Milestone 3.8 coexistence case work without a second path.
[[nodiscard]] bool start();

// esp_wifi_stop(). Does nothing when the driver was never started.
void stop();

// The access point's own IPv4 address, e.g. "192.168.4.1", as ESP-IDF's DHCP server
// configured it. Empty until the access-point interface exists.
//
// READ, NOT INVENTED. The address is a property of the netif, and hard-coding the
// well-known default would be a second source of truth that could silently disagree with
// the one a client is actually given.
[[nodiscard]] std::string_view access_point_address();

// Overwrites `size` bytes at `data` through a volatile pointer so the compiler cannot
// elide the store. Used on every buffer that has held a credential — §13.5's "embedded in
// crash dumps where avoidable" is about what a later stack reuse can surface.
void wipe(void* data, std::size_t size);

} // namespace prd::network::radio
