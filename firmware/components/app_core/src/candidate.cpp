// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include "candidate.hpp"

#include <array>
#include <cstring>

#include "storage/checksum.hpp"

namespace prd::app_core::candidate {
namespace {

// In `system`, beside the two active-generation pointers it coordinates.
//
// ⬅️ A NEW KEY, AND A DIFFERENT ONE FROM THE u32 THIS REPLACES. No released firmware ever wrote
// either, so there is nothing to migrate and §13.4's rule is satisfied without a migration. Using a
// fresh name rather than reinterpreting `pair.act` means a bench device that ran an unreleased build
// cannot present four stale bytes to a blob decoder: nothing reads that key any more, and R10 leaves
// the bytes where they are rather than erasing them.
constexpr std::string_view kMarkerKey = "pair.rec";

static_assert(storage::key_is_storable(kMarkerKey), "marker key must fit the store");

// Same shape as the config and secret records: magic, version, then the payload, with a CRC over
// both. Written byte by byte for the reason schema.cpp gives — a persisted format must not depend on
// this compiler's padding, alignment or endianness.
constexpr std::uint32_t kMarkerMagic = 0x50414952U; // "PAIR"
constexpr std::uint16_t kMarkerVersion = 1;

// magic(4) + version(2) + five u32 fields(20) + crc(4)
constexpr std::size_t kMarkerSize = 30;

void put_u16(std::uint8_t* const buffer, std::size_t& offset, const std::uint16_t value) {
    buffer[offset++] = static_cast<std::uint8_t>(value & 0xFFU);
    buffer[offset++] = static_cast<std::uint8_t>((value >> 8U) & 0xFFU);
}

void put_u32(std::uint8_t* const buffer, std::size_t& offset, const std::uint32_t value) {
    buffer[offset++] = static_cast<std::uint8_t>(value & 0xFFU);
    buffer[offset++] = static_cast<std::uint8_t>((value >> 8U) & 0xFFU);
    buffer[offset++] = static_cast<std::uint8_t>((value >> 16U) & 0xFFU);
    buffer[offset++] = static_cast<std::uint8_t>((value >> 24U) & 0xFFU);
}

[[nodiscard]] std::uint16_t get_u16(const std::uint8_t* const data, std::size_t& offset) {
    const auto low = static_cast<std::uint16_t>(data[offset++]);
    const auto high = static_cast<std::uint16_t>(data[offset++]);
    return static_cast<std::uint16_t>(low | static_cast<std::uint16_t>(high << 8U));
}

[[nodiscard]] std::uint32_t get_u32(const std::uint8_t* const data, std::size_t& offset) {
    std::uint32_t value = data[offset++];
    value |= static_cast<std::uint32_t>(data[offset++]) << 8U;
    value |= static_cast<std::uint32_t>(data[offset++]) << 16U;
    value |= static_cast<std::uint32_t>(data[offset++]) << 24U;
    return value;
}

std::size_t encode(const Recovery& recovery, std::uint8_t* const buffer) {
    std::size_t offset = 0;

    put_u32(buffer, offset, kMarkerMagic);
    put_u16(buffer, offset, kMarkerVersion);
    put_u32(buffer, offset, recovery.profile);
    put_u32(buffer, offset, recovery.target_configuration);
    put_u32(buffer, offset, recovery.target_secret);
    put_u32(buffer, offset, recovery.previous_configuration);
    put_u32(buffer, offset, recovery.previous_secret);

    // Over everything written so far, which is header and payload both. A torn write that leaves a
    // fresh profile beside a stale generation is exactly the corruption this record has to survive.
    put_u32(buffer, offset, storage::crc32(buffer, offset));

    return offset;
}

[[nodiscard]] MarkerRead decode(const std::uint8_t* const data, const std::size_t length) {
    MarkerRead read;
    read.outcome = MarkerOutcome::Corrupt;

    if (data == nullptr || length != kMarkerSize) {
        return read;
    }

    std::size_t offset = 0;

    if (get_u32(data, offset) != kMarkerMagic) {
        return read;
    }

    if (get_u16(data, offset) != kMarkerVersion) {
        return read;
    }

    Recovery recovery;
    recovery.profile = get_u32(data, offset);
    recovery.target_configuration = get_u32(data, offset);
    recovery.target_secret = get_u32(data, offset);
    recovery.previous_configuration = get_u32(data, offset);
    recovery.previous_secret = get_u32(data, offset);

    const std::uint32_t stored = get_u32(data, offset);

    if (storage::crc32(data, kMarkerSize - 4) != stored) {
        return read;
    }

    // A record that decodes but names nothing cannot be acted on, and must not be mistaken for an
    // absence: it is evidence that something wrote a marker, and it is retained as such.
    if (!recovery.valid()) {
        return read;
    }

    read.outcome = MarkerOutcome::Present;
    read.recovery = recovery;
    return read;
}

// --- Proving a half ---------------------------------------------------------

// Whether the exact configuration record at `generation` exists, is usable, and names `profile`.
//
// ⛔ EXACT, NOT "SOMETHING PLAUSIBLE". load_staged() reads the named generation and applies the same
// migration and validity rules load() applies to the active record, so a record that fails its
// checksum, needs a migration this firmware cannot do, or belongs to another profile is refused
// rather than activated.
[[nodiscard]] bool configuration_proves(storage::KeyValue& store, const std::uint32_t generation,
                                        const std::uint32_t profile) {
    if (generation == config::kNoGeneration) {
        return false;
    }

    const config::LoadReport report = config::load_staged(store, generation);

    return report.outcome == config::LoadOutcome::Loaded && report.generation == generation &&
           report.value.wifi_profile_id == profile;
}

// The credential half, judged by exactly the same standard.
[[nodiscard]] bool secret_proves(storage::KeyValue& store, const std::uint32_t generation,
                                 const std::uint32_t profile) {
    if (generation == secrets::kNoGeneration) {
        return false;
    }

    const secrets::LoadReport report = secrets::load_staged(store, generation);

    return report.outcome == secrets::LoadOutcome::Loaded && report.generation == generation &&
           report.value.profile_id == profile;
}

// A pair, named exactly, both halves proved.
[[nodiscard]] bool pair_proves(storage::KeyValue& store, const std::uint32_t configuration,
                               const std::uint32_t secret, const std::uint32_t profile) {
    return configuration_proves(store, configuration, profile) &&
           secret_proves(store, secret, profile);
}

// Moves both pointers to a named pair and proves the move by reread. Never partial in its reporting:
// a half-completed move returns false, and the caller falls through to the next strategy with the
// recovery record still in flash.
[[nodiscard]] bool activate_pair_exactly(storage::KeyValue& store,
                                         const std::uint32_t configuration,
                                         const std::uint32_t secret, const std::uint32_t profile) {
    if (config::activate(store, configuration) != config::SaveOutcome::Committed) {
        return false;
    }

    if (secrets::activate(store, secret) != secrets::SaveOutcome::Committed) {
        return false;
    }

    return pair_is_active(store, configuration, secret, profile);
}

// The profile a restored previous pair carries. Read rather than assumed: the previous pair predates
// this transaction and its profile is not the candidate's.
[[nodiscard]] std::uint32_t previous_profile(storage::KeyValue& store, const Recovery& recovery) {
    const config::LoadReport report = config::load_staged(store, recovery.previous_configuration);

    if (report.outcome != config::LoadOutcome::Loaded) {
        return 0;
    }

    return report.value.wifi_profile_id;
}

} // namespace

bool pair_is_active(storage::KeyValue& store, const std::uint32_t configuration,
                    const std::uint32_t secret, const std::uint32_t profile) {
    // ⛔ THE POINTERS THEMSELVES, NOT WHAT A READER HAPPENS TO GET. This used to ask config::load()
    // and secrets::load() what generation they returned, and those fall back to a previous
    // known-good record when the record the pointer names is unusable — reporting the FALLBACK's
    // generation. So a pointer naming a corrupt generation, with the fallback landing exactly on the
    // requested one, satisfied the old check while the pointer still named something else. Every
    // caller here uses this to decide whether the recovery record may be cleared, and clearing it on
    // a fallback would discard the evidence while the repair was incomplete.
    //
    // ⬅️ SO THE PROOF IS TWO EXACT FACTS. The pointer literally names the requested generation, AND
    // the record at that generation loads, decodes, migrates, validates and carries the expected
    // profile. Nothing here can be satisfied by a fallback, because load_staged() reads the named
    // generation and no other.
    const config::PointerRead configuration_pointer = config::active_generation(store);
    const secrets::PointerRead secret_pointer = secrets::active_generation(store);

    if (!configuration_pointer.readable || !secret_pointer.readable) {
        return false;
    }

    if (configuration_pointer.generation != configuration || secret_pointer.generation != secret) {
        return false;
    }

    return pair_proves(store, configuration, secret, profile);
}

std::string_view log_label(const MarkerOutcome outcome) {
    switch (outcome) {
    case MarkerOutcome::Absent:
        return "absent";
    case MarkerOutcome::Present:
        return "present";
    case MarkerOutcome::Unreadable:
        return "unreadable";
    case MarkerOutcome::Corrupt:
        return "corrupt";
    }

    return "unknown";
}

std::string_view log_label(const Outcome outcome) {
    switch (outcome) {
    case Outcome::Nothing:
        return "nothing";
    case Outcome::AlreadyComplete:
        return "already-complete";
    case Outcome::RolledForward:
        return "rolled-forward";
    case Outcome::Restored:
        return "restored";
    case Outcome::Unresolved:
        return "unresolved";
    }

    return "unknown";
}

bool resolved(const Outcome outcome) {
    return outcome != Outcome::Unresolved;
}

MarkerRead read_marker(storage::KeyValue& store) {
    MarkerRead read;

    std::array<std::uint8_t, kMarkerSize> buffer{};
    std::size_t length = 0;

    const storage::Result result = store.read_blob(storage::Namespace::System, kMarkerKey,
                                                   buffer.data(), buffer.size(), length);

    switch (result) {
    case storage::Result::NotFound:
        read.outcome = MarkerOutcome::Absent;
        return read;

    case storage::Result::TooLarge:
        // Present but longer than this firmware's record: written by a future schema. Evidence, not
        // an absence.
        read.outcome = MarkerOutcome::Corrupt;
        return read;

    case storage::Result::InvalidKey:
    case storage::Result::Failed:
        // ⛔ NOT AN ABSENCE. See MarkerOutcome.
        read.outcome = MarkerOutcome::Unreadable;
        return read;

    case storage::Result::Ok:
        break;
    }

    return decode(buffer.data(), length);
}

bool write_marker(storage::KeyValue& store, const Recovery& recovery) {
    if (!recovery.valid()) {
        return false;
    }

    std::array<std::uint8_t, kMarkerSize> written{};
    const std::size_t length = encode(recovery, written.data());

    if (store.write_blob(storage::Namespace::System, kMarkerKey, written.data(), length) !=
        storage::Result::Ok) {
        return false;
    }

    // Read back before it is relied on, on the same reasoning §13.3 steps 3-4 apply to the records
    // themselves: a recovery record that cannot be read is worse than none, because the caller would
    // activate believing a repair is possible.
    const MarkerRead confirmed = read_marker(store);

    return confirmed.outcome == MarkerOutcome::Present &&
           confirmed.recovery.profile == recovery.profile &&
           confirmed.recovery.target_configuration == recovery.target_configuration &&
           confirmed.recovery.target_secret == recovery.target_secret &&
           confirmed.recovery.previous_configuration == recovery.previous_configuration &&
           confirmed.recovery.previous_secret == recovery.previous_secret;
}

bool clear_marker(storage::KeyValue& store) {
    // Erased rather than zeroed. erase_key() exists and is deliberately narrow (R10); this is one
    // named key holding no user data, and an absent key is the state read_marker() reports as
    // Absent without having to distinguish "cleared" from "never written".
    const storage::Result result = store.erase_key(storage::Namespace::System, kMarkerKey);

    // NotFound is success: the record is already gone, which is the state this asks for. Reporting
    // it as a failure would make an idempotent retry look like a storage fault.
    return result == storage::Result::Ok || result == storage::Result::NotFound;
}

Reconciliation reconcile(storage::KeyValue& store) {
    Reconciliation result;

    const MarkerRead marker = read_marker(store);

    switch (marker.outcome) {
    case MarkerOutcome::Absent:
        // Nothing was mid-activation. The pointers stand as they are.
        return result;

    case MarkerOutcome::Unreadable:
    case MarkerOutcome::Corrupt:
        // ⛔ EVIDENCE WITHOUT CONTENT. Nothing may be activated from it and nothing may be cleared,
        // because clearing would destroy the only sign that an activation was in flight. The store
        // may hold a mixed pair, so the caller must not read it normally.
        result.outcome = Outcome::Unresolved;
        result.marker_retained = true;
        return result;

    case MarkerOutcome::Present:
        break;
    }

    const Recovery& recovery = marker.recovery;
    result.profile = recovery.profile;

    // 1. ALREADY DONE. Both pointers name the target pair and both records read whole, so the
    //    activation completed and only its own clear was lost. Idempotent.
    if (pair_is_active(store, recovery.target_configuration, recovery.target_secret,
                       recovery.profile)) {
        result.outcome = Outcome::AlreadyComplete;
        result.marker_retained = !clear_marker(store);
        return result;
    }

    // 2. ROLL FORWARD. The target pair is proved present and whole in both namespaces before either
    //    pointer moves, and the move is confirmed by reread.
    if (pair_proves(store, recovery.target_configuration, recovery.target_secret,
                    recovery.profile) &&
        activate_pair_exactly(store, recovery.target_configuration, recovery.target_secret,
                              recovery.profile)) {
        result.outcome = Outcome::RolledForward;
        result.marker_retained = !clear_marker(store);
        return result;
    }

    // 3. RESTORE THE EXACT PREVIOUS PAIR. Rolling forward could not be completed, so the device goes
    //    back to the pair it was running — named exactly by the recovery record, proved whole, and
    //    confirmed by reread. This is §13.3 step 7 performed rather than hoped for.
    if (recovery.has_previous()) {
        const std::uint32_t profile = previous_profile(store, recovery);

        if (profile != 0 &&
            pair_proves(store, recovery.previous_configuration, recovery.previous_secret,
                        profile) &&
            activate_pair_exactly(store, recovery.previous_configuration, recovery.previous_secret,
                                  profile)) {
            result.outcome = Outcome::Restored;
            result.profile = profile;
            result.marker_retained = !clear_marker(store);
            return result;
        }
    }

    // 4. NEITHER PAIR COULD BE PROVED WHOLE. The recovery record stays exactly where it is, so a
    //    later boot — one where a transient read succeeds — can still finish the job. Nothing is
    //    erased (R10) and nothing downstream may read the store as if it held a whole pair.
    result.outcome = Outcome::Unresolved;
    result.marker_retained = true;
    return result;
}

} // namespace prd::app_core::candidate
