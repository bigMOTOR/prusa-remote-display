// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstdint>
#include <string_view>

// Advertising this device on the local network — Architecture Spec §5.7, owner rulings R6
// and D of Milestone 3.9.
//
// NO ESP-IDF HEADER APPEARS HERE (Bootstrap Spec §11.3). The naming rules are a separate pure
// translation unit the host suite compiles; this is the adapter's contract.
//
// ⬅️ §5.7 ALREADY LISTS mDNS, BUT FOR THE OTHER DIRECTION. It names "DNS and mDNS-assisted
// hostname resolution" — resolving the printer. ADVERTISING OURSELVES IS THE NEW PART, and it
// exists because owner ruling R6 made the local web interface persistent: an interface the
// user can only reach at whatever address DHCP happened to hand out is not one they can
// return to, and R6 asks for "stable local discovery/addressing (for example mDNS) rather
// than requiring the user to discover the DHCP-assigned IP manually".
//
// ⛔ THIS ADVERTISES. IT RESOLVES NOTHING. There is no lookup entry point here, because owner
// ruling R4 keeps every printer protocol in Milestone 4 — including finding the printer.
// §5.7's HostResolver is still unbuilt.
//
// ⛔ AND IT IS NOT AN AUTHORIZATION BOUNDARY. Being discoverable is not being open: what a
// caller may do once it arrives is decided by the provisioning server's gate, where owner
// ruling A refuses every configuration mutation that did not come in on the setup access
// point. mDNS makes the read-only surface findable and changes nothing about the other one.

namespace prd::network::discovery {

// Starts mDNS, sets the stable hostname and advertises the local web interface on `port`.
//
// Idempotent: a second call while running does nothing and reports success.
//
// The caller supplies the port so the advertised number and the listening socket come from
// one constant rather than two that can drift — `provisioning::kServerPort` is that constant.
//
// Returns false if mDNS could not be started or the hostname could not be set. A device whose
// advertisement failed is still reachable by IP, so this is a degraded state and not a fault:
// it is logged, and the caller carries on.
[[nodiscard]] bool start(std::uint16_t port);

// Removes the service, stops mDNS and releases its resources. Idempotent.
void stop();

[[nodiscard]] bool running();

// "display-0c01" — the bare hostname, without the .local domain mDNS appends itself.
//
// Available before start() and after stop(): it is derived from the device's MAC, not from
// anything the advertiser owns, so a caller that needs the name does not have to wait for a
// network. Empty only if the MAC could not be read.
[[nodiscard]] std::string_view hostname();

// "display-0c01.local" — the name a person types into a browser, and the host half of the
// origin the persistent interface must accept (owner ruling B).
[[nodiscard]] std::string_view local_name();

} // namespace prd::network::discovery
