// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include "provisioning/server.hpp"

#include <array>
#include <cstring>
#include <string>

// ESP-IDF headers are not -Wundef clean. Suppress locally (per architecture
// spec §12.3) so the project -Wundef policy still applies to our own code.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wundef"
#include "cJSON.h"
#include "esp_err.h"
#include "esp_http_server.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "lwip/sockets.h"
#pragma GCC diagnostic pop

#include "app_model/build_info.hpp"
#include "credential_scrub.hpp"
#include "diagnostics/log.hpp"
#include "diagnostics/tags.hpp"
#include "escaping.hpp"
#include "request_guard.hpp"
#include "request_validation.hpp"
#include "server_view.hpp"
#include "session_token.hpp"
#include "surface.hpp"

// The HTTP adapter.
//
// IT CONTAINS NO POLICY. What may be submitted lives in request_validation.cpp; whether a
// request may proceed lives in request_guard.cpp; the token and its constant-time
// comparison live in session_token.cpp. All three are host-tested. What is here is socket
// plumbing, cJSON extraction, and obedience to what those three returned.
//
// ⛔ NOTHING SUBMITTED IS EVER LOGGED. Not an SSID, not a passphrase, not a host, not an
// API key — the log lines below carry outcomes and lengths and nothing else. The CSRF token
// is not logged either (owner ruling D9).

namespace prd::provisioning {
namespace {

constexpr std::string_view kTag = diagnostics::tag::kProvisioning;

// §10.8 puts the provisioning server at medium priority: below the renderer and the
// application task, above nothing in particular. It must never be able to starve either.
constexpr unsigned kTaskPriority = 4;
constexpr std::size_t kTaskStackBytes = 6144;

// One browser, one page. §14.4 wants the surface small; three sockets are reserved by
// esp_http_server for its own use, so this is four in total.
constexpr std::uint16_t kMaxOpenSockets = 4;
constexpr std::uint16_t kMaxUriHandlers = 8;
constexpr std::uint16_t kBacklog = 2;

// A client that opens a socket and says nothing must not hold one for long.
constexpr std::uint16_t kRecvTimeoutSeconds = 5;
constexpr std::uint16_t kSendTimeoutSeconds = 5;

// §14.4 "limit request sizes", applied to the request line as well as the body.
constexpr std::size_t kMaxUriLength = 128;

constexpr std::string_view kTokenHeader = "X-PRD-Setup-Token";

httpd_handle_t g_server = nullptr;
SessionToken g_token;
RateLimiter g_limiter;

SubmissionSink g_sink = nullptr;
UpdateSink g_update_sink = nullptr;
void* g_context = nullptr;

// --- The published flow state, and the one channel that carries it -------
//
// ⛔ THE MASTER COPY, AND THE APPLICATION TASK IS ITS ONLY ACCESSOR. The surfaces (owner ruling B),
// the setup state and the setup reason used to be three separate file-scope objects that the HTTP
// task read directly while this task wrote them — a `Surfaces` whose BoundedStrings are replaced
// field by field, a `volatile SetupState` standing in for synchronisation, and a `std::string` on
// the heap. See server_view.hpp for what that cost and why a snapshot replaces it.
ServerSnapshot g_published;

// Depth ONE with overwrite semantics — §10.7's "small UI state notification queue using
// overwrite/latest-value semantics", which is exactly what this is: the HTTP task needs the latest
// complete state and has no use for the history. The copy in and the copy out both happen inside the
// queue's own critical section, which is what makes a reader see one whole snapshot rather than a
// mixture of two.
QueueHandle_t g_snapshot = nullptr;

// "http://192.168.4.1" — the setup URL the panel shows, rebuilt from the access-point address
// each time it is asked for.
//
// ⬅️ G2, MILESTONE 3.10: NOT A CACHE ANY MORE, AND THAT WAS A REAL DEFECT. It used to be assigned
// inside start(), which returns early when the server is already running — so on a deliberate
// re-entry, where the server has never stopped and the access point comes up afterwards, it stayed
// empty and the panel showed no address while the network was up and answering. Storage this
// function owns, refilled from the one source of truth, so the two cannot disagree.
std::string g_url;

// The embedded assets (owner ruling D8). Real files under assets/, compiled in by
// EMBED_FILES; no generator, no filesystem, no partition.
extern "C" {
extern const std::uint8_t index_html_start[] asm("_binary_index_html_start");
extern const std::uint8_t index_html_end[] asm("_binary_index_html_end");
extern const std::uint8_t style_css_start[] asm("_binary_style_css_start");
extern const std::uint8_t style_css_end[] asm("_binary_style_css_end");
extern const std::uint8_t app_js_start[] asm("_binary_app_js_start");
extern const std::uint8_t app_js_end[] asm("_binary_app_js_end");
}

[[nodiscard]] std::uint32_t now_ms() {
    return static_cast<std::uint32_t>(esp_timer_get_time() / 1000);
}

std::uint8_t random_byte(void* /*context*/) {
    return static_cast<std::uint8_t>(esp_random() & 0xFFU);
}

// The one coherent copy a request is answered from.
//
// ⛔ CALLED ONCE PER REQUEST, AT ENTRY. That is the whole coherence rule: every decision and every
// response field below comes from this value, so an answer describes the state before a transition
// or the state after it and never half of each. handle_submit() used to read the surfaces twice and
// the setup state separately, three reads of three independently updated globals.
//
// ⬅️ AND NOTHING IS HELD AFTERWARDS. xQueuePeek copies the item under the queue's own critical
// section and releases it before returning, so no synchronisation primitive is alive while a handler
// parses a body, calls a sink or sends a response.
[[nodiscard]] ServerSnapshot snapshot_for_request() {
    ServerSnapshot snapshot;

    if (g_snapshot == nullptr || xQueuePeek(g_snapshot, &snapshot, 0) != pdTRUE) {
        // ⛔ FAIL-CLOSED, AND THE TWO REFUSALS ARE NOT THE SAME ONE. A default snapshot carries no
        // access-point address, so it recognises no privileged setup surface — but classify() does
        // not answer Surface::Unknown for that. Given a socket address it successfully resolved,
        // classify() conservatively answers Surface::StationLan whenever that address is not the
        // setup access point's; Surface::Unknown is what arrival_surface() returns when the
        // socket or local-address lookup itself FAILS. permits_mutation() refuses both, so either
        // way no mutation is authorised here — which is the property that matters.
        //
        // Unreachable in practice: start() seeds the queue before httpd_start() creates the task
        // that could read it.
        return ServerSnapshot{};
    }

    return snapshot;
}

// Hands the master copy to the HTTP task. Application task only.
//
// ⛔ THE WHOLE VALUE, EVERY TIME. Publishing a field would reintroduce precisely the tearing this
// replaced; the queue's guarantee is over one complete item.
void republish() {
    if (g_snapshot == nullptr) {
        return;
    }

    (void)xQueueOverwrite(g_snapshot, &g_published);
}

// Which of this device's surfaces accepted the connection — owner ruling A.
//
// ⛔ FROM THE SOCKET, NEVER FROM A HEADER. Host and Origin are written by the caller, so
// authorizing on either would let anyone on the LAN claim to have arrived on the setup access
// point simply by typing 192.168.4.1 into a header. getsockname() reports the local address
// the kernel actually accepted the connection on, which the caller does not choose.
//
// A failure here yields Surface::Unknown, and permits_mutation() refuses that. The two
// mistakes are not symmetrical: refusing a legitimate mutation strands a user who can retry
// and leaves a loud log line, while permitting an unidentified one is precisely what R7
// forbids. It is logged at warning level because on the setup access point it would break
// provisioning, and a silent version of that would be diagnosed as anything but this.
//
// ⬅️ THE SURFACES ARE A PARAMETER NOW, NOT A GLOBAL. The set this compares against must be the same
// set the gate compares origins against a few lines later, and reading a shared object twice is what
// made that a hope rather than a fact.
[[nodiscard]] Surface arrival_surface(httpd_req_t* const request, const Surfaces& surfaces) {
    const int socket = httpd_req_to_sockfd(request);
    if (socket < 0) {
        diagnostics::log(diagnostics::LogLevel::Warning, kTag,
                         "request socket unavailable; treating the surface as unidentified");
        return Surface::Unknown;
    }

    sockaddr_in6 local{};
    socklen_t length = sizeof(local);
    if (getsockname(socket, reinterpret_cast<sockaddr*>(&local), &length) != 0) {
        diagnostics::log(diagnostics::LogLevel::Warning, kTag,
                         "local address unavailable; treating the surface as unidentified");
        return Surface::Unknown;
    }

    // LWIP is built with IPv6 here, so an IPv4 connection arrives as an IPv4-mapped address.
    // Only the dotted-quad form is ever compared, because that is the form the access-point
    // netif reports and the only family this product serves.
    std::array<char, INET6_ADDRSTRLEN> text{};

    if (local.sin6_family == AF_INET6) {
        in_addr mapped{};
        std::memcpy(&mapped, &local.sin6_addr.un.u32_addr[3], sizeof(mapped));
        if (inet_ntop(AF_INET, &mapped, text.data(), text.size()) == nullptr) {
            return Surface::Unknown;
        }
    } else {
        const auto* const address = reinterpret_cast<const sockaddr_in*>(&local);
        if (inet_ntop(AF_INET, &address->sin_addr, text.data(), text.size()) == nullptr) {
            return Surface::Unknown;
        }
    }

    return surfaces.classify(std::string_view{text.data()});
}

[[nodiscard]] std::string_view header(httpd_req_t* const request, const std::string_view name) {
    static std::array<char, 256> buffer{};

    const std::string key{name};
    const std::size_t length = httpd_req_get_hdr_value_len(request, key.c_str());

    if (length == 0 || length >= buffer.size()) {
        return {};
    }

    if (httpd_req_get_hdr_value_str(request, key.c_str(), buffer.data(), buffer.size()) != ESP_OK) {
        return {};
    }

    return {buffer.data(), length};
}

esp_err_t send_asset(httpd_req_t* const request, const std::uint8_t* const begin,
                     const std::uint8_t* const end, const char* const type) {
    httpd_resp_set_type(request, type);

    // The assets are in the OTA image and change only with the firmware, so they are
    // cacheable — but the page carries a session token, so it must not be.
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    httpd_resp_set_hdr(request, "X-Content-Type-Options", "nosniff");

    return httpd_resp_send(request, reinterpret_cast<const char*>(begin),
                           static_cast<ssize_t>(end - begin));
}

esp_err_t handle_index(httpd_req_t* const request) {
    return send_asset(request, index_html_start, index_html_end, "text/html; charset=utf-8");
}

esp_err_t handle_style(httpd_req_t* const request) {
    return send_asset(request, style_css_start, style_css_end, "text/css; charset=utf-8");
}

esp_err_t handle_script(httpd_req_t* const request) {
    return send_asset(request, app_js_start, app_js_end, "text/javascript; charset=utf-8");
}

// GET /api/status.
//
// ⛔ PRESENCE ONLY, NEVER A VALUE. §5.11: the web UI "must never return stored secrets to
// the browser". No stored SSID, Wi-Fi password, API key or printer password appears here,
// and neither does the setup access point's own password — its holder already knows it, and
// a response would put it in browser history and cache.
//
// The CSRF token IS returned, and is not a secret in that sense: it is minted for this page,
// and no other origin can read this response, because no CORS header is ever sent.
//
// ⬅️ BUT ONLY ON THE SURFACE THAT CAN USE IT — Milestone 3.9. On the persistent LAN surface
// no mutation is permitted at all (owner ruling A), so the token there would authorise
// nothing. Withholding it costs the page nothing it can act on and removes one value from a
// response that anybody on the segment can now fetch. This is defence in depth rather than
// the defence itself: the surface refusal in the gate is what actually stops the mutation.
esp_err_t handle_status(httpd_req_t* const request) {
    const ServerSnapshot snapshot = snapshot_for_request();
    const auto build = app_model::current_build_info();
    const std::string version = build.version.to_string();

    // ⬅️ THE BODY IS BUILT BY A PURE FUNCTION TAKING THE SNAPSHOT. It has no global to reach, so
    // every field it writes necessarily describes one instant — and the shape of the response is
    // asserted by the host suite rather than reviewed here.
    const std::string body = status_body(snapshot, arrival_surface(request, snapshot.surfaces),
                                         version, CsrfToken{g_token.view()});

    httpd_resp_set_type(request, "application/json");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    httpd_resp_set_hdr(request, "X-Content-Type-Options", "nosniff");

    return httpd_resp_send(request, body.c_str(), static_cast<ssize_t>(body.size()));
}

void refuse(httpd_req_t* const request, const GateOutcome outcome) {
    diagnostics::log(diagnostics::LogLevel::Warning, kTag,
                     "setup submission refused reason=" + std::string{log_label(outcome)});

    // The body names nothing. A caller probing the gate learns the status and no more.
    const std::string status = std::to_string(status_for(outcome)) + " Refused";
    httpd_resp_set_status(request, status.c_str());
    httpd_resp_set_type(request, "application/json");
    (void)httpd_resp_send(request, "{\"accepted\":false}", HTTPD_RESP_USE_STRLEN);
}

// A body that validated but could not be handed to app_core.
//
// ⛔ 503 AND NOT 200, BECAUSE THE REQUEST DID NOT DO WHAT THE CALLER ASKED. The grammar was fine, so
// this is not a refusal of the input and the form has nothing to correct; the device could not take
// the work. §16.4's "abort the current request safely" and D-6's recoverable failure both land here:
// the access point stays up, the form stays editable, nothing was persisted, and a retry is the
// whole recovery.
//
// ⛔ ONE FIXED BODY, NO SUBMITTED VALUE, AND `no-store` LIKE EVERY OTHER ANSWER ON THIS ROUTE. The
// reason is a constant chosen at compile time — there is nothing here a caller could steer.
void unavailable(httpd_req_t* const request) {
    httpd_resp_set_status(request, "503 Service Unavailable");
    httpd_resp_set_type(request, "application/json");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    httpd_resp_set_hdr(request, "X-Content-Type-Options", "nosniff");
    (void)httpd_resp_send(request, kUnavailableBody.data(),
                          static_cast<ssize_t>(kUnavailableBody.size()));
}

[[nodiscard]] std::string_view field(const cJSON* const root, const char* const name) {
    const cJSON* const item = cJSON_GetObjectItemCaseSensitive(root, name);
    if (item == nullptr || !cJSON_IsString(item) || item->valuestring == nullptr) {
        return {};
    }

    return {item->valuestring};
}

// The parsed request body, owned so that it is scrubbed and freed on EVERY exit from the handler.
//
// ⛔ WHY THIS IS A TYPE AND NOT TWO MORE LINES BEFORE TWO cJSON_Delete() CALLS. cJSON_Delete() frees
// each `valuestring` without overwriting it, so the passphrase and the API key survived in freed
// heap — §13.5's "embedded in crash dumps where avoidable", and avoidable through a public field.
// The handler already left by two paths after parsing, and the next added `return` is exactly what
// a pair of hand-placed cleanups loses. Ownership makes it structural.
//
// ⬅️ AND IT SCRUBS BEFORE IT FREES, IN THAT ORDER. Freeing first would hand the allocator a buffer
// still holding the credential, which is the whole defect.
class ParsedBody {
  public:
    explicit ParsedBody(cJSON* const root) : root_{root} {}

    ~ParsedBody() {
        // The credential-bearing values only. `printerHost` and `printerPort` are ConfigV1 fields,
        // not SecretConfigV1 ones, and wiping non-secrets would blur what this list means.
        //
        // ⛔ wifiSsid IS ON THIS LIST. The owner clarification of 2026-08-23 settled that the actual
        // home network name is credential material under §§7.3, 13.2 and 13.5.
        for (const char* const name : {"wifiSsid", "wifiPassword", "printerApiKey"}) {
            cJSON* const item = cJSON_GetObjectItemCaseSensitive(root_, name);
            if (item != nullptr && cJSON_IsString(item)) {
                scrub::text(item->valuestring);
            }
        }

        cJSON_Delete(root_);
    }

    ParsedBody(const ParsedBody&) = delete;
    ParsedBody& operator=(const ParsedBody&) = delete;
    ParsedBody(ParsedBody&&) = delete;
    ParsedBody& operator=(ParsedBody&&) = delete;

    [[nodiscard]] cJSON* get() const {
        return root_;
    }

  private:
    cJSON* root_;
};

// Hands a validated submission to app_core and reports whether it was taken.
//
// ⛔ NO SINK MEANS UNAVAILABLE, NOT ACCEPTED. A server started without one cannot provision at all,
// so answering 200 would be the same lie as a failed handoff. The old code skipped the call in that
// case and still said accepted.
//
// ⛔ AND EVERY BOUNDED ASSIGNMENT IS CHECKED, even though validate() should have made a bad fit
// unreachable. `Submitted::wifi_password` is 63 bytes against the flow record's 64 — WPA2's
// passphrase limit against what SecretConfigV1 stores — so the two bounds are deliberately
// different and a future change to either could make this reachable. An unchecked assign() would
// truncate a credential and hand the driver a passphrase nobody typed.
[[nodiscard]] SubmissionHandoff deliver(const Submission& submission,
                                        const SubmissionReport& report, const Stage stage) {
    if (g_sink == nullptr) {
        return SubmissionHandoff::Unavailable;
    }

    Submitted submitted;

    // ⛔ AND THIS STACK OBJECT IS WIPED ON EVERY EXIT. app_core wipes its own heap object — this was
    // the one link in that chain that did not, so the credential stayed on the server task's stack
    // for whatever reused the frame to surface. Guarded rather than wiped after the call, so the
    // truncation return below cannot leave it behind. `Submitted` is BoundedStrings and scalars —
    // trivially copyable, so sizeof is the whole object and there is nothing indirect to miss.
    const scrub::Scope submitted_scrub{&submitted, sizeof(submitted)};

    submitted.stage = stage == Stage::Printer ? SubmissionStage::Printer : SubmissionStage::Wifi;
    submitted.printer_port = report.port_value;

    // Written so that every assign() runs whatever an earlier one returned: the operand order puts
    // the call first, so `fits` can only narrow and no field is skipped.
    bool fits = true;
    fits = submitted.wifi_ssid.assign(submission.wifi_ssid) && fits;
    fits = submitted.wifi_password.assign(submission.wifi_password) && fits;
    fits = submitted.printer_host.assign(submission.printer_host) && fits;
    fits = submitted.printer_api_key.assign(submission.printer_api_key) && fits;

    if (!fits) {
        diagnostics::log(diagnostics::LogLevel::Error, kTag,
                         "setup submission not handed over: a validated field did not fit the "
                         "handoff record");
        return SubmissionHandoff::Unavailable;
    }

    return g_sink(submitted, g_context);
}

// POST /api/setup.
//
// ⚠️ IT VALIDATES. IT DOES NOT SAVE. Milestone 3.7 has nowhere to put a configuration and
// says so by having no Save action on the page; this endpoint exists so that the §14.4
// protections around it are real and exercised rather than scaffolding written for a later
// checkpoint. M3.8 gives app_core's sink a body that changes something.
esp_err_t handle_submit(httpd_req_t* const request) {
    // ⛔ ONE SNAPSHOT FOR THE WHOLE REQUEST — the arrival check, the gate's origin membership and the
    // stage admission below. Those were three separate reads of state the application task was
    // writing, so a submission arriving as provisioning moved could be refused on one instant's
    // surfaces and admitted on another instant's setup state.
    const ServerSnapshot snapshot = snapshot_for_request();

    RequestFacts facts;

    // Resolved before any header is read, because owner ruling A's refusal does not depend on
    // anything the caller sent and should not be reached through parsing what they sent.
    facts.arrival = arrival_surface(request, snapshot.surfaces);

    facts.origin = header(request, "Origin");
    const std::string origin{facts.origin};
    facts.content_type = header(request, "Content-Type");
    const std::string content_type{facts.content_type};
    facts.token = header(request, kTokenHeader);
    facts.content_length = static_cast<std::size_t>(request->content_len);

    // The three header reads share one static buffer, so each view is invalidated by the
    // next read. Re-pointed at the owned copies before the gate sees them.
    facts.origin = origin;
    facts.content_type = content_type;
    const std::string token{facts.token};
    facts.token = token;

    const GateOutcome gate = evaluate(facts, snapshot.surfaces, g_token, g_limiter, now_ms());

    if (gate != GateOutcome::Allow) {
        refuse(request, gate);
        return ESP_OK;
    }

    // Bounded by the gate above, so this allocation cannot be driven by the caller beyond
    // kMaxBodyBytes.
    std::string body(facts.content_length, '\0');

    // ⛔ THE RAW BODY IS A CREDENTIAL BUFFER. It holds the passphrase and the API key exactly as the
    // browser sent them, and a std::string frees its heap without overwriting it. Guarded here, on
    // the line after the declaration, so every path out of this handler — the short read below, the
    // parse failure, the stage refusal, the success — leaves it zeroed.
    //
    // ⛔ AND `body` IS NEVER RESIZED WHILE THIS IS ALIVE. scrub::Scope records the pointer; the
    // string is sized once, at construction, and only written through by httpd_req_recv() below.
    const scrub::Scope body_scrub{body.data(), body.size()};

    const int received = httpd_req_recv(request, body.data(), body.size());
    if (received <= 0 || static_cast<std::size_t>(received) != body.size()) {
        refuse(request, GateOutcome::BodyMissing);
        return ESP_OK;
    }

    const ParsedBody parsed{cJSON_ParseWithLength(body.c_str(), body.size())};
    cJSON* const root = parsed.get();
    if (root == nullptr) {
        refuse(request, GateOutcome::BodyMissing);
        return ESP_OK;
    }

    Submission submission;
    submission.wifi_ssid = field(root, "wifiSsid");
    submission.wifi_password = field(root, "wifiPassword");
    submission.printer_host = field(root, "printerHost");
    submission.printer_port = field(root, "printerPort");
    submission.printer_api_key = field(root, "printerApiKey");

    // Absent or unrecognised means the Wi-Fi stage. A caller cannot reach the printer stage
    // by omitting the field, which is the safe direction for a default to fail in.
    const std::string_view stage_text = field(root, "stage");
    const Stage stage = stage_text == "printer" ? Stage::Printer : Stage::Wifi;

    // ⛔ THE PRINTER STAGE REQUIRES A VALIDATED WI-FI STAGE. Refused here as well as in
    // app_core, which is authoritative: this one gives the page a proper HTTP answer
    // instead of a silent no-op, and stops a caller skipping step one.
    if (!admits_printer_stage(stage, snapshot.setup_state)) {
        diagnostics::log(diagnostics::LogLevel::Warning, kTag,
                         "printer stage refused: no validated wi-fi stage");
        refuse(request, GateOutcome::BodyMissing);
        return ESP_OK;
    }

    const SubmissionReport report = validate(submission, stage);

    // ⛔ THE HANDOFF HAPPENS BEFORE THE ANSWER IS COMPOSED, AND THAT ORDER IS THE POINT. The response
    // used to be built and the sink called afterwards, so `"accepted": true` was written from the
    // grammar alone and no failure after that could change it. Now the only way to reach the
    // acceptance response is through a handoff that actually moved the submission, and every other
    // outcome leaves this function before the word "accepted" is ever put in a buffer.
    const bool validated = report.accepted(stage);
    const SubmissionHandoff handoff =
        validated ? deliver(submission, report, stage) : SubmissionHandoff::Unavailable;

    if (submit_answer(validated, handoff) == SubmitAnswer::Unavailable) {
        // ⛔ NOTHING IS LOGGED HERE, AND THE OMISSION IS THE POINT.
        //
        // One of the ways to arrive at this branch is `new (std::nothrow)` having just answered
        // "there is no heap". A diagnostic would build a std::string to say so — and
        // diagnostics::log() materialises its tag as one before it reaches ESP_LOGx — so the line
        // explaining the failure could be the allocation that turns a recoverable 503 into an
        // abort. With -fno-exceptions a std::bad_alloc here takes the device down over a form
        // submission.
        //
        // ⬅️ AND NOTHING IS LOST BY IT. app_core is the sink: it increments its non-secret refusal
        // counter without formatting it and reports the total on the next line it can afford to
        // emit. The failures that are NOT exhaustion — a bounded field that would not fit, a
        // mailbox that refused — are each logged where they happen, on paths that are not
        // represented as heap-exhaustion-safe.
        unavailable(request);
        return ESP_OK;
    }

    std::string response;
    response.reserve(256);
    response += '{';
    append_json_bool(response, FieldName{"accepted"}, report.accepted(stage));
    response += ',';
    append_json_string(response, FieldName{"stage"}, log_label(stage));

    // ⬅️ ONLY THIS STAGE'S FIELDS ARE REPORTED. The page marks what it is told about, so
    // reporting the other stage's untouched fields would put errors on a section the user
    // has not reached — which is precisely what failed physical acceptance.
    if (stage == Stage::Wifi) {
        response += ',';
        append_json_string(response, FieldName{"wifiSsid"}, log_label(report.wifi_ssid));
        response += ',';
        append_json_string(response, FieldName{"wifiPassword"}, log_label(report.wifi_password));
    } else {
        response += ',';
        append_json_string(response, FieldName{"printerHost"}, log_label(report.printer_host));
        response += ',';
        append_json_string(response, FieldName{"printerPort"}, log_label(report.printer_port));
        response += ',';
        append_json_string(response, FieldName{"printerApiKey"}, log_label(report.printer_api_key));
    }

    response += '}';

    // ⬅️ NO cJSON_Delete() HERE ANY MORE. `parsed` owns the tree and scrubs it on the way out, which
    // also means the views in `submission` stay valid until this function returns rather than
    // outliving the buffer they point into by the length reads below.

    // Outcomes and lengths. Not one submitted byte.
    //
    // ⬅️ THE LENGTHS ARE WHY THIS LINE EXISTS IN THIS SHAPE. A printer step that failed every
    // field was diagnosed for a whole session from outcomes alone, and outcomes cannot
    // distinguish "the user typed nothing" from "the value carried one byte the grammar
    // refuses". A count answers that in one glance and discloses nothing: §13.5 forbids the
    // credential, and a length is not the credential. Printer fields only — the Wi-Fi
    // passphrase's length is not needed by any question this diagnostic exists to answer.
    diagnostics::log(diagnostics::LogLevel::Info, kTag,
                     "setup submission stage=" + std::string{log_label(stage)} +
                         " accepted=" + std::string{report.accepted(stage) ? "yes" : "no"} +
                         " ssid=" + std::string{log_label(report.wifi_ssid)} +
                         " wifi_password=" + std::string{log_label(report.wifi_password)} +
                         " host=" + std::string{log_label(report.printer_host)} +
                         " port=" + std::string{log_label(report.printer_port)} +
                         " api_key=" + std::string{log_label(report.printer_api_key)} +
                         " host_len=" + std::to_string(submission.printer_host.size()) +
                         " port_len=" + std::to_string(submission.printer_port.size()) +
                         " key_len=" + std::to_string(submission.printer_api_key.size()));

    httpd_resp_set_type(request, "application/json");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return httpd_resp_send(request, response.c_str(), static_cast<ssize_t>(response.size()));
}

// POST /api/update. The same physical-surface, exact-Origin, JSON, CSRF and rate gate as setup;
// unlike setup its body has no value to parse or echo. app_core remains the authority that binds
// the action to the currently advertised release and acquires exclusivity.
esp_err_t handle_update(httpd_req_t* const request) {
    const ServerSnapshot snapshot = snapshot_for_request();
    RequestFacts facts;
    facts.arrival = arrival_surface(request, snapshot.surfaces);
    facts.origin = header(request, "Origin");
    const std::string origin{facts.origin};
    facts.content_type = header(request, "Content-Type");
    const std::string content_type{facts.content_type};
    facts.token = header(request, kTokenHeader);
    const std::string token{facts.token};
    facts.content_length = static_cast<std::size_t>(request->content_len);
    facts.origin = origin;
    facts.content_type = content_type;
    facts.token = token;

    const GateOutcome gate = evaluate(facts, snapshot.surfaces, g_token, g_limiter, now_ms());
    if (gate != GateOutcome::Allow) {
        refuse(request, gate);
        return ESP_OK;
    }

    std::array<char, 8> body{};
    if (facts.content_length > body.size() ||
        httpd_req_recv(request, body.data(), facts.content_length) !=
            static_cast<int>(facts.content_length)) {
        refuse(request, GateOutcome::BodyMissing);
        return ESP_OK;
    }

    if (snapshot.update_version.empty() || snapshot.update_installing || g_update_sink == nullptr ||
        g_update_sink(g_context) != UpdateHandoff::Accepted) {
        unavailable(request);
        return ESP_OK;
    }

    httpd_resp_set_type(request, "application/json");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return httpd_resp_send(request, "{\"accepted\":true}", HTTPD_RESP_USE_STRLEN);
}

// Four initialisers, not seven: httpd_uri_t's is_websocket, handle_ws_control_frames and
// supported_subprotocol exist only under CONFIG_HTTPD_WS_SUPPORT, which this project does
// not enable. Spelling them would not compile — and would also be claiming a capability the
// server does not have.
//
// EVERY ROUTE IS AN EXACT MATCH. No wildcard handler, so a path this component did not name
// is a 404 from esp_http_server rather than something reaching our code.
const std::array<httpd_uri_t, 6> kRoutes{{
    {"/", HTTP_GET, &handle_index, nullptr},
    {"/style.css", HTTP_GET, &handle_style, nullptr},
    {"/app.js", HTTP_GET, &handle_script, nullptr},
    {"/api/status", HTTP_GET, &handle_status, nullptr},
    {"/api/setup", HTTP_POST, &handle_submit, nullptr},
    {"/api/update", HTTP_POST, &handle_update, nullptr},
}};

} // namespace

bool start(const Config& config) {
    if (g_server != nullptr) {
        return true;
    }

    // ⬅️ AN EMPTY ACCESS-POINT ADDRESS IS NO LONGER A REASON NOT TO SERVE. Until Milestone
    // 3.9 the only way to reach this server was the setup access point, so no address meant
    // nowhere to be reached and starting was pointless. Owner ruling R6 makes the interface
    // persistent: on a boot that is already provisioned there is no access point at all, and
    // the server's whole job is the LAN. It listens on INADDR_ANY either way.
    //
    // What an empty address DOES mean is that no mutation can be authorised — Surfaces has no
    // access-point address to recognise, so every mutating request classifies as StationLan
    // and owner ruling A refuses it. Read-only routes do not pass through the gate and work.
    const bool setup_surface = !config.address.empty();

    g_sink = config.sink;
    g_update_sink = config.update_sink;
    g_context = config.context;

    // ⛔ THE CHANNEL EXISTS BEFORE httpd_start() CREATES THE TASK THAT READS IT, and is seeded below
    // before the same call, so the first request of a session already has a complete snapshot. A
    // reader that found it empty would fall back to a default — no active surface, so no mutation
    // authorised — which is the safe direction rather than the intended one.
    if (g_snapshot == nullptr) {
        g_snapshot = xQueueCreate(1, sizeof(ServerSnapshot));
    }

    if (g_snapshot == nullptr) {
        diagnostics::log(diagnostics::LogLevel::Error, kTag,
                         "setup server state channel not created");
        return false;
    }

    // Seeds the access-point half of the reachable set. The other two slots stay empty — an
    // empty slot matches nothing, which is the safe default.
    //
    // ⛔ THIS IS NOT THE ONLY MECHANISM, AND IT MUST NOT BE RELIED ON AS ONE. This function
    // returns at its `g_server != nullptr` guard above whenever the server is already running,
    // which is every deliberate re-entry — so on that path this line never executes. app_core
    // publishes the reachable set the moment the access point comes up, before anything reads it;
    // see the note beside its refresh_reachability() call in start_setup_ap(). Milestone 3.11 T4
    // finding F-B was exactly the belief that this line always runs.
    g_published.surfaces.set_access_point(config.address);
    republish();

    // ⬅️ AND NO SETUP URL IS BUILT HERE ANY MORE — G2. url() derives it from whichever
    // access-point address is active when it is asked, which is what makes a deliberate re-entry
    // report an address at all: this function returns early when the server is already running,
    // so anything assigned here would be skipped on exactly the path that needs it.

    g_token.regenerate(&random_byte, nullptr);
    g_limiter.reset();

    httpd_config_t settings = HTTPD_DEFAULT_CONFIG();

    // Stated rather than inherited from the default, so the port mDNS advertises and the port
    // the socket listens on are one constant. They were already both 80; now they cannot
    // silently stop being.
    settings.server_port = kServerPort;

    settings.task_priority = kTaskPriority;
    settings.stack_size = kTaskStackBytes;
    settings.max_open_sockets = kMaxOpenSockets;
    settings.max_uri_handlers = kMaxUriHandlers;
    settings.backlog_conn = kBacklog;
    settings.lru_purge_enable = true;
    settings.recv_wait_timeout = kRecvTimeoutSeconds;
    settings.send_wait_timeout = kSendTimeoutSeconds;
    settings.max_uri_len = kMaxUriLength;

    if (httpd_start(&g_server, &settings) != ESP_OK) {
        diagnostics::log(diagnostics::LogLevel::Error, kTag, "setup server did not start");
        g_server = nullptr;
        g_url.clear();
        g_token.clear();
        return false;
    }

    for (const auto& route : kRoutes) {
        if (httpd_register_uri_handler(g_server, &route) != ESP_OK) {
            diagnostics::log(diagnostics::LogLevel::Error, kTag, "setup route registration failed");
            stop();
            return false;
        }
    }

    diagnostics::log(diagnostics::LogLevel::Info, kTag,
                     "local server listening port=" + std::to_string(kServerPort) +
                         " setup_surface=" + (setup_surface ? "yes" : "no") +
                         " routes=" + std::to_string(kRoutes.size()) +
                         " max_body=" + std::to_string(kMaxBodyBytes) +
                         "B (mutations require the setup access point)");
    return true;
}

void stop() {
    if (g_server == nullptr) {
        return;
    }

    (void)httpd_stop(g_server);
    g_server = nullptr;

    // The token dies with the session it belonged to (owner ruling D9).
    g_token.clear();
    g_limiter.reset();
    g_url.clear();
    g_sink = nullptr;
    g_update_sink = nullptr;
    g_context = nullptr;

    // ⬅️ AFTER httpd_stop(), WHICH JOINS. httpd_stop() sends its own control message and then spins
    // until the server thread reports THREAD_STOPPED (verified in the pinned esp_http_server), so
    // there is no HTTP task left to read any of this while it is being cleared. The snapshot is
    // republished anyway, so a server started again later cannot inherit a withdrawn surface.
    g_published = ServerSnapshot{};
    republish();

    diagnostics::log(diagnostics::LogLevel::Info, kTag, "local server stopped");
}

bool running() {
    return g_server != nullptr;
}

// ⬅️ log_label(SetupState) IS NOT HERE ANY MORE. It moved to server_view.cpp, which the host suite
// can compile, because status_body() needs it and this translation unit includes esp_http_server.h.

void publish(const SetupState state, const std::string_view reason) {
    // ⬅️ THE WRITE ORDER NO LONGER CARRIES THE GUARANTEE, AND THAT IS THE IMPROVEMENT. This used to
    // assign the reason before the state so that a page reading Failed could not find the previous
    // attempt's reason — a discipline that depended on the reader seeing the two writes in order,
    // which nothing guaranteed. They are now one value, handed over once.
    g_published.setup_state = state;
    (void)g_published.setup_reason.assign(reason);
    republish();

    // ⬅️ THE LAST LINK, AND THE ONE NOBODY HAD TIMED. Everything before this happens on the
    // device; this is the moment the answer becomes readable by the browser. If a validation
    // completes quickly and the phone still waits, the gap is between this record and the poll
    // that follows it — which is a different fault from a slow association, and until now the
    // log could not tell them apart.
    diagnostics::timeline(kTag, "browser-state-published", "state",
                          static_cast<std::int64_t>(state));
}

void publish_update(const std::string_view version, const bool installing) {
    (void)g_published.update_version.assign(version);
    g_published.update_installing = installing;
    republish();
}

void publish_addresses(const Addresses& addresses) {
    // ⛔ ALL THREE, THEN ONE HANDOVER. The three setters used to be visible to the HTTP task
    // individually, so a request arriving mid-call could classify its own arrival against a set that
    // was half updated — ruling B's "while each is genuinely active" read against two instants.
    g_published.surfaces.set_access_point(addresses.access_point);
    g_published.surfaces.set_station(addresses.station);
    g_published.surfaces.set_hostname(addresses.hostname);
    republish();
}

std::string_view url() {
    // ⬅️ THE ACCESS POINT IS THE CONDITION, NOT THE SERVER. From Milestone 3.9 the server
    // outlives the access point, so "the server is listening" no longer implies "this address
    // answers". The panel's Address row is driven from here and must not name an address that
    // has gone away — see the contract in server.hpp.
    // The master copy, on the task that owns it — url() is called from app_core, never from a
    // handler, so this is not a cross-task read.
    const std::string_view access_point = g_published.surfaces.access_point();

    // Derived, not remembered — and by surface.cpp's setup_url_for(), which is host-tested,
    // rather than by a second copy of the same concatenation living here.
    g_url = setup_url_for(access_point);
    return g_url;
}

} // namespace prd::provisioning
