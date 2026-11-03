// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstdint>
#include <string_view>

// Validation of a submitted provisioning request — Architecture Spec §14.4.
//
// PURE, AND HOST-TESTED. No ESP-IDF header, no JSON parser, no global state. The adapter
// extracts fields from the request body and hands them here as views; every rule about
// what a field may contain lives in this file, where the host suite can walk it
// exhaustively. §14.4 requires the server to "validate host and port input", and a rule
// that can only be exercised by pointing a browser at a board is a rule nobody checks.
//
// ⛔ A REJECTION NEVER ECHOES THE VALUE. Each outcome names the field and the rule it
// broke and carries nothing the caller sent — §14.4's "escape all displayed values" is
// easiest to honour by never putting a submitted byte into a response at all.

namespace prd::provisioning {

// What a field failed on. One value per rule, so a response can name the rule without
// quoting the input, and a test can assert which rule fired rather than which message.
enum class FieldOutcome : std::uint8_t {
    Ok,

    Missing,     // absent or empty where a value is required
    TooShort,    //
    TooLong,     //
    BadCharacter // a byte the field's grammar does not permit
};

[[nodiscard]] std::string_view log_label(FieldOutcome outcome);

// --- Field limits -----------------------------------------------------------
//
// The Wi-Fi figures are IEEE 802.11 and WPA2, not preferences: an SSID is at most 32
// octets, and a WPA2 passphrase is 8 to 63 printable characters. The printer figures match
// what ConfigV1 and SecretConfigV1 already reserve, so a value that validates here cannot
// fail to fit the record M3.8 will write it to.

inline constexpr std::size_t kSsidMax = 32;
inline constexpr std::size_t kWifiPasswordMin = 8;
inline constexpr std::size_t kWifiPasswordMax = 63;
inline constexpr std::size_t kPrinterHostMax = 64;
inline constexpr std::size_t kApiKeyMax = 64;

inline constexpr std::uint32_t kPortMin = 1;
inline constexpr std::uint32_t kPortMax = 65535;

// --- Field rules ------------------------------------------------------------

// 1..32 octets, no control characters. The standard permits any octet; control bytes are
// refused because they cannot be shown on the panel or typed back by a person.
[[nodiscard]] FieldOutcome validate_ssid(std::string_view value);

// WPA2-PSK: 8..63 printable characters, or empty for an open network.
//
// EMPTY IS A DELIBERATE VALUE, not a missing one. An open network is a real thing a user
// may have, and refusing it would make the product unusable on it. The distinction between
// "no password because the network is open" and "the user has not typed one yet" belongs to
// the page, not here.
[[nodiscard]] FieldOutcome validate_wifi_password(std::string_view value);

// An IPv4 literal, an RFC 1123 hostname, or a `.local` name — and nothing else.
//
// REJECTS, EXPLICITLY: a scheme (`http://`), any path or query, an embedded port (`:8080`),
// userinfo (`user:pass@`), whitespace and control characters. Each of those is a way of
// smuggling something past a naive concatenation into a URL later, and §14.4 asks for this
// field in particular to be validated.
[[nodiscard]] FieldOutcome validate_printer_host(std::string_view value);

// Printable, length-bounded. The key's grammar is the printer's, not ours, so nothing more
// is assumed about it than that a person can type it.
[[nodiscard]] FieldOutcome validate_api_key(std::string_view value);

// --- Port -------------------------------------------------------------------

struct PortResult {
    FieldOutcome outcome{FieldOutcome::Missing};
    std::uint16_t value{0};
};

// Decimal digits only: no sign, no whitespace, no leading `+`, no hex. Overflow is
// detected during accumulation rather than after it, so a long digit run cannot wrap into
// a plausible port.
[[nodiscard]] PortResult validate_printer_port(std::string_view value);

// --- The whole submission ---------------------------------------------------

// What the adapter extracted. Views into the request body; nothing here owns anything.
struct Submission {
    std::string_view wifi_ssid;
    std::string_view wifi_password;
    std::string_view printer_host;
    std::string_view printer_port;
    std::string_view printer_api_key;
};

// Which step of the two-stage flow a submission belongs to — owner ruling 1, 2026-08-16.
//
// WHY THE FLOW IS TWO STAGES. Physical validation failed the single-press form: a user with
// a wrong Wi-Fi password could not discover that without also inventing a printer address
// and an API key, because every field was required at once. The stages exist so the user's
// mental model — connect to Wi-Fi, THEN configure the printer — is the flow's model too.
//
// ⬅️ ONLY THE BROWSER FLOW IS STAGED. The persistence transaction is not: nothing is written
// until the printer stage succeeds, and then config and secrets commit exactly as they
// always did. That is the whole point of taking this option over a two-stage transaction.
enum class Stage : std::uint8_t {
    // SSID and password. The device joins and validates for real, and stores nothing.
    Wifi,

    // Address, port and API key. Persists the pair.
    Printer,
};

[[nodiscard]] std::string_view log_label(Stage stage);

// One outcome per field. `accepted()` is true only when every field THIS STAGE requires
// passed — a Wi-Fi submission is not refused for lacking a printer, which is the defect
// that failed acceptance.
struct SubmissionReport {
    FieldOutcome wifi_ssid{FieldOutcome::Ok};
    FieldOutcome wifi_password{FieldOutcome::Ok};
    FieldOutcome printer_host{FieldOutcome::Ok};
    FieldOutcome printer_port{FieldOutcome::Ok};
    FieldOutcome printer_api_key{FieldOutcome::Ok};

    std::uint16_t port_value{0};

    [[nodiscard]] bool accepted(const Stage stage) const {
        if (stage == Stage::Wifi) {
            return wifi_ssid == FieldOutcome::Ok && wifi_password == FieldOutcome::Ok;
        }

        // Owner ruling 4: the printer stage requires all three. There is no
        // finish-with-Wi-Fi-only path in Milestone 3.8.
        return printer_host == FieldOutcome::Ok && printer_port == FieldOutcome::Ok &&
               printer_api_key == FieldOutcome::Ok;
    }
};

// Validates the fields `stage` requires. Fields belonging to the other stage are left at
// their default Ok and must not be read: a Wi-Fi submission carries no printer values, so
// reporting them as Missing would put errors on a section the user has not reached.
[[nodiscard]] SubmissionReport validate(const Submission& submission, Stage stage);

} // namespace prd::provisioning
