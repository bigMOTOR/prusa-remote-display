// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "app_model/bounded_string.hpp"
#include "storage/key_value.hpp"

// Stored credentials — Architecture Spec §13.2's SecretConfigV1, §13.5's handling rules.
//
// NO ESP-IDF HEADER ANYWHERE IN THIS COMPONENT, as with `config`: it talks only to
// storage::KeyValue, so the whole of it is compiled and tested by the host suite.
//
// ⛔ §13.5 IS THE POINT OF THIS COMPONENT, NOT A CAVEAT ON IT. Credentials must never be
// written to logs, included in AppState, returned by configuration endpoints, embedded in
// crash dumps where avoidable, included in OTA manifests, sent outside the LAN, or
// displayed on screen. Four of those seven are this component's to enforce and it enforces
// them structurally:
//
//   - NOTHING HERE RETURNS A SECRET TO A LOG. describe() builds the only diagnostic line
//     this component produces and is given no access to the values — see its contract.
//   - NOTHING HERE REACHES AppState. The component exposes a LoadReport; `app_core` turns
//     it into has-flags and a generation, and §24.3 permits nothing else.
//   - The values leave only through SecretConfigV1 itself, to a caller that asked for
//     them by name.
//
// The remaining three bind later checkpoints — the web interface (M3.7), the printer
// transport (M4) and OTA (M7) — and are recorded here so they are inherited rather than
// forgotten.
//
// ⛔ PLAINTEXT, AND THAT IS THE PRODUCT'S CURRENT STORAGE POLICY. Owner ruling R10: NVS
// encryption and HMAC/eFuse key provisioning are deferred production-security hardening
// with no scheduled milestone. This component adds no cryptography and must not; the
// threat model it serves is §13.6's — credentials protected from casual inspection on a
// trusted home or workshop network, with the device never exposed to the internet.
//
// ⛔ IT KNOWS NOTHING ABOUT NON-SECRET CONFIGURATION. Owner ruling, 2026-08-15. It does not
// include config's header, does not know what a printer host or an auth method is, and
// stores `profile_id` only as an OPAQUE ASSOCIATION HANDLE — an integer it never
// interprets. `app_core` is the composition root that decides whether that handle matches
// the configuration's, because only `app_core` may see both.

namespace prd::secrets {

// §13.2's SecretConfigV1.
//
// FIELD ORDER IS THE PERSISTED WIRE ORDER, as with ConfigV1: encode() and decode() walk
// these in declaration order, so reordering them invalidates every record already written.
//
// §13.2: "Only fields relevant to the selected authentication method are populated." That
// rule cannot be enforced here — the auth method is non-secret configuration this component
// may not see — so it belongs to whichever caller writes the record. What IS enforced here
// is the structural consistency below.
struct SecretConfigV1 {
    // The opaque handle tying this record to a Wi-Fi profile. 0 means unassociated.
    //
    // ONE RECORD, ONE PROFILE, FOR THE MVP — owner ruling, 2026-08-15: no per-profile
    // secret storage and no multi-network abstraction. The handle exists so `app_core` can
    // notice a record left behind by a previous profile, not so a second one can be added.
    std::uint32_t profile_id{0};

    app_model::BoundedString<32> wifi_ssid;
    app_model::BoundedString<64> wifi_password;

    app_model::BoundedString<64> printer_api_key;
    app_model::BoundedString<32> printer_username;
    app_model::BoundedString<64> printer_password;
};

[[nodiscard]] bool operator==(const SecretConfigV1& lhs, const SecretConfigV1& rhs);
[[nodiscard]] bool operator!=(const SecretConfigV1& lhs, const SecretConfigV1& rhs);

// An empty credential set. Valid by construction, asserted by a host test.
[[nodiscard]] SecretConfigV1 defaults();

// Which credentials a record carries, as booleans and nothing more.
//
// THIS IS THE ONLY SHAPE IN WHICH CREDENTIAL FACTS MAY LEAVE THIS COMPONENT TOWARD AppState
// OR A LOG. §13.5 forbids the values in both, and §24.3 permits the application state only
// "has-credential flags and auth metadata" — so the flags exist as a type rather than being
// recomputed by each caller from values it should not be holding.
struct Presence {
    bool wifi_ssid{false};
    bool wifi_password{false};
    bool printer_api_key{false};
    bool printer_username{false};
    bool printer_password{false};
};

[[nodiscard]] Presence presence_of(const SecretConfigV1& value);

// Mirrors config::LoadOutcome, and deliberately does not share it: the two components know
// nothing about each other, and a shared enum would be the first thread between them.
enum class LoadOutcome : std::uint8_t {
    Absent,
    Loaded,
    LoadedFromPreviousGeneration,
    Invalid,
    MigrationRequired,
    StorageFailure,
};

[[nodiscard]] std::string_view log_label(LoadOutcome outcome);

struct LoadReport {
    LoadOutcome outcome{LoadOutcome::Absent};
    SecretConfigV1 value{};

    // The generation the loaded record carried. 0 when nothing was loaded.
    //
    // THIS IS WHAT ConfigurationState::credential_generation MUST CARRY — owner ruling,
    // 2026-08-15. A credential can change while nothing non-secret does: the same printer
    // at the same address with a newly entered API key. Sourcing the generation from the
    // configuration record would leave that change invisible, and §16.2's "retry only after
    // configuration change" would never fire for the one case that most needs it.
    std::uint32_t generation{0};
};

enum class SaveOutcome : std::uint8_t {
    Committed,
    Rejected,
    CandidateFailed,
    CommitFailed,
};

[[nodiscard]] std::string_view log_label(SaveOutcome outcome);

// Loads the active credential record from the `secrets` namespace.
//
// Same staged scheme as the configuration record — two slots, an active-generation pointer,
// a CRC over header and payload, previous-known-good fallback — because the guarantees are
// the same and a credential deserves them at least as much. NOTHING HERE ERASES: ruling R10
// applies unchanged, and a record that cannot be read is reported and left where it is.
[[nodiscard]] LoadReport load(storage::KeyValue& store);

// Writes `value` using §13.3's staged pattern.
// The generation nothing holds.
inline constexpr std::uint32_t kNoGeneration = 0;

// --- §13.3's staged pattern, split at step 5 (Milestone 4) ------------------
//
// The credential half of the pair. See config.hpp for why the split exists: once §13.3 step 5's
// connection test becomes applicable, activating either half before the printer answers means a
// failed probe has already replaced what the device was running. Both halves must therefore stage
// first and activate together, and app_core's candidate reconciliation is what makes "together"
// survive a power loss between the two activations.
struct StageReport {
    SaveOutcome outcome{SaveOutcome::Committed};
    std::uint32_t generation{kNoGeneration};
};

// §13.3 steps 1–4 for the credential record. Writes the candidate to the inactive slot and reads it
// back; moves nothing.
[[nodiscard]] StageReport stage(storage::KeyValue& store, const SecretConfigV1& value);

// §13.3 step 6 for the credential record. One write_u32, and that store is this namespace's
// activation boundary.
[[nodiscard]] SaveOutcome activate(storage::KeyValue& store, std::uint32_t generation);

// Reads a specific generation's record, whether or not it is the active one.
//
// ⛔ THE CREDENTIAL HALF OF EXACT-IDENTITY RECOVERY, AND IT EXISTS SO THE TWO NAMESPACES ARE JUDGED
// BY ONE STANDARD. Boot reconciliation must prove that the exact record it is about to activate is
// present, decodes, checksums and names the expected profile — for the credential as much as for
// the configuration. Without this the credential half could only be activated by generation
// arithmetic, which is a pointer write with nothing behind it. Returns Absent when no slot holds
// that generation.
[[nodiscard]] LoadReport load_staged(storage::KeyValue& store, std::uint32_t generation);

// stage() then activate(), for callers with no applicable connection test.
[[nodiscard]] SaveOutcome save(storage::KeyValue& store, const SecretConfigV1& value);

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
    Cleared,
    Failed,
};

[[nodiscard]] std::string_view log_label(ClearOutcome outcome);

// Clears the active-generation pointer. After this, load() reports Absent.
[[nodiscard]] ClearOutcome deactivate(storage::KeyValue& store);

// Overwrites both credential slots with zeros and then removes them.
//
// ⚠️ OVERWRITE-THEN-ERASE, AND THE HONEST LIMIT OF IT. nvs_erase_key marks an entry erased;
// the flash page is reclaimed when NVS next compacts. Writing zeros over the blob first is the
// strongest removal the accepted storage abstraction offers, and it is what makes this a wipe
// rather than an abandoned pointer to a readable password. It is NOT a guaranteed physical
// scrub: under plaintext NVS (owner ruling R10) a flash dump taken before compaction may still
// contain residual bytes. Documented rather than overclaimed.
[[nodiscard]] ClearOutcome wipe(storage::KeyValue& store);

// The one diagnostic line this component produces.
//
// REDACTION AT THE CALL SITE, DONE STRUCTURALLY — owner ruling, 2026-08-15, which approved
// redaction where sensitive data could enter logging and rejected a global finished-string
// filter. The strongest form of "this line cannot contain a secret" is a builder that is
// never given one: this takes an outcome, a generation and a Presence, so there is no
// value in scope to leak. A host test drives it with every field populated and asserts none
// of the material appears.
//
// diagnostics::redact() remains the tool for the later call sites that legitimately handle
// user input — the provisioning form (M3.7) and Wi-Fi error text (M3.4) — where a message
// must be built from something that might contain a credential.
[[nodiscard]] std::string describe(LoadOutcome outcome, std::uint32_t generation,
                                   const Presence& presence);

} // namespace prd::secrets
