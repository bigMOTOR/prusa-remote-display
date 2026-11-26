// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstdint>

namespace prd::app_model {

enum class PrinterValidationStage : std::uint8_t {
    None,
    AddressCheck,
    RequestPreparation,
    VersionProbe,
    Status,
};

enum class PrinterValidationReason : std::uint8_t {
    None,
    Unreachable,
    Timeout,
    TransportFailed,
    RequestRejected,
    ResponseTooLarge,
    Unauthorized,
    EndpointUnsupported,
    RequestUnsupported,
    PrinterError,
    UnexpectedStatus,
    MalformedBody,
    SchemaMismatch,
    HostUnusable,
    NameNotResolved,
    NotOnLan,
    Cancelled,
};

// Safe to copy across tasks. A default value means no validation failure was reported.
struct PrinterValidationFailure {
    PrinterValidationStage stage{PrinterValidationStage::None};
    PrinterValidationReason reason{PrinterValidationReason::None};

    // A valid HTTP status received with the response headers, even if reading the body failed.
    // Zero means no valid status was available.
    std::uint16_t http_status{0};

    [[nodiscard]] constexpr bool has_http_status() const {
        return http_status >= 100 && http_status <= 599;
    }

    [[nodiscard]] constexpr bool body_interrupted() const {
        return has_http_status() && (reason == PrinterValidationReason::Timeout ||
                                     reason == PrinterValidationReason::TransportFailed);
    }

    [[nodiscard]] constexpr bool operator==(const PrinterValidationFailure& other) const {
        return stage == other.stage && reason == other.reason && http_status == other.http_status;
    }

    [[nodiscard]] constexpr bool operator!=(const PrinterValidationFailure& other) const {
        return !(*this == other);
    }
};

} // namespace prd::app_model
