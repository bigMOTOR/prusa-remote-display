// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include "request_validation.hpp"

#include <cstddef>

namespace prd::provisioning {
namespace {

[[nodiscard]] bool is_printable(const char character) {
    const auto byte = static_cast<unsigned char>(character);
    return byte >= 0x20U && byte <= 0x7EU;
}

// Control characters and DEL. Everything else, including bytes above 0x7E, is left to the
// field's own rule — an SSID may legitimately be UTF-8.
[[nodiscard]] bool is_control(const char character) {
    const auto byte = static_cast<unsigned char>(character);
    return byte < 0x20U || byte == 0x7FU;
}

[[nodiscard]] bool is_digit(const char character) {
    return character >= '0' && character <= '9';
}

[[nodiscard]] bool is_host_character(const char character) {
    return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
           is_digit(character) || character == '-' || character == '.';
}

// RFC 1123: labels are 1..63 characters, alphanumeric or hyphen, and may neither begin nor
// end with a hyphen. An empty label means a doubled or trailing dot.
[[nodiscard]] bool labels_are_wellformed(const std::string_view value) {
    std::size_t label_length = 0;
    char previous = '.';

    for (const char character : value) {
        if (character == '.') {
            if (label_length == 0 || previous == '-') {
                return false;
            }
            label_length = 0;
            previous = character;
            continue;
        }

        if (label_length == 0 && character == '-') {
            return false;
        }

        ++label_length;
        if (label_length > 63) {
            return false;
        }

        previous = character;
    }

    return label_length != 0 && previous != '-';
}

} // namespace

std::string_view log_label(const FieldOutcome outcome) {
    switch (outcome) {
    case FieldOutcome::Ok:
        return "ok";

    case FieldOutcome::Missing:
        return "missing";

    case FieldOutcome::TooShort:
        return "too-short";

    case FieldOutcome::TooLong:
        return "too-long";

    case FieldOutcome::BadCharacter:
        return "bad-character";
    }

    return "bad-character";
}

FieldOutcome validate_ssid(const std::string_view value) {
    if (value.empty()) {
        return FieldOutcome::Missing;
    }

    if (value.size() > kSsidMax) {
        return FieldOutcome::TooLong;
    }

    for (const char character : value) {
        if (is_control(character)) {
            return FieldOutcome::BadCharacter;
        }
    }

    return FieldOutcome::Ok;
}

FieldOutcome validate_wifi_password(const std::string_view value) {
    // An open network. See the header: absent is a value here, not an omission.
    if (value.empty()) {
        return FieldOutcome::Ok;
    }

    if (value.size() < kWifiPasswordMin) {
        return FieldOutcome::TooShort;
    }

    if (value.size() > kWifiPasswordMax) {
        return FieldOutcome::TooLong;
    }

    for (const char character : value) {
        if (!is_printable(character)) {
            return FieldOutcome::BadCharacter;
        }
    }

    return FieldOutcome::Ok;
}

FieldOutcome validate_printer_host(const std::string_view value) {
    if (value.empty()) {
        return FieldOutcome::Missing;
    }

    if (value.size() > kPrinterHostMax) {
        return FieldOutcome::TooLong;
    }

    // The character set alone refuses a scheme (`/`, `:`), a path (`/`), a query (`?`,
    // `&`, `=`), userinfo (`@`, `:`), an embedded port (`:`), whitespace and every control
    // byte — before any structural rule runs. Anything that survives is dots, hyphens and
    // alphanumerics, which is the only shape a host may take.
    for (const char character : value) {
        if (!is_host_character(character)) {
            return FieldOutcome::BadCharacter;
        }
    }

    if (!labels_are_wellformed(value)) {
        return FieldOutcome::BadCharacter;
    }

    return FieldOutcome::Ok;
}

FieldOutcome validate_api_key(const std::string_view value) {
    if (value.empty()) {
        return FieldOutcome::Missing;
    }

    if (value.size() > kApiKeyMax) {
        return FieldOutcome::TooLong;
    }

    for (const char character : value) {
        if (!is_printable(character)) {
            return FieldOutcome::BadCharacter;
        }
    }

    return FieldOutcome::Ok;
}

PortResult validate_printer_port(const std::string_view value) {
    if (value.empty()) {
        return {FieldOutcome::Missing, 0};
    }

    std::uint32_t accumulated = 0;

    for (const char character : value) {
        if (!is_digit(character)) {
            return {FieldOutcome::BadCharacter, 0};
        }

        accumulated = (accumulated * 10U) + static_cast<std::uint32_t>(character - '0');

        // Checked inside the loop, not after it: a long run of digits would otherwise wrap
        // and land on a plausible port number.
        if (accumulated > kPortMax) {
            return {FieldOutcome::TooLong, 0};
        }
    }

    if (accumulated < kPortMin) {
        return {FieldOutcome::TooShort, 0};
    }

    return {FieldOutcome::Ok, static_cast<std::uint16_t>(accumulated)};
}

std::string_view log_label(const Stage stage) {
    switch (stage) {
    case Stage::Wifi:
        return "wifi";

    case Stage::Printer:
        return "printer";
    }

    return "wifi";
}

SubmissionReport validate(const Submission& submission, const Stage stage) {
    SubmissionReport report;

    if (stage == Stage::Wifi) {
        report.wifi_ssid = validate_ssid(submission.wifi_ssid);
        report.wifi_password = validate_wifi_password(submission.wifi_password);
        return report;
    }

    report.printer_host = validate_printer_host(submission.printer_host);
    report.printer_api_key = validate_api_key(submission.printer_api_key);

    const PortResult port = validate_printer_port(submission.printer_port);
    report.printer_port = port.outcome;
    report.port_value = port.value;

    return report;
}

} // namespace prd::provisioning
