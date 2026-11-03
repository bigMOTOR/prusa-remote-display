// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstddef>
#include <cstdint>

// The integrity checksum for persisted records — Architecture Spec §13.3's "integrity
// checksum where appropriate".
//
// PURE, and deliberately in `storage` rather than in either component that uses it.
// Milestone 3.2 defined it inside `config`; Milestone 3.3 needs the same function for the
// secret record, and the alternative to moving it was a second hand-written CRC that would
// have to stay byte-identical forever without anything checking that it did. It belongs
// here because it is persistence machinery and no product policy at all — `config` and
// `secrets` both use it while knowing nothing about each other.

namespace prd::storage {

// CRC-32 (IEEE 802.3, reflected), computed bitwise.
//
// Table-free on purpose: a 1 kB table would sit in flash for a checksum computed a handful
// of times per boot, and the bitwise form is short enough to read and verify. Chosen over
// a simpler additive or FNV hash because the failure it must catch is a torn or partially
// written record — a burst of wrong bytes, which is what CRC is good at and a sum is not.
[[nodiscard]] std::uint32_t crc32(const std::uint8_t* data, std::size_t length);

// The same CRC continued over a second span: crc32_update(crc32(a), b) equals the CRC of
// a followed by b.
//
// IT EXISTS BECAUSE A RECORD'S CHECKSUM FIELD SITS IN THE MIDDLE OF WHAT IT COVERS. Both
// record formats checksum the header before the field plus the whole payload after it,
// which is two spans, and a one-shot call can only take one. The first version of the
// configuration record called the one-shot form over the header alone while its comment
// claimed both — the framing was protected and the configuration inside it was not. Three
// tests caught it, and that is why this overload is documented rather than merely present.
[[nodiscard]] std::uint32_t crc32_update(std::uint32_t crc, const std::uint8_t* data,
                                         std::size_t length);

} // namespace prd::storage
