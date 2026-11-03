// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstdint>
#include <string_view>

#include "app_model/device_data.hpp"
#include "storage/key_value.hpp"

// User-consented factory reset — Milestone 3.10, owner Decision 4.
//
// PURE, AND HOST-TESTED. It takes a storage::KeyValue and nothing else: no clock, no driver, no
// global. The host suite's FakeKeyValue can fail any single step on demand, so "the device lost
// power in the middle of a reset" is a test rather than a paragraph.
//
// ⛔ WHAT A FACTORY RESET IS. It removes the user and configuration state that makes this device
// belong to the current installation, and returns it to true first-boot behaviour. It is NOT a
// storage format: no partition is erased, no key is enumerated, no wildcard is used. SEVEN named
// keys, plus this module's own journal, deleted individually — that is the whole of it.
//
//     cfg.gen   System    config active-generation marker
//     sec.gen   System    secrets active-generation marker
//     pair.rec  System    pair-activation recovery record (Milestone 4)
//     sec.0     Secrets   SecretConfigV1 — wifi ssid/password, printer api key/user/password
//     sec.1     Secrets   the other slot
//     cfg.0     Config    ConfigV1 — wifi profile id, printer host/port/alias, brightness,
//     cfg.1     Config    dim and sleep delays, update channel
//
// ⬅️ WHY pair.rec IS IN THE INVENTORY EVEN THOUGH IT HOLDS NO CREDENTIAL — owner-approved amendment
// to ADR-0008 decision 8, 2026-08-23. Its payload is five u32 fields — one profile id and four
// generation numbers — so it is not a secret. But it CHANGES WHAT THE NEXT BOOT DOES: candidate::reconcile() reads it before the
// configuration is loaded and will activate a pair from it. Leaving it behind would mean a device
// that had been reset could boot into activation recovery — either re-activating records the reset
// removed, or reporting an unresolved recovery on a store that is meant to be factory-fresh. True
// first-boot behaviour is the guarantee ADR-0008 decision 8 makes, and after Milestone 4 that
// guarantee needs this key. The amendment is a narrow preservation of an existing promise; it
// authorises nothing wider.
//
// ⛔ WHAT SURVIVES, DELIBERATELY. The `runtime` and `diagnostics` namespaces (declared, never
// written — nothing to remove). The otadata partition, so the boot selection and rollback state
// are not disturbed. phy_init's RF calibration. factory_cfg, which is manufacturing identity by
// definition. The coredump partition, which is diagnostic evidence and needs its own explicit
// instruction. Both app images. And every eFuse: ruling R10 leaves no reachable eFuse path and
// this adds none.
//
// ⛔ UNREACHABLE FROM ANY FAILURE PATH. A migration that cannot run, a Wi-Fi that will not join,
// a store that reports a fault and every automatic recovery route reach RecoveryMode and stop
// there. The only caller is app_core's executor, which runs when AppState::factory_resetting
// becomes true — and that happens in exactly one place, behind a second deliberate very-long
// press inside the confirmation window. Owner ruling R10 forbids automatic erasure, and this is
// how "user-consented" is made structural rather than promised.

namespace prd::app_core::factory_reset {

// The journal key, in the `system` namespace beside the activation markers.
//
// ⬅️ WHY A JOURNAL IS NECESSARY AND NOT BELT-AND-BRACES. The sequence clears the activation
// markers first, so that a power loss leaves the device un-provisioned rather than half
// configured. But the resulting store state — records present, no marker — is EXACTLY the state
// an interrupted first provisioning leaves behind, which Milestone 3.8 deliberately treats as an
// orphan candidate and leaves alone. Automatically wiping that state would erase a record no user
// asked to lose, which is the automatic erasure R10 forbids.
//
// So the journal is what distinguishes the two: it is written before anything is removed, and it
// records that a user confirmed a reset. Resumption happens if and only if it is present. Without
// it, either interrupted resets leave a readable password behind, or orphan candidates get erased
// automatically — and both are wrong.
inline constexpr std::string_view kJournalKey = "rst.pend";

// --- What the journal says, and it is FOUR ANSWERS -------------------------
//
// ⛔ THIS REPLACED A BOOLEAN, AND THE BOOLEAN WAS THE DEFECT — audit item 5, 2026-08-28. `pending()`
// mapped NotFound and every read failure alike to false, so an ARMED journal whose read failed once
// was indistinguishable from a device that had never been reset. The measured consequence: the boot
// skipped the resume, loaded a valid configuration, presented the ordinary operational screen and
// permitted every mutation — with the journal still armed and invisible. A later boot, whose read
// succeeded, then erased whatever the user had configured in between.
//
// ⬅️ AND THE PROJECT HAD ALREADY LEARNED THIS ONCE. candidate::MarkerOutcome says it verbatim:
// "Absent AND Unreadable ARE DIFFERENT ANSWERS, AND CONFLATING THEM WAS A DEFECT." That reasoning
// was applied to `pair.rec` and never carried across to `rst.pend` — the one key in this firmware
// that authorises destruction.
enum class Journal : std::uint8_t {
    // Read, and there is no key. NO AUTHORISATION EXISTS, and that is a fact rather than an absence
    // of facts: an ordinary boot may continue.
    Absent,

    // Read, present, and carrying exactly the value this firmware writes. One resume attempt is
    // authorised on this boot.
    Armed,

    // The store would not answer. Authorisation can be neither proved nor disproved, so nothing may
    // be erased AND the boot may not proceed as though nothing were owed.
    Unreadable,

    // Read, present, and NOT an armed value — zero, or anything this firmware did not write.
    // Evidence of something; authority for nothing.
    //
    // ⛔ STRICTER THAN THE RULE IT REPLACES, DELIBERATELY. `pending()` accepted any non-zero value.
    // Only the exact value this firmware wrote proves this firmware armed it, and ruling R10 asks
    // for proof of consent before an erase — not for a plausible-looking byte.
    Invalid,
};

[[nodiscard]] std::string_view log_label(Journal state);

// Reads the journal. Never invents an absence; see Journal.
[[nodiscard]] Journal probe(storage::KeyValue& store);

enum class Outcome : std::uint8_t {
    // Every named key is absent and the journal is cleared. The device will boot as if it had
    // never been configured.
    Complete,

    // The journal write in step 1 did not succeed, so NO DESTRUCTIVE STEP RAN and the store is
    // exactly as this call found it.
    //
    // ⛔ IT DOES NOT MEAN "NO JOURNAL EXISTS" AND IT DOES NOT MEAN "NOTHING IS OWED". Those depend
    // on how the call was entered, and this outcome cannot see that:
    //
    //   • from a newly confirmed runtime reset, no journal was established, so nothing is owed —
    //     but the store has just refused a write and is not trusted for further mutation;
    //   • from a boot resumption, an ALREADY-PROVED armed journal is still in flash and still owed;
    //     the rewrite merely failed to restate it.
    //
    // Both settle into the same terminal fault, and neither may be described as a completed reset.
    JournalWriteFailed,

    // The journal was written and at least one removal did not finish. The journal is deliberately
    // LEFT IN PLACE, so the next boot resumes rather than reporting a reset that did not happen.
    Failed,
};

[[nodiscard]] std::string_view log_label(Outcome outcome);

// Runs a reset, or resumes an interrupted one. Idempotent, and safe to call when there is
// nothing to remove.
//
// ⛔ PRECONDITION: THE CALLER MUST ALREADY HOLD THE PRODUCT STATE EXCLUSIVELY. Every printer and
// provisioning activity that could write storage must be quiesced before this is called — see
// app_core's exclusive_operation.hpp. This function erases; it cannot also police what else is
// running, and a validation that activated a candidate after the erase would leave the device
// carrying storage state a reset was supposed to remove.
//
// THE ORDER IS THE SAFETY PROPERTY:
//
//   1. write the journal            — records the user's consent durably
//   2. clear both activation markers — the configuration becomes INACTIVE here; from this
//                                      moment load() reports Absent and a power loss leaves a
//                                      device that boots un-provisioned, never half-configured
//   3. remove the pair-activation recovery record — nothing may reconstruct an activation from
//                                      it once the pointers it coordinates are gone
//   4. overwrite and remove the credential records — the material itself, not just the pointer
//   5. remove the configuration records
//   6. clear the journal            — last, so an interruption anywhere above resumes
//
// Step 2 before step 4 is the whole argument. Doing it the other way round would leave a window
// in which a marker points at a record that has been erased, which is a half-configured device.
//
// ⛔ AND STEP 3 IS SAFE ONLY BECAUSE OF WHERE RESUMPTION SITS. Between steps 2 and 3 the store holds
// no pointers, both records, and a recovery record — which candidate::reconcile() would roll FORWARD
// into an activated pair. A power loss there is survivable because the journal is already armed and
// app_core resumes this function at boot BEFORE load_configuration() reaches reconciliation, so the
// record is removed before anything can act on it. Moving the resumption after the configuration
// load would break that, which is why it is stated here rather than left to be rediscovered.
//
// ⛔ AND THE ORDERING ALONE IS NOT ENOUGH — audit item 5. It was implemented and the OUTCOME was
// discarded, so a resume that FAILED fell through into the very reconciliation the ordering exists
// to get in front of. resume_if_pending() below is what closes that: the ordering and the outcome
// are now one answer, and the configuration load is not reachable without it.
//
// ⬅️ AND THE KEY ITSELF IS NOT NAMED HERE. candidate::clear_marker() removes it, so the string has
// one owner (candidate.cpp) and this module cannot drift from it.
[[nodiscard]] Outcome run(storage::KeyValue& store);

// --- What the boot does about it -------------------------------------------

// The boot's whole answer, and there are exactly three.
//
// ⛔ EACH ONE CARRIES ITS TERMINAL REASON. An ambiguous "halt" would make the composition root
// re-derive WHY it was halting, and re-derivation by a consumer is the shape checkpoint 3e was
// rejected for the first time round: the routing policy has to travel with the answer, not be
// worked out again at the call site.
enum class BootAction : std::uint8_t {
    // Nothing is owed, or the job that was owed is finished. Load and reconcile the configuration
    // normally — after a completed resume that store is a genuine first-boot store.
    LoadConfiguration,

    // A reset was proved to have been authorised and could not be finished. Do not load, do not
    // reconcile, do not raise setup, do not permit printer or provisioning work.
    HaltFactoryResetIncomplete,

    // The journal could not be read, or held a value this firmware did not write.
    //
    // ⛔ THIS BOOT PERFORMS NO ERASE, and that is provable — the executor is not called. ⛔ IT IS NOT
    // A CLAIM THAT NOTHING WAS EVER ERASED: an earlier boot's reset may have removed records before
    // its journal later became unreadable, and nothing here can see that. Nor may the boot carry on
    // as though nothing were owed, because that is precisely what could not be established.
    HaltStorageFailure,
};

[[nodiscard]] std::string_view log_label(BootAction action);

// ⛔ WHAT THIS BOOT MAY TRUTHFULLY SAY IT DID — and the bound is route-specific.
//
// The first version of this log said "nothing was erased on this path" for BOTH halts, and that is
// false twice over. On HaltFactoryResetIncomplete an authorised resume ran and may have removed
// several records before a later step failed. And on either route it is false as a claim about the
// DEVICE'S HISTORY: an earlier boot's reset may have erased data before its journal later became
// unreadable, and this boot cannot know.
//
// ⬅️ SO EACH NOTE IS BOUNDED TO WHAT THIS BOOT DID, WHICH IS ALL IT CAN OBSERVE. Neither says
// anything about what any earlier boot removed, because neither can. It is host-tested, so the
// bound is a property rather than a review note.
[[nodiscard]] std::string_view halt_note(BootAction action);

// The device fault each action settles into. Total over BootAction, so the mapping is host-tested
// rather than spelled out in the composition root. LoadConfiguration is not a fault and maps to
// DeviceFaultReason::None; the two halts carry the reason each of them settles into.
[[nodiscard]] app_model::DeviceFaultReason fault_for(BootAction action);

// ⛔ THE WHOLE BOOT DECISION, IN ONE CALL THE CALLER CANNOT SKIP OR DISCARD. It performs the single
// authorised resume attempt itself, so there is no `run()` at the boot site to `(void)` away — which
// is exactly what the defect was: the boot called run(), discarded its `[[nodiscard]]` result with a
// cast, and walked into load_configuration() whatever had happened.
//
// ⛔ ONE ATTEMPT PER BOOT, NOT PER LIFETIME. A later boot probes again and may try again. That is
// what makes "restart the display" an instruction a user can act on, and it is why no automatic
// retry and no automatic reboot exist anywhere in this module.
//
// ⛔ THE EXECUTOR IS NOT CALLED AT ALL FOR Unreadable OR Invalid. Not called, rather than called and
// ignored: an erase that cannot prove a user asked for it must not be attempted, and a count of
// erase_key() calls is what the tests assert.
[[nodiscard]] BootAction resume_if_pending(storage::KeyValue& store);

} // namespace prd::app_core::factory_reset
