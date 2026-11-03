// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include "bring_up.hpp"

#include <cstddef>

namespace prd::storage {
namespace {

// Every reason bring-up can stop short, and all of them leave the partition exactly as it
// was found.
//
// StorageStatus::RecoveredWithDefaults is NEVER PRODUCED, and its absence is the ruling
// rather than an oversight. Its documented meaning is "storage opened but its contents
// were not usable and defaults were applied" — applying defaults means discarding what
// was there, which this component no longer does under any circumstance. The value
// becomes reachable again only through Milestone 3.10's user-consented recovery.
[[nodiscard]] Report failure(const Outcome outcome) {
    Report report;
    report.status = app_model::StorageStatus::Failed;
    report.outcome = outcome;
    return report;
}

} // namespace

std::string_view log_label(const BackendResult result) {
    switch (result) {
    case BackendResult::Ok:
        return "ok";

    case BackendResult::NoFreePages:
        return "no-free-pages";

    case BackendResult::VersionAhead:
        return "version-ahead";

    case BackendResult::Failed:
        return "failed";
    }

    return "failed";
}

Report bring_up(Backend& backend) {
    // NOTHING BELOW ERASES, REFORMATS OR RETRIES. Every failure returns the partition
    // untouched, and the outcome carries which failure it was so Milestone 3.10 can offer
    // the right remedy to a user who has physically consented to it.
    //
    // The two cases that most tempt an automatic reformat are exactly the two that must
    // not have one. NO_FREE_PAGES on a device in service means a partition that HAS
    // content — ESP-IDF's own description is "may happen if NVS partition was truncated",
    // and a blank partition initialises normally — so erasing would silently return a
    // configured device to factory-blank, destroying the user's Wi-Fi password and API
    // key with no notification. VERSION_AHEAD means the contents are intact and belong to
    // a newer firmware, which §13.4 addresses in binding language: "migration failure
    // must not erase secrets automatically".
    switch (backend.initialise()) {
    case BackendResult::Ok:
        break;

    case BackendResult::NoFreePages:
        return failure(Outcome::NoFreePages);

    case BackendResult::VersionAhead:
        return failure(Outcome::VersionAhead);

    case BackendResult::Failed:
        return failure(Outcome::InitialisationFailed);
    }

    // Every namespace, in declaration order, so the log reports the first failure
    // deterministically rather than in whatever order a container iterated.
    for (std::size_t index = 0; index < kNamespaceCount; ++index) {
        const auto space = static_cast<Namespace>(index);

        if (backend.probe(space) != BackendResult::Ok) {
            Report report = failure(Outcome::NamespaceUnusable);
            report.failed_namespace = space;
            return report;
        }
    }

    Report report;
    report.status = app_model::StorageStatus::Ready;
    report.outcome = Outcome::Ready;
    return report;
}

} // namespace prd::storage
