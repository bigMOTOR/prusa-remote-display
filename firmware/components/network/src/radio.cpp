// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include "radio.hpp"

#include <array>
#include <cstdio>
#include <type_traits>

// ESP-IDF headers are not -Wundef clean. Suppress locally (per architecture
// spec §12.3) so the project -Wundef policy still applies to our own code.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wundef"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_private/wifi.h"
#include "esp_wifi.h"
#pragma GCC diagnostic pop

#include "bringup.hpp"
#include "diagnostics/log.hpp"
#include "diagnostics/tags.hpp"
#include "wifi_log_policy.hpp"

namespace prd::network::radio {
namespace {

bool g_stack_ready = false;

// Latched only once the stack exists AND both handler registrations succeeded, so a
// failure leaves the sequence retryable rather than recorded as done. See bringup.hpp.
bool g_events_ready = false;

bool g_driver_ready = false;
bool g_started = false;

esp_netif_t* g_station_netif = nullptr;
esp_netif_t* g_access_point_netif = nullptr;

// The one place a role becomes a driver mode.
//
// No default label: adding a role without deciding its mode must not compile.
[[nodiscard]] wifi_mode_t driver_mode(const Mode mode) {
    switch (mode) {
    case Mode::Station:
        return WIFI_MODE_STA;

    case Mode::AccessPoint:
        return WIFI_MODE_AP;

    case Mode::AccessPointStation:
        return WIFI_MODE_APSTA;
    }

    return WIFI_MODE_NULL;
}

[[nodiscard]] bool ensure_stack() {
    if (g_stack_ready) {
        return true;
    }

    // Both calls are idempotent in ESP-IDF but report it differently: esp_netif_init()
    // returns ESP_OK on a repeat call, while esp_event_loop_create_default() returns
    // ESP_ERR_INVALID_STATE. Treating that as a failure would make a second role's
    // bring-up fail for having succeeded already.
    if (esp_netif_init() != ESP_OK) {
        diagnostics::log(diagnostics::LogLevel::Error, diagnostics::tag::kNetwork,
                         "wifi netif unavailable");
        return false;
    }

    const esp_err_t loop = esp_event_loop_create_default();
    if (loop != ESP_OK && loop != ESP_ERR_INVALID_STATE) {
        diagnostics::log(diagnostics::LogLevel::Error, diagnostics::tag::kNetwork,
                         "wifi event loop unavailable");
        return false;
    }

    g_stack_ready = true;
    return true;
}

// Created once per role and kept. Creating one twice would leak the first and leave the
// netif layer holding two default handles for the same interface.
void ensure_netif(const Mode mode) {
    switch (mode) {
    case Mode::Station:
        if (g_station_netif == nullptr) {
            g_station_netif = esp_netif_create_default_wifi_sta();
        }
        break;

    case Mode::AccessPoint:
        if (g_access_point_netif == nullptr) {
            g_access_point_netif = esp_netif_create_default_wifi_ap();
        }
        break;

    // Both interfaces, because both roles are about to carry traffic. Each is still created
    // at most once, so arriving here after either single role has already run is safe.
    case Mode::AccessPointStation:
        if (g_station_netif == nullptr) {
            g_station_netif = esp_netif_create_default_wifi_sta();
        }
        if (g_access_point_netif == nullptr) {
            g_access_point_netif = esp_netif_create_default_wifi_ap();
        }
        break;
    }
}

[[nodiscard]] bool ensure_driver() {
    if (g_driver_ready) {
        return true;
    }

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    if (esp_wifi_init(&init) != ESP_OK) {
        diagnostics::log(diagnostics::LogLevel::Error, diagnostics::tag::kNetwork,
                         "wifi driver init failed");
        return false;
    }

    // ⛔ THE VENDOR STACK'S OWN LOG LEVEL, LOWERED BEFORE IT CAN ASSOCIATE — MILESTONE 3.11 T19.
    // At Info, libnet80211 prints "connected with <ssid> ... bssid = <router mac>" on every
    // association, and §13.2 puts `wifiSsid` inside SecretConfigV1 while §13.5 forbids credentials
    // in logs. See wifi_log_policy.hpp for why this is a level rather than a redacting filter, why
    // esp_log_level_set("wifi", ...) does not reach these lines, and why it must run here.
    //
    // ⬅️ HERE IS BOTH LATE AND EARLY ENOUGH. esp_wifi_init() has just overwritten the level from
    // CONFIG_LOG_MAXIMUM_LEVEL, so earlier would be undone; and the radio cannot associate until
    // esp_wifi_start() below, so nothing sensitive can have been emitted yet. This is the only
    // esp_wifi_init() call site in the project and it runs once, behind g_driver_ready.
    static_assert(static_cast<int>(wifi_log_policy::Level::None) == WIFI_LOG_NONE);
    static_assert(static_cast<int>(wifi_log_policy::Level::Error) == WIFI_LOG_ERROR);
    static_assert(static_cast<int>(wifi_log_policy::Level::Warning) == WIFI_LOG_WARNING);
    static_assert(static_cast<int>(wifi_log_policy::Level::Info) == WIFI_LOG_INFO);
    static_assert(static_cast<int>(wifi_log_policy::Level::Debug) == WIFI_LOG_DEBUG);
    static_assert(static_cast<int>(wifi_log_policy::Level::Verbose) == WIFI_LOG_VERBOSE);

    // Reported, not ignored: a stack left at Info would be a credential leak, so a refusal here is
    // worth a line. It cannot fail for the value we pass — the API rejects only invalid levels —
    // which is why the level itself is a compile-time constant the header has already checked.
    if (esp_wifi_internal_set_log_level(static_cast<wifi_log_level_t>(wifi_log_policy::kApplied)) !=
        ESP_OK) {
        diagnostics::log(diagnostics::LogLevel::Error, diagnostics::tag::kNetwork,
                         "could not lower the wifi stack log level; refusing to continue because "
                         "the stack would log the network name");
        return false;
    }

    // ⛔ RAM, NOT FLASH — AND THE DEFAULT IS FLASH.
    //
    // esp_wifi.h in the pinned v5.5.4 says of esp_wifi_set_config: "The configuration will
    // be stored in NVS for station and soft-AP", and WIFI_STORAGE_FLASH — "all
    // configuration will store in both memory and flash" — is what the driver starts with.
    // So every esp_wifi_set_config() call writes its password into the `nvs` partition, in
    // plaintext, under the driver's own namespace.
    //
    // That breaks two things at once. Owner ruling R1 makes the setup access point's
    // password a per-session value that is NEVER PERSISTED, and this would persist it on
    // every boot that provisions. And the station's credential — which `secrets` stores
    // deliberately, with a generation, a checksum and structural redaction — would acquire
    // a second uncontrolled copy that no part of this project manages, migrates or erases.
    //
    // Nothing is lost by switching it off: the driver's persisted copy exists to let an
    // application reconnect without supplying credentials, and this one always supplies
    // them — app_core reads `secrets` on every boot and hands them to network::start().
    //
    // Verified against the pinned ESP-IDF's own header rather than assumed, which is the
    // rule the Milestone 3.1 eFuse audit left behind.
    if (esp_wifi_set_storage(WIFI_STORAGE_RAM) != ESP_OK) {
        diagnostics::log(diagnostics::LogLevel::Error, diagnostics::tag::kNetwork,
                         "wifi storage could not be set to RAM; refusing to continue rather "
                         "than persist a credential to flash");
        return false;
    }

    g_driver_ready = true;
    return true;
}

} // namespace

bool prepare(const Mode mode) {
    if (!ensure_stack()) {
        return false;
    }

    // Before ensure_driver(), deliberately: ESP-IDF's own station and softAP examples both
    // create the default interface ahead of esp_wifi_init, and this sequence exists to keep
    // that order rather than leave it to each caller.
    ensure_netif(mode);

    if (!ensure_driver()) {
        return false;
    }

    // Bracketed because a mode change is not free: on one radio, adding a station to a running
    // access point can move the shared channel, and the cost of that is exactly what the
    // provisioning timeline needs to attribute rather than infer.
    diagnostics::timeline(diagnostics::tag::kNetwork, "mode-set-begin", "mode",
                          static_cast<std::int64_t>(mode));

    if (esp_wifi_set_mode(driver_mode(mode)) != ESP_OK) {
        diagnostics::log(diagnostics::LogLevel::Error, diagnostics::tag::kNetwork,
                         "wifi mode selection failed");
        return false;
    }

    diagnostics::timeline(diagnostics::tag::kNetwork, "mode-set-end", "mode",
                          static_cast<std::int64_t>(mode));

    return true;
}

bool ensure_events(const EventHandler handler, void* const context) {
    // ⛔ THE HAND-SPELLED SIGNATURE MUST STILL BE THE SDK'S. radio.hpp names no ESP-IDF type on
    // purpose, so EventHandler is written out by hand; this is what stops that spelling drifting
    // away from the pinned v5.5.4 typedef without anyone noticing. Checked against the header
    // rather than assumed, which is the rule the Milestone 3.1 eFuse audit left behind.
    static_assert(std::is_same_v<EventHandler, esp_event_handler_t>,
                  "radio.hpp spells the ESP-IDF event-handler signature by hand so the header "
                  "names no ESP-IDF type; the pinned SDK's typedef must still match it");

    // The stack first, then the handlers, and never the handlers alone — bringup.hpp explains
    // why both halves live in one call. ensure_stack() is itself idempotent, so the
    // first-time-provisioning shape, where the setup access point already brought the stack up,
    // takes the same path and simply finds there is nothing left to create.
    const bool ready = bringup::ensure_events_once(
        g_events_ready, [] { return ensure_stack(); },
        [handler, context] {
            return esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, handler,
                                                       context, nullptr) == ESP_OK &&
                   esp_event_handler_instance_register(IP_EVENT, ESP_EVENT_ANY_ID, handler, context,
                                                       nullptr) == ESP_OK;
        });

    if (!ready) {
        diagnostics::log(diagnostics::LogLevel::Error, diagnostics::tag::kNetwork,
                         "wifi event registration failed");
    }

    return ready;
}

bool start() {
    if (g_started) {
        return true;
    }

    if (esp_wifi_start() != ESP_OK) {
        diagnostics::log(diagnostics::LogLevel::Error, diagnostics::tag::kNetwork,
                         "wifi start failed");
        return false;
    }

    g_started = true;
    return true;
}

void stop() {
    if (!g_started) {
        return;
    }

    (void)esp_wifi_stop();
    g_started = false;
}

std::string_view access_point_address() {
    if (g_access_point_netif == nullptr) {
        return {};
    }

    esp_netif_ip_info_t info{};
    if (esp_netif_get_ip_info(g_access_point_netif, &info) != ESP_OK) {
        return {};
    }

    // Formatted once into storage this function owns; the caller copies what it needs.
    static std::array<char, 16> text{};
    std::snprintf(text.data(), text.size(), IPSTR, IP2STR(&info.ip));
    return {text.data()};
}

void wipe(void* const data, const std::size_t size) {
    auto* const bytes = static_cast<volatile unsigned char*>(data);
    for (std::size_t index = 0; index < size; ++index) {
        bytes[index] = 0;
    }
}

} // namespace prd::network::radio
