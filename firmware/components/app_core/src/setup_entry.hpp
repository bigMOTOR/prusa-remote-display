// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstdint>
#include <string_view>

// The order in which a setup session comes into existence — Milestone 3.11.
//
// INTERNAL HEADER of the app_core component. Pure — no ESP-IDF, no driver, no store, no clock —
// so the sequence below is host-tested against the same code the firmware runs.
//
// ⛔ WHY THE ORDER IS A UNIT AND NOT A RUN OF STATEMENTS. Four operations have to happen in one
// order and there is no local check that catches getting it wrong:
//
//   1. the access point starts, so an address exists at all;
//   2. the reachable set is published, so that address is one the device admits to owning;
//   3. the local server starts, or is already running from a previous entry;
//   4. the panel is handed the session — SSID, password, and the address DERIVED from step 2.
//
// Milestone 3.11 T4 found step 4 running before step 2. On first-time provisioning it happened to
// work, because provisioning::start() seeds the access-point address itself when it actually
// starts a server; on a deliberate re-entry the server is already up, that seeding is skipped at
// its `g_server != nullptr` guard, and the address the panel was given came from an empty set. The
// panel showed a network and a password with no address, and the device published the address ten
// milliseconds later, at the end of the same tick.
//
// ⬅️ AND THE PANEL WAS THE VISIBLE HALF, NOT THE WORST HALF. Until step 2 runs, the access point's
// own Origin is not in the allow-list, so a submission from the setup page — the one caller the
// privileged surface exists for — is refused for origin mismatch. That window is bounded by an
// application task tick, not by the ten milliseconds observed once.
//
// Milestone 3.9's G2 correction had already fixed the same class of bug one layer up, by deriving
// the URL instead of caching it. It moved the staleness from a cached string into the set the
// derivation reads. Writing the order down once, here, is what stops it moving again.

namespace prd::app_core::setup_entry {

// The operations the composition root must supply. None of them is a decision — each is a driver,
// a surface or a presentation call — and none may be reordered by its implementation.
//
// Function pointers with an explicit context rather than std::function: no allocation, and the
// same shape the network component's own injected steps already use.
struct Steps {
    // Raises the access point for the freshly minted session. False if it did not come up.
    bool (*start_access_point)(void* context);

    // Publishes every address the device now answers on, so the reachable set includes the
    // access point that has just appeared. MUST run before anything reads an address.
    void (*publish_addresses)(void* context);

    // Starts the local server, or confirms one is already running. False if nothing is listening,
    // which is not fatal: the session still exists and the panel simply shows no address.
    bool (*start_server)(void* context);

    // Hands the panel the session to draw, including the address derived from the published set.
    void (*submit_session)(void* context);

    // Discards the session. Reached only when the access point failed, so nothing that was
    // minted for it outlives it.
    void (*discard_session)(void* context);
};

enum class Outcome : std::uint8_t {
    // The access point is up, the address is published, and the panel has the session.
    Started,

    // As Started, but nothing is listening — the panel shows a joinable network and no address,
    // which is honest: a device whose server failed must not name an address that would refuse
    // the connection.
    StartedWithoutServer,

    // The access point did not come up. The session was discarded and nothing was submitted.
    AccessPointUnavailable,
};

[[nodiscard]] std::string_view log_label(Outcome outcome);

// Performs the four steps in the one order that works.
[[nodiscard]] Outcome begin(const Steps& steps, void* context);

} // namespace prd::app_core::setup_entry
