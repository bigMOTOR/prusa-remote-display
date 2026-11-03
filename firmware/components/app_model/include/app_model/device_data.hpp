// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstdint>
#include <string_view>

#include "app_model/board_identity.hpp"
#include "app_model/bounded_string.hpp"
#include "app_model/observation.hpp"
#include "app_model/values.hpp"

// Device-owned data: Architecture Spec §7.2 to §7.5, §7.11, §7.12.
//
// "Device" here means the companion, not the printer. The split matters: UIAD 001
// §2 separates what the subject is doing from what the companion itself is doing,
// and §10 gives the companion's own faults the highest presentation priority
// precisely because they are a different kind of problem.
//
// Modelling NetworkState and PrinterConnectionState is not implementing Wi-Fi or
// PrusaLink. Nothing here opens a socket, and no transport type appears: the
// response code below is a plain integer, because §5.1 forbids HTTP types in
// app_model while §7.5 and §19.3 both require the value to be part of the state.

namespace prd::app_model {

// §7.2 boot phases. The single boot-state model: the Milestone 1 `BootState`
// scaffolding was collapsed into this in Milestone 2 Phase B, so there is one
// concept, logged through one label function.
//
// Not every phase is emitted yet. Storage and configuration loading arrive in
// Milestone 3, and a phase is logged when the phase actually happens rather than
// as a decoration — a boot line claiming `storage-initializing` while nothing
// initialises storage would be false.
enum class BootPhase {
    Starting,
    HardwareInitializing,
    StorageInitializing,
    LoadingConfiguration,
    StartingServices,
    Ready,
    FatalError,
};

[[nodiscard]] std::string_view log_label(BootPhase phase);

// §7.2 storageStatus.
enum class StorageStatus {
    Unknown,
    Ready,

    // Storage opened but its contents were not usable and defaults were applied.
    // §13.4: "migration failure must not erase secrets automatically".
    RecoveredWithDefaults,

    Failed,
};

[[nodiscard]] std::string_view log_label(StorageStatus status);

// §7.3 configuration status.
enum class ConfigurationStatus {
    Missing,
    Loading,
    Valid,
    Invalid,
    MigrationRequired,
    StorageFailure,
};

[[nodiscard]] std::string_view log_label(ConfigurationStatus status);

// §2.1: API-key auth for the initial prototype, architecture-ready for Digest.
enum class AuthMethod {
    None,
    ApiKey,
    Digest,
};

[[nodiscard]] std::string_view log_label(AuthMethod method);

// §7.4 network phases.
// §16.1's Wi-Fi error classes, as a typed fact rather than as text.
//
// ⛔ TYPED BECAUSE THE PANEL MUST NOT READ last_error, AND NOTHING MAY MATCH ON STRINGS. NetworkState
// already carries a `last_error` for diagnostics; deciding a presentation from its WORDS would make
// the product's highest-priority screen depend on a diagnostic string that any future edit could
// silently change. This enumerator is what the projection and the reducer read.
enum class NetworkFault {
    None,

    // The link went away. §16.2 retries this automatically; it is ordinary connectivity loss.
    LinkLost,

    // The access point rejected the stored credential. The user can fix it by retyping something.
    CredentialsRejected,

    // ⛔ THE DRIVER ITSELF STOPPED ANSWERING. Not a statement about the user's network at all — see
    // DeviceFaultReason::WifiUnavailable, which this raises.
    DriverUnresponsive,
};

[[nodiscard]] std::string_view log_label(NetworkFault fault);

enum class NetworkPhase {
    Disabled,
    Starting,
    Connecting,
    Connected,
    Reconnecting,
    Unavailable,
};

[[nodiscard]] std::string_view log_label(NetworkPhase phase);

// §7.5 printer connection phases.
//
// §7.5's closing rule, and §24.5: "Connection state must never be inferred from
// printer operational state." These phases are set only by connection
// observations, and the reducer never derives one from a printer snapshot.
enum class ConnectionPhase {
    Unconfigured,
    WaitingForNetwork,
    Resolving,
    Probing,
    Authenticating,
    Connected,
    Degraded,
    Unauthorized,
    PrinterUnavailable,
    UnsupportedApi,
    InvalidResponse,
};

[[nodiscard]] std::string_view log_label(ConnectionPhase phase);

// §7.11 display power.
enum class DisplayPower {
    On,
    Dimmed,
    Off,
};

[[nodiscard]] std::string_view log_label(DisplayPower power);

// §7.12 OTA phases.
enum class OtaPhase {
    Idle,
    Checking,
    Available,
    Downloading,
    Verifying,
    Installing,
    RebootRequired,
    Validating,
    Failed,
};

[[nodiscard]] std::string_view log_label(OtaPhase phase);

// Owner-approved install lifecycle: selection does not release mutation guards;
// the device proceeds to a communicated reboot. No presentation delay is implied.
[[nodiscard]] constexpr bool ota_holds_device(OtaPhase phase) {
    return phase == OtaPhase::Downloading || phase == OtaPhase::Verifying ||
           phase == OtaPhase::Installing || phase == OtaPhase::RebootRequired;
}

// Why the companion itself cannot be trusted to operate — §16.1's device error
// category, and nothing else.
//
// This enum is the structural guarantee behind UIAD 001 §10: a device fault
// "shall never represent subject errors, authentication failures, unsupported
// interfaces or ordinary connectivity loss". None of those can be expressed here,
// so no amount of reducer logic can raise a device fault for them.
enum class DeviceFaultReason {
    None,
    StorageFailure,
    BoardMismatch,
    DisplayInitializationFailure,
    LowMemory,
    TaskWatchdog,
    OtaValidationFailure,

    // ⛔ THE WI-FI DRIVER STOPPED ANSWERING — the owner's DriverStalled ruling, 2026-08-25. A driver
    // operation was accepted and its documented terminal completion never arrived, a synchronous
    // driver call was refused in a state the pinned SDK documents it to succeed in, or the driver's
    // event evidence was lost. The station is quarantined: it issues no further driver operation and
    // only an explicit restart clears it.
    //
    // ⛔ IT IS A LOCAL DEVICE FAULT AND NOT ORDINARY CONNECTIVITY LOSS, WHICH §16.1 KEEPS SEPARATE.
    // Ordinary loss is a NetworkPhase and a confidence question — the network went away and the
    // device is still working. This is the opposite: the network may be perfectly healthy and it is
    // the DEVICE that can no longer participate, cannot be recovered by retrying, and needs the user
    // to power-cycle it. That is §16.1's device-error class, and it is why the actionable instruction
    // belongs on the panel rather than in a confidence indicator.
    //
    // ⬅️ AND IT IS NOT RecoveryMode, WHICH MEANS SOMETHING ELSE ENTIRELY: that a stored
    // configuration cannot be used. The configuration here is fine and nothing has been erased.
    WifiUnavailable,

    // ⛔ A USER-CONFIRMED FACTORY RESET DID NOT FINISH — audit item 5, ADR-0008's 2026-08-28
    // amendment. Two routes reach it and they leave DIFFERENT store states:
    //
    //   • the journal could not be written, so no journal was established, NOTHING WAS REMOVED on
    //     this attempt and no resumable job is owed;
    //   • the journal was written and a removal did not finish, so records may be gone and a
    //     resumption IS owed.
    //
    // ⛔ WHAT IS TRUE OF BOTH — and it is all this reason asserts — is that the reset the user asked
    // for DID NOT FINISH because a required storage operation failed, and that the store is
    // therefore not trusted for further mutation. ⛔ It does NOT assert that a journal exists, that a
    // job is owed, or that anything was or was not erased. This boot cannot see what an earlier one
    // removed.
    //
    // ⬅️ AND IT IS NOT "the store has just refused a write", WHICH THIS COMMENT USED TO SAY. That is
    // only true of the first route. On the second the journal write SUCCEEDED and a later removal
    // failed, so the step that failed was an erase, not a write. The common fact is the failed
    // storage operation, whichever one it was.
    //
    // ⬅️ IT IS §16.1's "storage failure" DEVICE-ERROR CLASS, AT A FINER GRAIN, and not a new
    // category. It is separated from StorageFailure because the two need DIFFERENT USER-FACING
    // SENTENCES: this one can truthfully say a reset could not finish, and StorageFailure — raised
    // when the store would not answer at all — cannot, because on that route no reset is proved to
    // exist. A single reason could not carry both without one of them claiming something it has
    // not established.
    //
    // ⛔ IT HOLDS THE DEVICE EXCLUSIVELY UNTIL AN EXPLICIT RESTART. A reset that half-finished must
    // not release the device into provisioning or printer work while its journal is still armed —
    // the next boot would erase whatever was configured in the meantime. See
    // app_core/exclusive_operation.hpp.
    FactoryResetIncomplete,
};

[[nodiscard]] std::string_view log_label(DeviceFaultReason reason);

// ⛔ WHETHER THIS FAULT HOLDS THE DEVICE — and therefore must not be silently replaced by a later
// observation. Audit item 5, 2026-08-28.
//
// TWO REASONS, AND THE LIST IS THE SAME ONE app_core's exclusive-operation barrier uses; that unit
// calls this function rather than keeping a second copy. A fault that holds the device is one where
// the store may not be written on the strength of what is known, so releasing it — or presenting
// something else in its place — is a product error, not a display preference.
//
// ⬅️ WHY REPLACEMENT WAS A REAL DEFECT AND NOT A THEORETICAL ONE. `device_fault` is a single mutable
// field that five reducer paths assign. A NetworkObserved carrying DriverUnresponsive that was
// already on the application queue when a factory reset failed would reach the next drain and
// overwrite FactoryResetIncomplete with WifiUnavailable — which released the barrier and replaced a
// truthful "Reset could not finish" with "Wi-Fi unavailable" on the panel. Measured, not supposed.
[[nodiscard]] bool fault_holds_device(DeviceFaultReason reason);

// --- Composite device state -----------------------------------------------

// §7.2. The board identity and capacity results come from the Milestone 1
// hardware probe, which already produces them as ValidationState.
struct BootStatus {
    BootPhase phase{BootPhase::Starting};

    BoundedString<32> reset_reason;
    BoundedString<32> board_target;

    ValidationState flash_validation{ValidationState::Unknown};
    ValidationState psram_validation{ValidationState::Unknown};

    StorageStatus storage{StorageStatus::Unknown};

    // §15.4. Entered after repeated early boot failures; preserves credentials.
    bool safe_mode{false};
};

[[nodiscard]] bool operator==(const BootStatus& lhs, const BootStatus& rhs);
[[nodiscard]] bool operator!=(const BootStatus& lhs, const BootStatus& rhs);

// §7.3. Secrets are never here: §7.3 and §24.3 allow only has-credential flags
// and auth metadata in AppState.
struct ConfigurationState {
    ConfigurationStatus status{ConfigurationStatus::Missing};
    std::uint32_t schema_version{0};

    // §13.3's "active-record generation", applied to secrets. Incremented by the
    // configuration owner whenever a stored credential is replaced.
    //
    // It exists because a credential can change without anything else changing: the
    // same printer at the same address with a newly entered API key. Without a
    // generation there is no observable difference between that and a re-report of
    // the old configuration, and §16.2's "retry only after configuration change"
    // would never fire for the one case that most needs it.
    std::uint32_t credential_generation{0};

    bool has_wifi_credentials{false};

    // A printer configuration that has been VALIDATED against the real printer and
    // promoted to active.
    bool has_printer_configuration{false};

    // A structurally valid printer configuration stored as a CANDIDATE that has not yet
    // been validated or promoted — owner ruling R8, 2026-08-14.
    //
    // Three states, and they must stay distinguishable:
    //
    //   has == false, pending == false   no printer configuration
    //   has == false, pending == true    configured, awaiting validation
    //   has == true,  pending == false   active, validated
    //
    // WHY A FIELD RATHER THAN AN INFERENCE. Ruling R4 keeps every printer protocol in
    // Milestone 4, so Milestone 3 persists an address and a credential without ever
    // contacting the printer. Without this flag "no printer configured" and "configured
    // but unproven" are the same state, and Milestone 4 cannot tell whether there is
    // anything to validate. It is deliberately NOT a state machine — R8 is explicit that
    // it must not grow beyond this three-way distinction.
    //
    // An extension to Architecture §7.3's listed schema, on the same recorded footing as
    // credential_generation above.
    bool printer_configuration_pending{false};

    BoundedString<64> printer_address;
    BoundedString<32> printer_alias;

    AuthMethod auth_method{AuthMethod::None};
};

[[nodiscard]] bool operator==(const ConfigurationState& lhs, const ConfigurationState& rhs);
[[nodiscard]] bool operator!=(const ConfigurationState& lhs, const ConfigurationState& rhs);

// §7.4, with §7.3's rule taking precedence over its field list.
//
// ⛔ NO SSID. §7.4 once listed `ssidDisplayName` here and this struct carried it. The owner
// clarification of 2026-08-23 settled that the actual home network name IS credential material:
// §13.2 places `wifiSsid` inside SecretConfigV1, §13.5 forbids credentials in `AppState`, and §7.3
// says outright that "Secrets are never placed inside `AppState`". The field was also read by
// nothing — no scene, no presentation, no projection — so it carried a credential into the
// authoritative state and out of it again at every snapshot, for no product behaviour at all.
//
// ⬅️ AND NO REPLACEMENT ALIAS. What provisioning shows on the panel is the device's OWN generated
// setup network (ui::SetupSession, from network::session_credential), which is device identity
// rather than a user credential and never travels through here. A "display name" field would be
// the same value under a name that made it look safe.
struct NetworkState {
    NetworkPhase phase{NetworkPhase::Disabled};

    BoundedString<16> ip_address;

    // dBm. Absent until associated.
    bool signal_known{false};
    std::int8_t signal_strength_dbm{0};

    Timestamp last_connected_at{};
    std::uint32_t consecutive_failures{0};
    BoundedString<48> last_error;

    // The same fact as `last_error`, in the form a decision may be taken from. `last_error` is for a
    // reader; this is for the product.
    NetworkFault fault{NetworkFault::None};
};

[[nodiscard]] bool operator==(const NetworkState& lhs, const NetworkState& rhs);
[[nodiscard]] bool operator!=(const NetworkState& lhs, const NetworkState& rhs);

// §7.5.
struct PrinterConnectionState {
    ConnectionPhase phase{ConnectionPhase::Unconfigured};

    BoundedString<64> endpoint;
    AuthMethod auth_method{AuthMethod::None};

    // §7.5 capabilities, as the capability tier the probe resolved. The full
    // capability set is a prusalink concern and stays out of app_model.
    BoundedString<24> capability_tier;

    Timestamp last_attempt_at{};
    Timestamp last_success_at{};
    std::uint32_t consecutive_failures{0};

    // §7.5 lastHttpStatus and §19.3's "last HTTP status", as an opaque transport
    // response code. Kept an integer so no transport type reaches app_model (§5.1).
    bool response_code_known{false};
    std::uint16_t last_response_code{0};

    bool latency_known{false};
    std::uint32_t last_latency_ms{0};

    BoundedString<48> last_error;

    // §7.5 dataFreshness. Recomputed from observation age by explicit ageing, so
    // no clock is read to maintain it.
    DataFreshness freshness{DataFreshness::Unknown};
};

[[nodiscard]] bool operator==(const PrinterConnectionState& lhs, const PrinterConnectionState& rhs);
[[nodiscard]] bool operator!=(const PrinterConnectionState& lhs, const PrinterConnectionState& rhs);

// §7.11. §7.11's closing rule and §17.8: display power never stops monitoring.
struct DisplayState {
    DisplayPower power{DisplayPower::On};
    std::uint8_t brightness_percent{100};

    // §7.11 idleMode: the companion has become visually quieter under UI
    // Behaviour Decision 001 §3, distinct from the panel being dimmed.
    bool idle_mode{false};

    Timestamp last_interaction_at{};
};

[[nodiscard]] bool operator==(const DisplayState& lhs, const DisplayState& rhs);
[[nodiscard]] bool operator!=(const DisplayState& lhs, const DisplayState& rhs);

// §7.12.
struct OtaState {
    OtaPhase phase{OtaPhase::Idle};

    // OTA metadata: accepts every supported 31-character image version. Forwarded to the
    // update composition while downloading; still not persisted as an ABI.
    BoundedString<32> available_version;

    // Firmware update progress is its own value and Production UI §9 forbids
    // representing it with the operational Progress Ring.
    ProgressValue progress;

    BoundedString<48> error;

    // §15.3, §18.4: a newly installed image stays pending until health criteria
    // pass. Printer connectivity is not one of them (§24.15).
    bool pending_validation{false};

    // One acknowledgement per discovered release. Availability itself remains until the
    // release is replaced or installation begins; only this first-safe-Ready statement retires
    // with the ordinary display acknowledgement window.
    bool notice_spent{false};
    bool notice_presenting{false};
};

[[nodiscard]] bool operator==(const OtaState& lhs, const OtaState& rhs);
[[nodiscard]] bool operator!=(const OtaState& lhs, const OtaState& rhs);

// §7.1 diagnosticsSummary, and §19.2's structured context.
//
// These counters are deliberately excluded from the change detection in
// app_state.hpp: a rejected duplicate is worth recording but is not a change in
// what the product is showing, and publishing a new state revision for it would
// make an idempotent event look like a mutation.
struct DiagnosticsSummary {
    std::uint32_t events_applied{0};

    // Observations rejected by the sequence rule in §8.1.
    std::uint32_t events_rejected_stale{0};

    // Events whose timestamp moved backwards. Time is clamped forward, never
    // rewound, and the fact is counted rather than hidden.
    std::uint32_t events_out_of_order{0};

    // §24.8: unknown enum values are preserved for diagnostics. This counts how
    // often one arrived; the value itself is in PrinterOperationalState::raw.
    std::uint32_t unknown_printer_states_seen{0};

    // §7.8: a progress value outside 0...100. Recorded here because the clamp
    // happens for display only and the original must not vanish silently.
    std::uint32_t out_of_range_progress_seen{0};
};

[[nodiscard]] bool operator==(const DiagnosticsSummary& lhs, const DiagnosticsSummary& rhs);
[[nodiscard]] bool operator!=(const DiagnosticsSummary& lhs, const DiagnosticsSummary& rhs);

} // namespace prd::app_model
