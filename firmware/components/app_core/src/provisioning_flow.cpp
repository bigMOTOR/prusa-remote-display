// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include "provisioning_flow.hpp"

namespace prd::app_core::provisioning_flow {
namespace {

// Unsigned subtraction, so a monotonic clock that wraps produces a small elapsed time
// rather than an enormous one. The same care the provisioning rate limiter takes.
[[nodiscard]] std::uint32_t elapsed_since(const std::uint32_t start, const std::uint32_t now) {
    return now - start;
}

} // namespace

std::string_view log_label(const Stage stage) {
    switch (stage) {
    case Stage::Wifi:
        return "wifi";

    case Stage::Printer:
        return "printer";
    }

    return "wifi";
}

std::string_view log_label(const Failure failure) {
    switch (failure) {
    case Failure::None:
        return "none";

    case Failure::WifiUnreachable:
        return "wifi-unreachable";

    case Failure::WifiTimedOut:
        return "wifi-timed-out";

    case Failure::WifiDriverStalled:
        return "wifi-driver-stalled";

    case Failure::WifiEvidenceLost:
        return "wifi-evidence-lost";

    case Failure::StorageRejected:
        return "storage-rejected";

    case Failure::StorageCandidateFailed:
        return "storage-candidate-failed";

    case Failure::StorageCommitFailed:
        return "storage-commit-failed";

    case Failure::PrinterUnreachable:
        return "printer-unreachable";

    case Failure::PrinterUnauthorized:
        return "printer-unauthorized";

    case Failure::PrinterUnsupported:
        return "printer-unsupported";

    case Failure::PrinterUnusableResponse:
        return "printer-unusable-response";

    case Failure::PromotionFailed:
        return "promotion-failed";

    case Failure::WifiRestoreFailed:
        return "wifi-restore-failed";

    case Failure::WifiRollbackUnavailable:
        return "wifi-rollback-unavailable";
    }

    return "none";
}

// --- WifiValidation --------------------------------------------------------

void WifiValidation::begin(const std::uint32_t now_ms, const Candidate candidate) {
    verdict_ = Verdict::Pending;
    failure_ = Failure::None;
    started_ms_ = now_ms;
    connected_since_ms_ = 0;
    connected_ = false;
    candidate_ = candidate.tag;
}

void WifiValidation::reset() {
    verdict_ = Verdict::Idle;
    failure_ = Failure::None;
    started_ms_ = 0;
    connected_since_ms_ = 0;
    connected_ = false;
    candidate_ = network::station::kNoTag;
}

Verdict WifiValidation::settle(const Verdict verdict, const Failure failure) {
    verdict_ = verdict;
    failure_ = failure;
    return verdict_;
}

Verdict WifiValidation::observe(const network::station::Tag tag,
                                const network::station::Outcome outcome,
                                const std::uint32_t now_ms) {
    if (verdict_ != Verdict::Pending) {
        return verdict_;
    }

    // ⛔ AN IDENTITY THAT IS NOT THIS CANDIDATE'S DECIDES NOTHING. Only time advances, so a
    // superseded attempt's answer can neither fail this one nor validate it — and, unlike the
    // phase guard this replaced, a candidate that genuinely reaches the same state as its
    // predecessor is not mistaken for it. kNoTag is never a live identity and is refused with the
    // rest. The overall timeout still applies throughout, so a station that reports nothing at all
    // still fails terminally rather than hanging.
    if (tag == network::station::kNoTag || tag != candidate_) {
        return tick(now_ms);
    }

    switch (outcome) {
    case network::station::Outcome::Leased:
        // The dwell starts at the FIRST lease and is not restarted by a repeat, so a station that
        // reports a lease twice does not keep pushing its own deadline out.
        if (!connected_) {
            connected_ = true;
            connected_since_ms_ = now_ms;
        }
        break;

    case network::station::Outcome::LeaseInterrupted:
        // ⬅️ LOSING THE LEASE RESTARTS THE DWELL RATHER THAN FAILING. A link that flaps once is
        // not a configuration the user typed wrongly, and the overall timeout is what stops an
        // endlessly flapping one.
        connected_ = false;
        connected_since_ms_ = 0;
        break;

    case network::station::Outcome::Unreachable:
        // §16.2's "do not continuously retry invalid configuration" has already run inside the
        // station: reaching this means it stopped trying, so waiting out the remaining timeout
        // would tell the user nothing and cost them twenty seconds.
        return settle(Verdict::Failed, Failure::WifiUnreachable);

    case network::station::Outcome::DriverStalled:
        return settle(Verdict::Failed, Failure::WifiDriverStalled);

    case network::station::Outcome::EvidenceLost:
        return settle(Verdict::Failed, Failure::WifiEvidenceLost);
    }

    return tick(now_ms);
}

Verdict WifiValidation::tick(const std::uint32_t now_ms) {
    if (verdict_ != Verdict::Pending) {
        return verdict_;
    }

    if (connected_ && elapsed_since(connected_since_ms_, now_ms) >= policy_.dwell_ms) {
        return settle(Verdict::Validated, Failure::None);
    }

    // Checked AFTER the dwell, so an attempt that completed its dwell on the same
    // millisecond the timeout expired is a success. The alternative punishes a join for
    // being exactly as slow as the budget allows.
    if (elapsed_since(started_ms_, now_ms) >= policy_.timeout_ms) {
        return settle(Verdict::Failed, Failure::WifiTimedOut);
    }

    return verdict_;
}

// --- The records -----------------------------------------------------------

std::uint32_t next_profile_id(const std::uint32_t current) {
    const std::uint32_t next = current + 1;

    // 0 means "no profile", so a wrap must not land there. Reaching this needs four billion
    // provisioning transactions on one device; it costs one comparison to make that
    // harmless rather than a device that silently reads as un-provisioned.
    return next == 0 ? 1U : next;
}

Records compose(const Submission& submission, const config::ConfigV1& base,
                const std::uint32_t profile_id) {
    Records records;

    // Everything the browser did not supply is carried forward: brightness, the dim and
    // sleep delays, the update channel. A provisioning submission configures a network and
    // a printer, and must not quietly reset display preferences the user already has.
    records.configuration = base;

    records.configuration.wifi_profile_id = profile_id;
    records.configuration.printer_host = submission.printer_host;
    records.configuration.printer_port = submission.printer_port;
    records.configuration.auth_method = submission.printer_api_key.empty()
                                            ? app_model::AuthMethod::None
                                            : app_model::AuthMethod::ApiKey;

    // ⛔ ALWAYS false IN MILESTONE 3 (ruling R4). The printer has not been contacted, so it
    // is a candidate. Milestone 4 validates it against the real printer and promotes it.
    records.configuration.printer_validated = false;

    records.secret = secrets::defaults();
    records.secret.profile_id = profile_id;
    records.secret.wifi_ssid = submission.wifi_ssid;
    records.secret.wifi_password = submission.wifi_password;
    records.secret.printer_api_key = submission.printer_api_key;

    return records;
}

// --- The commit ------------------------------------------------------------

namespace {

[[nodiscard]] Failure failure_for(const config::SaveOutcome outcome) {
    switch (outcome) {
    case config::SaveOutcome::Committed:
        return Failure::None;

    case config::SaveOutcome::Rejected:
        return Failure::StorageRejected;

    case config::SaveOutcome::CandidateFailed:
        return Failure::StorageCandidateFailed;

    case config::SaveOutcome::CommitFailed:
        return Failure::StorageCommitFailed;
    }

    return Failure::StorageRejected;
}

[[nodiscard]] Failure failure_for(const secrets::SaveOutcome outcome) {
    switch (outcome) {
    case secrets::SaveOutcome::Committed:
        return Failure::None;

    case secrets::SaveOutcome::Rejected:
        return Failure::StorageRejected;

    case secrets::SaveOutcome::CandidateFailed:
        return Failure::StorageCandidateFailed;

    case secrets::SaveOutcome::CommitFailed:
        return Failure::StorageCommitFailed;
    }

    return Failure::StorageRejected;
}

} // namespace

StagedPair stage_pair(storage::KeyValue& store, const Records& records) {
    StagedPair report;
    report.profile = records.configuration.wifi_profile_id;

    // ⬅️ WHAT IS ACTIVE NOW, CAPTURED BEFORE ANYTHING IS WRITTEN. This is the restore target the
    // recovery record will name, and it must be read here rather than reconstructed later: after the
    // candidates are staged there is no way to tell an active generation from a staged one by
    // arithmetic, and arithmetic is exactly what the previous design got wrong. kNoGeneration on
    // either side simply means there was no previous pair — a first provisioning, where nothing is
    // at risk and there is nothing to restore.
    const config::LoadReport active_configuration = config::load(store);
    const secrets::LoadReport active_secret = secrets::load(store);

    if (active_configuration.outcome == config::LoadOutcome::Loaded &&
        active_secret.outcome == secrets::LoadOutcome::Loaded &&
        active_configuration.value.wifi_profile_id != 0 &&
        active_configuration.value.wifi_profile_id == active_secret.value.profile_id) {
        report.previous_configuration_generation = active_configuration.generation;
        report.previous_secret_generation = active_secret.generation;
    }

    const config::StageReport configuration = config::stage(store, records.configuration);
    report.configuration = configuration.outcome;
    report.configuration_generation = configuration.generation;

    if (configuration.outcome != config::SaveOutcome::Committed) {
        report.failure = failure_for(configuration.outcome);
        return report;
    }

    report.secret_attempted = true;
    const secrets::StageReport secret = secrets::stage(store, records.secret);
    report.secret = secret.outcome;
    report.secret_generation = secret.generation;

    if (secret.outcome != secrets::SaveOutcome::Committed) {
        report.failure = failure_for(secret.outcome);
        return report;
    }

    // ⛔ NOTHING IS ACTIVE YET. Both candidates are in flash and verified; both pointers still name
    // the previous pair. A device that stops here — power loss, failed probe, a new submission —
    // carries on with the configuration it already had.
    report.staged = true;
    return report;
}

ActivatedPair activate_pair(storage::KeyValue& store, const StagedPair& staged) {
    ActivatedPair report;

    if (!staged.staged || staged.profile == 0) {
        report.failure = Failure::PromotionFailed;
        return report;
    }

    candidate::Recovery recovery;
    recovery.profile = staged.profile;
    recovery.target_configuration = staged.configuration_generation;
    recovery.target_secret = staged.secret_generation;
    recovery.previous_configuration = staged.previous_configuration_generation;
    recovery.previous_secret = staged.previous_secret_generation;

    // ⛔ THE RECOVERY RECORD FIRST, AND WRITTEN AND READ BACK BEFORE EITHER POINTER MOVES. Without it
    // in flash a power loss between the two activations below leaves a mixed pair with nothing to
    // repair it — so a record that cannot be written is a reason not to activate at all.
    if (!candidate::write_marker(store, recovery)) {
        report.failure = Failure::PromotionFailed;
        return report;
    }

    if (config::activate(store, staged.configuration_generation) !=
        config::SaveOutcome::Committed) {
        // Neither pointer moved. The recovery record is retained and the next boot rolls forward if
        // the pair proves whole, or restores the exact previous pair if it does not — either way
        // there is a named destination rather than an inference.
        report.failure = Failure::PromotionFailed;
        return report;
    }

    if (secrets::activate(store, staged.secret_generation) != secrets::SaveOutcome::Committed) {
        // ⬅️ THE MIXED PAIR, AND IT IS RECOVERABLE RATHER THAN SILENT. Configuration moved and the
        // credential did not, so this boot reads as un-provisioned — but the recovery record is in
        // flash and names both destinations, so the next boot finishes or reverts deliberately.
        report.failure = Failure::PromotionFailed;
        return report;
    }

    // ⛔ NOTHING IS CLAIMED UNTIL A REREAD PROVES IT. Two accepted pointer writes are not the same
    // fact as "a reader now sees the new pair", and the previous design reported success on the
    // writes alone.
    if (!candidate::pair_is_active(store, staged.configuration_generation, staged.secret_generation,
                                   staged.profile)) {
        report.failure = Failure::PromotionFailed;
        return report;
    }

    // The record has done its job. A failure to clear it is not fatal: the next boot re-reads it,
    // finds the target pair already active, and clears it then — reconciliation is idempotent.
    report.marker_retained = !candidate::clear_marker(store);

    report.activated = true;
    return report;
}

Failure failure_for_printer(const app_model::ConnectionPhase phase) {
    switch (phase) {
    case app_model::ConnectionPhase::Unauthorized:
        return Failure::PrinterUnauthorized;

    case app_model::ConnectionPhase::UnsupportedApi:
        return Failure::PrinterUnsupported;

    case app_model::ConnectionPhase::InvalidResponse:
        return Failure::PrinterUnusableResponse;

    // Nothing answered, or the attempt never got far enough to learn anything more specific.
    //
    // ⬅️ Connected IS IN THIS GROUP DELIBERATELY. A validation attempt that ended Connected did
    // not fail, so asking this function about it is already a caller error — but the answer must
    // still be a failure, because a caller asking why validation failed cannot be handed "no
    // failure" as the reason. That would settle the transaction as a success with no printer
    // behind it, which is the exact outcome §14.3 step 9 exists to prevent.
    case app_model::ConnectionPhase::Connected:
    case app_model::ConnectionPhase::PrinterUnavailable:
    case app_model::ConnectionPhase::Unconfigured:
    case app_model::ConnectionPhase::WaitingForNetwork:
    case app_model::ConnectionPhase::Resolving:
    case app_model::ConnectionPhase::Probing:
    case app_model::ConnectionPhase::Authenticating:
    case app_model::ConnectionPhase::Degraded:
        return Failure::PrinterUnreachable;
    }

    return Failure::PrinterUnreachable;
}

app_model::ProvisioningPhase phase_for(const Verdict verdict) {
    switch (verdict) {
    case Verdict::Idle:
        return app_model::ProvisioningPhase::SetupActive;

    case Verdict::Pending:
        return app_model::ProvisioningPhase::ValidatingConfiguration;

    case Verdict::Validated:
        return app_model::ProvisioningPhase::Inactive;

    case Verdict::Failed:
        return app_model::ProvisioningPhase::SetupFailed;
    }

    return app_model::ProvisioningPhase::SetupActive;
}

} // namespace prd::app_core::provisioning_flow
