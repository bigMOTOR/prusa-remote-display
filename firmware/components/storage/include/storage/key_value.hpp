// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "storage/storage.hpp"

// The narrow key-value surface over §5.10's namespaces.
//
// NO ESP-IDF HEADER APPEARS HERE, and none may be added (Bootstrap Spec §11.3). The NVS
// implementation is a separate translation unit; this header is standard C++17, which is
// what lets `config` be tested end to end on the host against a fake.
//
// DELIBERATELY NARROW — owner-approved scope, 2026-08-15: "only the typed operations
// required by ConfigV1; no iteration; no raw NVS handles; key deletion is acceptable and
// must remain explicitly distinct from partition erase."
//
// So there is no iterator, no key enumeration, no "find", no handle accessor and no
// partition-level operation of any kind. Milestone 3.1 shipped no key-value API at all;
// this is the smallest one that can carry a versioned, checksummed configuration record
// and its active-generation pointer, and nothing beyond that has been added on
// speculation.
//
// ⚠️ erase_key() ERASES ONE KEY. IT IS NOT AND MUST NOT BECOME A PARTITION ERASE.
// Owner ruling R10 forbids automatically discarding a partition that could hold user
// configuration or credentials, and storage.hpp's Backend has no erase operation for
// exactly that reason. Removing one named key that this firmware itself wrote is a
// different act from formatting the store, and the distinction is load-bearing: if the
// two ever merge, R10 is silently gone.

namespace prd::storage {

enum class Result : std::uint8_t {
    Ok,

    // The key is absent. A NORMAL CONDITION, NOT A FAILURE — a first boot has no
    // configuration record, and a caller that treats this as an error reports a broken
    // device on every factory-fresh unit.
    NotFound,

    // The stored value does not fit the caller's buffer. Distinguished from Failed so a
    // record written by a future firmware with a larger payload is diagnosable rather
    // than merely broken.
    TooLarge,

    // The key is empty or longer than the store can hold.
    InvalidKey,

    Failed,
};

[[nodiscard]] std::string_view log_label(Result result);

// NVS stores a key name in a 16-byte field including its terminator.
//
// Declared here rather than included from nvs.h for the same reason namespaces.cpp
// declares the namespace bound, and cross-checked against ESP-IDF's own constant in the
// adapter translation unit.
inline constexpr std::size_t kMaxKeyLength = 15;

[[nodiscard]] constexpr bool key_is_storable(const std::string_view key) {
    return !key.empty() && key.size() <= kMaxKeyLength;
}

// Typed access to one namespace's keys.
//
// EVERY WRITE COMMITS BEFORE IT RETURNS, and there is deliberately no separate commit
// operation. Two reasons. A retained handle is the only way to make an uncommitted write
// observable, and retaining one would put an NVS handle in this component's state where
// §5.10 says none may live. And §13.3's staged pattern does not need a storage-level
// commit anyway: its "commit" is the logical act of moving the active-record pointer
// after the candidate is written and read back, which `config` performs with these
// operations rather than underneath them.
class KeyValue {
  public:
    KeyValue() = default;

    KeyValue(const KeyValue&) = delete;
    KeyValue& operator=(const KeyValue&) = delete;
    KeyValue(KeyValue&&) = delete;
    KeyValue& operator=(KeyValue&&) = delete;

    virtual ~KeyValue() = default;

    // Writes `length` bytes. An existing value under the same key is replaced.
    [[nodiscard]] virtual Result write_blob(Namespace space, std::string_view key,
                                            const std::uint8_t* data, std::size_t length) = 0;

    // Reads into `buffer`, reporting the stored length. `NotFound` when the key is
    // absent; `TooLarge` when the stored value exceeds `capacity`, in which case
    // `length_out` still reports the stored length so the caller can say so.
    [[nodiscard]] virtual Result read_blob(Namespace space, std::string_view key,
                                           std::uint8_t* buffer, std::size_t capacity,
                                           std::size_t& length_out) = 0;

    [[nodiscard]] virtual Result write_u32(Namespace space, std::string_view key,
                                           std::uint32_t value) = 0;

    [[nodiscard]] virtual Result read_u32(Namespace space, std::string_view key,
                                          std::uint32_t& value_out) = 0;

    // Removes ONE key. See the warning at the top of this header: this is not a partition
    // erase and must never be widened into one.
    [[nodiscard]] virtual Result erase_key(Namespace space, std::string_view key) = 0;
};

// The NVS-backed instance. Defined in the adapter translation unit, which is the only
// place in the project that includes nvs.h.
//
// Valid only after storage::initialise() has reported Ready; before that every operation
// fails, because the partition is not mounted.
[[nodiscard]] KeyValue& key_value();

} // namespace prd::storage
