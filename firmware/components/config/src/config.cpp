// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include "config/config.hpp"

#include <array>
#include <cstddef>
#include <cstring>

#include "schema.hpp"

// The staged write and the active-record selection — Architecture Spec §13.3.
//
// TWO SLOTS AND A POINTER, and the shape follows directly from what §13.3 asks for:
// "versioned records, active-record generation, integrity checksum where appropriate,
// previous-known-good generation for critical configuration".
//
//   config/cfg.0, config/cfg.1   the two record slots; slot = generation % 2
//   system/cfg.gen               the active generation
//
// Placement is the owner-approved one of 2026-08-15: records in `config`, active-generation
// metadata in `system`, matching §5.10's separation between user configuration and
// device-owned facts.
//
// WHY A CANDIDATE NEVER OVERWRITES THE ACTIVE RECORD. A new record is always written to
// the slot the active generation is NOT using, verified by reading it back, and only then
// does the pointer move. So at every instant between power-on and power-off there is at
// least one intact record on the flash, and a save that dies half-way leaves the device
// running exactly what it was running before. The abandoned slot then holds the previous
// known-good generation, which is what a torn pointer falls back to.
//
// NOTHING HERE ERASES A PARTITION (ruling R10). A record that will not decode is reported
// and left alone; the only writes are to this component's own two keys.

namespace prd::config {
namespace {

constexpr std::size_t kSlotCount = 2;

// Keys. Each is within NVS's 15-character bound, asserted rather than assumed.
constexpr std::array<std::string_view, kSlotCount> kSlotKeys{"cfg.0", "cfg.1"};
constexpr std::string_view kActiveGenerationKey = "cfg.gen";

static_assert(storage::key_is_storable(kSlotKeys[0]), "slot key must fit the store");
static_assert(storage::key_is_storable(kSlotKeys[1]), "slot key must fit the store");
static_assert(storage::key_is_storable(kActiveGenerationKey), "pointer key must fit the store");

// Generation 0 means "no record has ever been committed", so the first save writes
// generation 1 into slot 1. The value is never reused: it only increases.

[[nodiscard]] std::size_t slot_for(const std::uint32_t generation) {
    return generation % kSlotCount;
}

struct SlotContents {
    bool present{false};
    DecodeResult decoded{};
};

[[nodiscard]] SlotContents read_slot(storage::KeyValue& store, const std::size_t slot,
                                     bool& storage_failed) {
    SlotContents contents;

    std::array<std::uint8_t, kMaxRecordSize> buffer{};
    std::size_t length = 0;

    const storage::Result result = store.read_blob(storage::Namespace::Config, kSlotKeys[slot],
                                                   buffer.data(), buffer.size(), length);

    switch (result) {
    case storage::Result::Ok:
        break;

    case storage::Result::NotFound:
        // Nothing has been written to this slot. The ordinary state of slot 0 on a device
        // that has been configured exactly once.
        return contents;

    case storage::Result::TooLarge:
        // A record longer than this firmware's buffer: written by a future schema. Present
        // and undecodable, which is a different fact from absent, so it is reported as a
        // malformed slot rather than an empty one.
        contents.present = true;
        return contents;

    case storage::Result::InvalidKey:
    case storage::Result::Failed:
        storage_failed = true;
        return contents;
    }

    contents.present = true;
    contents.decoded = decode(buffer.data(), length);
    return contents;
}

// Turns a decoded slot into the configuration to use, applying §13.4's migration before
// §5.9's validation — in that order, because a record from an older schema must be brought
// forward before it can be judged against today's rules.
//
// TOTAL OVER DecodeOutcome, and it has to be. An earlier version only special-cased
// SchemaTooNew and fell through for everything else, which meant a record that failed its
// checksum arrived here carrying a DEFAULT-CONSTRUCTED ConfigV1 — decode() returns early
// and never populates the value — and the defaults are valid by construction. So a corrupt
// record loaded successfully as a blank configuration, taking the device's real settings
// with it. Two tests caught it. A switch with no default is what keeps the next outcome
// from doing the same thing silently.
[[nodiscard]] LoadOutcome finish(const DecodeResult& decoded, ConfigV1& value_out) {
    switch (decoded.outcome) {
    case DecodeOutcome::Ok:
        break;

    case DecodeOutcome::Malformed:
    case DecodeOutcome::ChecksumMismatch:
        return LoadOutcome::Invalid;

    case DecodeOutcome::SchemaTooNew:
        return LoadOutcome::MigrationRequired;
    }

    ConfigV1 value = decoded.value;

    if (!migrate(value, decoded.schema_version)) {
        return LoadOutcome::MigrationRequired;
    }

    if (!validate(value)) {
        return LoadOutcome::Invalid;
    }

    value_out = value;
    return LoadOutcome::Loaded;
}

using Slots = std::array<SlotContents, kSlotCount>;

// §13.3's "previous-known-good generation": the highest ACTIVATED generation that decodes,
// checksums, migrates and validates. After a failed commit that is the record the device
// was already running.
//
// ⬅️ BOUNDED BY THE ACTIVE GENERATION, AND THAT BOUND IS THE POINT. A slot holding a
// generation ABOVE the pointer is an abandoned candidate from a save whose commit never
// completed — it was written and possibly even read back, but it was never activated.
// Preferring it here would promote a failed transaction to success on the next boot, which
// is exactly the defect this bound closes. Generations at or below the pointer were
// activated at some time, so they remain legitimate recovery material.
[[nodiscard]] bool select_known_good(const Slots& slots, const std::uint32_t active_generation,
                                     ConfigV1& value_out, std::uint32_t& generation_out) {
    bool found = false;

    for (const SlotContents& slot : slots) {
        if (!slot.present) {
            continue;
        }

        // Never activated. See the bound above.
        if (slot.decoded.generation > active_generation) {
            continue;
        }

        ConfigV1 candidate{};
        if (finish(slot.decoded, candidate) != LoadOutcome::Loaded) {
            continue;
        }

        if (!found || slot.decoded.generation > generation_out) {
            found = true;
            generation_out = slot.decoded.generation;
            value_out = candidate;
        }
    }

    return found;
}

// Why nothing was usable, taking the most specific complaint any slot has.
//
// A record from a newer firmware must never be reported as plain corruption: §13.4 makes
// it a migration question, and Invalid would invite provisioning to overwrite it.
[[nodiscard]] LoadOutcome diagnose(const Slots& slots) {
    for (const SlotContents& slot : slots) {
        if (slot.present && slot.decoded.outcome == DecodeOutcome::SchemaTooNew) {
            return LoadOutcome::MigrationRequired;
        }
    }

    return LoadOutcome::Invalid;
}

} // namespace

std::string_view log_label(const UpdateChannel channel) {
    switch (channel) {
    case UpdateChannel::Stable:
        return "stable";
    }

    return "stable";
}

std::string_view log_label(const LoadOutcome outcome) {
    switch (outcome) {
    case LoadOutcome::Absent:
        return "absent";

    case LoadOutcome::Loaded:
        return "loaded";

    case LoadOutcome::LoadedFromPreviousGeneration:
        return "loaded-previous-generation";

    case LoadOutcome::Invalid:
        return "invalid";

    case LoadOutcome::MigrationRequired:
        return "migration-required";

    case LoadOutcome::StorageFailure:
        return "storage-failure";
    }

    return "storage-failure";
}

std::string_view log_label(const SaveOutcome outcome) {
    switch (outcome) {
    case SaveOutcome::Committed:
        return "committed";

    case SaveOutcome::Rejected:
        return "rejected";

    case SaveOutcome::CandidateFailed:
        return "candidate-failed";

    case SaveOutcome::CommitFailed:
        return "commit-failed";
    }

    return "candidate-failed";
}

ConfigV1 defaults() {
    return ConfigV1{};
}

bool operator==(const ConfigV1& lhs, const ConfigV1& rhs) {
    return lhs.wifi_profile_id == rhs.wifi_profile_id && lhs.printer_host == rhs.printer_host &&
           lhs.printer_port == rhs.printer_port && lhs.printer_alias == rhs.printer_alias &&
           lhs.auth_method == rhs.auth_method && lhs.printer_validated == rhs.printer_validated &&
           lhs.brightness == rhs.brightness && lhs.dim_delay_seconds == rhs.dim_delay_seconds &&
           lhs.sleep_delay_seconds == rhs.sleep_delay_seconds &&
           lhs.update_channel == rhs.update_channel;
}

bool operator!=(const ConfigV1& lhs, const ConfigV1& rhs) {
    return !(lhs == rhs);
}

LoadReport load(storage::KeyValue& store) {
    LoadReport report;

    bool storage_failed = false;

    std::uint32_t active_generation = kNoGeneration;
    const storage::Result pointer =
        store.read_u32(storage::Namespace::System, kActiveGenerationKey, active_generation);

    if (pointer == storage::Result::Failed || pointer == storage::Result::InvalidKey) {
        storage_failed = true;
    } else if (pointer == storage::Result::NotFound) {
        // No pointer. Both slots are still read: a device that lost the pointer but kept a
        // record is recoverable, and reporting it Absent would send a configured device
        // back into provisioning.
        active_generation = kNoGeneration;
    }

    Slots slots{};
    for (std::size_t slot = 0; slot < kSlotCount; ++slot) {
        slots[slot] = read_slot(store, slot, storage_failed);
    }

    if (storage_failed) {
        report.outcome = LoadOutcome::StorageFailure;
        return report;
    }

    const bool anything_present = slots[0].present || slots[1].present;

    if (!anything_present) {
        report.outcome = LoadOutcome::Absent;
        return report;
    }

    // ⛔ NO ACTIVE-GENERATION MARKER, NO ACTIVE CONFIGURATION — owner ruling, 2026-08-16.
    //
    // §13.3's staged update is candidate -> commit and read back -> mark active, and the
    // pointer IS that final mark. A device with no pointer has never activated anything, so
    // whatever sits in the slots is a candidate from a transaction that did not finish. It
    // decodes, it checksums, it may even have been read back successfully during the save —
    // and none of that makes it active. Adopting it here would turn a failed first
    // provisioning into a success on the next boot, which is the defect this guard closes.
    //
    // Absent, not Invalid: nothing is wrong with the store, and the product's question is
    // "is there a configuration to use", whose answer is no. Invalid would send a device
    // that has simply never been configured to recovery instead of to setup.
    //
    // ⬅️ WHY THIS IS NOT DESTRUCTIVE. A pointer that could not be READ sets storage_failed
    // above and returns StorageFailure, so reaching here means the read succeeded and
    // reported NotFound — the key was never written, rather than lost. Nothing is erased
    // either way (R10): the orphan bytes stay where they are, and the next successful
    // transaction overwrites that slot in the ordinary course of a staged write.
    if (active_generation == kNoGeneration) {
        report.outcome = LoadOutcome::Absent;
        return report;
    }

    // The slot the pointer names, first — and it exists, because the guard above returned
    // for a device with no pointer. Preferring it over a higher generation elsewhere
    // is deliberate: the pointer IS the commit, so a slot carrying a larger generation
    // that the pointer does not name is an abandoned candidate from a save that never
    // completed, and running it would resurrect a configuration the device never accepted.
    const SlotContents& named = slots[slot_for(active_generation)];

    if (named.present && named.decoded.generation == active_generation) {
        const LoadOutcome outcome = finish(named.decoded, report.value);
        report.outcome = outcome;
        report.generation = active_generation;

        if (outcome == LoadOutcome::Loaded) {
            return report;
        }

        // A named record that fails to migrate or validate still falls through to the
        // previous generation below: §13.3's previous-known-good exists precisely for
        // the case where the newest record is unusable.
    }

    ConfigV1 known_good{};
    std::uint32_t known_good_generation = kNoGeneration;

    if (select_known_good(slots, active_generation, known_good, known_good_generation)) {
        report.outcome = LoadOutcome::LoadedFromPreviousGeneration;
        report.value = known_good;
        report.generation = known_good_generation;
        return report;
    }

    report.outcome = diagnose(slots);
    report.generation = 0;
    return report;
}

StageReport stage(storage::KeyValue& store, const ConfigV1& value) {
    StageReport report;

    // §13.3 step 1.
    if (!validate(value)) {
        report.outcome = SaveOutcome::Rejected;
        return report;
    }

    std::uint32_t active_generation = kNoGeneration;
    const storage::Result pointer =
        store.read_u32(storage::Namespace::System, kActiveGenerationKey, active_generation);

    if (pointer == storage::Result::Failed || pointer == storage::Result::InvalidKey) {
        report.outcome = SaveOutcome::CandidateFailed;
        return report;
    }

    if (pointer == storage::Result::NotFound) {
        active_generation = kNoGeneration;
    }

    const std::uint32_t next_generation = active_generation + 1;
    const std::size_t slot = slot_for(next_generation);

    // §13.3 step 2 — the candidate, into the slot the active generation is not using.
    std::array<std::uint8_t, kMaxRecordSize> written{};
    const std::size_t length = encode(value, next_generation, written.data(), written.size());

    if (length == 0) {
        report.outcome = SaveOutcome::CandidateFailed;
        return report;
    }

    if (store.write_blob(storage::Namespace::Config, kSlotKeys[slot], written.data(), length) !=
        storage::Result::Ok) {
        report.outcome = SaveOutcome::CandidateFailed;
        return report;
    }

    // §13.3 steps 3-4 — read back and compare. Byte for byte rather than field by field: a decoder
    // that happens to normalise a corrupt byte into a plausible value would make a field comparison
    // agree about a record that is not what was written.
    std::array<std::uint8_t, kMaxRecordSize> reread{};
    std::size_t reread_length = 0;

    if (store.read_blob(storage::Namespace::Config, kSlotKeys[slot], reread.data(), reread.size(),
                        reread_length) != storage::Result::Ok) {
        report.outcome = SaveOutcome::CandidateFailed;
        return report;
    }

    if (reread_length != length || std::memcmp(written.data(), reread.data(), length) != 0) {
        report.outcome = SaveOutcome::CandidateFailed;
        return report;
    }

    // ⛔ AND THAT IS ALL. The pointer is NOT moved. The device is still running the previous record,
    // and will keep running it if nothing activates this one — which is §13.3 step 7 obtained by
    // construction rather than by a restore path that could itself fail.
    report.generation = next_generation;
    return report;
}

SaveOutcome activate(storage::KeyValue& store, const std::uint32_t generation) {
    if (generation == kNoGeneration) {
        return SaveOutcome::Rejected;
    }

    // §13.3 step 6 — mark active. Until this line the device is still running the previous record,
    // and if power is lost here it continues to.
    if (store.write_u32(storage::Namespace::System, kActiveGenerationKey, generation) !=
        storage::Result::Ok) {
        return SaveOutcome::CommitFailed;
    }

    return SaveOutcome::Committed;
}

LoadReport load_staged(storage::KeyValue& store, const std::uint32_t generation) {
    LoadReport report;

    if (generation == kNoGeneration) {
        report.outcome = LoadOutcome::Absent;
        return report;
    }

    bool storage_failed = false;

    // Both slots are read rather than only slot_for(generation): the slot a generation OUGHT to
    // occupy is an invariant of the writer, and a reader that assumed it would be unable to report a
    // record that landed elsewhere.
    for (std::size_t slot = 0; slot < kSlotCount; ++slot) {
        const SlotContents contents = read_slot(store, slot, storage_failed);

        if (storage_failed) {
            report.outcome = LoadOutcome::StorageFailure;
            return report;
        }

        if (contents.present && contents.decoded.generation == generation) {
            // finish() applies the same migration dispatch and validity rules load() uses, so a
            // staged record is judged by exactly the standard the active one would be.
            report.outcome = finish(contents.decoded, report.value);
            report.generation = contents.decoded.generation;
            return report;
        }
    }

    report.outcome = LoadOutcome::Absent;
    return report;
}

PointerRead active_generation(storage::KeyValue& store) {
    PointerRead report;

    std::uint32_t generation = kNoGeneration;
    const storage::Result result =
        store.read_u32(storage::Namespace::System, kActiveGenerationKey, generation);

    switch (result) {
    case storage::Result::Ok:
        report.readable = true;
        report.generation = generation;
        return report;

    case storage::Result::NotFound:
        // Never activated. A fact, not a failure.
        report.readable = true;
        return report;

    case storage::Result::Failed:
    case storage::Result::InvalidKey:
    case storage::Result::TooLarge:
        return report;
    }

    return report;
}

SaveOutcome save(storage::KeyValue& store, const ConfigV1& value) {
    const StageReport staged = stage(store, value);

    if (staged.outcome != SaveOutcome::Committed) {
        return staged.outcome;
    }

    return activate(store, staged.generation);
}

// --- User-consented removal (Milestone 3.10) --------------------------------

std::string_view log_label(const ClearOutcome outcome) {
    switch (outcome) {
    case ClearOutcome::Cleared:
        return "cleared";

    case ClearOutcome::Failed:
        return "failed";
    }

    return "failed";
}

ClearOutcome deactivate(storage::KeyValue& store) {
    // NotFound is success: the postcondition is "no active-generation pointer exists", and a
    // device that never had one already satisfies it. Treating an absent key as a failure
    // would make a resumed reset report failure for the step it had already completed.
    const storage::Result result =
        store.erase_key(storage::Namespace::System, kActiveGenerationKey);

    if (result != storage::Result::Ok && result != storage::Result::NotFound) {
        return ClearOutcome::Failed;
    }

    return ClearOutcome::Cleared;
}

ClearOutcome discard(storage::KeyValue& store) {
    ClearOutcome outcome = ClearOutcome::Cleared;

    // Both slots, each by its own name. No enumeration and no wildcard: the keys this
    // component wrote are the keys it removes, and nothing else in the namespace is touched.
    for (const std::string_view key : kSlotKeys) {
        const storage::Result result = store.erase_key(storage::Namespace::Config, key);

        if (result != storage::Result::Ok && result != storage::Result::NotFound) {
            // Continue rather than return: a second slot that CAN be removed should be,
            // and the caller still learns that the whole step did not succeed.
            outcome = ClearOutcome::Failed;
        }
    }

    return outcome;
}

} // namespace prd::config
