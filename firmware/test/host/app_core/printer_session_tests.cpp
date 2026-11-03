// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include <cstdint>

#include <gtest/gtest.h>

#include "printer_session.hpp"
#include "provisioning_flow.hpp"
#include "support/fake_key_value.hpp"

// Validation transaction identity and report ingress — the two defects that survived the previous
// repair pass, and the reason they survived it.
//
// ⛔ WHY THESE TESTS EXIST AT ALL. app_core.cpp is not compiled by this suite and cannot be: it
// includes FreeRTOS. So every decision it made inline was unprovable, and 1100 passing tests
// coexisted with a defect that made provisioning categorically impossible on every device. The
// decisions moved into printer_session.hpp for exactly that reason, and these are the tests that
// would have failed.

namespace prd::app_core::printer_session {
namespace {

using prusalink::Generation;
using prusalink::Purpose;
using prusalink::Report;
using prusalink::Transaction;
using prusalink::Verdict;

// A staged pair, as stage_pair() would report it.
[[nodiscard]] provisioning_flow::StagedPair
staged_pair(const std::uint32_t profile, const std::uint32_t configuration_generation,
            const std::uint32_t secret_generation) {
    provisioning_flow::StagedPair staged;
    staged.staged = true;
    staged.profile = profile;
    staged.configuration_generation = configuration_generation;
    staged.secret_generation = secret_generation;
    staged.secret_attempted = true;
    return staged;
}

[[nodiscard]] Report report_of(const Purpose purpose, const Generation generation,
                               const Transaction transaction) {
    Report report;
    report.purpose = purpose;
    report.generation = generation;
    report.transaction = transaction;
    return report;
}

// A Poll report carrying all three observation kinds, which is what a successful cycle produces.
[[nodiscard]] Report poll_report(const Generation generation, const Transaction transaction) {
    Report report = report_of(Purpose::Poll, generation, transaction);
    report.connection_present = true;
    report.identity_present = true;
    report.status_present = true;
    report.status.printer.generation = generation;
    report.status.printer.sequence = 1;
    return report;
}

// --- The single construction site ------------------------------------------

TEST(ValidationIdentityTests, AFirstSetupTestsCandidateOneAndNotTheAbsentProfileZero) {
    // ⛔ THE EXACT SHAPE OF THE DEFECT, AT ITS SIMPLEST. On a virgin device config::load() reports
    // Absent with wifi_profile_id 0, and next_profile_id(0) stages profile 1. The broken code handed
    // the worker profile 1 and recorded profile 0, so the verdict for candidate 1 was discarded as
    // belonging to another transaction and setup timed out reporting an unreachable printer.
    const Transaction transaction = transaction_for(staged_pair(1, 1, 1), 1);

    EXPECT_TRUE(transaction.valid());
    EXPECT_EQ(transaction.profile, 1U);
    EXPECT_EQ(transaction.attempt, 1U);

    // NEGATIVE CONTROL: the ACTIVE profile — which is what the broken code recorded — must not
    // match. If this ever passes, the two sides have been reconnected to different sources.
    const Session session = armed(Purpose::Validate, 7, transaction);
    EXPECT_EQ(classify(session, report_of(Purpose::Validate, 7, Transaction{0, 1}), false),
              Ingress::TransactionMismatch);
}

TEST(ValidationIdentityTests, AnAtoBReconfigurationTestsTheCandidateAndNotTheRunningPair) {
    // A device already running profile 4 is reconfigured. next_profile_id(4) stages 5, and 5 is the
    // profile under test — the one whose printer has not answered yet.
    const Transaction transaction = transaction_for(staged_pair(5, 9, 9), 1);
    const Session session = armed(Purpose::Validate, 12, transaction);

    EXPECT_EQ(classify(session, report_of(Purpose::Validate, 12, Transaction{5, 1}), false),
              Ingress::Accept);

    // NEGATIVE CONTROL: the pair the device is still RUNNING must not settle the transaction that
    // is testing its replacement.
    EXPECT_EQ(classify(session, report_of(Purpose::Validate, 12, Transaction{4, 1}), false),
              Ingress::TransactionMismatch);
}

TEST(ValidationIdentityTests, THE_RECORDED_TRANSACTION_IS_THE_HANDED_TRANSACTION) {
    // ⬅️ THE PROPERTY, STATED DIRECTLY. There is one value. Whatever is handed to the worker is what
    // the session holds, so no comparison between them can fail for a reason other than a genuinely
    // different worker.
    const provisioning_flow::StagedPair staged = staged_pair(5, 9, 9);

    const Transaction handed = transaction_for(staged, 3);
    const Session session = armed(Purpose::Validate, 12, handed);

    EXPECT_EQ(session.transaction, handed);
    EXPECT_EQ(classify(session, report_of(Purpose::Validate, 12, handed), false), Ingress::Accept);

    // And the construction is a function of its inputs, so two calls cannot disagree.
    EXPECT_EQ(transaction_for(staged, 3), handed);
}

TEST(ValidationIdentityTests, ARetryOfTheSameProfileCannotBeSettledByTheEarlierAttempt) {
    // A retry writes no new profile id, so `attempt` is the only thing separating the two. Attempt 1
    // times out; attempt 2 starts against the same candidate on a new worker.
    const provisioning_flow::StagedPair staged = staged_pair(5, 9, 9);

    const Session second = armed(Purpose::Validate, 13, transaction_for(staged, 2));

    EXPECT_EQ(classify(second, report_of(Purpose::Validate, 13, Transaction{5, 2}), false),
              Ingress::Accept);

    // ⛔ THE FIRST ATTEMPT'S VERDICT, ARRIVING LATE. Rejected on generation first — it came from a
    // different worker — and it would be rejected on attempt even if the generation matched.
    EXPECT_EQ(classify(second, report_of(Purpose::Validate, 12, Transaction{5, 1}), false),
              Ingress::GenerationSuperseded);
    EXPECT_EQ(classify(second, report_of(Purpose::Validate, 13, Transaction{5, 1}), false),
              Ingress::TransactionMismatch);
}

TEST(ValidationIdentityTests, NothingStagedMeansNoIdentityAndNothingToArm) {
    // A staging failure leaves no candidate, so there is nothing to test. An invalid transaction
    // cannot arm a session, which is what stops a stray report from settling a transaction that
    // never began.
    provisioning_flow::StagedPair unstaged;
    unstaged.profile = 5;
    EXPECT_FALSE(unstaged.staged);

    EXPECT_FALSE(transaction_for(unstaged, 1).valid());
    EXPECT_FALSE(armed(Purpose::Validate, 12, transaction_for(unstaged, 1)).armed);

    // A pair reported staged but naming no profile is equally unusable.
    provisioning_flow::StagedPair nameless = staged_pair(0, 1, 1);
    EXPECT_FALSE(transaction_for(nameless, 1).valid());

    // POSITIVE CONTROL.
    EXPECT_TRUE(transaction_for(staged_pair(5, 9, 9), 1).valid());
}

TEST(ValidationIdentityTests, AFailedWorkerStartLeavesNoArmedIdentity) {
    // ⬅️ THERE IS NOTHING TO ROLL BACK, WHICH IS STRONGER THAN ROLLING BACK CORRECTLY. A session is
    // armed only from a generation start() reported, and a refusal reports kNoGeneration — so a
    // failed start cannot leave an identity behind for a stray report to match.
    const Transaction transaction = transaction_for(staged_pair(5, 9, 9), 1);

    const Session refused = armed(Purpose::Validate, prusalink::kNoGeneration, transaction);

    EXPECT_FALSE(refused.armed);
    EXPECT_EQ(classify(refused, report_of(Purpose::Validate, prusalink::kNoGeneration, transaction),
                       false),
              Ingress::NoSession);

    // POSITIVE CONTROL: a real lease does arm.
    EXPECT_TRUE(armed(Purpose::Validate, 1, transaction).armed);
}

TEST(ValidationIdentityTests, ADisarmedValidationAcceptsNothingAndNeverExpires) {
    // The whole attempt is forgotten in one act, so a default Validation must be inert on every
    // axis: no session to match, and no deadline to fire.
    const Validation none;

    EXPECT_FALSE(none.session.armed);
    EXPECT_FALSE(none.staged.staged);
    EXPECT_EQ(none.deadline_us, 0);

    EXPECT_EQ(classify(none.session, report_of(Purpose::Validate, 12, Transaction{5, 1}), false),
              Ingress::NoSession);

    EXPECT_FALSE(expired(none, 0));
    EXPECT_FALSE(expired(none, 999999999));

    // POSITIVE CONTROL: an armed deadline does expire, and exactly at its instant.
    Validation running;
    running.deadline_us = 1000;
    EXPECT_FALSE(expired(running, 999));
    EXPECT_TRUE(expired(running, 1000));
    EXPECT_TRUE(expired(running, 1001));
}

// --- Report ingress --------------------------------------------------------

TEST(ReportIngressTests, ALiveGenerationIsAccepted) {
    const Session session = armed(Purpose::Poll, 4, Transaction{7, 0});

    EXPECT_EQ(classify(session, poll_report(4, Transaction{7, 0}), false), Ingress::Accept);
}

TEST(ReportIngressTests, A_STOP_AFTER_PUBLICATION_BUT_BEFORE_CONSUMPTION_REJECTS_THE_REPORT) {
    // ⛔ THE EXACT INTERLEAVING THE AUDIT NAMED, AND THE ONE THE OLD DESIGN COULD NOT SURVIVE. The
    // worker checked its generation, passed, and posted. Then the application task superseded it.
    // Then the posted events were reduced — with no further check anywhere. Here the supersession
    // disarms the session before the report is consumed, so the report has nothing to match.
    Session session = armed(Purpose::Poll, 4, Transaction{7, 0});
    const Report published = poll_report(4, Transaction{7, 0});

    // ... prusalink::stop() and printer_supervise() run ...
    session = Session{};

    EXPECT_EQ(classify(session, published, false), Ingress::NoSession);
}

TEST(ReportIngressTests, AnOutgoingWorkersFinalReportIsRejectedByItsSuccessor) {
    // The other half of the same hazard: the device stayed eligible, so a replacement worker armed a
    // new session. The outgoing worker's last report carries the OLD lease.
    const Session replacement = armed(Purpose::Poll, 5, Transaction{7, 0});

    EXPECT_EQ(classify(replacement, poll_report(4, Transaction{7, 0}), false),
              Ingress::GenerationSuperseded);

    // POSITIVE CONTROL: the replacement's own report is accepted, so the rejection above is the
    // generation and not a rule that rejects everything.
    EXPECT_EQ(classify(replacement, poll_report(5, Transaction{7, 0}), false), Ingress::Accept);
}

TEST(ReportIngressTests, EVERY_OBSERVATION_IN_A_STALE_REPORT_IS_REJECTED_TOGETHER) {
    // ⛔ WHY THE WHOLE REPORT CROSSES THE BOUNDARY. The old design translated a Poll report into
    // three separate events on the worker task, so a supersession could land between them and admit
    // a subset. One report means one verdict: connection, identity and snapshot are accepted or
    // rejected as a unit, and a rejected report refreshes nothing — including observed_at.
    const Session replacement = armed(Purpose::Poll, 5, Transaction{7, 0});

    const Report stale = poll_report(4, Transaction{7, 0});

    ASSERT_TRUE(stale.connection_present);
    ASSERT_TRUE(stale.identity_present);
    ASSERT_TRUE(stale.status_present);

    // One decision covers all three. There is no path by which any of them is applied.
    EXPECT_NE(classify(replacement, stale, false), Ingress::Accept);
}

TEST(ReportIngressTests, AValidateReportNeverReachesAPollSession) {
    // ⛔ Validate REPORTS NEVER ENTER OPERATIONAL STATE. §14.3 step 9 and §13.3 step 6 both place
    // activation after validation, so a candidate must not colour the printer state, the connection
    // state or the panel while it is still a candidate. Purpose is checked before anything else that
    // could admit it.
    const Session polling = armed(Purpose::Poll, 4, Transaction{7, 0});

    Report validate = report_of(Purpose::Validate, 4, Transaction{7, 0});
    validate.verdict = Verdict::Succeeded;

    EXPECT_EQ(classify(polling, validate, false), Ingress::PurposeMismatch);

    // Even with a matching generation AND transaction, which is the case a weaker check would let
    // through.
    ASSERT_EQ(polling.generation, validate.generation);
    ASSERT_EQ(polling.transaction, validate.transaction);
}

TEST(ReportIngressTests, APollReportNeverSettlesAProvisioningTransaction) {
    // The mirror rule. A polling cycle is not a verdict, and must not promote or fail a candidate.
    const Session validating = armed(Purpose::Validate, 4, Transaction{7, 1});

    EXPECT_EQ(classify(validating, poll_report(4, Transaction{7, 1}), false),
              Ingress::PurposeMismatch);

    // POSITIVE CONTROL: a Validate report on the same session is accepted.
    EXPECT_EQ(classify(validating, report_of(Purpose::Validate, 4, Transaction{7, 1}), false),
              Ingress::Accept);
}

TEST(ReportIngressTests, ADefaultConstructedReportMatchesNothing) {
    // A report that arrived from nowhere — a zeroed struct, a queue read that should not have
    // happened — must not be accepted by an armed session.
    const Session session = armed(Purpose::Validate, 4, Transaction{7, 1});

    EXPECT_NE(classify(session, Report{}, false), Ingress::Accept);

    // And an unarmed session accepts nothing either, whatever the report says.
    EXPECT_EQ(classify(Session{}, report_of(Purpose::Validate, 4, Transaction{7, 1}), false),
              Ingress::NoSession);
}

TEST(ReportIngressTests, AnExclusiveOperationOutranksAPerfectlyMatchingSession) {
    // ⛔ CHECKED BEFORE IDENTITY, AND THAT ORDER IS THE POINT. See exclusive_operation.hpp: a queued
    // validation success matching the armed session exactly must still be refused once a confirmed
    // factory reset owns the product state, because acting on it would write storage the reset has
    // already erased. The behavioural cases live in exclusive_operation_tests.cpp.
    const Session session = armed(Purpose::Validate, 7, Transaction{2, 1});
    const Report matching = report_of(Purpose::Validate, 7, Transaction{2, 1});

    EXPECT_EQ(classify(session, matching, false), Ingress::Accept);
    EXPECT_EQ(classify(session, matching, true), Ingress::ExclusiveOperation);

    // And it outranks every other rejection reason too, so the log names the real cause.
    EXPECT_EQ(classify(Session{}, matching, true), Ingress::ExclusiveOperation);
    EXPECT_EQ(classify(armed(Purpose::Poll, 7, Transaction{2, 1}), matching, true),
              Ingress::ExclusiveOperation);
    EXPECT_EQ(classify(armed(Purpose::Validate, 8, Transaction{2, 1}), matching, true),
              Ingress::ExclusiveOperation);
}

TEST(ReportIngressTests, EveryIngressVerdictHasALogLabel) {
    for (const Ingress verdict :
         {Ingress::Accept, Ingress::ExclusiveOperation, Ingress::NoSession,
          Ingress::PurposeMismatch, Ingress::GenerationSuperseded, Ingress::TransactionMismatch}) {
        EXPECT_FALSE(log_label(verdict).empty());
        EXPECT_NE(log_label(verdict), "unknown");
    }
}

// --- The report channel ----------------------------------------------------

TEST(ReportChannelTests, AllocationFailureAndSaturationAreDistinctAndNeitherIsSilent) {
    // §16.4 asks for a typed report on allocation failure; §10.7 asks for saturation to be counted
    // and to trigger a controlled policy. They are different faults with different remedies, so they
    // are different values rather than one "dropped".
    EXPECT_EQ(publication_of(true, true), Publication::Queued);
    EXPECT_EQ(publication_of(true, false), Publication::Dropped);
    EXPECT_EQ(publication_of(false, false), Publication::NoChannel);

    // A missing channel cannot have accepted anything, so the accepted flag does not override it.
    EXPECT_EQ(publication_of(false, true), Publication::NoChannel);

    EXPECT_FALSE(lost(Publication::Queued));
    EXPECT_TRUE(lost(Publication::Dropped));
    EXPECT_TRUE(lost(Publication::NoChannel));

    for (const Publication publication :
         {Publication::Queued, Publication::NoChannel, Publication::Dropped}) {
        EXPECT_FALSE(log_label(publication).empty());
        EXPECT_NE(log_label(publication), "unknown");
    }
}

} // namespace
} // namespace prd::app_core::printer_session
