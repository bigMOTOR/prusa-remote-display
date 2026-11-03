// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstdint>
#include <string_view>

#include "app_model/bounded_string.hpp"

// The local web server — Architecture Spec §5.11, §10.5, §14.4; owner rulings R6, R7 and
// A–D of Milestone 3.9.
//
// NO ESP-IDF HEADER APPEARS HERE (Bootstrap Spec §11.3), and — owner ruling D5 — this
// component neither includes nor manages esp_wifi. `network` owns the radio, the access
// point and the mDNS advertisement; this component is handed addresses and serves on them.
//
// ⬅️ IT IS NO LONGER ONLY A PROVISIONING SERVER — OWNER RULING R6. "The local web interface
// must not disappear after provisioning… Treat the web UI as a persistent local device
// management interface, with provisioning as one mode, not as a disposable setup page." From
// Milestone 3.9 this server outlives the setup access point and stays reachable over the
// user's LAN through the station interface.
//
// ⛔ NO RESTART AND NO REBIND IS INVOLVED IN THAT, and this is worth stating because the
// plain reading of "rebinds on the station interface" suggests otherwise. ESP-IDF's
// httpd_config_t has no bind-address field and its listener binds INADDR_ANY, so the running
// server already answers on every interface the device holds — the station one included, the
// moment it has an address. RESTARTING THE SERVER TO "REBIND" WOULD BREAK M3.8: the session
// token is minted at start, and the page treats a changed token as proof the display
// restarted mid-flow.
//
// ⛔ THE LAN SURFACE IS READ-ONLY IN MILESTONE 3.9 — OWNER RULING A. R7 requires explicit
// authorization for any endpoint that mutates sensitive configuration, and its preferred
// mechanism — short-lived authorization from deliberate physical presence — is built at
// M3.10 with the button gesture it shares with R1's re-entry. Until then every mutating
// request that did not arrive on the setup access point is refused. See surface.hpp; the
// enforcement is in the gate, not in a handler.
//
// ⛔ ORIGIN VALIDATION IS WIDENED, NOT RELAXED — OWNER RULING B. Ruling D9's exact-origin
// test now runs against an explicit set of origins the device genuinely owns: the setup-AP
// origin, the current station-IP origin and the accepted `.local` hostname origin, each only
// while it is actually active. No wildcard, no `*`, no prefix or substring match.
//
// THE PAGE STAYS HONEST BY OMISSION, which is the rule owner ruling D6 set: it offered no
// Save action while a Save could not save. The same rule now hides the form on a read-only
// surface, because a control whose submission is guaranteed to be refused is the same lie in
// a different place. The page learns which surface it is on from one non-secret field on the
// status response it already polls (owner ruling D-3's mechanism, not a second one).
//
// §10.5's DISCIPLINE SURVIVES INTACT, and matters more now that the server is long-lived:
// this component "performs no direct application state mutation". It never touches the store,
// `config`, `secrets` or `network`. A validated submission is handed to app_core through the
// sink below, on the server's own task, and app_core decides what — if anything — becomes an
// application event.
//
// ⛔ STILL NO IDLE SHUTDOWN, AND THE REASON HAS CHANGED — owner ruling C of Milestone 3.9.
// §14.4 asks the setup server to "stop after a bounded idle period WHEN SAFE", and D7
// deferred that to M3.10. R6 has since made this server permanent, so the rule is recorded
// as reinterpreted rather than merely postponed: §14.4's bounded idle shutdown applies to
// temporary provisioning and privileged-authorization exposure, NOT to the persistent
// read-only LAN server. The setup access point still stops after successful provisioning;
// this server does not stop merely because it is idle. M3.10 owns the bounded lifetime of
// re-entry and physical-presence authorization.

namespace prd::provisioning {

// The port the server listens on and the port mDNS advertises.
//
// ONE CONSTANT FOR BOTH, deliberately: an advertisement that named a different port from the
// listener would be discoverable and dead. 80 so a browser needs no port in the URL, which
// also keeps the Origin header free of one — see origin_matches_host().
inline constexpr std::uint16_t kServerPort = 80;

// A validated submission, handed over as owned values.
//
// ⛔ IT CARRIES CREDENTIALS. Nothing here may be logged, and nothing here reaches AppState
// (§13.5, §24.3). The sink is called on the server task and the values do not outlive the
// call.
// Which step this submission is. The Wi-Fi step carries no printer values and the printer
// step carries no credential — the validated Wi-Fi credential is already in app_core's
// transient session and is not sent twice.
enum class SubmissionStage : std::uint8_t {
    Wifi,
    Printer,
};

struct Submitted {
    SubmissionStage stage{SubmissionStage::Wifi};

    app_model::BoundedString<32> wifi_ssid;
    app_model::BoundedString<63> wifi_password;
    app_model::BoundedString<64> printer_host;
    app_model::BoundedString<64> printer_api_key;
    std::uint16_t printer_port{0};
};

// Whether the sink actually took the submission.
//
// ⛔ IT RETURNS A RESULT NOW, AND THE HTTP RESPONSE DEPENDS ON IT. The sink used to return void, so
// a handler that had validated a body announced `"accepted": true` and moved on — with no way to
// know whether app_core had received anything at all. A refused allocation, a full mailbox or a
// server started with no sink all produced the same cheerful answer, and the browser then waited
// for a validation that was never going to begin. §14.3's flow only starts once the application
// task holds the submission, so that is the fact the response has to carry.
enum class SubmissionHandoff : std::uint8_t {
    // app_core owns it. Validation will begin on its next tick.
    Accepted,

    // It could not be handed over. Nothing was started, nothing was persisted, and a submission
    // already pending — if there was one — is untouched. The caller must answer 503 rather than
    // claim acceptance; the user's retry is the recovery.
    Unavailable,
};

// How a validated submission reaches app_core — the ObservationSink convention, not a
// second one. Runs on the server task, which may not dispatch into the store (§10.2).
using SubmissionSink = SubmissionHandoff (*)(const Submitted& submitted, void* context);

enum class UpdateHandoff : std::uint8_t { Accepted, Unavailable };
using UpdateSink = UpdateHandoff (*)(void* context);

struct Config {
    // The address the access point answers on, e.g. "192.168.4.1". Supplied by app_core
    // from `network`; this component does not ask the radio anything.
    //
    // It seeds the access-point half of the reachable set below, so the very first request of
    // a provisioning session carries a valid origin without waiting for app_core's first
    // publish_addresses() call.
    std::string_view address;

    SubmissionSink sink{nullptr};
    UpdateSink update_sink{nullptr};
    void* context{nullptr};
};

// --- Where the device can be reached, as it stands (owner ruling B) --------
//
// Written by the application task and read by the HTTP task, exactly like publish() below.
// app_core owns every value: the access-point address from `network::setup_ap::address()`,
// the station address from the application state the store already carries, and the hostname
// from `network::discovery::local_name()`.
//
// ⛔ AN EMPTY FIELD MEANS "NOT ACTIVE", NEVER "ANY". A cleared slot matches no Origin at all.
// That is what makes ruling B's "while each is genuinely active" enforceable rather than
// aspirational: when the access point stops, app_core clears its address, and the origin it
// used to accept stops being accepted in the same call.
struct Addresses {
    // Bare hosts, never origins — the scheme belongs to this component so there cannot be
    // two spellings of one address.
    std::string_view access_point;
    std::string_view station;
    std::string_view hostname;
};

// Replaces the whole reachable set. Safe before start() and after stop().
void publish_addresses(const Addresses& addresses);

// Starts the server. Returns false if it could not bind or register its handlers.
//
// A fresh CSRF token is minted here, so restarting the server invalidates every token the
// previous session issued (owner ruling D9).
[[nodiscard]] bool start(const Config& config);

// Stops the server, clears the session token and withdraws every origin. Idempotent.
//
// ⬅️ NOTHING IN THE PRODUCT CALLS THIS FROM MILESTONE 3.9 ONWARD, and that is the checkpoint's
// whole substance. app_core used to call it two seconds after provisioning succeeded; R6 makes
// the interface persistent, so the call is gone and the server runs for the life of the boot.
// It is kept because it is the counterpart of start() and because the checkpoint that owns
// recovery will need it — not because it still runs. Anyone tracing "when does the server
// stop?" should find that answer here rather than by failing to find a call site.
void stop();

[[nodiscard]] bool running();

// --- What the page is told about the flow (owner ruling D-3) ---------------
//
// The browser learns the outcome by polling GET /api/status. No WebSocket, no SSE, no
// second endpoint: the page already fetches that route, and a provisioning result is one
// more non-secret field on a response it was making anyway.
//
// ⛔ STATE AND A REASON LABEL, NEVER A SUBMITTED VALUE. §5.11's "the web UI must never
// return stored secrets to the browser" and §13.5 both apply to the thing the user just
// typed, not only to what was already stored.
enum class SetupState : std::uint8_t {
    // No stage has run. The Wi-Fi form is editable.
    Editing,

    // The submitted network is being joined and held for its dwell.
    Validating,

    // ⬅️ STAGE 1 SUCCEEDED AND NOTHING WAS PERSISTED — owner ruling 1. The credential is
    // held in RAM, the station stays joined, and the page advances to the printer step. A
    // power loss here leaves the device un-provisioned, exactly as before the flow started.
    WifiValidated,

    Succeeded,

    // Recoverable — owner ruling D-6. The form stays editable and another submission is
    // accepted without a reboot.
    Failed,
};

[[nodiscard]] std::string_view log_label(SetupState state);

// Publishes the flow's state for the page to read. Called from the application task; read
// on the HTTP task. `reason` is a fixed non-secret label and is shown only for Failed.
void publish(SetupState state, std::string_view reason);

// Publishes only non-secret release availability. An empty version withdraws the action; installing
// keeps the action disabled while allowing the page to state what the display is doing.
void publish_update(std::string_view version, bool installing);

// The setup URL the panel should show, e.g. "http://192.168.4.1".
//
// EMPTY UNLESS THE SERVER IS LISTENING **AND** THE ACCESS POINT IS STILL UP — the Address row
// on the device is driven from this, and a panel that advertised an address nothing answered
// would be worse than one that showed none.
//
// ⬅️ THE SECOND HALF OF THAT CONDITION IS NEW, AND IT IS THE SAME PROMISE. Before Milestone
// 3.9 the server died with the access point, so "listening" implied "reachable at this
// address". Now the server outlives it, and the AP address stops answering while the server
// carries on — so this returns empty as soon as app_core clears the access-point address, and
// the M3.6 scene hides the row exactly as it already does when the value is absent.
//
// It is deliberately still the SETUP address and not the persistent `.local` name: owner
// ruling D keeps the panel showing the setup-AP address while provisioning is active and
// leaves exposing the LAN identity on the panel to a later checkpoint.
[[nodiscard]] std::string_view url();

} // namespace prd::provisioning
