// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include "network/http.hpp"

#include <array>
#include <string>

#include "esp_err.h"
#include "esp_http_client.h"
#include "esp_timer.h"

#include "diagnostics/log.hpp"
#include "diagnostics/tags.hpp"

#include "redirect_policy.hpp"
#include "request_budget.hpp"
#include "vendor_log_policy.hpp"

// The ESP-IDF adapter behind §5.7's HTTP wrapper.
//
// ⬅️ ONE WALL-CLOCK BUDGET, ENFORCED HERE, BECAUSE ESP-IDF DOES NOT OFFER ONE. In the pinned
// v5.5.4 `esp_http_client_config_t::timeout_ms` is documented as "Network timeout" and is passed to
// each esp_transport_connect/read/write call individually (esp_http_client.c:1276, 1339, 1546,
// 1606, 1729). It is a PER-OPERATION timeout. A server trickling one byte just inside it therefore
// keeps a client alive indefinitely, and the "total request timeout" §11.4 asks for does not exist
// unless this code creates it. So the budget is a deadline computed from a monotonic clock, each
// blocking call is given `min(remaining, slice)`, and control returns here between slices to check
// the deadline and the caller's abandon predicate.
//
// ⛔ AND CONNECT IS THE EXCEPTION THAT NEEDED ITS OWN ALLOWANCE. It is a single call that cannot be
// re-entered, so handing it the read loop's slice bounded the whole phase at 400 ms and left the
// remaining budget unusable — §11.4 asks for 2-3 s there. request_budget.hpp holds the division and
// the clamp that keeps a phase allowance from outliving the request; the arithmetic is pure so it is
// proved by the host suite rather than asserted here.
//
// ⬅️ WHICH IS ALSO THE CANCELLATION MECHANISM. See AbandonPredicate in the header for why polling
// beats calling esp_http_client_cancel_request() from another task. The handle below never escapes
// this function, so its lifetime needs no cross-task synchronization at all.
//
// ⛔ NOTHING HERE LOGS A HEADER NAME OR VALUE, A URL OR A BODY. The only account of the request is
// the line at the end: outcome, status, latency, length and header count.

namespace prd::network {
namespace {

// Read granularity. Small enough that an oversized body is detected close to the limit, large
// enough that a 2 kB response needs few reads. A stack buffer on the calling task.
constexpr std::size_t kReadChunk = 256;

// ESP-IDF returns this, negated, from read() and fetch_headers() when a blocking call timed out
// with no data — as opposed to ESP_FAIL for a genuine transport error. The distinction is what lets
// a slice expiry be retried and a broken connection be reported.
constexpr int kReadTimedOut = -ESP_ERR_HTTP_EAGAIN;

[[nodiscard]] std::uint32_t elapsed_ms(const std::int64_t started_us) {
    const std::int64_t delta = esp_timer_get_time() - started_us;
    return delta <= 0 ? 0U : static_cast<std::uint32_t>(delta / 1000);
}

// What the exchange should do next, checked between blocking slices.
enum class Continue : std::uint8_t { Yes, Deadline, Abandoned };

// The three bounds this request carries, as the one value the budget unit takes.
[[nodiscard]] budget::Allowances allowances_of(const HttpRequest& request) {
    budget::Allowances allowances;
    allowances.total_ms = request.total_budget_ms;
    allowances.slice_ms = request.slice_ms;
    allowances.connect_ms = request.connect_budget_ms;
    return allowances;
}

[[nodiscard]] Continue may_continue(const HttpRequest& request, const std::int64_t started_us) {
    if (request.abandon != nullptr && request.abandon(request.abandon_context)) {
        return Continue::Abandoned;
    }

    return budget::exhausted(allowances_of(request), elapsed_ms(started_us)) ? Continue::Deadline
                                                                             : Continue::Yes;
}

// The timeout to hand the next ordinary blocking call.
[[nodiscard]] int slice_timeout_ms(const HttpRequest& request, const std::int64_t started_us) {
    return static_cast<int>(
        budget::slice_timeout_ms(allowances_of(request), elapsed_ms(started_us)));
}

// The timeout to hand the one connect call. §11.4's 2-3 s, clamped to the remaining total.
[[nodiscard]] int connect_timeout_ms(const HttpRequest& request, const std::int64_t started_us) {
    return static_cast<int>(
        budget::connect_timeout_ms(allowances_of(request), elapsed_ms(started_us)));
}

[[nodiscard]] HttpOutcome outcome_for_open(const esp_err_t error) {
    switch (error) {
    case ESP_ERR_HTTP_CONNECT:
        return HttpOutcome::ConnectFailed;

    case ESP_ERR_HTTP_EAGAIN:
        return HttpOutcome::Timeout;

    case ESP_ERR_INVALID_ARG:
        return HttpOutcome::Rejected;

    default:
        return HttpOutcome::TransportFailed;
    }
}

[[nodiscard]] HttpOutcome outcome_for(const Continue verdict) {
    return verdict == Continue::Abandoned ? HttpOutcome::Cancelled : HttpOutcome::Timeout;
}

} // namespace

std::string_view log_label(const HttpOutcome outcome) {
    switch (outcome) {
    case HttpOutcome::Ok:
        return "ok";
    case HttpOutcome::ConnectFailed:
        return "connect-failed";
    case HttpOutcome::Timeout:
        return "timeout";
    case HttpOutcome::Cancelled:
        return "cancelled";
    case HttpOutcome::ResponseTooLarge:
        return "response-too-large";
    case HttpOutcome::TransportFailed:
        return "transport-failed";
    case HttpOutcome::Rejected:
        return "rejected";
    }

    return "unknown";
}

HttpResponse get(const HttpRequest& request, BodyBuffer& buffer) {
    HttpResponse response;
    buffer.reset();

    if (request.url.empty() || request.total_budget_ms == 0) {
        return response;
    }

    // ⬅️ APPLIED BEFORE THE FIRST REQUEST THIS FIRMWARE EVER MAKES. Idempotent, and sited here
    // rather than at boot so no future call path can reach the client without it.
    vendor_log_policy::apply();

    // esp_http_client needs a NUL-terminated URL. The string is local to this call and holds no
    // credential: the key is a header.
    const std::string url{request.url};
    const std::int64_t started_us = esp_timer_get_time();

    esp_http_client_config_t config = {};
    config.url = url.c_str();
    config.method = HTTP_METHOD_GET;
    // ⛔ THE CONNECT ALLOWANCE, NOT A SLICE. In the pinned v5.5.4 this value reaches esp_tls as both
    // the connect select() bound (esp_tls.c:409-422) and the socket's SO_RCVTIMEO/SO_SNDTIMEO
    // (esp_tls.c:279-290), so it is exactly the connect timeout — and it also bounds the header
    // write inside open(). The read loop below calls esp_http_client_set_timeout_ms() per slice, so
    // the larger value here never widens a read: base_poll_read()'s select() uses whatever the loop
    // last set, and the socket options can only make a call return sooner.
    config.timeout_ms = connect_timeout_ms(request, started_us);

    // ⛔ REDIRECT CONTAINMENT. The request carries an API key in a header, so a followed 3xx would
    // re-send it to an authority the owner never configured (redirect_policy.hpp).
    //
    // ⬅️ AND ONE CORRECTION, BECAUSE THE OBVIOUS READING OF THIS FIELD IS WRONG. In the pinned
    // v5.5.4, `max_redirection_count == 0` does NOT mean "no redirections": esp_http_client.c:522
    // replaces zero with DEFAULT_MAX_REDIRECT, which is 10. Zero is therefore NOT a barrier and is
    // not claimed as one. What actually contains a redirect here is (1) disable_auto_redirect, and
    // (2) the fact that redirect following lives only in esp_http_client_set_redirection(), reached
    // only from perform()'s state machine (:1104-1107, whose process_again is "used only in the
    // blocking mode") — and this code uses open()/fetch_headers()/read() and never perform(). A 3xx
    // comes back as an ordinary status and is classified as a bounded failure. The field is still
    // set explicitly, so a future switch to perform() inherits the intent rather than the default.
    config.disable_auto_redirect = !redirect_policy::kFollowRedirects;
    config.max_redirection_count = redirect_policy::kMaxRedirectionSetting;

    static_assert(!redirect_policy::kFollowRedirects,
                  "disable_auto_redirect is derived from this; it must stay false");

    esp_http_client_handle_t client = esp_http_client_init(&config);

    if (client == nullptr) {
        return response;
    }

    for (std::size_t index = 0; index < request.header_count; ++index) {
        const HttpHeader& header = request.headers[index];

        if (header.name.empty()) {
            continue;
        }

        const std::string name{header.name};
        const std::string value{header.value};

        // The only place a credential is handled, and it goes straight into the client. Not
        // logged, not copied elsewhere, and both strings die with the loop.
        if (esp_http_client_set_header(client, name.c_str(), value.c_str()) != ESP_OK) {
            esp_http_client_cleanup(client);
            return response;
        }
    }

    const esp_err_t opened = esp_http_client_open(client, 0);

    if (opened != ESP_OK) {
        const std::uint32_t spent = elapsed_ms(started_us);
        const Continue verdict = may_continue(request, started_us);

        if (verdict != Continue::Yes) {
            // The absolute deadline passed, or the worker withdrew. Both outrank whatever ESP-IDF
            // said about the socket.
            response.outcome = outcome_for(verdict);
        } else if (opened == ESP_ERR_HTTP_CONNECT &&
                   budget::connect_spent(allowances_of(request), spent)) {
            // ⬅️ THE CLOCK TELLS THE TWO APART WHERE THE ERROR CODE CANNOT. ESP-IDF returns
            // ESP_ERR_HTTP_CONNECT both for a refused connection and for one that ran out of time.
            // A refusal comes back in milliseconds; a connect that consumed its whole allowance
            // timed out. §16.1 keeps "connection refused" and "timeout" distinct, and this is what
            // makes them distinct here.
            response.outcome = HttpOutcome::Timeout;
        } else {
            response.outcome = outcome_for_open(opened);
        }

        // ⛔ NO REOPEN. A failed esp_http_client_open() leaves the client's transport in a state the
        // pinned implementation does not define, so the handle is cleaned up and the failure
        // reported. The allowance above is what makes a retry unnecessary rather than merely
        // unattempted.
        response.latency_ms = spent;
        esp_http_client_cleanup(client);
        return response;
    }

    bool settled = false;

    // Headers, on the same budget. fetch_headers() blocks for one slice at a time and reports a
    // data-less timeout distinguishably, so a server that opens a socket and then says nothing is
    // bounded by the deadline rather than by its own patience.
    for (;;) {
        const Continue verdict = may_continue(request, started_us);

        if (verdict != Continue::Yes) {
            response.outcome = outcome_for(verdict);
            settled = true;
            break;
        }

        (void)esp_http_client_set_timeout_ms(client, slice_timeout_ms(request, started_us));

        const std::int64_t headers = esp_http_client_fetch_headers(client);

        if (headers >= 0) {
            break;
        }

        if (headers != kReadTimedOut) {
            response.outcome = HttpOutcome::TransportFailed;
            settled = true;
            break;
        }
    }

    if (!settled) {
        response.status = static_cast<std::uint16_t>(esp_http_client_get_status_code(client));

        std::array<char, kReadChunk> chunk{};
        HttpOutcome outcome = HttpOutcome::Ok;

        for (;;) {
            const Continue verdict = may_continue(request, started_us);

            if (verdict != Continue::Yes) {
                outcome = outcome_for(verdict);
                break;
            }

            (void)esp_http_client_set_timeout_ms(client, slice_timeout_ms(request, started_us));

            const int read =
                esp_http_client_read(client, chunk.data(), static_cast<int>(kReadChunk));

            if (read == kReadTimedOut) {
                // A slice with no data. The loop re-checks the deadline, which is what terminates
                // a trickling or silent server.
                continue;
            }

            if (read < 0) {
                outcome = HttpOutcome::TransportFailed;
                break;
            }

            if (read == 0) {
                // Complete when the client agrees the body is finished; otherwise the peer stopped
                // mid-body, which is a transport failure rather than a short response.
                if (!esp_http_client_is_complete_data_received(client)) {
                    outcome = HttpOutcome::TransportFailed;
                }

                break;
            }

            if (!buffer.append(std::string_view{chunk.data(), static_cast<std::size_t>(read)})) {
                outcome = HttpOutcome::ResponseTooLarge;
                break;
            }
        }

        response.outcome = outcome;
        response.body_length = buffer.length();
    }

    response.latency_ms = elapsed_ms(started_us);

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    // No URL, no header, no body content — the body can carry a printer identity and this line
    // describes the exchange, not its contents.
    diagnostics::log(diagnostics::LogLevel::Debug, diagnostics::tag::kNetwork,
                     "http get outcome=" + std::string{log_label(response.outcome)} +
                         " status=" + std::to_string(response.status) +
                         " latency_ms=" + std::to_string(response.latency_ms) +
                         " budget_ms=" + std::to_string(request.total_budget_ms) +
                         " bytes=" + std::to_string(response.body_length) +
                         " headers=" + std::to_string(request.header_count));

    return response;
}

} // namespace prd::network
