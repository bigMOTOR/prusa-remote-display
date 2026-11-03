// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "ota/compatibility.hpp"
#include <array>
#include <cstddef>

namespace prd::ota {
inline constexpr std::size_t kManifestBodyLimit = 8192;

// Owned data only; candidate() constructs views into THIS object's arrays, so
// copying/moving the object cannot leave cached views into the old instance.
// A returned Candidate must not outlive its owning DecodedManifest.
struct DecodedManifest {
    std::array<char, 32> board{};
    std::array<char, 16> chip{};
    std::array<char, 32> version{};
    std::array<char, 65> sha256{};
    std::uint32_t schema{0};
    std::uint32_t minimum_schema{0};
    std::uint64_t image_size{0};
    [[nodiscard]] Candidate candidate() const;
};

enum class ManifestStatus : std::uint8_t { Decoded, TooLarge, Malformed, InvalidField };
// Consumes the existing packaging schema, not discovery/URL/channel policy.
// Failure clears output. Success proves syntax/representation only, NOT
// compatibility, signature authenticity, trust in JSON signing claims or install authority.
// Input is capped at 8192 bytes and reader depth 8; output uses fixed arrays.
// Only known fields/keys require printable ASCII (escaped ASCII accepted).
// Descriptive unknown fields are structurally skipped, including Unicode escapes;
// the legacy structural scanner does not validate UTF-8 or surrogate pairing.
// Duplicate relevant members are rejected after ASCII escape decoding. Unknown
// names cannot alias required names through NUL, replacement or truncation.
[[nodiscard]] ManifestStatus decode_manifest(std::string_view body, DecodedManifest& output);
} // namespace prd::ota
