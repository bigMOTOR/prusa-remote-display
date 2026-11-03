// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "app_model/device_data.hpp"

// Persistent storage primitives — Architecture Spec §5.10.
//
// NO ESP-IDF HEADER APPEARS HERE, and none may be added (Bootstrap Spec §11.3: "public
// headers do not include ESP-IDF headers"). The NVS adapter is a separate translation
// unit; everything this header exposes is standard C++17 plus the application model.
//
// NO RAW HANDLE ESCAPES. §5.10 is explicit — "It should not expose raw NVS handles
// outside the component" — and the guarantee here is structural rather than a
// convention: there is no handle type in this header to return.
//
// PLAINTEXT NVS IS THE PRODUCT'S CURRENT STORAGE POLICY, in every profile including
// release (owner ruling, 2026-08-14). Encrypted NVS and HMAC/eFuse key provisioning are
// deferred production-security hardening with no scheduled milestone, and the
// configuration that made automatic key provisioning reachable has been switched off —
// see the block in sdkconfig.defaults. This is a product decision, not a development
// escape hatch, so there is no flag here to select between the two.
//
// WHAT MILESTONE 3.1 DELIVERS, and what it deliberately does not. This checkpoint brings
// the partition up, proves every §5.10 namespace is usable, and reports the result. It
// stores and reads nothing: `config` (M3.2) owns the configuration record and `secrets`
// (M3.3) owns credentials. A key-value API added here would be an interface with no
// caller, which Bootstrap Spec §6.7 rules out in the same breath as empty components.

namespace prd::storage {

// §5.10's namespace separation, as a closed set.
//
// The five are not a suggestion this component reads from elsewhere: they are the
// boundary between kinds of persisted data, and every later component asks for one of
// them by name rather than inventing its own string. A component that hard-codes a
// namespace literal defeats the separation exactly the way a hard-coded log tag defeats
// the tag registry.
enum class Namespace : std::uint8_t {
    // Device-owned facts: boot counters, rollback markers, schema generation.
    //
    // ⚠️ AND THE TWO ACTIVATION MARKERS, WHICH A FACTORY RESET DOES CLEAR. This comment used to say
    // the namespace holds facts that "survive a factory reset of user configuration", which was
    // true only while no reset existed. `cfg.gen` and `sec.gen` live here — the pointers that make
    // a stored record active — and Milestone 3.10's reset removes them by name along with the
    // records they point at. Everything else here survives. Corrected rather than left to mislead
    // whoever next reasons about what a reset touches.
    System = 0,

    // Non-secret user configuration — §13.2's ConfigV1. M3.2.
    Config = 1,

    // §13.2's SecretConfigV1. M3.3.
    Secrets = 2,

    // Working state that is persisted but may be discarded without user-visible loss,
    // such as §13.3's persisted completion snapshot.
    Runtime = 3,

    // Counters and last-error context for local diagnostics (§19.3).
    Diagnostics = 4,
};

inline constexpr std::size_t kNamespaceCount = 5;

// The stored name. Never empty, distinct from every other, and within NVS's limit —
// all three asserted at compile time in namespaces.cpp.
[[nodiscard]] std::string_view name(Namespace space);

// The same string. Named separately from name() because the two are different jobs
// that happen to agree today: one is a persisted identifier that may never change
// without a migration, the other is a diagnostic label. Keeping them distinct means a
// future log-wording change cannot silently rename a namespace on the flash.
[[nodiscard]] std::string_view log_label(Namespace space);

// Why bring-up ended where it did.
//
// Finer-grained than StorageStatus on purpose. §7.2's status answers "may the product
// rely on storage", which is what the application state needs; this answers "what
// exactly happened", which is what a boot log and a future recovery flow need. Collapsing
// them would mean the one fact that decides the remedy — whether user data is sitting
// intact behind a partition we refused to touch — existed nowhere.
enum class Outcome : std::uint8_t {
    // initialise() has not run.
    NotAttempted,

    Ready,

    // ESP_ERR_NVS_NO_FREE_PAGES: the partition holds no usable pages — truncated, or
    // resized by a partition-table change. NOT ERASED. See recovery_required().
    NoFreePages,

    // ESP_ERR_NVS_NEW_VERSION_FOUND: written by a newer NVS format than this firmware
    // understands, which is what a firmware DOWNGRADE looks like from below. NOT ERASED.
    VersionAhead,

    // Any other initialisation error.
    InitialisationFailed,

    // The partition opened but a §5.10 namespace could not be.
    NamespaceUnusable,
};

[[nodiscard]] std::string_view log_label(Outcome outcome);

// Whether user-consented recovery could plausibly resolve this outcome.
//
// TRUE MEANS DATA WAS PRESERVED AND SOMEBODY MUST DECIDE WHAT TO DO WITH IT. Owner
// ruling, 2026-08-14: storage is never erased automatically after NO_FREE_PAGES, a
// version mismatch, corruption, migration failure, or any other mount failure that could
// contain user configuration or credentials. The bytes stay where they are and the
// product reports that recovery is required; the erase belongs to Milestone 3.10, behind
// the deliberate physical presence R1 and R7 already require.
[[nodiscard]] bool recovery_required(Outcome outcome);

// What bring-up concluded.
struct Report {
    app_model::StorageStatus status{app_model::StorageStatus::Unknown};
    Outcome outcome{Outcome::NotAttempted};

    // Meaningful only when `outcome` is NamespaceUnusable.
    Namespace failed_namespace{Namespace::System};
};

// Brings persistent storage up — Architecture Spec §15.1 step 6.
//
// Idempotent: a second call returns the first call's report without touching the
// partition, so a caller that cannot easily prove it runs once is not a hazard.
//
// NOTHING HERE EVER ERASES. There is no code path in this component that formats,
// erases or otherwise discards the partition, and the backend interface it drives has no
// erase operation to call.
Report initialise();

// The last report. `NotAttempted` before initialise() has run, which is a different fact
// from a failure and is why Outcome carries both.
[[nodiscard]] Report report();

} // namespace prd::storage
