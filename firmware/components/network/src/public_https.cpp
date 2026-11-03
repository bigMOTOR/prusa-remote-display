// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include "network/public_https.hpp"

#include <algorithm>
#include <array>
#include <string>

#include "esp_crt_bundle.h"
#include "esp_err.h"
#include "esp_http_client.h"
#include "esp_timer.h"

#include "diagnostics/log.hpp"
#include "diagnostics/tags.hpp"

#include "public_stream.hpp"
#include "request_budget.hpp"
#include "vendor_log_policy.hpp"

namespace prd::network {
namespace {

constexpr std::size_t kReadChunk = 1024;
constexpr std::size_t kMaximumUrl = 1024;
constexpr std::size_t kMaximumLocation = 1024;
constexpr std::uint8_t kHardRedirectLimit = 2;
constexpr int kReadTimedOut = -ESP_ERR_HTTP_EAGAIN;

struct HeaderCapture {
    std::array<char, kMaximumLocation + 1> location{};
    std::size_t length{0};
    bool duplicate{false};
    bool overflow{false};
};

[[nodiscard]] bool ascii_equal_folded(const char* left, const std::string_view right) {
    if (left == nullptr) {
        return false;
    }
    std::size_t index = 0;
    while (left[index] != '\0' && index < right.size()) {
        const auto value = static_cast<unsigned char>(left[index]);
        const char folded = value >= 'A' && value <= 'Z' ? static_cast<char>(value + 0x20U)
                                                         : static_cast<char>(value);
        if (folded != right[index]) {
            return false;
        }
        ++index;
    }
    return index == right.size() && left[index] == '\0';
}

esp_err_t capture_header(esp_http_client_event_t* event) {
    if (event == nullptr || event->event_id != HTTP_EVENT_ON_HEADER ||
        !ascii_equal_folded(event->header_key, "location")) {
        return ESP_OK;
    }
    auto* capture = static_cast<HeaderCapture*>(event->user_data);
    if (capture == nullptr || event->header_value == nullptr) {
        return ESP_OK;
    }
    if (capture->length != 0 || capture->overflow) {
        capture->duplicate = true;
        return ESP_OK;
    }
    const std::string_view value{event->header_value};
    if (value.empty() || value.size() > kMaximumLocation) {
        capture->overflow = true;
        return ESP_OK;
    }
    std::copy(value.begin(), value.end(), capture->location.begin());
    capture->length = value.size();
    return ESP_OK;
}

[[nodiscard]] std::uint32_t elapsed_ms(const std::int64_t started_us) {
    const std::int64_t elapsed = esp_timer_get_time() - started_us;
    return elapsed <= 0 ? 0U : static_cast<std::uint32_t>(elapsed / 1000);
}

[[nodiscard]] budget::Allowances allowances(const PublicHttpsRequest& request) {
    return {request.total_budget_ms, request.slice_ms, request.connect_budget_ms};
}

enum class Continue : std::uint8_t { Yes, Deadline, Abandoned };

[[nodiscard]] Continue may_continue(const PublicHttpsRequest& request,
                                    const std::int64_t started_us) {
    if (request.abandon != nullptr && request.abandon(request.abandon_context)) {
        return Continue::Abandoned;
    }
    return budget::exhausted(allowances(request), elapsed_ms(started_us)) ? Continue::Deadline
                                                                          : Continue::Yes;
}

[[nodiscard]] PublicHttpsOutcome stopped_outcome(const Continue verdict) {
    return verdict == Continue::Abandoned ? PublicHttpsOutcome::Cancelled
                                          : PublicHttpsOutcome::Timeout;
}

[[nodiscard]] PublicHttpsOutcome open_outcome(const esp_err_t error) {
    if (error == ESP_ERR_HTTP_CONNECT) {
        return PublicHttpsOutcome::ConnectFailed;
    }
    if (error == ESP_ERR_HTTP_EAGAIN) {
        return PublicHttpsOutcome::Timeout;
    }
    if (error == ESP_ERR_INVALID_ARG) {
        return PublicHttpsOutcome::Rejected;
    }
    return PublicHttpsOutcome::TransportFailed;
}

[[nodiscard]] bool redirect_status(const std::uint16_t status) {
    return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

[[nodiscard]] bool request_shape_valid(const PublicHttpsRequest& request) {
    return !request.url.empty() && request.url.size() <= kMaximumUrl &&
           request.url.compare(0, 8, "https://") == 0 && request.maximum_body_bytes != 0 &&
           request.total_budget_ms != 0 && request.sink != nullptr &&
           request.url_policy != nullptr && request.maximum_redirects <= kHardRedirectLimit;
}

} // namespace

std::string_view log_label(const PublicHttpsOutcome outcome) {
    switch (outcome) {
    case PublicHttpsOutcome::Ok:
        return "ok";
    case PublicHttpsOutcome::HttpStatus:
        return "http-status";
    case PublicHttpsOutcome::ConnectFailed:
        return "connect-failed";
    case PublicHttpsOutcome::Timeout:
        return "timeout";
    case PublicHttpsOutcome::Cancelled:
        return "cancelled";
    case PublicHttpsOutcome::ResponseTooLarge:
        return "response-too-large";
    case PublicHttpsOutcome::SinkRejected:
        return "sink-rejected";
    case PublicHttpsOutcome::RedirectRejected:
        return "redirect-rejected";
    case PublicHttpsOutcome::TransportFailed:
        return "transport-failed";
    case PublicHttpsOutcome::Rejected:
        return "rejected";
    }
    return "unknown";
}

PublicHttpsResponse get_public_https(const PublicHttpsRequest& request) {
    PublicHttpsResponse response;
    if (!request_shape_valid(request) ||
        !request.url_policy(request.url_policy_context, {}, request.url, 0)) {
        return response;
    }

    vendor_log_policy::apply();
    const std::int64_t started_us = esp_timer_get_time();
    std::string current{request.url};
    PublicStream stream{request.maximum_body_bytes, request.sink, request.sink_context};

    for (;;) {
        const Continue before_connect = may_continue(request, started_us);
        if (before_connect != Continue::Yes) {
            response.outcome = stopped_outcome(before_connect);
            break;
        }

        HeaderCapture headers;
        esp_http_client_config_t config = {};
        config.url = current.c_str();
        config.method = HTTP_METHOD_GET;
        config.timeout_ms = static_cast<int>(
            budget::connect_timeout_ms(allowances(request), elapsed_ms(started_us)));
        config.disable_auto_redirect = true;
        config.max_redirection_count = 1;
        if (request.certificate_pem != nullptr) {
            config.cert_pem = request.certificate_pem;
        } else {
            config.crt_bundle_attach = esp_crt_bundle_attach;
        }
        config.event_handler = capture_header;
        config.user_data = &headers;

        esp_http_client_handle_t client = esp_http_client_init(&config);
        if (client == nullptr) {
            response.outcome = PublicHttpsOutcome::Rejected;
            break;
        }

        const bool json = request.media != PublicMedia::Binary;
        const char* accept = json ? "application/vnd.github+json" : "application/octet-stream";
        if (esp_http_client_set_header(client, "Accept", accept) != ESP_OK ||
            esp_http_client_set_header(client, "User-Agent", "prusa-remote-display") != ESP_OK) {
            esp_http_client_cleanup(client);
            response.outcome = PublicHttpsOutcome::Rejected;
            break;
        }
        if (request.media == PublicMedia::GithubJson &&
            esp_http_client_set_header(client, "X-GitHub-Api-Version", "2026-03-10") != ESP_OK) {
            esp_http_client_cleanup(client);
            response.outcome = PublicHttpsOutcome::Rejected;
            break;
        }

        const esp_err_t opened = esp_http_client_open(client, 0);
        if (opened != ESP_OK) {
            const Continue verdict = may_continue(request, started_us);
            response.outcome =
                verdict == Continue::Yes ? open_outcome(opened) : stopped_outcome(verdict);
            esp_http_client_cleanup(client);
            break;
        }

        bool header_failed = false;
        for (;;) {
            const Continue verdict = may_continue(request, started_us);
            if (verdict != Continue::Yes) {
                response.outcome = stopped_outcome(verdict);
                header_failed = true;
                break;
            }
            const auto timeout =
                budget::slice_timeout_ms(allowances(request), elapsed_ms(started_us));
            (void)esp_http_client_set_timeout_ms(client, static_cast<int>(timeout));
            const std::int64_t fetched = esp_http_client_fetch_headers(client);
            if (fetched >= 0) {
                break;
            }
            if (fetched != kReadTimedOut) {
                response.outcome = PublicHttpsOutcome::TransportFailed;
                header_failed = true;
                break;
            }
        }

        if (header_failed) {
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            break;
        }

        response.status = static_cast<std::uint16_t>(esp_http_client_get_status_code(client));
        if (redirect_status(response.status)) {
            const std::string_view location{headers.location.data(), headers.length};
            const auto next_count = static_cast<std::uint8_t>(response.redirects + 1U);
            const bool allowed =
                !headers.duplicate && !headers.overflow && !location.empty() &&
                next_count <= request.maximum_redirects && location.size() <= kMaximumUrl &&
                request.url_policy(request.url_policy_context, current, location, next_count);
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            if (!allowed) {
                response.outcome = PublicHttpsOutcome::RedirectRejected;
                break;
            }
            current.assign(location);
            response.redirects = next_count;
            continue;
        }

        if (response.status != 200) {
            response.outcome = PublicHttpsOutcome::HttpStatus;
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            break;
        }

        const std::int64_t declared = esp_http_client_get_content_length(client);
        if (declared > 0 && static_cast<std::uint64_t>(declared) > request.maximum_body_bytes) {
            response.outcome = PublicHttpsOutcome::ResponseTooLarge;
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            break;
        }

        std::array<std::uint8_t, kReadChunk> chunk{};
        response.outcome = PublicHttpsOutcome::Ok;
        for (;;) {
            const Continue verdict = may_continue(request, started_us);
            if (verdict != Continue::Yes) {
                response.outcome = stopped_outcome(verdict);
                break;
            }
            const auto timeout =
                budget::slice_timeout_ms(allowances(request), elapsed_ms(started_us));
            (void)esp_http_client_set_timeout_ms(client, static_cast<int>(timeout));
            const int read = esp_http_client_read(client, reinterpret_cast<char*>(chunk.data()),
                                                  static_cast<int>(chunk.size()));
            if (read == kReadTimedOut) {
                continue;
            }
            if (read < 0) {
                response.outcome = PublicHttpsOutcome::TransportFailed;
                break;
            }
            if (read == 0) {
                if (!esp_http_client_is_complete_data_received(client)) {
                    response.outcome = PublicHttpsOutcome::TransportFailed;
                }
                break;
            }
            const auto streamed = stream.append(chunk.data(), static_cast<std::size_t>(read));
            if (streamed == PublicStreamResult::TooLarge) {
                response.outcome = PublicHttpsOutcome::ResponseTooLarge;
                break;
            }
            if (streamed == PublicStreamResult::SinkRejected) {
                response.outcome = PublicHttpsOutcome::SinkRejected;
                break;
            }
        }
        response.body_length = stream.length();
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        break;
    }

    response.latency_ms = elapsed_ms(started_us);
    diagnostics::log(diagnostics::LogLevel::Debug, diagnostics::tag::kNetwork,
                     "public https outcome=" + std::string{log_label(response.outcome)} +
                         " status=" + std::to_string(response.status) +
                         " latency_ms=" + std::to_string(response.latency_ms) +
                         " bytes=" + std::to_string(response.body_length) +
                         " redirects=" + std::to_string(response.redirects));
    return response;
}

} // namespace prd::network
