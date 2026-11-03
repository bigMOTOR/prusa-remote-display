// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include "schema.hpp"

#include <cstring>

namespace prd::config {
namespace {

// Bounds a validator can state without inventing product policy.
//
// Each is either arithmetic (a port is 16-bit and 0 means unset) or already fixed by an
// accepted source (brightness is a percentage). The two delays are the exception and are
// bounded only to keep a corrupt record from producing a nonsense timer: 24 hours is not a
// product decision about idle behaviour, it is the largest value that can still be
// described as a delay. UI Behaviour Decision 001's actual dimming and sleep policy is
// unaffected and unimplemented.
constexpr std::uint8_t kMaxBrightness = 100;
constexpr std::uint32_t kMaxDelaySeconds = 24U * 60U * 60U;

// --- Little-endian primitives ----------------------------------------------
//
// Explicit, byte by byte, rather than memcpy of a struct. A persisted format must not
// depend on this compiler's padding, alignment or endianness — those are properties of a
// build, and a record outlives the build that wrote it.

void put_u8(std::uint8_t* const buffer, std::size_t& offset, const std::uint8_t value) {
    buffer[offset++] = value;
}

void put_u16(std::uint8_t* const buffer, std::size_t& offset, const std::uint16_t value) {
    buffer[offset++] = static_cast<std::uint8_t>(value & 0xFFU);
    buffer[offset++] = static_cast<std::uint8_t>((value >> 8U) & 0xFFU);
}

void put_u32(std::uint8_t* const buffer, std::size_t& offset, const std::uint32_t value) {
    buffer[offset++] = static_cast<std::uint8_t>(value & 0xFFU);
    buffer[offset++] = static_cast<std::uint8_t>((value >> 8U) & 0xFFU);
    buffer[offset++] = static_cast<std::uint8_t>((value >> 16U) & 0xFFU);
    buffer[offset++] = static_cast<std::uint8_t>((value >> 24U) & 0xFFU);
}

[[nodiscard]] std::uint8_t get_u8(const std::uint8_t* const data, std::size_t& offset) {
    return data[offset++];
}

[[nodiscard]] std::uint16_t get_u16(const std::uint8_t* const data, std::size_t& offset) {
    const auto low = static_cast<std::uint16_t>(data[offset++]);
    const auto high = static_cast<std::uint16_t>(data[offset++]);
    return static_cast<std::uint16_t>(low | static_cast<std::uint16_t>(high << 8U));
}

[[nodiscard]] std::uint32_t get_u32(const std::uint8_t* const data, std::size_t& offset) {
    std::uint32_t value = data[offset++];
    value |= static_cast<std::uint32_t>(data[offset++]) << 8U;
    value |= static_cast<std::uint32_t>(data[offset++]) << 16U;
    value |= static_cast<std::uint32_t>(data[offset++]) << 24U;
    return value;
}

// A length-prefixed string. The prefix is one byte, which bounds every string in the
// record at 255 and is checked against the field's own capacity on the way out.
template <std::size_t Capacity>
void put_string(std::uint8_t* const buffer, std::size_t& offset,
                const app_model::BoundedString<Capacity>& value) {
    const auto text = value.view();
    put_u8(buffer, offset, static_cast<std::uint8_t>(text.size()));

    for (const char character : text) {
        buffer[offset++] = static_cast<std::uint8_t>(character);
    }
}

// Returns false when the declared length runs past the record or past the field. Both are
// corruption, and neither may be allowed to read out of bounds.
template <std::size_t Capacity>
[[nodiscard]] bool get_string(const std::uint8_t* const data, std::size_t& offset,
                              const std::size_t length,
                              app_model::BoundedString<Capacity>& value_out) {
    if (offset >= length) {
        return false;
    }

    const std::size_t text_length = get_u8(data, offset);

    if (text_length > Capacity || offset + text_length > length) {
        return false;
    }

    value_out.assign(std::string_view{reinterpret_cast<const char*>(data + offset), text_length});
    offset += text_length;
    return true;
}

[[nodiscard]] bool auth_method_is_known(const app_model::AuthMethod method) {
    switch (method) {
    case app_model::AuthMethod::None:
    case app_model::AuthMethod::ApiKey:
    case app_model::AuthMethod::Digest:
        return true;
    }

    return false;
}

[[nodiscard]] bool update_channel_is_known(const UpdateChannel channel) {
    switch (channel) {
    case UpdateChannel::Stable:
        return true;
    }

    return false;
}

} // namespace

bool validate(const ConfigV1& value) {
    if (value.brightness > kMaxBrightness) {
        return false;
    }

    if (value.dim_delay_seconds > kMaxDelaySeconds ||
        value.sleep_delay_seconds > kMaxDelaySeconds) {
        return false;
    }

    if (!auth_method_is_known(value.auth_method) ||
        !update_channel_is_known(value.update_channel)) {
        return false;
    }

    // A truncated string is a record that did not survive a previous write intact. It is
    // reported rather than silently accepted, per §9.3's "oversized and malformed input
    // shall be classified rather than quietly discarded".
    if (value.printer_host.truncated() || value.printer_alias.truncated()) {
        return false;
    }

    // WITH NO PRINTER HOST, NOTHING ABOUT A PRINTER MAY BE ASSERTED. A port, a validated
    // flag or an auth method without an address is a record describing a printer that was
    // never configured — which would make R8's three-way distinction unreadable.
    if (value.printer_host.empty()) {
        return value.printer_port == 0 && !value.printer_validated &&
               value.auth_method == app_model::AuthMethod::None;
    }

    // 0 means unset and is permitted: Milestone 3 persists a port without deciding a
    // default, because choosing one is a printer-protocol decision and R4 keeps those in
    // Milestone 4.
    return true;
}

std::size_t encode(const ConfigV1& value, const std::uint32_t generation,
                   std::uint8_t* const buffer, const std::size_t capacity) {
    if (capacity < kMaxRecordSize) {
        return 0;
    }

    // The payload first, so its length and checksum are known before the header is laid
    // down over them.
    std::size_t offset = kHeaderSize;

    put_u32(buffer, offset, value.wifi_profile_id);
    put_string(buffer, offset, value.printer_host);
    put_u16(buffer, offset, value.printer_port);
    put_string(buffer, offset, value.printer_alias);
    put_u8(buffer, offset, static_cast<std::uint8_t>(value.auth_method));
    put_u8(buffer, offset, value.printer_validated ? 1U : 0U);
    put_u8(buffer, offset, value.brightness);
    put_u32(buffer, offset, value.dim_delay_seconds);
    put_u32(buffer, offset, value.sleep_delay_seconds);
    put_u8(buffer, offset, static_cast<std::uint8_t>(value.update_channel));

    const std::size_t payload_length = offset - kHeaderSize;

    std::size_t header_offset = 0;
    put_u32(buffer, header_offset, kRecordMagic);
    put_u16(buffer, header_offset, kRecordVersion);
    put_u16(buffer, header_offset, kSchemaVersion);
    put_u32(buffer, header_offset, generation);
    put_u16(buffer, header_offset, static_cast<std::uint16_t>(payload_length));
    put_u16(buffer, header_offset, 0);

    // OVER THE HEADER AND THE PAYLOAD BOTH, excluding only the checksum field itself —
    // two spans, which is why crc32_update exists.
    //
    // Covering the header is the point that is easy to miss: a torn write that leaves a
    // stale generation or a wrong declared length beside a fresh payload is exactly the
    // corruption the active-record scheme has to survive, and a checksum over the payload
    // alone would call it intact. Covering the payload is the point that is easy to
    // assume — and the first version of this function did assume it, checksumming the
    // header alone while the comment claimed both.
    const std::uint32_t checksum = storage::crc32_update(storage::crc32(buffer, header_offset),
                                                         buffer + kHeaderSize, payload_length);
    put_u32(buffer, header_offset, checksum);

    return kHeaderSize + payload_length;
}

DecodeResult decode(const std::uint8_t* const data, const std::size_t length) {
    DecodeResult result;

    if (data == nullptr || length < kHeaderSize) {
        return result;
    }

    std::size_t offset = 0;

    if (get_u32(data, offset) != kRecordMagic) {
        return result;
    }

    if (get_u16(data, offset) != kRecordVersion) {
        return result;
    }

    result.schema_version = get_u16(data, offset);
    result.generation = get_u32(data, offset);

    const std::size_t payload_length = get_u16(data, offset);
    offset += 2; // reserved

    if (kHeaderSize + payload_length != length) {
        return result;
    }

    const std::uint32_t stored_checksum = get_u32(data, offset);

    // The same two spans encode() covered, in the same order.
    const std::uint32_t computed = storage::crc32_update(storage::crc32(data, kHeaderSize - 4),
                                                         data + kHeaderSize, payload_length);

    if (computed != stored_checksum) {
        result.outcome = DecodeOutcome::ChecksumMismatch;
        return result;
    }

    // The framing is sound, so the version can be trusted enough to refuse. A newer schema
    // is not corruption and must not be reported as such — §13.4 makes it a migration
    // question, and the record stays exactly where it is.
    if (result.schema_version > kSchemaVersion) {
        result.outcome = DecodeOutcome::SchemaTooNew;
        return result;
    }

    ConfigV1 value{};

    value.wifi_profile_id = get_u32(data, offset);

    if (!get_string(data, offset, length, value.printer_host)) {
        return result;
    }

    if (offset + 2 > length) {
        return result;
    }

    value.printer_port = get_u16(data, offset);

    if (!get_string(data, offset, length, value.printer_alias)) {
        return result;
    }

    // 1 + 1 + 1 + 4 + 4 + 1 for the fixed tail.
    if (offset + 12 > length) {
        return result;
    }

    value.auth_method = static_cast<app_model::AuthMethod>(get_u8(data, offset));
    value.printer_validated = get_u8(data, offset) != 0;
    value.brightness = get_u8(data, offset);
    value.dim_delay_seconds = get_u32(data, offset);
    value.sleep_delay_seconds = get_u32(data, offset);
    value.update_channel = static_cast<UpdateChannel>(get_u8(data, offset));

    result.value = value;
    result.outcome = DecodeOutcome::Ok;
    return result;
}

bool migrate(ConfigV1& /*value*/, const std::uint16_t from_version) {
    // The only version that exists. When kSchemaVersion becomes 2, this becomes a
    // fall-through chain — case 1 upgrades to 2, case 2 to 3 — so a record from any
    // supported past version walks forward one step at a time. §13.4: explicit, one-way.
    //
    // A version this firmware does not recognise returns false and the caller reports it.
    // NOTHING IS REWRITTEN OR ERASED on that path: §13.4 says "migration failure must not
    // erase secrets automatically", and R10 extends the same protection to configuration.
    return from_version == kSchemaVersion;
}

} // namespace prd::config
