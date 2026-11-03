// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstdint>
#include <string_view>

namespace prd::ota {

// Parsed metadata only. Borrowed strings must remain valid during validation.
// No image bytes, signature verification or installation authority are represented.
struct Candidate {
    std::uint32_t manifest_schema{0};
    std::string_view board;
    std::string_view chip;
    std::string_view version;
    std::uint32_t min_bootloader_schema{0};
    std::uint64_t image_size{0};
    std::string_view sha256;
};

// Supplied by the local device, never copied from candidate slot claims.
struct DeviceCompatibility {
    std::string_view board;
    std::string_view chip;
    std::string_view version;
    std::uint32_t bootloader_schema{0};
    std::uint64_t inactive_slot_size{0};
};

enum class DowngradePolicy : std::uint8_t { Reject, ExplicitRecovery };

enum class Compatibility : std::uint8_t {
    NotChecked,
    MetadataCompatible,
    InvalidLocalIdentity,
    UnsupportedManifestSchema,
    WrongBoard,
    WrongChip,
    UnsupportedBootloaderSchema,
    InvalidImageSize,
    InvalidDigestDeclaration,
    InvalidCandidateVersion,
    Downgrade,
};

// MetadataCompatible is not a verified image or permission to install it.
// Equal version precedence is compatible, not an instruction to reinstall.
[[nodiscard]] Compatibility check_compatibility(const Candidate& candidate,
                                                const DeviceCompatibility& device,
                                                DowngradePolicy policy);

} // namespace prd::ota
