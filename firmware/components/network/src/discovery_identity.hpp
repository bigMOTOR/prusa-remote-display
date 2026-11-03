// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <string_view>

#include "app_model/bounded_string.hpp"

// The device's stable local-network identity — owner ruling D of Milestone 3.9.
//
// PURE, AND HOST-TESTED. No ESP-IDF header, no MAC read, no mDNS type. The adapter reads the
// MAC and calls these; the naming rules are here, where a test can hold them to the ruling.
//
// ⬅️ WHY A STABLE NAME AT ALL. Owner ruling R6 requires the local web interface to stay
// reachable over the LAN after provisioning, and to "prefer stable local discovery/addressing
// rather than requiring the user to discover the DHCP-assigned IP manually". A DHCP lease is
// not an address a person can write down.
//
// ⛔ BRAND-NEUTRAL, BY RULING D. The name is "display-<suffix>", never "prusa-" and never the
// future product brand: LICENSING.md reserves the project and product names, logos and
// wordmarks as all rights reserved, so a hostname coupled to a brand would be a product
// decision made by a firmware constant. The suffix is device_suffix()'s, so the setup network
// a user joined and the LAN name they reach afterwards identify the same device.

namespace prd::network::discovery_identity {

// "display-" — lowercase, because a hostname is what a browser resolves and DNS names are
// conventionally lowercase even though resolution is case-insensitive.
inline constexpr std::string_view kHostnamePrefix = "display-";

// The domain multicast DNS serves. mDNS itself appends this to the hostname it is given, so
// the constant exists for the names WE have to spell out — the origin allow-list and anything
// shown to a person.
inline constexpr std::string_view kLocalDomain = ".local";

// "Remote Display 0C01" — the human-readable service instance a discovery browser lists.
// Uppercase suffix here, matching the setup SSID a user has already read off the panel.
inline constexpr std::string_view kServiceInstancePrefix = "Remote Display ";

// The advertised service type. `_http._tcp` is what a browser and every mDNS utility already
// look for; a private type would make the device invisible to the tools people have.
inline constexpr std::string_view kServiceType = "_http";
inline constexpr std::string_view kServiceProtocol = "_tcp";

// The bare hostname, e.g. "display-0c01" — WITHOUT the .local domain, because that is the
// form mdns_hostname_set() expects and appending it there would advertise
// "display-0c01.local.local".
[[nodiscard]] app_model::BoundedString<24> hostname_for(std::string_view suffix);

// The full name a person types and a browser sends as the host half of an Origin, e.g.
// "display-0c01.local".
[[nodiscard]] app_model::BoundedString<32> local_name_for(std::string_view suffix);

// The service instance name, e.g. "Remote Display 0C01".
[[nodiscard]] app_model::BoundedString<48> service_instance_for(std::string_view suffix);

} // namespace prd::network::discovery_identity
