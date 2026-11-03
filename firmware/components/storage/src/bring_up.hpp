// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstdint>
#include <string_view>

#include "storage/storage.hpp"

// The bring-up sequence, as a pure function over a narrow persistence interface.
//
// INTERNAL HEADER of the storage component.
//
// WHY THIS IS SEPARATE FROM THE ADAPTER. Bootstrap Spec §11.3: "clocks and persistence
// use injected narrow interfaces". The order in which a partition is opened and probed,
// and the mapping from what went wrong onto what the product may rely on, are the only
// things in this checkpoint that can be wrong in an interesting way — and they are
// exactly what a target-only translation unit cannot test. So the decision lives here,
// over two virtual calls, and storage.cpp becomes an argument-for-argument NVS
// translation with no policy of its own.

namespace prd::storage {

// What an attempt on the partition concluded, in this component's terms rather than
// ESP-IDF's. The adapter maps `esp_err_t` onto these and nothing else.
enum class BackendResult : std::uint8_t {
    Ok,

    // The partition holds no usable pages — truncated, or resized by a partition-table
    // change.
    NoFreePages,

    // The partition was written by a NEWER NVS format than this firmware understands:
    // what a firmware DOWNGRADE looks like from below.
    VersionAhead,

    Failed,
};

[[nodiscard]] std::string_view log_label(BackendResult result);

// The narrow persistence interface.
//
// THERE IS NO erase(). That is the ruling of 2026-08-14 expressed as a type rather than
// as a rule someone has to remember: this component never automatically discards a
// partition that could hold user configuration or credentials, and the way to make that
// unbreakable is to leave it no operation with which to try. §13.4 forbids the automatic
// case that matters most — "migration failure must not erase secrets automatically" —
// and §14.1 shares the instinct for configuration. User-consented recovery is Milestone
// 3.10's, and it will need its own explicitly named operation when it arrives.
class Backend {
  public:
    Backend() = default;

    Backend(const Backend&) = delete;
    Backend& operator=(const Backend&) = delete;
    Backend(Backend&&) = delete;
    Backend& operator=(Backend&&) = delete;

    virtual ~Backend() = default;

    // Opens the partition for use.
    [[nodiscard]] virtual BackendResult initialise() = 0;

    // Opens and closes one namespace, creating it if it does not exist. A namespace that
    // cannot be opened is a storage failure now rather than a surprise at the first
    // write, which is worth one boot-time probe per namespace.
    [[nodiscard]] virtual BackendResult probe(Namespace space) = 0;
};

// Runs the sequence. One initialise() and at most kNamespaceCount probes, so it
// terminates regardless of what the backend does.
[[nodiscard]] Report bring_up(Backend& backend);

} // namespace prd::storage
