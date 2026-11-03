// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

// The one ordering rule a station bring-up must not get wrong — Milestone 3.11.
//
// INTERNAL HEADER of the network component. Pure — no ESP-IDF, no driver, no clock — so the
// rule below is host-tested against the same code the firmware runs, on the same footing as the
// phase machine in lifecycle.hpp and the retry ladder in backoff.hpp.
//
// ⛔ WHY THIS IS A FILE AND NOT A COMMENT ON A CALL SITE.
//
// Milestone 3.8 fixed a real defect by moving event registration AHEAD of radio::prepare():
// prepare() calls esp_wifi_set_mode(), and with the setup access point already running that
// starts the station interface and posts WIFI_EVENT_STA_START from inside the call — so
// registering afterwards was registering deaf, esp_wifi_connect() was never called, and the
// device idled the full validation bound with zero disconnect reasons. That fix is correct and
// it stays.
//
// What it did not account for is that prepare() is also the ONLY route to the shared stack:
// esp_netif_init() and the default event loop are created inside it. On a CONFIGURED boot no
// access point is raised, so nothing had created the loop, and registration ran against a loop
// that did not exist. It failed, network::start() returned false, and a provisioned device
// never reached its network again after its first reboot — first-time provisioning worked,
// every boot after it did not.
//
// Physically confirmed at Milestone 3.11 T1 on `8e7ddb78`: `station-start-requested apsta=0` →
// `wifi event registration failed` → `wifi station did not start`, with zero mode-set records
// and zero Wi-Fi driver lines anywhere in the boot. Latent since Milestone 3.8; not an M3.9 or
// M3.10 regression, both of which touched wifi.cpp by one licence header line.
//
// ⬅️ THE TWO REQUIREMENTS ARE BOTH REAL AND THEY DO NOT CONFLICT. The stack must exist BEFORE
// registration, and registration must happen BEFORE any mode change. Satisfying one while
// losing the other is what produced both defects, one at each end. Written here as a single
// sequence so a future reader cannot reorder half of it.

namespace prd::network::bringup {

// Ensures the shared stack, then registers the event handlers on it — at most once.
//
// `already` is the caller's own latch. It is set ONLY on full success, so a failed attempt
// leaves the sequence retryable rather than recorded as half-done: a device whose first
// registration failed for a transient reason must be able to try again, and a device that
// succeeded must not register a second handler instance for the same handler.
//
// ⛔ `register_handlers` IS NOT CALLED AT ALL IF `ensure_stack` FAILS. That is the whole rule.
// Registering on a stack that is not there is the defect this function exists to make
// unrepresentable, rather than to detect after the fact.
//
// Both operations must themselves be idempotent — the caller may hold a stack that another
// role already brought up, which is the first-time-provisioning shape, where the setup access
// point created it before any station existed.
template <typename EnsureStack, typename RegisterHandlers>
[[nodiscard]] bool ensure_events_once(bool& already, EnsureStack ensure_stack,
                                      RegisterHandlers register_handlers) {
    if (already) {
        return true;
    }

    if (!ensure_stack()) {
        return false;
    }

    if (!register_handlers()) {
        return false;
    }

    already = true;
    return true;
}

} // namespace prd::network::bringup
