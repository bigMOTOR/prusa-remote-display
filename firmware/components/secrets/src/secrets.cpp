// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include "secrets/secrets.hpp"

#include <array>
#include <cstddef>
#include <cstring>

#include "secret_schema.hpp"

// The staged write and active-record selection for the credential record.
//
//   secrets/sec.0, secrets/sec.1   the two record slots; slot = generation % 2
//   system/sec.gen                 the active generation
//
// The credential record's active-generation metadata sits in `system` beside the
// configuration's, for the same §5.10 reason: a pointer is a device-owned fact about
// storage, not user data. The RECORDS live in `secrets`, which is the namespace §5.10
// reserves for them.
//
// SAME GUARANTEES AS THE CONFIGURATION RECORD. A candidate always goes to the slot the
// active generation is not using, is read back byte for byte, and only then does the
// pointer move — so a save interrupted at any point leaves the device running exactly the
// credentials it was running, and never a half-written one. NOTHING HERE ERASES (R10).

namespace prd::secrets {
namespace {

constexpr std::size_t kSlotCount = 2;

constexpr std::array<std::string_view, kSlotCount> kSlotKeys{"sec.0", "sec.1"};
constexpr std::string_view kActiveGenerationKey = "sec.gen";

static_assert(storage::key_is_storable(kSlotKeys[0]), "slot key must fit the store");
static_assert(storage::key_is_storable(kSlotKeys[1]), "slot key must fit the store");
static_assert(storage::key_is_storable(kActiveGenerationKey), "pointer key must fit the store");

[[nodiscard]] std::size_t slot_for(const std::uint32_t generation) {
    return generation % kSlotCount;
}

struct SlotContents {
    bool present{false};
    DecodeResult decoded{};
};

using Slots = std::array<SlotContents, kSlotCount>;

[[nodiscard]] SlotContents read_slot(storage::KeyValue& store, const std::size_t slot,
                                     bool& storage_failed) {
    SlotContents contents;

    std::array<std::uint8_t, kMaxRecordSize> buffer{};
    std::size_t length = 0;

    const storage::Result result = store.read_blob(storage::Namespace::Secrets, kSlotKeys[slot],
                                                   buffer.data(), buffer.size(), length);

    switch (result) {
    case storage::Result::Ok:
        break;

    case storage::Result::NotFound:
        return contents;

    case storage::Result::TooLarge:
        // Present but longer than this firmware's buffer: written by a future schema.
        // Present-and-undecodable is a different fact from absent.
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

// Total over DecodeOutcome, for the reason config::finish() records: a record that failed
// its checksum carries a DEFAULT-CONSTRUCTED value, and defaults are valid by
// construction. Falling through would load an empty credential set as if it were real —
// which for secrets means silently forgetting the user's password.
[[nodiscard]] LoadOutcome finish(const DecodeResult& decoded, SecretConfigV1& value_out) {
    switch (decoded.outcome) {
    case DecodeOutcome::Ok:
        break;

    case DecodeOutcome::Malformed:
    case DecodeOutcome::ChecksumMismatch:
        return LoadOutcome::Invalid;

    case DecodeOutcome::SchemaTooNew:
        return LoadOutcome::MigrationRequired;
    }

    SecretConfigV1 value = decoded.value;

    if (!migrate(value, decoded.schema_version)) {
        return LoadOutcome::MigrationRequired;
    }

    if (!validate(value)) {
        return LoadOutcome::Invalid;
    }

    value_out = value;
    return LoadOutcome::Loaded;
}

[[nodiscard]] bool select_known_good(const Slots& slots, const std::uint32_t active_generation,
                                     SecretConfigV1& value_out, std::uint32_t& generation_out) {
    bool found = false;

    for (const SlotContents& slot : slots) {
        if (!slot.present) {
            continue;
        }

        // Never activated. See the bound above.
        if (slot.decoded.generation > active_generation) {
            continue;
        }

        SecretConfigV1 candidate{};
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

[[nodiscard]] LoadOutcome diagnose(const Slots& slots) {
    for (const SlotContents& slot : slots) {
        if (slot.present && slot.decoded.outcome == DecodeOutcome::SchemaTooNew) {
            return LoadOutcome::MigrationRequired;
        }
    }

    return LoadOutcome::Invalid;
}

} // namespace

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

SecretConfigV1 defaults() {
    return SecretConfigV1{};
}

bool operator==(const SecretConfigV1& lhs, const SecretConfigV1& rhs) {
    return lhs.profile_id == rhs.profile_id && lhs.wifi_ssid == rhs.wifi_ssid &&
           lhs.wifi_password == rhs.wifi_password && lhs.printer_api_key == rhs.printer_api_key &&
           lhs.printer_username == rhs.printer_username &&
           lhs.printer_password == rhs.printer_password;
}

bool operator!=(const SecretConfigV1& lhs, const SecretConfigV1& rhs) {
    return !(lhs == rhs);
}

Presence presence_of(const SecretConfigV1& value) {
    Presence presence;
    presence.wifi_ssid = !value.wifi_ssid.empty();
    presence.wifi_password = !value.wifi_password.empty();
    presence.printer_api_key = !value.printer_api_key.empty();
    presence.printer_username = !value.printer_username.empty();
    presence.printer_password = !value.printer_password.empty();
    return presence;
}

std::string describe(const LoadOutcome outcome, const std::uint32_t generation,
                     const Presence& presence) {
    // NO SECRET IS IN SCOPE HERE, and that is the mechanism rather than a promise: the
    // signature admits an outcome, a number and five booleans. Nothing this function can
    // reach is a credential, so no ordering of its statements can leak one.
    const auto yes_no = [](const bool present) { return present ? "yes" : "no"; };

    return "secrets outcome=" + std::string{log_label(outcome)} +
           " generation=" + std::to_string(generation) + " ssid=" + yes_no(presence.wifi_ssid) +
           " wifi_password=" + yes_no(presence.wifi_password) +
           " api_key=" + yes_no(presence.printer_api_key) +
           " username=" + yes_no(presence.printer_username) +
           " password=" + yes_no(presence.printer_password);
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

    if (!slots[0].present && !slots[1].present) {
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

    // The slot the pointer names wins over a higher generation elsewhere: the pointer IS
    // the commit, so a larger generation it does not name is an abandoned candidate from a
    // save that never completed.
    const SlotContents& named = slots[slot_for(active_generation)];

    if (named.present && named.decoded.generation == active_generation) {
        const LoadOutcome outcome = finish(named.decoded, report.value);

        if (outcome == LoadOutcome::Loaded) {
            report.outcome = outcome;
            report.generation = active_generation;
            return report;
        }
    }

    SecretConfigV1 known_good{};
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

StageReport stage(storage::KeyValue& store, const SecretConfigV1& value) {
    StageReport report;

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

    std::array<std::uint8_t, kMaxRecordSize> written{};
    const std::size_t length = encode(value, next_generation, written.data(), written.size());

    if (length == 0) {
        report.outcome = SaveOutcome::CandidateFailed;
        return report;
    }

    if (store.write_blob(storage::Namespace::Secrets, kSlotKeys[slot], written.data(), length) !=
        storage::Result::Ok) {
        report.outcome = SaveOutcome::CandidateFailed;
        return report;
    }

    std::array<std::uint8_t, kMaxRecordSize> reread{};
    std::size_t reread_length = 0;

    if (store.read_blob(storage::Namespace::Secrets, kSlotKeys[slot], reread.data(), reread.size(),
                        reread_length) != storage::Result::Ok) {
        report.outcome = SaveOutcome::CandidateFailed;
        return report;
    }

    if (reread_length != length || std::memcmp(written.data(), reread.data(), length) != 0) {
        report.outcome = SaveOutcome::CandidateFailed;
        return report;
    }

    // ⛔ THE POINTER IS NOT MOVED. The active credential is still the previous one, so a device that
    // stops here — by power loss or by a failed printer probe — keeps working with the credential it
    // already had. That is §13.3 step 7 by construction.
    report.generation = next_generation;
    return report;
}

SaveOutcome activate(storage::KeyValue& store, const std::uint32_t generation) {
    if (generation == kNoGeneration) {
        return SaveOutcome::Rejected;
    }

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

SaveOutcome save(storage::KeyValue& store, const SecretConfigV1& value) {
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
    const storage::Result result =
        store.erase_key(storage::Namespace::System, kActiveGenerationKey);

    if (result != storage::Result::Ok && result != storage::Result::NotFound) {
        return ClearOutcome::Failed;
    }

    return ClearOutcome::Cleared;
}

ClearOutcome wipe(storage::KeyValue& store) {
    ClearOutcome outcome = ClearOutcome::Cleared;

    for (const std::string_view key : kSlotKeys) {
        // ⬅️ THE STORED LENGTH FIRST, so the overwrite covers exactly what is there. A fixed
        // guess would either leave a tail of the real record behind or write a blob of the
        // wrong size, and the second is a change of content rather than a removal of it.
        std::array<std::uint8_t, kMaxRecordSize> buffer{};
        std::size_t stored = 0;
        const storage::Result read =
            store.read_blob(storage::Namespace::Secrets, key, buffer.data(), buffer.size(), stored);

        // Absent already satisfies the postcondition; nothing to overwrite and nothing to
        // erase. TooLarge still reports the stored length, so it is overwritten too.
        if (read == storage::Result::NotFound) {
            continue;
        }

        if (read == storage::Result::Ok || read == storage::Result::TooLarge) {
            const std::size_t length = stored < buffer.size() ? stored : buffer.size();
            const std::array<std::uint8_t, kMaxRecordSize> zeros{};

            // ⛔ OVERWRITE BEFORE ERASE. erase_key marks the entry erased; the bytes survive
            // in the page until NVS compacts. Writing zeros over the record is the strongest
            // removal this abstraction offers — see the honest limit stated in secrets.hpp.
            if (store.write_blob(storage::Namespace::Secrets, key, zeros.data(), length) !=
                storage::Result::Ok) {
                outcome = ClearOutcome::Failed;
            }
        } else {
            // The record is there and could not be read, so it cannot be overwritten either.
            // The erase below is still attempted — removing it is better than leaving it —
            // but the step is reported as failed.
            outcome = ClearOutcome::Failed;
        }

        const storage::Result erased = store.erase_key(storage::Namespace::Secrets, key);
        if (erased != storage::Result::Ok && erased != storage::Result::NotFound) {
            outcome = ClearOutcome::Failed;
        }
    }

    // The buffers above held credential material. Nothing leaves this function with it.
    return outcome;
}

} // namespace prd::secrets
