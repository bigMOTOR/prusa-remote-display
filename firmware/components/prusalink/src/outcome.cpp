// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include "outcome.hpp"

namespace prd::prusalink {
namespace {

// The fixed label set. Every string §7.5's lastError can hold comes from here, so the
// field's contents are a closed set rather than whatever a caller passed in.
constexpr std::string_view kErrorUnreachable = "unreachable";
constexpr std::string_view kErrorTimeout = "timeout";
constexpr std::string_view kErrorTransport = "transport";
constexpr std::string_view kErrorRejected = "request-rejected";
constexpr std::string_view kErrorTooLarge = "response-too-large";
constexpr std::string_view kErrorUnauthorized = "unauthorized";
constexpr std::string_view kErrorEndpoint = "endpoint-unsupported";
constexpr std::string_view kErrorRequest = "request-unsupported";
constexpr std::string_view kErrorPrinterFault = "printer-error";
constexpr std::string_view kErrorStatus = "unexpected-status";
constexpr std::string_view kErrorMalformed = "malformed-body";
constexpr std::string_view kErrorSchema = "schema-mismatch";
constexpr std::string_view kErrorHostUnusable = "printer-address-unusable";
constexpr std::string_view kErrorNotResolved = "printer-name-not-resolved";
constexpr std::string_view kErrorNotOnLan = "printer-not-on-lan";
constexpr std::string_view kErrorCancelled = "cancelled";

[[nodiscard]] AttemptOutcome from_status(const std::uint16_t status) {
    // 401 and 403 are §16.1's authentication errors. They are not transient and are
    // the one case where the product must tell the user something specific: the key
    // is wrong, and no amount of waiting will fix it.
    if (status == 401 || status == 403) {
        return {app_model::ConnectionPhase::Unauthorized, kErrorUnauthorized, false};
    }

    // §16.1's "endpoint missing" — a service answering on this address that has no
    // such path is not a PrusaLink this build can use.
    if (status == 404) {
        return {app_model::ConnectionPhase::UnsupportedApi, kErrorEndpoint, false};
    }

    // §16.2 lists temporary 5xx among the failures to retry: the printer is there and
    // failing, which is unavailability rather than incompatibility.
    if (status >= 500) {
        return {app_model::ConnectionPhase::PrinterUnavailable, kErrorPrinterFault, false};
    }

    if (status >= 400) {
        return {app_model::ConnectionPhase::UnsupportedApi, kErrorRequest, false};
    }

    // Redirects are disabled deliberately, so a 3xx means the endpoint is not where
    // this build expects it — a compatibility fact, not a transport one.
    if (status < 200 || status >= 300) {
        return {app_model::ConnectionPhase::UnsupportedApi, kErrorStatus, false};
    }

    return {app_model::ConnectionPhase::Connected, {}, true};
}

[[nodiscard]] AttemptOutcome from_decode(const DecodeError decode) {
    switch (decode) {
    case DecodeError::None:
        return {app_model::ConnectionPhase::Connected, {}, true};

    case DecodeError::Malformed:
        return {app_model::ConnectionPhase::InvalidResponse, kErrorMalformed, false};

    // A 200 whose body lacks the required object or field is a schema mismatch, not a
    // syntax error: something answered correctly at the HTTP level and did not speak
    // the contract. §16.1 keeps "malformed required field" among the API compatibility
    // errors, and §7.5's InvalidResponse is where a body that cannot be trusted lands.
    case DecodeError::MissingObject:
    case DecodeError::MissingField:
    case DecodeError::UnusableField:
        return {app_model::ConnectionPhase::InvalidResponse, kErrorSchema, false};
    }

    return {app_model::ConnectionPhase::InvalidResponse, kErrorMalformed, false};
}

} // namespace

AttemptOutcome classify(const network::HttpOutcome transport, const std::uint16_t status,
                        const DecodeError decode) {
    switch (transport) {
    case network::HttpOutcome::ConnectFailed:
        return {app_model::ConnectionPhase::PrinterUnavailable, kErrorUnreachable, false};

    case network::HttpOutcome::Timeout:
        return {app_model::ConnectionPhase::PrinterUnavailable, kErrorTimeout, false};

    // ⬅️ CANCELLED IS NOT A PRINTER FAULT, AND MUST NOT LOOK LIKE ONE. The exchange was abandoned
    // because this worker was superseded — a reconfiguration, a network drop, a factory reset. The
    // printer may be perfectly healthy. It is reported as Degraded rather than PrinterUnavailable so
    // a log or a panel never blames the printer for the device's own lifecycle, and in practice the
    // report is dropped by the identity check anyway: a superseded worker's result is not published.
    case network::HttpOutcome::Cancelled:
        return {app_model::ConnectionPhase::Degraded, kErrorCancelled, false};

    case network::HttpOutcome::TransportFailed:
        return {app_model::ConnectionPhase::PrinterUnavailable, kErrorTransport, false};

    // Our own request could not be issued. The user-visible fact is still that the
    // printer was not reached, and the label is what separates it in diagnostics from
    // a printer that failed to answer.
    case network::HttpOutcome::Rejected:
        return {app_model::ConnectionPhase::PrinterUnavailable, kErrorRejected, false};

    // A body that exceeded its bound is a response this device may not decode
    // (§16.1's "response too large", §9.3). The transport worked, so the fact is
    // about the response rather than the printer's reachability.
    case network::HttpOutcome::ResponseTooLarge:
        return {app_model::ConnectionPhase::InvalidResponse, kErrorTooLarge, false};

    case network::HttpOutcome::Ok:
        break;
    }

    const AttemptOutcome by_status = from_status(status);

    if (!by_status.usable) {
        return by_status;
    }

    return from_decode(decode);
}

AttemptOutcome classify_target(const TargetVerdict verdict) {
    switch (verdict) {
    case TargetVerdict::Ok:
        return {app_model::ConnectionPhase::Connected, {}, true};

    // The stored host cannot be used at all. Unconfigured is the honest phase: what exists is not a
    // usable printer configuration, and no amount of network will change that.
    case TargetVerdict::HostUnusable:
        return {app_model::ConnectionPhase::Unconfigured, kErrorHostUnusable, false};

    // A name that does not resolve. The printer is not reachable by the name given, which is a
    // reachability fact and retried like one.
    case TargetVerdict::NotResolved:
        return {app_model::ConnectionPhase::PrinterUnavailable, kErrorNotResolved, false};

    // ⛔ It resolved off the LAN. Unconfigured rather than unavailable, and deliberately: the
    // address is wrong for this product, not temporarily unreachable, and retrying cannot fix it.
    case TargetVerdict::NotOnLan:
        return {app_model::ConnectionPhase::Unconfigured, kErrorNotOnLan, false};
    }

    return {app_model::ConnectionPhase::Unconfigured, kErrorHostUnusable, false};
}

std::chrono::milliseconds next_delay(const app_model::ConnectionPhase phase) {
    switch (phase) {
    // §11.1's idle-or-ready interval, which is the only operational state this
    // checkpoint can reach.
    case app_model::ConnectionPhase::Connected:
        return std::chrono::milliseconds{5000};

    // §16.2: not transient failures, and not to be retried aggressively. An
    // infrequent probe keeps a corrected API key or an upgraded printer from needing
    // a reboot to be noticed, without polling something that has refused us.
    case app_model::ConnectionPhase::Unauthorized:
    case app_model::ConnectionPhase::UnsupportedApi:

    // A destination the policy refuses, or a host that cannot be used, is in the same class: not a
    // transient network condition, and retrying it quickly would be a resolver storm against a name
    // that is not going to start being on the LAN.
    case app_model::ConnectionPhase::Unconfigured:
        return std::chrono::milliseconds{60000};

    case app_model::ConnectionPhase::PrinterUnavailable:
    case app_model::ConnectionPhase::InvalidResponse:
    case app_model::ConnectionPhase::Degraded:
    case app_model::ConnectionPhase::WaitingForNetwork:
    case app_model::ConnectionPhase::Resolving:
    case app_model::ConnectionPhase::Probing:
    case app_model::ConnectionPhase::Authenticating:
        return std::chrono::milliseconds{5000};
    }

    return std::chrono::milliseconds{5000};
}

std::chrono::milliseconds poll_pause(const PollRequest request, const AttemptOutcome& outcome) {
    // ⛔ THE ONE CASE THAT DOES NOT WAIT. §15.1 step 21 fetches status straight after the version
    // probe; the interval is step 22's business.
    if (request == PollRequest::VersionProbe && outcome.usable) {
        return std::chrono::milliseconds{0};
    }

    // Everything else keeps the pacing it already had: a failed probe, and every status cycle
    // including the first.
    return next_delay(outcome.phase);
}

} // namespace prd::prusalink
