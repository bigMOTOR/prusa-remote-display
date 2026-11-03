// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

// How a credential is copied into ESP-IDF's fixed-size station configuration fields.
//
// INTERNAL HEADER of the network component. Pure — no ESP-IDF, no driver, no global — so the two
// bounds below are host-tested against the same code the firmware runs, on the same footing as the
// phase machine in lifecycle.hpp, the retry ladder in backoff.hpp and the phase budgets in
// request_budget.hpp. wifi.cpp static_asserts each constant against the real ESP-IDF struct.
//
// ⛔ THE DEFECT THIS CLOSES. Both call sites in wifi.cpp used to copy
//
//     ssid.size() < sizeof(config.sta.ssid) ? ssid.size() : sizeof(config.sta.ssid) - 1
//
// which at exactly 32 bytes takes the false branch and copies 31. Provisioning accepts a 32-byte
// SSID (request_validation.hpp's kSsidMax), so a legal maximum-length network name was silently
// truncated by one byte, the station tried to join a name that does not exist, and the user was told
// the network was unreachable while both the SSID and the password they typed were correct.
//
// ⬅️ WHY THE TWO FIELDS DO NOT SHARE ONE RULE, AND THAT IS THE WHOLE POINT. In the pinned v5.5.4
// `wifi_sta_config_t` declares
//
//     uint8_t ssid[32];        // no companion length field
//     uint8_t password[64];
//
// The station SSID has NO `ssid_len`, unlike `wifi_ap_config_t`, which declares `ssid[32]` *plus*
// `ssid_len` precisely because a 32-byte name cannot also carry a terminator. So for the station a
// full 32-byte array IS the representation of a 32-byte SSID, and reserving a byte for a NUL is not
// caution — it is data loss. The password keeps its terminator, because 64 bytes there is a
// WPA2 passphrase of at most 63 characters plus one.
//
// ⛔ NEITHER FUNCTION LOGS, MEASURES OR RETAINS ANYTHING. They copy and report success. §13.5 governs
// every byte they touch, and a helper that held a credential in order to describe it would be the
// wrong shape — the same argument wifi_log_policy.hpp makes about redaction.

namespace prd::network::driver_credential {

// `wifi_sta_config_t::ssid` — 32 bytes, no length companion, so all 32 are usable.
inline constexpr std::size_t kStationSsidBytes = 32;

// `wifi_sta_config_t::password` — 64 bytes, NUL-terminated, so 63 are usable.
inline constexpr std::size_t kStationPasswordBytes = 64;

// Copies an SSID into a driver field of `field_size` bytes.
//
// Uses the WHOLE field when the name fills it: at `field_size` bytes the result carries no
// terminator, which is what the driver expects for a station SSID.
//
// Refuses — returning false and leaving `field` untouched — for an empty name and for one longer
// than the field. Refusing rather than truncating is the correction: a truncated SSID is a
// different network, and reporting "did not fit" lets the caller fail honestly instead of
// attempting a join that cannot succeed.
//
// The bytes after the name are zeroed, so a field reused across two configurations cannot leave the
// tail of a previous name behind.
[[nodiscard]] bool copy_ssid(std::string_view ssid, std::uint8_t* field, std::size_t field_size);

// Copies a passphrase into a driver field of `field_size` bytes, terminator included.
//
// An EMPTY PASSPHRASE IS VALID and yields an all-zero field: request_validation.cpp treats absence
// as a value, because an open network has no passphrase.
//
// Refuses a passphrase that would leave no room for the terminator — `size() >= field_size`. For
// every length the product can actually produce (request_validation.hpp caps it at 63 against a
// 64-byte field) the copy is byte-for-byte what the previous inline expression produced; only the
// unreachable 64-and-over case changes, from silent truncation to refusal.
[[nodiscard]] bool copy_password(std::string_view password, std::uint8_t* field,
                                 std::size_t field_size);

} // namespace prd::network::driver_credential
