// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include "outcome.hpp"

namespace prd::prusalink {
namespace {

using app_model::PrinterValidationReason;

[[nodiscard]] AttemptOutcome from_status(const std::uint16_t status) {
    // 401 and 403 are §16.1's authentication errors. They are not transient and are
    // the one case where the product must tell the user something specific: the key
    // is wrong, and no amount of waiting will fix it.
    if (status == 401 || status == 403) {
        return {app_model::ConnectionPhase::Unauthorized, PrinterValidationReason::Unauthorized,
                false};
    }

    // §16.1's "endpoint missing" — a service answering on this address that has no
    // such path is not a PrusaLink this build can use.
    if (status == 404) {
        return {app_model::ConnectionPhase::UnsupportedApi,
                PrinterValidationReason::EndpointUnsupported, false};
    }

    // §16.2 lists temporary 5xx among the failures to retry: the printer is there and
    // failing, which is unavailability rather than incompatibility.
    if (status >= 500) {
        return {app_model::ConnectionPhase::PrinterUnavailable,
                PrinterValidationReason::PrinterError, false};
    }

    if (status >= 400) {
        return {app_model::ConnectionPhase::UnsupportedApi,
                PrinterValidationReason::RequestUnsupported, false};
    }

    // Redirects are disabled deliberately, so a 3xx means the endpoint is not where
    // this build expects it — a compatibility fact, not a transport one.
    if (status < 200 || status >= 300) {
        return {app_model::ConnectionPhase::UnsupportedApi,
                PrinterValidationReason::UnexpectedStatus, false};
    }

    return {app_model::ConnectionPhase::Connected, PrinterValidationReason::None, true};
}

[[nodiscard]] AttemptOutcome from_decode(const DecodeError decode) {
    switch (decode) {
    case DecodeError::None:
        return {app_model::ConnectionPhase::Connected, PrinterValidationReason::None, true};

    case DecodeError::Malformed:
        return {app_model::ConnectionPhase::InvalidResponse, PrinterValidationReason::MalformedBody,
                false};

    // A 200 whose body lacks the required object or field is a schema mismatch, not a
    // syntax error: something answered correctly at the HTTP level and did not speak
    // the contract. §16.1 keeps "malformed required field" among the API compatibility
    // errors, and §7.5's InvalidResponse is where a body that cannot be trusted lands.
    case DecodeError::MissingObject:
    case DecodeError::MissingField:
    case DecodeError::UnusableField:
        return {app_model::ConnectionPhase::InvalidResponse,
                PrinterValidationReason::SchemaMismatch, false};
    }

    return {app_model::ConnectionPhase::InvalidResponse, PrinterValidationReason::MalformedBody,
            false};
}

} // namespace

std::string_view AttemptOutcome::error() const {
    switch (reason) {
    case PrinterValidationReason::None:
        return {};

    case PrinterValidationReason::Unreachable:
        return "unreachable";

    case PrinterValidationReason::Timeout:
        return "timeout";

    case PrinterValidationReason::TransportFailed:
        return "transport";

    case PrinterValidationReason::RequestRejected:
        return "request-rejected";

    case PrinterValidationReason::ResponseTooLarge:
        return "response-too-large";

    case PrinterValidationReason::Unauthorized:
        return "unauthorized";

    case PrinterValidationReason::EndpointUnsupported:
        return "endpoint-unsupported";

    case PrinterValidationReason::RequestUnsupported:
        return "request-unsupported";

    case PrinterValidationReason::PrinterError:
        return "printer-error";

    case PrinterValidationReason::UnexpectedStatus:
        return "unexpected-status";

    case PrinterValidationReason::MalformedBody:
        return "malformed-body";

    case PrinterValidationReason::SchemaMismatch:
        return "schema-mismatch";

    case PrinterValidationReason::HostUnusable:
        return "printer-address-unusable";

    case PrinterValidationReason::NameNotResolved:
        return "printer-name-not-resolved";

    case PrinterValidationReason::NotOnLan:
        return "printer-not-on-lan";

    case PrinterValidationReason::Cancelled:
        return "cancelled";
    }

    return {};
}

AttemptOutcome classify(const network::HttpOutcome transport, const std::uint16_t status,
                        const DecodeError decode) {
    switch (transport) {
    case network::HttpOutcome::ConnectFailed:
        return {app_model::ConnectionPhase::PrinterUnavailable,
                PrinterValidationReason::Unreachable, false};

    case network::HttpOutcome::Timeout:
        return {app_model::ConnectionPhase::PrinterUnavailable, PrinterValidationReason::Timeout,
                false};

    // ⬅️ CANCELLED IS NOT A PRINTER FAULT, AND MUST NOT LOOK LIKE ONE. The exchange was abandoned
    // because this worker was superseded — a reconfiguration, a network drop, a factory reset. The
    // printer may be perfectly healthy. It is reported as Degraded rather than PrinterUnavailable so
    // a log or a panel never blames the printer for the device's own lifecycle, and in practice the
    // report is dropped by the identity check anyway: a superseded worker's result is not published.
    case network::HttpOutcome::Cancelled:
        return {app_model::ConnectionPhase::Degraded, PrinterValidationReason::Cancelled, false};

    case network::HttpOutcome::TransportFailed:
        return {app_model::ConnectionPhase::PrinterUnavailable,
                PrinterValidationReason::TransportFailed, false};

    // Our own request could not be issued. The user-visible fact is still that the
    // printer was not reached, and the label is what separates it in diagnostics from
    // a printer that failed to answer.
    case network::HttpOutcome::Rejected:
        return {app_model::ConnectionPhase::PrinterUnavailable,
                PrinterValidationReason::RequestRejected, false};

    // A body that exceeded its bound is a response this device may not decode
    // (§16.1's "response too large", §9.3). The transport worked, so the fact is
    // about the response rather than the printer's reachability.
    case network::HttpOutcome::ResponseTooLarge:
        return {app_model::ConnectionPhase::InvalidResponse,
                PrinterValidationReason::ResponseTooLarge, false};

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
        return {app_model::ConnectionPhase::Connected, PrinterValidationReason::None, true};

    // The stored host cannot be used at all. Unconfigured is the honest phase: what exists is not a
    // usable printer configuration, and no amount of network will change that.
    case TargetVerdict::HostUnusable:
        return {app_model::ConnectionPhase::Unconfigured, PrinterValidationReason::HostUnusable,
                false};

    // A name that does not resolve. The printer is not reachable by the name given, which is a
    // reachability fact and retried like one.
    case TargetVerdict::NotResolved:
        return {app_model::ConnectionPhase::PrinterUnavailable,
                PrinterValidationReason::NameNotResolved, false};

    // ⛔ It resolved off the LAN. Unconfigured rather than unavailable, and deliberately: the
    // address is wrong for this product, not temporarily unreachable, and retrying cannot fix it.
    case TargetVerdict::NotOnLan:
        return {app_model::ConnectionPhase::Unconfigured, PrinterValidationReason::NotOnLan, false};
    }

    return {app_model::ConnectionPhase::Unconfigured, PrinterValidationReason::HostUnusable, false};
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
