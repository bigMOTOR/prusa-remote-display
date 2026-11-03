// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstddef>
#include <cstdint>

#include "secrets/secrets.hpp"
#include "storage/checksum.hpp"

// The persisted credential record, its validator and its migration dispatch.
//
// INTERNAL HEADER of the secrets component. Pure — no ESP-IDF, no I/O.
//
// NAMED secret_schema.hpp RATHER THAN schema.hpp, and the reason is the host test build:
// it puts every component's src/ directory on ONE include path so internal headers can be
// tested, and `config` already has a schema.hpp. Two files with the same name on a flat
// path resolve to whichever comes first — which is not a compile error, it is a test suite
// quietly exercising the wrong component. The next component to add an internal schema
// should prefix it too.
//
// DELIBERATELY A SECOND FRAMING, NOT A SHARED ONE. The configuration record and the
// credential record use the same shape, and extracting a generic record store was
// considered and not done: the two schemas version independently, live in different
// namespaces, validate against different rules, and — most of all — a shared framing would
// be the one type both `config` and `secrets` depend on, which is exactly the coupling the
//2026-08-15 ownership ruling forbids. What IS shared is storage::crc32, because a checksum
// is arithmetic rather than a boundary. The duplication is recorded rather than hidden; if
// a third record ever appears, extraction becomes worth its own checkpoint.

namespace prd::secrets {

// §13.2's schemaVersion for the secret record. Independent of the configuration record's:
// a credential field can be added without touching ConfigV1, and the reverse.
inline constexpr std::uint16_t kSchemaVersion = 1;

inline constexpr std::uint16_t kRecordVersion = 1;

// 'P','R','D','S' — S for secrets. Distinct from the configuration record's magic, so a
// record written into the wrong namespace is rejected rather than half-parsed.
inline constexpr std::uint32_t kRecordMagic = 0x50524453U;

// magic 4 + record_version 2 + schema_version 2 + generation 4 + payload_length 2 +
// reserved 2 + checksum 4.
inline constexpr std::size_t kHeaderSize = 20;

// The largest payload kSchemaVersion can produce: 4 + (1+32) + (1+64) + (1+64) + (1+32) +
// (1+64) = 265. Asserted against the real encoder by a host test rather than trusted.
inline constexpr std::size_t kMaxPayloadSize = 320;
inline constexpr std::size_t kMaxRecordSize = kHeaderSize + kMaxPayloadSize;

// Structural consistency only — §5.9's "validation", within what this component may know.
//
// It cannot check §13.2's "only fields relevant to the selected authentication method are
// populated", because the auth method is non-secret configuration and this component may
// not see it. That rule belongs to the caller that writes the record, and is recorded in
// secrets.hpp so it is inherited rather than lost.
[[nodiscard]] bool validate(const SecretConfigV1& value);

[[nodiscard]] std::size_t encode(const SecretConfigV1& value, std::uint32_t generation,
                                 std::uint8_t* buffer, std::size_t capacity);

enum class DecodeOutcome : std::uint8_t {
    Ok,
    Malformed,
    ChecksumMismatch,
    SchemaTooNew,
};

struct DecodeResult {
    DecodeOutcome outcome{DecodeOutcome::Malformed};
    SecretConfigV1 value{};
    std::uint32_t generation{0};
    std::uint16_t schema_version{0};
};

[[nodiscard]] DecodeResult decode(const std::uint8_t* data, std::size_t length);

// §13.4's migration dispatch — explicit, one-way, and failing rather than rewriting.
//
// §13.4 names this case specifically: "migration failure must not erase secrets
// automatically". Here that is not a policy the caller must remember — this component has
// no erase operation to reach for.
[[nodiscard]] bool migrate(SecretConfigV1& value, std::uint16_t from_version);

} // namespace prd::secrets
