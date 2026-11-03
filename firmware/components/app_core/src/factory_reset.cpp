// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include "factory_reset.hpp"

#include "candidate.hpp"
#include "config/config.hpp"
#include "secrets/secrets.hpp"

namespace prd::app_core::factory_reset {
namespace {

// THE ARMED VALUE, AND probe() REQUIRES EXACTLY IT.
//
// ⬅️ IT USED TO BE READ FOR PRESENCE ONLY — any non-zero value counted. Audit item 5 tightened that:
// a value this firmware did not write is not proof this firmware armed a reset, and R10 asks for
// proof of consent before an erase. Anything else that reads back is Journal::Invalid.
constexpr std::uint32_t kJournalArmed = 1;

// True when a step's result means the postcondition holds. An absent key already satisfies
// "gone", which is what makes every step idempotent and resumption safe.
[[nodiscard]] bool removed(const storage::Result result) {
    return result == storage::Result::Ok || result == storage::Result::NotFound;
}

} // namespace

std::string_view log_label(const Journal state) {
    switch (state) {
    case Journal::Absent:
        return "absent";

    case Journal::Armed:
        return "armed";

    case Journal::Unreadable:
        return "unreadable";

    case Journal::Invalid:
        return "invalid";
    }

    return "unreadable";
}

std::string_view log_label(const Outcome outcome) {
    switch (outcome) {
    case Outcome::Complete:
        return "complete";

    case Outcome::JournalWriteFailed:
        return "journal-write-failed";

    case Outcome::Failed:
        return "failed";
    }

    return "failed";
}

std::string_view log_label(const BootAction action) {
    switch (action) {
    case BootAction::LoadConfiguration:
        return "load-configuration";

    case BootAction::HaltFactoryResetIncomplete:
        return "halt-factory-reset-incomplete";

    case BootAction::HaltStorageFailure:
        return "halt-storage-failure";
    }

    return "halt-storage-failure";
}

std::string_view halt_note(const BootAction action) {
    switch (action) {
    case BootAction::LoadConfiguration:
        // Not a halt; the boot proceeds and has nothing to explain.
        return "";

    case BootAction::HaltFactoryResetIncomplete:
        // ⛔ IT DOES NOT SAY WHAT WAS OR WAS NOT REMOVED. The authorised resume ran, and how far it
        // got before failing is exactly what is not established here.
        return "the authorised resume ran and did not finish; this boot will not load or reconcile "
               "configuration and will not start setup or printer work";

    case BootAction::HaltStorageFailure:
        // ⛔ BOUNDED TO THIS BOOT. "No erase was attempted" is provable — the executor was not
        // called. Whether an EARLIER boot erased anything is unknown and is not claimed.
        return "this boot attempted no erase, because the journal's authority could not be "
               "established; it will not load or reconcile configuration and will not start setup "
               "or printer work";
    }

    return "";
}

app_model::DeviceFaultReason fault_for(const BootAction action) {
    switch (action) {
    case BootAction::LoadConfiguration:
        return app_model::DeviceFaultReason::None;

    case BootAction::HaltFactoryResetIncomplete:
        return app_model::DeviceFaultReason::FactoryResetIncomplete;

    case BootAction::HaltStorageFailure:
        return app_model::DeviceFaultReason::StorageFailure;
    }

    return app_model::DeviceFaultReason::StorageFailure;
}

Journal probe(storage::KeyValue& store) {
    std::uint32_t value = 0;

    // ⛔ EXHAUSTIVE OVER storage::Result AND WITH NO PERMISSIVE DEFAULT. Every outcome the interface
    // admits is answered here, so a new Result cannot arrive and be silently swept into "absent" —
    // which is the shape of the defect this replaces.
    switch (store.read_u32(storage::Namespace::System, kJournalKey, value)) {
    case storage::Result::NotFound:
        // A genuine absence, and the ONLY one. This is the answer for a device that was never
        // reset, and for a `system` namespace that has never been written at all — the real adapter
        // reports both as NotFound.
        return Journal::Absent;

    case storage::Result::Ok:
        // ⬅️ EXACT EQUALITY, NOT "NON-ZERO". Only the value this firmware writes proves this
        // firmware armed it. Zero and every foreign value are evidence without authority.
        return value == kJournalArmed ? Journal::Armed : Journal::Invalid;

    case storage::Result::TooLarge:
    case storage::Result::InvalidKey:
    case storage::Result::Failed:
        // ⛔ NOT AN ABSENCE, AND NOT AN AUTHORISATION EITHER. Reporting absence here let a failing
        // store hide an armed journal; reporting armed would let a failing store trigger an erase
        // nobody asked for. Both are wrong, and the tests assert both directions.
        //
        // ⬅️ THE REAL ADAPTER FOLDS MORE INTO Failed THAN A READER MIGHT EXPECT. nvs_key_value.cpp's
        // classify_read() maps ESP_ERR_NVS_TYPE_MISMATCH — a journal key that is PRESENT but is not
        // a u32 — into Failed through its default branch, as it does an nvs_open failure. So "the
        // key is there but unreadable" arrives here rather than at Ok.
        return Journal::Unreadable;
    }

    return Journal::Unreadable;
}

Outcome run(storage::KeyValue& store) {
    // Step 1. Before anything is removed, so an interruption after this point is resumable and
    // an interruption before it leaves the device exactly as it was.
    //
    // ⛔ A JOURNAL THAT CANNOT BE WRITTEN ABORTS THE RESET. Proceeding without it would mean an
    // interruption could leave credential material in flash on a device that presents itself as
    // factory-fresh, with nothing able to finish the job. Better to refuse and report.
    if (store.write_u32(storage::Namespace::System, kJournalKey, kJournalArmed) !=
        storage::Result::Ok) {
        // ⬅️ NAMED FOR WHAT FAILED, NOT FOR WHAT THE CALLER SHOULD CONCLUDE. Whether a journal
        // exists and whether one is owed depend on how this was entered; see the enumerator.
        return Outcome::JournalWriteFailed;
    }

    Outcome outcome = Outcome::Complete;

    // Step 2. Activation first — after these two the device is un-provisioned, and everything
    // that follows is inert cleanup rather than a window of half-configuration.
    if (config::deactivate(store) != config::ClearOutcome::Cleared) {
        outcome = Outcome::Failed;
    }

    if (secrets::deactivate(store) != secrets::ClearOutcome::Cleared) {
        outcome = Outcome::Failed;
    }

    // Step 3. The pair-activation recovery record.
    //
    // ⛔ AFTER THE POINTERS AND BEFORE THE RECORDS, AND BOTH HALVES OF THAT MATTER. After, because
    // until the pointers are gone the store is still a configured device and removing the recovery
    // record first would discard the only means of repairing an activation this reset had not yet
    // reached. Before, because between here and the record removals below a surviving recovery
    // record still names two provable generations — which candidate::reconcile() would roll forward
    // into an activated pair. A power loss in that window resumes here at boot, ahead of
    // reconciliation; see the sequence note in the header.
    //
    // ⬅️ THROUGH candidate::clear_marker(), SO THE KEY HAS ONE OWNER. Its NotFound-is-success rule is
    // exactly the idempotence every other step here has, so nothing is re-stated.
    if (!candidate::clear_marker(store)) {
        outcome = Outcome::Failed;
    }

    // Step 4. The credential material itself, overwritten before its keys are removed.
    if (secrets::wipe(store) != secrets::ClearOutcome::Cleared) {
        outcome = Outcome::Failed;
    }

    // Step 5. The non-secret configuration records.
    if (config::discard(store) != config::ClearOutcome::Cleared) {
        outcome = Outcome::Failed;
    }

    // Step 6. The journal, last, and ONLY if everything above succeeded.
    //
    // ⬅️ LEAVING IT ON FAILURE IS THE POINT. A reset that half-finished must be resumed on the
    // next boot, and clearing the journal here would be the firmware forgetting that it owes the
    // user a finished job.
    if (outcome != Outcome::Complete) {
        return outcome;
    }

    if (!removed(store.erase_key(storage::Namespace::System, kJournalKey))) {
        return Outcome::Failed;
    }

    return Outcome::Complete;
}

BootAction resume_if_pending(storage::KeyValue& store) {
    // ⛔ EXHAUSTIVE, NO DEFAULT. A fifth journal state cannot be added without deciding what the
    // boot does about it.
    switch (probe(store)) {
    case Journal::Absent:
        // Nothing was authorised, so nothing runs. The ordinary boot.
        return BootAction::LoadConfiguration;

    case Journal::Unreadable:
    case Journal::Invalid:
        // ⛔ run() IS NOT CALLED. Not called and ignored — not called. An erase that cannot prove a
        // user asked for it must never be attempted, and the store may hold anything, so the boot
        // does not read the configuration either.
        return BootAction::HaltStorageFailure;

    case Journal::Armed:
        break;
    }

    // THE ONE AUTHORISED ATTEMPT. Both failure outcomes settle the same way: a reset was proved to
    // have been authorised and this boot could not finish it, so the configuration is not read and
    // the device says so and waits for a restart.
    return run(store) == Outcome::Complete ? BootAction::LoadConfiguration
                                           : BootAction::HaltFactoryResetIncomplete;
}

} // namespace prd::app_core::factory_reset
