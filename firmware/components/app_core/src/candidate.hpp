// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "config/config.hpp"
#include "secrets/secrets.hpp"
#include "storage/key_value.hpp"

// THE ACTIVATION BOUNDARY FOR A CONFIG/CREDENTIAL PAIR, AS A WRITE-AHEAD RECOVERY RECORD.
//
// ⛔ THE PROBLEM. §13.3's staged pattern is per-record, but this product's configuration is two
// records in two namespaces — a `config` record and a `secrets` record — and they are only
// meaningful together: `configuration_observation.cpp` treats the device as provisioned only when
// both name the same Wi-Fi profile id. Staging both without activating either is safe (the previous
// pair stays active, which is step 7 by construction). Activating both is two `write_u32` calls, and
// a power loss between them leaves a MIXED pair: config naming the new profile, credentials naming
// the old. That reads as un-provisioned, which means the user's working configuration is gone.
//
// ⛔ AND WHY A BARE PROFILE NUMBER WAS NOT ENOUGH. The first version of this file wrote one u32 — the
// candidate profile — and recovered the missing half by ARITHMETIC, activating `active + 1` and
// hoping a record lived there. Three things went wrong with that, and all three are closed here:
//
//   • The credential half was activated with no record validation at all. A degraded load reports
//     generation 0, so `active + 1` became 1, and the pointer was moved to a generation no record
//     occupied — putting both real records ABOVE the pointer, where previous-known-good selection
//     will never look at them again.
//   • A failed repair still cleared the marker, on the stated grounds that the next boot would
//     classify it as stale. It would not: exactly one half still carries the profile, so the next
//     boot would have finished the job. Clearing destroyed the only recovery evidence and stranded
//     the mixed pair permanently.
//   • There was nothing to restore TO. If rolling forward was impossible the previous pair could not
//     be named, because the record never said what it was.
//
// ⬅️ SO THE MARKER IS A RECORD, NOT A NUMBER. Versioned, checksummed, and carrying exact identity:
// the candidate profile, the exact target generation in each namespace, and the exact previous
// known-good generation in each namespace. No generation is ever inferred. Every activation — roll
// forward or restore — loads the exact record first and proves it decodes, validates and names the
// profile it should.
//
// ⬅️ AND RECONCILIATION IS IDEMPOTENT, WHICH IS WHAT MAKES A REPEATED POWER LOSS SURVIVABLE. Each
// boot re-reads the record, re-proves both halves, and only clears the marker once a REREAD shows a
// whole pair. A boot that dies mid-repair leaves the evidence exactly as it found it.
//
// ⬅️ WHY ROLLING FORWARD IS THE FIRST CHOICE AND NOT A GUESS. activate_pair() is reached only after
// the printer has answered — §14.3 step 9 and §13.3 step 6 put it there, and the only caller is the
// success path of a validated transaction. So a marker in flash is proof that a validation succeeded
// for that profile. Rolling back first would discard a configuration the device had already proved.
// Restoration is the fallback for when the target cannot be proved whole, and it restores an EXACT
// named pair rather than whatever happens to be lying around.
//
// ⛔ AND NOTHING HERE REVALIDATES. The marker's meaning is "validation already succeeded". The boot
// path never contacts a printer (§15.1 keeps a network dependency out of the boot decision), so an
// orphaned candidate with no marker is simply not activated — never probed.
//
// ⬅️ WHAT HAPPENS TO A CANDIDATE THAT WAS ONLY STAGED. Nothing: no marker was written, so the
// previous pair is active and the orphaned candidate is overwritten by the next transaction. The
// ruling permits this explicitly — "discard/return to setup truthfully rather than leaving an
// indefinite acquiring state" — and it IS truthful: the setup the user did not finish did not
// happen, and the configuration they had still works.

namespace prd::app_core::candidate {

// --- The recovery record ---------------------------------------------------

// The exact identity of one pair activation. Every field is a generation or a profile that some
// record actually carries; nothing here is derived.
struct Recovery {
    // The profile id both target records name. Never 0 in a valid record.
    std::uint32_t profile{0};

    // The exact generations to activate.
    std::uint32_t target_configuration{config::kNoGeneration};
    std::uint32_t target_secret{secrets::kNoGeneration};

    // The exact generations that were active before, to restore when rolling forward is impossible.
    // kNoGeneration when there was no previous pair — a first provisioning, where there is nothing
    // to restore and nothing was at risk.
    std::uint32_t previous_configuration{config::kNoGeneration};
    std::uint32_t previous_secret{secrets::kNoGeneration};

    [[nodiscard]] bool valid() const {
        return profile != 0 && target_configuration != config::kNoGeneration &&
               target_secret != secrets::kNoGeneration;
    }

    [[nodiscard]] bool has_previous() const {
        return previous_configuration != config::kNoGeneration &&
               previous_secret != secrets::kNoGeneration;
    }
};

// How a read of the recovery record turned out.
//
// ⛔ Absent AND Unreadable ARE DIFFERENT ANSWERS, AND CONFLATING THEM WAS A DEFECT. "No activation
// was in flight" is a fact; "the store would not tell us" is an absence of facts. Treating the
// second as the first skips reconciliation and lets a genuinely mixed pair be presented as an
// un-provisioned device — with the user's configuration apparently gone.
enum class MarkerOutcome : std::uint8_t {
    // No record. Nothing was mid-activation.
    Absent,

    // Read, decoded, checksum verified, contents valid.
    Present,

    // The store failed the read. Nothing may be concluded and nothing may be cleared.
    Unreadable,

    // Read, but the bytes are not a record this firmware can trust: wrong magic, wrong version,
    // failed checksum, or a payload naming no profile. Evidence is retained, not acted on.
    Corrupt,
};

struct MarkerRead {
    MarkerOutcome outcome{MarkerOutcome::Absent};
    Recovery recovery{};
};

[[nodiscard]] std::string_view log_label(MarkerOutcome outcome);

// Reads the recovery record. Never invents an absence: see MarkerOutcome.
[[nodiscard]] MarkerRead read_marker(storage::KeyValue& store);

// Records the activation that is about to happen. False when the write or its read-back failed, in
// which case the caller must not activate anything — an activation with no recovery record behind it
// is the mixed-pair hazard with nothing to repair it.
[[nodiscard]] bool write_marker(storage::KeyValue& store, const Recovery& recovery);

// Clears the record. False when the write failed; the next boot then reconciles again, which is
// harmless because reconciliation is idempotent.
[[nodiscard]] bool clear_marker(storage::KeyValue& store);

// --- Reconciliation --------------------------------------------------------

// What a boot did about an interrupted activation.
enum class Outcome : std::uint8_t {
    // No record. The active pair is whatever the two pointers say, and they were not mid-move.
    Nothing,

    // Both halves already carried the target pair: the activation finished and only the record's own
    // clear was lost. Idempotent tidy-up.
    AlreadyComplete,

    // The target pair was proved and both pointers now name it, confirmed by reread.
    RolledForward,

    // The target could not be proved whole, and the exact previous pair was restored instead,
    // confirmed by reread.
    Restored,

    // ⛔ NEITHER PAIR COULD BE PROVED WHOLE. The recovery record is RETAINED and the caller must not
    // let downstream configuration loading observe the store: it may hold a mixed pair. This is the
    // storage/recovery failure path.
    Unresolved,
};

// Whether the caller may go on to read the configuration normally.
[[nodiscard]] bool resolved(Outcome outcome);

[[nodiscard]] std::string_view log_label(Outcome outcome);

struct Reconciliation {
    Outcome outcome{Outcome::Nothing};

    // The profile the store ended up on, 0 when none could be established. Diagnostics only.
    std::uint32_t profile{0};

    // True when a whole pair was established but the record's own clear failed. Harmless — the next
    // boot reconciles again and reaches AlreadyComplete — but reported so a maintainer sees the
    // storage complaint.
    bool marker_retained{false};
};

// Whether both active pointers name exactly this pair and both records read back whole.
//
// ⛔ THE REREAD, AND EVERY "SUCCEEDED" CLAIM RESTS ON IT. Two accepted pointer writes say the store
// took two stores; they do not say a reader will now see a whole pair. activate_pair() and
// reconcile() both call this before reporting success and before clearing the recovery record.
[[nodiscard]] bool pair_is_active(storage::KeyValue& store, std::uint32_t configuration,
                                  std::uint32_t secret, std::uint32_t profile);

// ⛔ THE REAL EXECUTOR, HOST-TESTED AGAINST AN INJECTED STORE. The previous design host-tested a pure
// enum decision and left the writes, the rereads and the clear in an untestable binding — which is
// where all three defects lived. This function performs the whole transaction, so the power-loss
// matrix exercises what production runs.
[[nodiscard]] Reconciliation reconcile(storage::KeyValue& store);

} // namespace prd::app_core::candidate
