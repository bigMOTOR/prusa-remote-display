// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

// The generic bounded HTTP client wrapper (Architecture Spec §5.7).
//
// §5.7 assigns this component the HTTP client, request timeout and cancellation, and lists what
// the wrapper must centralize: connection and total timeouts, response-size limits, status-code
// capture, header redaction, latency measurement and safe response buffering. It also states the
// boundary this header keeps: "It must not understand printer states or PrusaLink JSON." Nothing
// here names an endpoint, a printer or a credential.
//
// ⛔ HEADER VALUES ARE NEVER LOGGED BY THIS COMPONENT. The API key travels as a header value (API
// Research Report §10.2), so §13.5's "credentials must never be written to logs" is met
// structurally: this component logs the header *count*, never a name/value pair. ESP-IDF's own
// HTTP components do print header values at Debug, and vendor_log_policy.hpp holds them below it.

namespace prd::network {

// Why a request produced no usable response, or that it did.
//
// These are transport facts only. What a given failure *means* for the product — which connection
// phase a user sees — is a printer-domain decision and belongs to `prusalink`, per §4.2.
enum class HttpOutcome : std::uint8_t {
    // The exchange completed and `status` carries the server's code. A 401 or a 500 is still Ok
    // here: the transport worked.
    Ok,

    // The connection could not be established: refused, unreachable, or a host that did not
    // resolve.
    //
    // §16.1 lists DNS failure as a transport error distinct from connection refused, and this
    // adapter does not claim that distinction — a caller that needs it resolves first through
    // network/lan.hpp, which reports resolution separately and is what the credentialed path does.
    ConnectFailed,

    // The total budget elapsed. §11.4's "total status request timeout", enforced as a wall clock
    // across connect, header fetch and every body read rather than claimed per socket operation.
    Timeout,

    // The caller's own abandon predicate asked for the exchange to stop — a superseded worker, a
    // configuration replaced under it. Kept distinct from Timeout so the product can classify
    // truthfully: nothing is wrong with the printer in this case.
    Cancelled,

    // The body exceeded the caller's buffer. Reported rather than truncated: a partially read body
    // must never reach a decoder (§9.3).
    ResponseTooLarge,

    // Any other transport-level failure, including a reset mid-body.
    TransportFailed,

    // The request itself was refused before any I/O — an unusable URL, or a client that could not
    // be constructed. Distinct from a network failure because retrying it cannot help.
    Rejected,
};

[[nodiscard]] std::string_view log_label(HttpOutcome outcome);

struct HttpHeader {
    std::string_view name;

    // ⛔ May be a credential. Never logged, never copied anywhere but the request.
    std::string_view value;
};

// Whether the caller wants the exchange abandoned now. Called between blocking slices, on the
// requesting task.
//
// ⬅️ CANCELLATION IS POLLED, NOT PUSHED, AND THAT IS THE WHOLE DESIGN. The obvious alternative —
// letting another task call esp_http_client_cancel_request() on the handle — does not work here:
// in the pinned v5.5.4 that function closes the transport and then immediately RECONNECTS
// (esp_http_client.c:440-448), it is meant for abandoning a response body on a reused connection,
// and it is not safe against a concurrent blocked read on the same transport. Polling instead
// means the handle is only ever touched by the task that owns it, so there is no shared lifetime
// to synchronize and no window in which a handle is freed under another task's call.
using AbandonPredicate = bool (*)(void* context);

struct HttpRequest {
    std::string_view url;

    const HttpHeader* headers{nullptr};
    std::size_t header_count{0};

    // The whole exchange's wall-clock budget: resolution excluded, connect through last body byte
    // included. API Research Report §12.4 proposes 4 s total and §11.4 approximately 5 s; both are
    // marked as requiring physical validation, so this is a parameter rather than a constant.
    std::uint32_t total_budget_ms{4000};

    // How long any single blocking call may take before control returns here to check the budget
    // and the abandon predicate. Bounds the latency of a cancellation and of the deadline; it does
    // not extend either.
    std::uint32_t slice_ms{400};

    // ⛔ THE CONNECT PHASE'S OWN ALLOWANCE, BECAUSE IT IS ONE CALL AND CANNOT BE SLICED. §11.4
    // specifies "TCP/connect timeout: approximately 2-3 s"; 2500 ms is the middle of it. Giving
    // connect the read loop's 400 ms slice instead — which is what this code used to do — reported a
    // printer unreachable after 400 ms with most of the budget unspent, and a station using ESP32
    // Wi-Fi power save can spend a DTIM interval on each of the ARP and the handshake.
    //
    // ⬅️ IT CANNOT EXTEND THE TOTAL. budget::connect_ms() clamps it to what remains of
    // total_budget_ms, so this shortens a phase and never lengthens a request.
    std::uint32_t connect_budget_ms{2500};

    // Optional. When null, the exchange runs to its budget.
    AbandonPredicate abandon{nullptr};
    void* abandon_context{nullptr};
};

struct HttpResponse {
    HttpOutcome outcome{HttpOutcome::Rejected};

    // The HTTP status, when `outcome` is Ok. Zero otherwise.
    std::uint16_t status{0};

    // Measured across the whole exchange, for §7.5's lastLatencyMs.
    std::uint32_t latency_ms{0};

    std::size_t body_length{0};
};

// Accumulates a response body into a caller-owned buffer, refusing to exceed it.
//
// Separated from the transport so the bound is provable without a network: this is the unit that
// satisfies §16.4's "every network body and queue must be bounded", and it is compiled into the
// host suite.
class BodyBuffer {
  public:
    BodyBuffer(char* storage, std::size_t capacity) : storage_(storage), capacity_(capacity) {}

    // Appends a chunk. False once the buffer is full, and the overflow latches: a body that was
    // too large stays too large even if later chunks would fit.
    bool append(std::string_view chunk);

    [[nodiscard]] std::string_view view() const {
        return {storage_, length_};
    }

    [[nodiscard]] std::size_t length() const {
        return length_;
    }

    [[nodiscard]] std::size_t capacity() const {
        return capacity_;
    }

    [[nodiscard]] bool overflowed() const {
        return overflowed_;
    }

    void reset() {
        length_ = 0;
        overflowed_ = false;
    }

  private:
    char* storage_{nullptr};
    std::size_t capacity_{0};
    std::size_t length_{0};
    bool overflowed_{false};
};

// Performs one bounded GET, writing the body into `buffer`.
//
// Sequential and blocking, but never for longer than `total_budget_ms` — §10.4's single request
// worker calls this and nothing else does. It never dispatches, never touches application state,
// and holds no state between calls.
[[nodiscard]] HttpResponse get(const HttpRequest& request, BodyBuffer& buffer);

} // namespace prd::network
