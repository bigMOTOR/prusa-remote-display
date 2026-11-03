// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include "secret_schema.hpp"

namespace prd::secrets {
namespace {

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

template <std::size_t Capacity>
void put_string(std::uint8_t* const buffer, std::size_t& offset,
                const app_model::BoundedString<Capacity>& value) {
    const auto text = value.view();
    put_u8(buffer, offset, static_cast<std::uint8_t>(text.size()));

    for (const char character : text) {
        buffer[offset++] = static_cast<std::uint8_t>(character);
    }
}

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

} // namespace

bool validate(const SecretConfigV1& value) {
    // A truncated credential is worse than a missing one: it would be silently wrong at the
    // moment it is used, and the user would see an authentication failure with no cause.
    // §9.3 requires oversized input to be classified rather than quietly accepted.
    if (value.wifi_ssid.truncated() || value.wifi_password.truncated() ||
        value.printer_api_key.truncated() || value.printer_username.truncated() ||
        value.printer_password.truncated()) {
        return false;
    }

    // A password with no network to belong to. An open network legitimately has an SSID and
    // no password, so only this direction is an inconsistency.
    if (!value.wifi_password.empty() && value.wifi_ssid.empty()) {
        return false;
    }

    // A record holding nothing must not claim an association, and a record holding
    // something must have one: the handle is how `app_core` decides whether these
    // credentials still belong to the configured profile, and either half alone makes that
    // question unanswerable.
    const bool holds_wifi = !value.wifi_ssid.empty();
    const bool is_associated = value.profile_id != 0;

    return holds_wifi == is_associated;
}

std::size_t encode(const SecretConfigV1& value, const std::uint32_t generation,
                   std::uint8_t* const buffer, const std::size_t capacity) {
    if (capacity < kMaxRecordSize) {
        return 0;
    }

    std::size_t offset = kHeaderSize;

    put_u32(buffer, offset, value.profile_id);
    put_string(buffer, offset, value.wifi_ssid);
    put_string(buffer, offset, value.wifi_password);
    put_string(buffer, offset, value.printer_api_key);
    put_string(buffer, offset, value.printer_username);
    put_string(buffer, offset, value.printer_password);

    const std::size_t payload_length = offset - kHeaderSize;

    std::size_t header_offset = 0;
    put_u32(buffer, header_offset, kRecordMagic);
    put_u16(buffer, header_offset, kRecordVersion);
    put_u16(buffer, header_offset, kSchemaVersion);
    put_u32(buffer, header_offset, generation);
    put_u16(buffer, header_offset, static_cast<std::uint16_t>(payload_length));
    put_u16(buffer, header_offset, 0);

    // Over the header and the payload both, excluding only the checksum field — the two
    // spans storage::crc32_update exists for.
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

    const std::uint32_t computed = storage::crc32_update(storage::crc32(data, kHeaderSize - 4),
                                                         data + kHeaderSize, payload_length);

    if (computed != stored_checksum) {
        result.outcome = DecodeOutcome::ChecksumMismatch;
        return result;
    }

    // Intact framing, so the version can be trusted enough to refuse. A newer schema is not
    // corruption: §13.4 makes it a migration question and the record stays where it is.
    if (result.schema_version > kSchemaVersion) {
        result.outcome = DecodeOutcome::SchemaTooNew;
        return result;
    }

    SecretConfigV1 value{};

    value.profile_id = get_u32(data, offset);

    if (!get_string(data, offset, length, value.wifi_ssid) ||
        !get_string(data, offset, length, value.wifi_password) ||
        !get_string(data, offset, length, value.printer_api_key) ||
        !get_string(data, offset, length, value.printer_username) ||
        !get_string(data, offset, length, value.printer_password)) {
        return result;
    }

    result.value = value;
    result.outcome = DecodeOutcome::Ok;
    return result;
}

bool migrate(SecretConfigV1& /*value*/, const std::uint16_t from_version) {
    // The only version that exists. When kSchemaVersion becomes 2 this becomes a
    // fall-through chain, one step per version. §13.4: explicit and one-way.
    //
    // A version this firmware does not recognise returns false and the caller reports it.
    // NOTHING IS REWRITTEN OR ERASED — §13.4 names this exact case: "migration failure must
    // not erase secrets automatically".
    return from_version == kSchemaVersion;
}

} // namespace prd::secrets
