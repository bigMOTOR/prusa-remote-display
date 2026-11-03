// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include "network/discovery.hpp"

#include <array>
#include <cstdint>
#include <string>

// ESP-IDF headers are not -Wundef clean. Suppress locally (per architecture
// spec §12.3) so the project -Wundef policy still applies to our own code.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wundef"
#include "esp_err.h"
#include "esp_mac.h"
#include "mdns.h"
#pragma GCC diagnostic pop

#include "diagnostics/log.hpp"
#include "diagnostics/tags.hpp"
#include "discovery_identity.hpp"
#include "session_credential.hpp"

// The mDNS adapter.
//
// IT CONTAINS NO POLICY, on the same footing as the station and access-point adapters: what
// the device is called is discovery_identity.cpp's, when to advertise at all is app_core's,
// and the port belongs to the component that listens on it. What is here is the translation
// into mdns calls.
//
// ⛔ NOTHING SECRET IS ADVERTISED, AND THERE ARE NO TXT RECORDS. A TXT record is a broadcast
// to everyone on the segment, so the temptation it creates — publishing a serial, a firmware
// version, a printer address, a provisioning state — is exactly what §13.5 and §13.6 rule out
// putting on a wire the product does not control. The advertisement carries a name, a service
// type and a port, which is all a browser needs to find the page.

namespace prd::network::discovery {
namespace {

bool g_running = false;

// Resolved once, from the MAC, and then stable for the life of the boot.
//
// SAME SUFFIX AS THE SETUP NETWORK, by owner ruling D. ESP_MAC_WIFI_SOFTAP is the address
// session_ssid() uses, so the network a user joined during setup and the name they reach the
// device at afterwards identify the same device — reading a different MAC here would produce
// two names for one board and no way to tell they matched.
app_model::BoundedString<24> g_hostname;
app_model::BoundedString<32> g_local_name;
app_model::BoundedString<48> g_instance;

void resolve_identity() {
    if (!g_hostname.empty()) {
        return;
    }

    std::array<std::uint8_t, 6> mac{};
    if (esp_read_mac(mac.data(), ESP_MAC_WIFI_SOFTAP) != ESP_OK) {
        // A zero MAC still yields a valid, resolvable name — every device would simply
        // advertise the same one, and two on a segment would collide. Worth a warning; not
        // worth refusing to be reachable.
        diagnostics::log(diagnostics::LogLevel::Warning, diagnostics::tag::kNetwork,
                         "mdns mac unavailable; the advertised name is not unique");
    }

    const auto suffix = device_suffix(mac);
    g_hostname = discovery_identity::hostname_for(suffix.view());
    g_local_name = discovery_identity::local_name_for(suffix.view());
    g_instance = discovery_identity::service_instance_for(suffix.view());
}

} // namespace

bool start(const std::uint16_t port) {
    resolve_identity();

    if (g_running) {
        return true;
    }

    if (mdns_init() != ESP_OK) {
        diagnostics::log(diagnostics::LogLevel::Warning, diagnostics::tag::kNetwork,
                         "mdns did not start; the display is reachable by address only");
        return false;
    }

    if (mdns_hostname_set(g_hostname.c_str()) != ESP_OK) {
        // Started but nameless is worse than not started: the device would answer mDNS
        // queries under whatever default the library chose, which is a second name nothing
        // else in this firmware knows about.
        mdns_free();
        diagnostics::log(diagnostics::LogLevel::Warning, diagnostics::tag::kNetwork,
                         "mdns hostname refused; advertisement abandoned");
        return false;
    }

    if (mdns_instance_name_set(g_instance.c_str()) != ESP_OK) {
        // Cosmetic: the instance name is what a discovery browser lists. A device that is
        // resolvable but unlabelled is still reachable, so this does not abandon the
        // advertisement.
        diagnostics::log(diagnostics::LogLevel::Warning, diagnostics::tag::kNetwork,
                         "mdns instance name refused; the service is unlabelled");
    }

    // No TXT records — see the header comment above. nullptr and 0, explicitly.
    if (mdns_service_add(nullptr, std::string{discovery_identity::kServiceType}.c_str(),
                         std::string{discovery_identity::kServiceProtocol}.c_str(), port, nullptr,
                         0) != ESP_OK) {
        diagnostics::log(diagnostics::LogLevel::Warning, diagnostics::tag::kNetwork,
                         "mdns service not advertised; the name resolves but is not browsable");
    }

    g_running = true;

    diagnostics::log(diagnostics::LogLevel::Info, diagnostics::tag::kNetwork,
                     "mdns up name=" + std::string{g_local_name.view()} +
                         " port=" + std::to_string(port));
    return true;
}

void stop() {
    if (!g_running) {
        return;
    }

    (void)mdns_service_remove(std::string{discovery_identity::kServiceType}.c_str(),
                              std::string{discovery_identity::kServiceProtocol}.c_str());
    mdns_free();
    g_running = false;

    diagnostics::log(diagnostics::LogLevel::Info, diagnostics::tag::kNetwork, "mdns down");
}

bool running() {
    return g_running;
}

std::string_view hostname() {
    resolve_identity();
    return g_hostname.view();
}

std::string_view local_name() {
    resolve_identity();
    return g_local_name.view();
}

} // namespace prd::network::discovery
