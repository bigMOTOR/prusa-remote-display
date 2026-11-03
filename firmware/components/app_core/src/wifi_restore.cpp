// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include "wifi_restore.hpp"

namespace prd::app_core::wifi_restore {
namespace {

// Whether a read produced a trustworthy answer about what is stored.
//
// ⛔ Absent IS TRUSTWORTHY AND THE OTHERS ARE NOT, WHICH IS THE WHOLE DISCRIMINATOR. "Nothing has
// ever been written there" is a fact a first-boot device relies on; "the store failed", "the bytes
// do not validate", "this needs a migration this firmware cannot perform" and "the active record
// was damaged so the previous known-good was used instead" are all absences of facts. Treating the
// second group as the first would report a first provisioning on a device whose configuration is
// merely unreadable — and then withdraw its station and call that a success.
//
// ⬅️ LoadedFromPreviousGeneration IS IN THE SECOND GROUP ON PURPOSE. The record it returned is
// usable, but it is not the generation the active pointer names, and stage_pair() requires a plain
// Loaded before it will record a previous pair at all. Accepting it here would give this firmware
// two different answers to "which pair was active", which is precisely the divergence the exact
// generations exist to prevent.
[[nodiscard]] bool trustworthy(const config::LoadOutcome outcome) {
    switch (outcome) {
    case config::LoadOutcome::Absent:
    case config::LoadOutcome::Loaded:
        return true;

    case config::LoadOutcome::LoadedFromPreviousGeneration:
    case config::LoadOutcome::Invalid:
    case config::LoadOutcome::MigrationRequired:
    case config::LoadOutcome::StorageFailure:
        return false;
    }

    return false;
}

// The same rule for the credential namespace. Two enumerations rather than one shared one, because
// config and secrets deliberately know nothing about each other.
[[nodiscard]] bool trustworthy(const secrets::LoadOutcome outcome) {
    switch (outcome) {
    case secrets::LoadOutcome::Absent:
    case secrets::LoadOutcome::Loaded:
        return true;

    case secrets::LoadOutcome::LoadedFromPreviousGeneration:
    case secrets::LoadOutcome::Invalid:
    case secrets::LoadOutcome::MigrationRequired:
    case secrets::LoadOutcome::StorageFailure:
        return false;
    }

    return false;
}

void overwrite(void* const data, const std::size_t size) {
    auto* const bytes = static_cast<volatile unsigned char*>(data);
    for (std::size_t index = 0; index < size; ++index) {
        bytes[index] = 0;
    }
}

} // namespace

std::string_view log_label(const Action action) {
    switch (action) {
    case Action::Restore:
        return "restore";

    case Action::Withdraw:
        return "withdraw";

    case Action::Unavailable:
        return "unavailable";
    }

    return "unavailable";
}

Target target_for(storage::KeyValue& store) {
    Target target;

    const config::LoadReport configuration = config::load(store);
    const secrets::LoadReport secret = secrets::load(store);

    // ⛔ DISTRUST FIRST, BECAUSE A DAMAGED READ OUTRANKS EVERY CONCLUSION DRAWN FROM IT. Testing the
    // whole-pair predicate first would send a device whose configuration failed to read down the
    // "there was never a previous pair" path, which reads as a successful withdrawal.
    if (!trustworthy(configuration.outcome) || !trustworthy(secret.outcome)) {
        target.action = Action::Unavailable;
        return target;
    }

    // stage_pair()'s predicate, unchanged: both halves loaded, a real profile, and both records
    // naming it. A missing half, an orphan credential left by a previous provisioning, or two
    // records that disagree all mean the same thing — there is no pair to go back to.
    const bool whole = configuration.outcome == config::LoadOutcome::Loaded &&
                       secret.outcome == secrets::LoadOutcome::Loaded &&
                       configuration.value.wifi_profile_id != 0 &&
                       configuration.value.wifi_profile_id == secret.value.profile_id;

    // ⬅️ AND THE PAIR MUST NAME A NETWORK. configuration_observation.cpp reads exactly this before
    // it will call a device provisioned, so a pair with no SSID is already presented as
    // un-provisioned; a restore to it would be a join with nothing to join. Only the PRESENCE of
    // the name is read here, never its value.
    if (!whole || !secrets::presence_of(secret.value).wifi_ssid) {
        target.action = Action::Withdraw;
        return target;
    }

    target.action = Action::Restore;
    target.configuration_generation = configuration.generation;
    target.secret_generation = secret.generation;
    target.profile = configuration.value.wifi_profile_id;
    return target;
}

Credential credential_for(storage::KeyValue& store, const Target& target) {
    Credential credential;

    if (target.action != Action::Restore) {
        return credential;
    }

    const config::LoadReport configuration =
        config::load_staged(store, target.configuration_generation);

    if (configuration.outcome != config::LoadOutcome::Loaded ||
        configuration.value.wifi_profile_id != target.profile || target.profile == 0) {
        return credential;
    }

    secrets::LoadReport secret = secrets::load_staged(store, target.secret_generation);

    // ⛔ EVERY EXIT FROM HERE ON WIPES THE LOADED RECORD, INCLUDING THE REFUSALS. It carries the
    // Wi-Fi passphrase, the printer API key and the printer password, and only the first two fields
    // of it are ever copied out. §13.5's "embedded in crash dumps where avoidable" is avoidable
    // here, so it is avoided on the refusal paths as well as on the success one.
    if (secret.outcome == secrets::LoadOutcome::Loaded &&
        secret.value.profile_id == target.profile && secrets::presence_of(secret.value).wifi_ssid) {
        (void)credential.ssid.assign(secret.value.wifi_ssid.view());
        (void)credential.password.assign(secret.value.wifi_password.view());
        credential.loaded = true;
    }

    overwrite(&secret, sizeof(secret));

    if (!credential.loaded) {
        // A partially built credential must not escape: assign() above runs before `loaded` is set
        // only inside the accepting branch, but a future edit that reordered them would leak an
        // SSID into a refusal. Wiping unconditionally makes that unrepresentable.
        wipe(credential);
    }

    return credential;
}

void wipe(Credential& credential) {
    overwrite(&credential, sizeof(credential));
}

bool agrees(const Target& target, const std::uint32_t staged_previous_configuration,
            const std::uint32_t staged_previous_secret) {
    switch (target.action) {
    case Action::Restore:
        return staged_previous_configuration == target.configuration_generation &&
               staged_previous_secret == target.secret_generation;

    case Action::Withdraw:
        // Both sides agreeing that there was no whole previous pair. stage_pair() reports
        // kNoGeneration for exactly that case, which is the same fact target_for() answered.
        return staged_previous_configuration == config::kNoGeneration &&
               staged_previous_secret == secrets::kNoGeneration;

    case Action::Unavailable:
        // Never established, so there is nothing for a capture to match. Returning false is what
        // sends the transaction down the rollback-unavailable route rather than letting an
        // unestablished target look confirmed.
        return false;
    }

    return false;
}

// --- Routing ----------------------------------------------------------------

std::string_view log_label(const Route route) {
    switch (route) {
    case Route::Discard:
        return "discard";

    case Route::CandidateValidation:
        return "candidate";

    case Route::RestoreSupervision:
        return "restore";
    }

    return "discard";
}

Route route_for(const Routing& routing) {
    // Nothing to route. The caller still clears its slot, which is what stops an answer sitting in
    // it until some later transaction picks it up.
    if (!routing.evidence) {
        return Route::Discard;
    }

    // ⛔ A TRANSACTION OUTRANKS A RESTORATION, because a submission is the newer instruction. In
    // production the two never overlap — begin_transaction() abandons the supervisor before it
    // submits — but a router that answered ambiguously in that state would be a router whose
    // behaviour depended on a caller's discipline rather than on its own rule.
    if (routing.transaction_running) {
        return Route::CandidateValidation;
    }

    if (routing.restore_armed) {
        return Route::RestoreSupervision;
    }

    // ⛔ AND THIS IS THE BRANCH WHOSE ABSENCE WAS THE DEFECT. Before the supervisor existed, every
    // record reaching here was dropped — including every record an accepted restoration was waiting
    // for. Now "nobody is listening" is a genuine third answer rather than the default.
    return Route::Discard;
}

// --- The supervisor ---------------------------------------------------------

void Supervisor::arm_restore(const network::station::Tag tag) {
    phase_ = Phase::Restoring;
    tag_ = tag;
    carry_ = provisioning_flow::Failure::None;

    // A restore's own failure has never been published: nothing knows about it yet.
    already_reported_ = false;
}

void Supervisor::arm_withdrawal(const provisioning_flow::Failure carry,
                                const bool already_reported) {
    phase_ = Phase::Withdrawing;

    // ⛔ NO TAG. A withdrawal produces no candidate evidence at all — its disconnect is a retirement,
    // which station_intent deliberately does not report as a verdict — so it is judged by the
    // published phase reaching Disabled and by nothing else.
    tag_ = network::station::kNoTag;

    carry_ = carry;
    already_reported_ = already_reported;
}

Supervisor::Result Supervisor::abandon() {
    if (phase_ == Phase::Idle) {
        return Result::Pending;
    }

    return settle(Result::Abandoned);
}

Supervisor::Result Supervisor::observe(const app_model::NetworkPhase phase,
                                       const app_model::NetworkFault fault, const bool evidence,
                                       const network::station::Tag tag,
                                       const network::station::Outcome outcome) {
    if (phase_ == Phase::Idle) {
        return Result::Pending;
    }

    // ⛔ THE DEVICE FAULT OUTRANKS EVERYTHING HERE, AND IT IS CHECKED FIRST — PM ruling 3. A driver
    // that stopped answering is already the typed WifiUnavailable Device Fault through
    // NetworkFault::DriverUnresponsive, and a restore verdict on top would be a second, weaker
    // statement about the same radio. It is read as a LEVEL because a quarantine is terminal: the
    // station publishes it once and stays there.
    if (fault == app_model::NetworkFault::DriverUnresponsive) {
        return settle(Result::Abandoned);
    }

    if (phase_ == Phase::Withdrawing) {
        // ⛔ ACCEPTANCE IS NOT COMPLETION — PM ruling, 2026-08-27. `withdraw()` returning true means
        // the request was issued or retained; the station has actually come down only when it says
        // Disabled. Read as a level rather than an edge, because a station that was already Disabled
        // when the withdrawal was accepted publishes no change at all and an edge would never arrive.
        if (phase != app_model::NetworkPhase::Disabled) {
            return Result::Pending;
        }

        return settle(carry_ == provisioning_flow::Failure::WifiRollbackUnavailable
                          ? Result::RollbackUnavailable
                          : Result::Withdrawn);
    }

    // Restoring. Only evidence charged to THIS identity decides anything — §8.1's "rejects stale
    // responses using request generation or sequence IDs". kNoTag is never a live identity.
    if (!evidence || tag == network::station::kNoTag || tag != tag_) {
        return Result::Pending;
    }

    switch (outcome) {
    case network::station::Outcome::Leased:
        // ⛔ THE ONLY THING THAT MAKES A RESTORATION SUCCEED. Not the acceptance of the submission,
        // and not an association without a lease: §7.4 carries an address, and a station that cannot
        // reach anything has not been restored.
        return settle(Result::Rejoined);

    case network::station::Outcome::Unreachable:
        // §16.2 has already given up inside the station, so the previous network is not coming back
        // on its own. This is the ONE route to Failure::WifiRestoreFailed.
        return settle(Result::RestoreFailed);

    case network::station::Outcome::DriverStalled:
    case network::station::Outcome::EvidenceLost:
        // PM ruling 3 again, by the other route: the same conditions that raise the Device Fault,
        // reported as evidence rather than as a published fault level.
        return settle(Result::Abandoned);

    case network::station::Outcome::LeaseInterrupted:
        // A link that flaps once is not a verdict. The station's own ladder and give-up bound how
        // long this may continue, which is why nothing here needs a deadline of its own.
        return Result::Pending;
    }

    return Result::Pending;
}

Supervisor::Result Supervisor::settle(const Result result) {
    phase_ = Phase::Idle;
    tag_ = network::station::kNoTag;
    carry_ = provisioning_flow::Failure::None;

    // ⬅️ `already_reported_` IS NOT CLEARED HERE, AND THAT IS DELIBERATE. The caller reads it in the
    // same breath as the result it was just handed; clearing it would make it unreadable at exactly
    // the moment it decides whether to publish. Every arm_*() overwrites it, so it cannot outlive the
    // cleanup it described.
    return result;
}

std::string_view log_label(const Supervisor::Result result) {
    switch (result) {
    case Supervisor::Result::Pending:
        return "pending";

    case Supervisor::Result::Rejoined:
        return "rejoined";

    case Supervisor::Result::Withdrawn:
        return "withdrawn";

    case Supervisor::Result::RestoreFailed:
        return "restore-failed";

    case Supervisor::Result::RollbackUnavailable:
        return "rollback-unavailable";

    case Supervisor::Result::Abandoned:
        return "abandoned";
    }

    return "pending";
}

// --- Publication ------------------------------------------------------------

Report report_for(const Supervisor::Result result, const Circumstances& circumstances) {
    Report report;

    // ⛔ THE QUARANTINE OUTRANKS EVERY OTHER ANSWER — PM ruling 3. Whatever the supervisor concluded,
    // a station the driver has stopped answering for belongs to the typed WifiUnavailable Device
    // Fault and to nothing else. Nothing is recorded and nothing is published.
    //
    // ⬅️ AND IT DOES NOT STRAND THE BROWSER, because the candidate's own settlement has already moved
    // the page off Validating with its own reason before any cleanup was scheduled. Suppressing here
    // withholds a SECOND statement about the same radio; it does not withhold the first.
    if (circumstances.quarantined) {
        return report;
    }

    switch (result) {
    case Supervisor::Result::Pending:
    case Supervisor::Result::Rejoined:
    case Supervisor::Result::Withdrawn:
    case Supervisor::Result::Abandoned:
        // Two successes and one deliberate silence. Rejoined and Withdrawn are the outcomes §13.3
        // step 7 asks for; an abandonment is an instruction that outranked the restoration.
        return report;

    case Supervisor::Result::RestoreFailed:
        report.failure = provisioning_flow::Failure::WifiRestoreFailed;
        break;

    case Supervisor::Result::RollbackUnavailable:
        report.failure = provisioning_flow::Failure::WifiRollbackUnavailable;
        break;
    }

    // ⛔ THE TYPED OUTCOME IS RECORDED EITHER WAY; ONLY DELIVERY IS CONDITIONAL. Two independent
    // reasons not to deliver, and each is a different truth:
    //
    //   • the privileged access point has expired, so there is no browser to receive it and
    //     publishing into a surface that is gone would claim a delivery that cannot happen;
    //   • this exact typed failure has already been delivered once, by the settlement that moved the
    //     page off Validating. Publishing it again would be a second identical statement, and the
    //     terminal's job here is the accounting trail rather than a repeat announcement.
    //
    // `failure` above is set in both cases, which is what keeps the final settlement recorded even
    // when the user-facing failure was published earlier — PM ruling, 2026-08-27.
    report.publication = (circumstances.surface_live && !circumstances.already_reported)
                             ? Publication::SetupFailed
                             : Publication::None;
    return report;
}

} // namespace prd::app_core::wifi_restore
