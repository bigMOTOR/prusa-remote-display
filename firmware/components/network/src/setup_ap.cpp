// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include "network/setup_ap.hpp"

#include <array>
#include <cstring>
#include <string>

// ESP-IDF headers are not -Wundef clean. Suppress locally (per architecture
// spec §12.3) so the project -Wundef policy still applies to our own code.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wundef"
#include "esp_err.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_wifi.h"
#pragma GCC diagnostic pop

#include "diagnostics/log.hpp"
#include "diagnostics/tags.hpp"
#include "network/wifi.hpp"
#include "radio.hpp"
#include "session_credential.hpp"

// The access-point adapter.
//
// IT CONTAINS NO POLICY, on the same footing as the station adapter: which credential to
// use is session_credential.cpp's, when to provision at all is app_core's, and driver
// ownership is radio.cpp's. What is here is the translation into esp_wifi calls.
//
// NO PHASE MACHINE. The station has one because association can fail in several ways that
// mean different things; an access point is up or it is not. Adding a lifecycle here would
// be machinery with one state.
//
// ⛔ NO CREDENTIAL REACHES A LOG. The SSID is non-secret and is logged. The password is
// copied into wifi_config_t and never appears in a string — owner ruling D1 removed the
// serial-diagnostics path entirely, so there is no build of this firmware in which the
// session password can be printed.

namespace prd::network::setup_ap {
namespace {

// §14.2's setup AP serves one browser. A larger limit would only cost RAM in the driver's
// station table for connections the product has no use for.
constexpr std::uint8_t kMaxConnections = 1;

// A working baseline, and only that. Channel selection has no measurement behind it and
// nothing in M3.5 can evaluate one; M3.11 is where a real access point gets to disagree.
constexpr std::uint8_t kChannel = 1;

bool g_running = false;

std::uint8_t random_byte(void* /*context*/) {
    return static_cast<std::uint8_t>(esp_random() & 0xFFU);
}

} // namespace

Session new_session() {
    // ESP_MAC_WIFI_SOFTAP is readable before the driver starts, which is what lets the
    // session exist before radio::start().
    std::array<std::uint8_t, 6> mac{};
    if (esp_read_mac(mac.data(), ESP_MAC_WIFI_SOFTAP) != ESP_OK) {
        // A zero MAC still yields a valid, joinable network — every device would simply
        // advertise the same name. Worth a warning, not worth refusing to provision.
        diagnostics::log(diagnostics::LogLevel::Warning, diagnostics::tag::kNetwork,
                         "setup ap mac unavailable; the setup network name is not unique");
    }

    return make_session(mac, &random_byte, nullptr);
}

bool start(const Session& session) {
    if (g_running) {
        return true;
    }

    if (session.ssid.empty() || session.password.view().size() < 8) {
        // WPA2-PSK requires at least eight characters. A shorter one would be silently
        // downgraded to an open network by the driver, which is the one failure mode worth
        // refusing outright: an unprotected setup AP accepts anyone in radio range.
        diagnostics::log(diagnostics::LogLevel::Error, diagnostics::tag::kNetwork,
                         "setup ap credential unusable; not starting an open network");
        return false;
    }

    // ⬅️ G1, MILESTONE 3.10: THE STATION MUST SURVIVE THIS. Until now the access point always
    // came up FIRST and the station asked for APSTA when it started (wifi.cpp), so AP-only was
    // always right here. Deliberate reconfiguration reverses that order — the device is already
    // joined and serving a persistent local interface when the user long-presses — and requesting
    // AP-only would drop the station, taking away the LAN reachability owner ruling R6 made
    // permanent and the connection a browser may be reading the panel's address over.
    //
    // Symmetrical with stop() below, which already returns the radio to Station when the station
    // is running rather than stopping the driver. Mode ownership stays central (owner ruling D4):
    // this is a mode SELECTION, not a second place that manages the driver.
    const radio::Mode mode =
        network::running() ? radio::Mode::AccessPointStation : radio::Mode::AccessPoint;

    if (!radio::prepare(mode)) {
        return false;
    }

    const auto ssid = session.ssid.view();
    const auto password = session.password.view();

    wifi_config_t config{};
    std::memcpy(config.ap.ssid, ssid.data(),
                ssid.size() < sizeof(config.ap.ssid) ? ssid.size() : sizeof(config.ap.ssid));
    config.ap.ssid_len = static_cast<std::uint8_t>(
        ssid.size() < sizeof(config.ap.ssid) ? ssid.size() : sizeof(config.ap.ssid));
    std::memcpy(config.ap.password, password.data(),
                password.size() < sizeof(config.ap.password) ? password.size()
                                                             : sizeof(config.ap.password) - 1);

    config.ap.channel = kChannel;
    config.ap.max_connection = kMaxConnections;
    config.ap.authmode = WIFI_AUTH_WPA2_PSK;
    config.ap.ssid_hidden = 0;

    const bool configured = esp_wifi_set_config(WIFI_IF_AP, &config) == ESP_OK;

    // The credential leaves no copy behind (§13.5), on the same discipline as the station
    // adapter: wifi_config_t is a stack local and is wiped before it goes out of scope.
    radio::wipe(&config, sizeof(config));

    if (!configured) {
        diagnostics::log(diagnostics::LogLevel::Error, diagnostics::tag::kNetwork,
                         "setup ap configuration failed");
        return false;
    }

    if (!radio::start()) {
        return false;
    }

    g_running = true;

    // The SSID only. §14.2 shows the SSID and the URL; the password is the panel's job
    // from M3.6 and appears in no log in any profile.
    //
    // ⛔ THE ALLOWANCE BELOW IS BOUND TO THE NEXT CALL AND SPENT ON IT. It replaced a path-wide
    // exemption for this whole file, which would have let a log of `password` two lines down pass
    // unnoticed; a first attempt then allowed anything within three lines, which two one-line calls
    // could both fit inside. So the marker now claims exactly one logging call — only whitespace and
    // comments may separate them — and is consumed by it. A SECOND session-SSID log would need its
    // own marker, and a marker that binds to nothing is itself reported.
    //
    // ⛔ AND IT DOES NOT LAUNDER THIS CALL. Adding a credential here is still a finding: the gate
    // checks the never-loggable class first and unconditionally.
    //
    // credential-log-allowance: device-generated setup SSID (§14.2)
    diagnostics::log(diagnostics::LogLevel::Info, diagnostics::tag::kNetwork,
                     "setup ap up ssid=" + std::string{ssid} +
                         " channel=" + std::to_string(kChannel) +
                         " (no server yet; the password is not logged in any build)");
    return true;
}

void stop() {
    if (!g_running) {
        return;
    }

    // ⬅️ THE STATION MAY STILL NEED THE RADIO — Milestone 3.8, owner ruling D-2. The access
    // point is stopped only after the device has joined the user's network and the browser
    // has seen the result, so stopping the DRIVER here would drop the connection that was
    // just validated. When a station is running the access point is dropped by returning
    // the radio to Station mode; only a device with nothing else on the radio stops it.
    if (network::running()) {
        (void)radio::prepare(radio::Mode::Station);
    } else {
        radio::stop();
    }

    g_running = false;

    diagnostics::log(diagnostics::LogLevel::Info, diagnostics::tag::kNetwork, "setup ap down");
}

bool running() {
    return g_running;
}

std::string_view address() {
    if (!g_running) {
        return {};
    }

    return radio::access_point_address();
}

} // namespace prd::network::setup_ap
