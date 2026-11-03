// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstddef>
#include <cstdint>

#include "config/config.hpp"
#include "storage/checksum.hpp"

// The persisted record format, its validator and its migration dispatch.
//
// INTERNAL HEADER of the config component. Pure — no ESP-IDF, no storage, no I/O — so
// every property below is checked by the host suite against the same code the firmware
// runs.

namespace prd::config {

// kSchemaVersion is public — see config/config.hpp. It moved there in Milestone 3.3
// because app_core reports it in the observation it now assembles.

// The container around the payload. Distinct from the schema version because the two
// change for different reasons: this changes if the framing changes, that changes if the
// configuration's fields change.
inline constexpr std::uint16_t kRecordVersion = 1;

// 'P','R','D','C'. A record that does not begin with this was not written by us, and is
// reported rather than parsed.
inline constexpr std::uint32_t kRecordMagic = 0x50524443U;

// magic 4 + record_version 2 + schema_version 2 + generation 4 + payload_length 2 +
// reserved 2 + checksum 4.
inline constexpr std::size_t kHeaderSize = 20;

// The largest payload kSchemaVersion can produce: 4 + (1 + 64) + 2 + (1 + 32) + 1 + 1 + 1
// + 4 + 4 + 1 = 116. Rounded up so a schema addition does not immediately need a new
// buffer, and asserted against the real encoder by a host test rather than trusted.
inline constexpr std::size_t kMaxPayloadSize = 160;
inline constexpr std::size_t kMaxRecordSize = kHeaderSize + kMaxPayloadSize;

// The record's integrity checksum is storage::crc32 — moved there in Milestone 3.3 so the
// secret record could use the same one instead of a second hand-written copy. See
// storage/checksum.hpp for why it takes two spans.

// Whether every field is within its permitted range — §5.9's "validation".
//
// Applied to what was decoded, never to what is about to be trusted: a record that fails
// here is reported Invalid and left on the flash, because R10 forbids discarding it.
[[nodiscard]] bool validate(const ConfigV1& value);

// Serialises `value` at `generation` into `buffer`.
//
// Returns the number of bytes written, or 0 if the buffer is too small. Field order is
// ConfigV1's declaration order and is a persisted contract — see the note on the struct.
[[nodiscard]] std::size_t encode(const ConfigV1& value, std::uint32_t generation,
                                 std::uint8_t* buffer, std::size_t capacity);

enum class DecodeOutcome : std::uint8_t {
    Ok,

    // Not one of our records: too short, wrong magic, wrong container version, or a
    // declared payload length that does not match the bytes present.
    Malformed,

    // Framing is intact but the checksum does not match — a torn write, or bit rot.
    ChecksumMismatch,

    // The record's schemaVersion is newer than kSchemaVersion, so this firmware cannot
    // know what the payload means. §13.4: reported, never rewritten.
    SchemaTooNew,
};

struct DecodeResult {
    DecodeOutcome outcome{DecodeOutcome::Malformed};
    ConfigV1 value{};
    std::uint32_t generation{0};
    std::uint16_t schema_version{0};
};

[[nodiscard]] DecodeResult decode(const std::uint8_t* data, std::size_t length);

// §13.4's migration dispatch — explicit and one-way.
//
// Returns false when the record cannot be brought forward, which is reported rather than
// resolved by overwriting. There is exactly one schema version today, so the only work
// this does is reject anything that is not it; the dispatch exists now so that the first
// real migration is an addition rather than a redesign, and so the "never erase on
// failure" property is already under test.
[[nodiscard]] bool migrate(ConfigV1& value, std::uint16_t from_version);

} // namespace prd::config
