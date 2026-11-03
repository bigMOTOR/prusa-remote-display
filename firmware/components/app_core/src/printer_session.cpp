// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include "printer_session.hpp"

namespace prd::app_core::printer_session {

std::string_view log_label(const Ingress verdict) {
    switch (verdict) {
    case Ingress::Accept:
        return "accept";
    case Ingress::ExclusiveOperation:
        return "exclusive-operation";
    case Ingress::NoSession:
        return "no-session";
    case Ingress::PurposeMismatch:
        return "purpose-mismatch";
    case Ingress::GenerationSuperseded:
        return "generation-superseded";
    case Ingress::TransactionMismatch:
        return "transaction-mismatch";
    }

    return "unknown";
}

Session armed(const prusalink::Purpose purpose, const prusalink::Generation generation,
              const prusalink::Transaction transaction) {
    Session session;

    // A worker with no lease and a transaction naming no profile identify nothing, so arming on
    // either would produce a session that matches a default-constructed report.
    if (generation == prusalink::kNoGeneration || !transaction.valid()) {
        return session;
    }

    session.armed = true;
    session.purpose = purpose;
    session.generation = generation;
    session.transaction = transaction;
    return session;
}

Ingress classify(const Session& session, const prusalink::Report& report,
                 const bool exclusive_operation) {
    // ⛔ FIRST, AND AHEAD OF IDENTITY. A report from the correctly armed worker is still refused while
    // an exclusive operation holds the device — that is the whole point of the check.
    if (exclusive_operation) {
        return Ingress::ExclusiveOperation;
    }

    if (!session.armed) {
        return Ingress::NoSession;
    }

    if (session.purpose != report.purpose) {
        return Ingress::PurposeMismatch;
    }

    // Equality, not magnitude. A generation is a lease identifier; ordering it would make a wrapped
    // counter look like a superseded one.
    if (session.generation != report.generation) {
        return Ingress::GenerationSuperseded;
    }

    if (session.transaction != report.transaction) {
        return Ingress::TransactionMismatch;
    }

    return Ingress::Accept;
}

std::string_view log_label(const Publication publication) {
    switch (publication) {
    case Publication::Queued:
        return "queued";
    case Publication::NoChannel:
        return "no-channel";
    case Publication::Dropped:
        return "dropped-queue-full";
    }

    return "unknown";
}

Publication publication_of(const bool channel_present, const bool accepted) {
    if (!channel_present) {
        return Publication::NoChannel;
    }

    return accepted ? Publication::Queued : Publication::Dropped;
}

bool lost(const Publication publication) {
    return publication != Publication::Queued;
}

prusalink::Transaction transaction_for(const provisioning_flow::StagedPair& staged,
                                       const std::uint32_t attempt) {
    prusalink::Transaction transaction;

    // Both halves must have reached flash and been read back. A pair that did not stage has no
    // candidate to test, and an attempt against it would validate the record already active.
    if (!staged.staged || staged.profile == 0) {
        return transaction;
    }

    transaction.profile = staged.profile;
    transaction.attempt = attempt;
    return transaction;
}

bool expired(const Validation& validation, const std::int64_t now_us) {
    return validation.deadline_us != 0 && now_us >= validation.deadline_us;
}

bool operator==(const Target& lhs, const Target& rhs) {
    return lhs.address == rhs.address && lhs.auth_method == rhs.auth_method &&
           lhs.credential_generation == rhs.credential_generation &&
           lhs.configured == rhs.configured;
}

bool operator!=(const Target& lhs, const Target& rhs) {
    return !(lhs == rhs);
}

Target target_of(const app_model::ConfigurationState& configuration) {
    Target target;
    target.address = configuration.printer_address;
    target.auth_method = configuration.auth_method;
    target.credential_generation = configuration.credential_generation;
    target.configured = configuration.has_printer_configuration;
    return target;
}

bool target_superseded(const Polling& polling, const app_model::ConfigurationState& configuration) {
    // ⛔ AN UNARMED SESSION HAS NOTHING TO SUPERSEDE. Answering true here would make
    // printer_supervise() stop and drain on every tick of an idle device.
    if (!polling.session.armed) {
        return false;
    }

    // ⬅️ NOT `printer_alias`. A label the user chose for a printer that has not changed must not
    // stop the worker and discard a live job — the same exclusion apply_configuration() makes.
    return polling.target != target_of(configuration);
}

} // namespace prd::app_core::printer_session
