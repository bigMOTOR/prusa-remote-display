// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstdint>
#include <string_view>

#include "app_model/bounded_string.hpp"
#include "app_model/device_data.hpp"
#include "storage/key_value.hpp"

// Validated user configuration — Architecture Spec §5.9.
//
// NO ESP-IDF HEADER APPEARS ANYWHERE IN THIS COMPONENT, not even in an adapter: every
// translation unit is standard C++17 over storage::KeyValue, so the whole of `config` is
// compiled and tested by the host suite. That is the payoff of the injected narrow
// interface Bootstrap Spec §11.3 asks for — the record format, the staged write, the
// generation selection and the migration dispatch are all exercised where they can fail
// visibly rather than only on a board.
//
// NON-SECRET ONLY. §5.9 gives this component "non-secret configuration access"; §13.2's
// SecretConfigV1 — the Wi-Fi password, the API key — belongs to Milestone 3.3 and to the
// `secrets` namespace. Nothing here reads or writes a credential, and §24.3 keeps every
// one of them out of AppState regardless.
//
// NOTHING HERE ERASES A PARTITION. Owner ruling R10: a record that cannot be read is
// reported, never discarded. The component writes and replaces its own keys and does not
// possess a partition-level operation — storage::Backend has none to offer.

namespace prd::config {

// §13.2's schemaVersion, and §13.4's migration anchor.
//
// BUMP THIS WHENEVER THE PAYLOAD LAYOUT CHANGES, and add the migration that reads the old
// layout. A device in the field holds records written by whatever version it last ran;
// §13.4 makes migrations "explicit and one-way", so the only safe direction is forward and
// the only safe change to an existing field is none.
//
// Public since Milestone 3.3: app_core assembles the ConfigurationObservation and reports
// this in it, because the observation now combines a configuration report and a secret
// report that neither component may see together.
inline constexpr std::uint16_t kSchemaVersion = 1;

// §13.2's updateChannel.
//
// ONE VALUE, AND THAT IS THE HONEST SET. §18 speaks of a "release channel" and §18.2 of
// "the project's official release location", but no accepted source enumerates channels.
// A second enumerator would be a product decision invented to make an enum look
// plausible. The field is persisted and validated now for schema completeness and stays
// unused until the OTA milestone — owner ruling, 2026-08-15.
enum class UpdateChannel : std::uint8_t {
    Stable = 0,
};

[[nodiscard]] std::string_view log_label(UpdateChannel channel);

// §13.2's ConfigV1, field for field, plus one recorded extension.
//
// FIELD ORDER IS THE PERSISTED WIRE ORDER. encode() and decode() walk these in
// declaration order; reordering them silently invalidates every record already written to
// a device in the field. Add new fields at the end and bump kSchemaVersion.
struct ConfigV1 {
    // The non-secret handle for the stored Wi-Fi profile. 0 means none.
    //
    // §13.2 puts wifiSsid and wifiPassword in SecretConfigV1, so the network's identity is
    // not here; this is the id Milestone 3.3's secret record binds itself to. Keeping the
    // handle non-secret is what lets `config` know a profile exists without being able to
    // read it.
    std::uint32_t wifi_profile_id{0};

    // Hostname or IP. Empty means no printer has been configured at all.
    app_model::BoundedString<64> printer_host;

    // 0 means "unset"; any other value must be a valid port.
    //
    // PERSISTED AND VALIDATED ONLY. Owner ruling R4 and 2026-08-15: this introduces no
    // printer-protocol behaviour and no network probing before Milestone 4. Nothing in
    // Milestone 3 opens a socket to it.
    std::uint16_t printer_port{0};

    app_model::BoundedString<32> printer_alias;

    app_model::AuthMethod auth_method{app_model::AuthMethod::None};

    // R8's three-way distinction, persisted because it must survive a reboot.
    //
    //   printer_host empty                          → no printer configuration
    //   printer_host set, printer_validated false   → PENDING validation
    //   printer_host set, printer_validated true    → active, validated
    //
    // Milestone 3 only ever writes false: R4 forbids promoting a printer configuration on
    // the basis of syntax or Wi-Fi connectivity alone. Milestone 4 validates against the
    // real printer and promotes it atomically.
    //
    // AN EXTENSION TO §13.2's LISTED SCHEMA, on the recorded precedent of
    // ConfigurationState::credential_generation. Without it "configured" and "configured
    // but unproven" are the same stored fact, and M4 cannot tell whether to start
    // validating.
    bool printer_validated{false};

    // 0..100.
    std::uint8_t brightness{80};

    std::uint32_t dim_delay_seconds{60};
    std::uint32_t sleep_delay_seconds{600};

    UpdateChannel update_channel{UpdateChannel::Stable};
};

[[nodiscard]] bool operator==(const ConfigV1& lhs, const ConfigV1& rhs);
[[nodiscard]] bool operator!=(const ConfigV1& lhs, const ConfigV1& rhs);

// The defaults a device holds before anything has been configured. Valid by construction —
// asserted by a host test, because a default that fails its own validator would make every
// factory-fresh device report Invalid rather than Missing.
[[nodiscard]] ConfigV1 defaults();

// Why a load ended where it did.
//
// Finer-grained than ConfigurationStatus for the same reason storage::Outcome is finer
// than StorageStatus: the status answers "may the product rely on this", the outcome
// answers "what exactly happened", and only the second can choose a remedy.
enum class LoadOutcome : std::uint8_t {
    // No record has ever been written. The first-boot case, and the one that reaches
    // ProvisioningPhase::SetupRequired.
    Absent,

    Loaded,

    // The active record was unreadable or failed its checksum, and the previous
    // known-good generation was used instead (§13.3).
    LoadedFromPreviousGeneration,

    // A record decoded but failed validation.
    Invalid,

    // A record carries a schemaVersion this firmware does not know how to migrate.
    MigrationRequired,

    // The store itself failed.
    StorageFailure,
};

[[nodiscard]] std::string_view log_label(LoadOutcome outcome);

struct LoadReport {
    LoadOutcome outcome{LoadOutcome::Absent};
    ConfigV1 value{};

    // The generation the loaded record carried. 0 when nothing was loaded.
    std::uint32_t generation{0};
};

// Why a staged write ended where it did.
enum class SaveOutcome : std::uint8_t {
    Committed,

    // §13.3 step 1. Nothing was written.
    Rejected,

    // The candidate could not be written, or did not read back as written (§13.3 steps
    // 2-4). THE PREVIOUS ACTIVE RECORD IS UNTOUCHED — the candidate always goes to the
    // inactive slot, so a failed save cannot damage what the device was using.
    CandidateFailed,

    // The candidate was written and verified, but the active-generation pointer could not
    // be moved (§13.3 step 6). The device keeps running the previous record.
    CommitFailed,
};

[[nodiscard]] std::string_view log_label(SaveOutcome outcome);

// Loads the active configuration — Architecture Spec §15.1 step 10.
//
// Reads the active-generation pointer from the `system` namespace and the record from
// `config`, per the owner-approved placement of 2026-08-15. On a torn or unreadable
// active record it falls back to the previous known-good generation rather than failing,
// which is §13.3's "previous-known-good generation for critical configuration".
[[nodiscard]] LoadReport load(storage::KeyValue& store);

// --- §13.3's staged pattern, split at step 5 (Milestone 4) ------------------
//
// ⬅️ WHY THE SPLIT EXISTS. §13.3 is an ordered pattern: validate, write the candidate, commit it,
// read it back, **test the connection when applicable**, mark active, and retain the previous valid
// configuration on failure. Milestone 3 had no applicable connection test — R4 kept every printer
// probe out — so save() legitimately did all of it in one call, and its own comment said so.
//
// Milestone 4 makes step 5 applicable, and that changes everything: if the pointer moves before the
// printer answers, a failed probe has already replaced the configuration the device was running, and
// step 7's "retain the previous valid configuration" cannot be honoured because there is nothing
// holding it. So the write and the activation become two operations, and the caller puts the
// connection test between them.
//
// save() remains, unchanged in behaviour, as stage() followed by activate(). Callers with no
// connection test to run — a brightness change, a factory-reset default — keep using it and are
// unaffected.

// The generation nothing holds. A record can never carry it, so it is a safe "no candidate" marker.
inline constexpr std::uint32_t kNoGeneration = 0;

// What a staging attempt produced.
struct StageReport {
    // Committed here means "written to the inactive slot and read back byte for byte". It does NOT
    // mean active: nothing a reader can see has changed yet.
    SaveOutcome outcome{SaveOutcome::Committed};

    // The candidate's generation, which activate() needs and which identifies the staged record for
    // load_staged(). kNoGeneration when staging failed.
    std::uint32_t generation{kNoGeneration};
};

// §13.3 steps 1–4. Validates, writes the candidate into the slot the active generation is not
// using, and reads it back.
//
// ⛔ MOVES NOTHING. The active-generation pointer is untouched, so a device that loses power at any
// point inside this call — or that never activates at all — carries on running exactly the record it
// was running before.
[[nodiscard]] StageReport stage(storage::KeyValue& store, const ConfigV1& value);

// §13.3 step 6. Makes a staged generation the active one.
//
// One `write_u32`. That single store is the activation boundary for this namespace: before it the
// previous record is active, after it the staged record is, and there is no third state.
[[nodiscard]] SaveOutcome activate(storage::KeyValue& store, std::uint32_t generation);

// Reads a specific generation's record, whether or not it is the active one.
//
// Exists so a staged candidate can be validated — and reported on — without being activated first.
// Returns Absent when no slot holds that generation.
[[nodiscard]] LoadReport load_staged(storage::KeyValue& store, std::uint32_t generation);

// stage() then activate(), for callers with no applicable connection test.
[[nodiscard]] SaveOutcome save(storage::KeyValue& store, const ConfigV1& value);

// --- The active-generation pointer, read directly (Milestone 4) -------------

// What the active-generation pointer says, without loading a record.
//
// ⛔ WHY THIS EXISTS, AND WHY LoadReport::generation IS NOT THE SAME FACT. load() falls back to a
// previous known-good record when the record the pointer names is unusable, and it then reports THAT
// record's generation. So `load().generation == G` means "generation G is what a reader gets", not
// "the pointer names G". Boot-time pair recovery has to distinguish the two: it clears a write-ahead
// recovery record only once the POINTERS themselves have been repaired, and a fallback satisfying a
// pointer check would clear the record while a pointer still named something else.
//
// Exposed here rather than read by key from another component, so the pointer key stays this
// component's single source of truth.
struct PointerRead {
    // False only when the store itself failed. An absent pointer is readable and means kNoGeneration:
    // nothing has ever been activated.
    bool readable{false};

    std::uint32_t generation{kNoGeneration};
};

[[nodiscard]] PointerRead active_generation(storage::KeyValue& store);

// --- User-consented removal (Milestone 3.10) --------------------------------
//
// ⛔ THESE ARE THE "EXPLICITLY NAMED OPERATION" storage/bring_up.hpp PREDICTED, and they are
// the only removal this component has. Owner ruling R10 forbids automatically discarding
// anything that could hold user configuration or credentials, so `storage::Backend` was given
// no erase at all and `storage::KeyValue` was given erase_key() for ONE NAMED KEY. These build
// on that and nothing wider: no partition erase, no key enumeration, no wildcard.
//
// ⛔ REACHABLE ONLY FROM A CONFIRMED FACTORY RESET. app_core calls them from the factory-reset
// executor, which runs only after a second deliberate very-long press inside its confirmation
// window. No failure path — a migration that cannot run, a Wi-Fi that will not join, a store
// that reports a fault — may call them; that is the difference between user-consented recovery
// and the automatic erasure R10 rules out.
//
// SPLIT IN TWO, AND THE ORDER MATTERS. deactivate() clears the active-generation pointer,
// which is the whole of activation: "no active-generation marker, no active configuration".
// discard()/wipe() then removes the records themselves. Running deactivate() FIRST is what
// makes a reset interrupted by power loss safe — after it, load() reports Absent and the
// device is un-provisioned, so whatever bytes remain are inert rather than half-active.

enum class ClearOutcome : std::uint8_t {
    // Every named key is now absent. Includes the case where it was already absent, because
    // "gone" is the postcondition being asserted, not "I removed something".
    Cleared,

    // At least one named key could not be removed. The caller must not report success; a
    // record left behind is a record the next boot may still read.
    Failed,
};

[[nodiscard]] std::string_view log_label(ClearOutcome outcome);

// Clears the active-generation pointer. After this, load() reports Absent.
[[nodiscard]] ClearOutcome deactivate(storage::KeyValue& store);

// Removes both configuration slots. Call AFTER deactivate().
[[nodiscard]] ClearOutcome discard(storage::KeyValue& store);

// THERE IS DELIBERATELY NO observe() HERE. Until Milestone 3.3 this component projected its
// own LoadReport into a ConfigurationObservation — and got credential_generation wrong,
// because it sourced it from the configuration record and a credential can change while
// nothing non-secret does. Owner ruling, 2026-08-15: the observation is assembled by
// app_core from independent config and secrets reports, and this component knows nothing
// about secrets or secret generations. See app_core/src/configuration_observation.hpp.

} // namespace prd::config
