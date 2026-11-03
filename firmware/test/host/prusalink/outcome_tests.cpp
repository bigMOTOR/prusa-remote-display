// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include <array>
#include <cstdint>
#include <string_view>

#include <gtest/gtest.h>

#include "outcome.hpp"
#include "worker_lifecycle.hpp"

// Transport + status + decode → connection phase — Milestone 4's first vertical slice.
//
// ⛔ WHAT THESE PROTECT. §16.1 requires its error categories to "remain distinct in state
// and UI". A user who typed the wrong API key must be told that, not told the printer is
// unreachable — and a printer that is genuinely off must not produce an authentication
// error the user will go hunting for. This mapping is the only place that distinction
// exists, so it is proved exhaustively rather than sampled.

namespace prd::prusalink {
namespace {

using app_model::ConnectionPhase;

constexpr network::HttpOutcome kEveryTransport[] = {
    network::HttpOutcome::Ok,
    network::HttpOutcome::ConnectFailed,
    network::HttpOutcome::Timeout,
    network::HttpOutcome::ResponseTooLarge,
    network::HttpOutcome::TransportFailed,
    network::HttpOutcome::Rejected,
};

constexpr DecodeError kEveryDecode[] = {
    DecodeError::None,         DecodeError::Malformed,     DecodeError::MissingObject,
    DecodeError::MissingField, DecodeError::UnusableField,
};

TEST(OutcomeTests, ASuccessfulExchangeIsTheOnlyUsableResult) {
    const AttemptOutcome ok = classify(network::HttpOutcome::Ok, 200, DecodeError::None);

    EXPECT_EQ(ok.phase, ConnectionPhase::Connected);
    EXPECT_TRUE(ok.usable);
    EXPECT_TRUE(ok.error.empty());

    // Any other 2xx is equally usable: the contract does not promise exactly 200.
    EXPECT_TRUE(classify(network::HttpOutcome::Ok, 204, DecodeError::None).usable);
}

TEST(OutcomeTests, AuthenticationFailuresAreTheirOwnCategory) {
    // §16.1's authentication errors. Not transient, and the one failure the user can fix.
    for (const std::uint16_t status : {std::uint16_t{401}, std::uint16_t{403}}) {
        const AttemptOutcome outcome =
            classify(network::HttpOutcome::Ok, status, DecodeError::None);
        EXPECT_EQ(outcome.phase, ConnectionPhase::Unauthorized) << status;
        EXPECT_FALSE(outcome.usable) << status;
        EXPECT_EQ(outcome.error, "unauthorized") << status;
    }

    // NEGATIVE CONTROL: a neighbouring 4xx must NOT read as an authentication failure.
    // Without this, a mapping that returned Unauthorized for everything in the 400s
    // would pass the two assertions above.
    EXPECT_NE(classify(network::HttpOutcome::Ok, 400, DecodeError::None).phase,
              ConnectionPhase::Unauthorized);
    EXPECT_NE(classify(network::HttpOutcome::Ok, 404, DecodeError::None).phase,
              ConnectionPhase::Unauthorized);
    EXPECT_NE(classify(network::HttpOutcome::Ok, 500, DecodeError::None).phase,
              ConnectionPhase::Unauthorized);
}

TEST(OutcomeTests, CompatibilityFailuresAreSeparateFromUnavailability) {
    // A service answering with no such path is not a PrusaLink this build can use.
    EXPECT_EQ(classify(network::HttpOutcome::Ok, 404, DecodeError::None).phase,
              ConnectionPhase::UnsupportedApi);
    EXPECT_EQ(classify(network::HttpOutcome::Ok, 405, DecodeError::None).phase,
              ConnectionPhase::UnsupportedApi);

    // Redirects are disabled deliberately, so a 3xx means the endpoint moved.
    EXPECT_EQ(classify(network::HttpOutcome::Ok, 302, DecodeError::None).phase,
              ConnectionPhase::UnsupportedApi);

    // §16.2 lists temporary 5xx among the failures to retry: the printer is there and
    // failing, which is unavailability rather than incompatibility.
    EXPECT_EQ(classify(network::HttpOutcome::Ok, 500, DecodeError::None).phase,
              ConnectionPhase::PrinterUnavailable);
    EXPECT_EQ(classify(network::HttpOutcome::Ok, 503, DecodeError::None).phase,
              ConnectionPhase::PrinterUnavailable);
}

TEST(OutcomeTests, TransportFailuresNeverReadAsAuthenticationOrSchemaFaults) {
    // A printer that is off, unplugged or behind a dead router. The status argument is
    // deliberately varied to prove it is ignored when the transport failed: a stale
    // status from a previous attempt must not colour this one.
    for (const std::uint16_t status :
         {std::uint16_t{0}, std::uint16_t{200}, std::uint16_t{401}, std::uint16_t{500}}) {
        for (const network::HttpOutcome transport :
             {network::HttpOutcome::ConnectFailed, network::HttpOutcome::Timeout,
              network::HttpOutcome::TransportFailed, network::HttpOutcome::Rejected}) {
            const AttemptOutcome outcome = classify(transport, status, DecodeError::None);

            EXPECT_EQ(outcome.phase, ConnectionPhase::PrinterUnavailable);
            EXPECT_FALSE(outcome.usable);
            EXPECT_NE(outcome.phase, ConnectionPhase::Unauthorized);
        }
    }

    // The labels stay distinct so diagnostics can tell them apart even though the
    // user-visible phase is the same.
    EXPECT_EQ(classify(network::HttpOutcome::ConnectFailed, 0, DecodeError::None).error,
              "unreachable");
    EXPECT_EQ(classify(network::HttpOutcome::Timeout, 0, DecodeError::None).error, "timeout");
    EXPECT_EQ(classify(network::HttpOutcome::Rejected, 0, DecodeError::None).error,
              "request-rejected");
}

TEST(OutcomeTests, AnOversizedBodyIsAResponseFaultNotAnUnreachablePrinter) {
    // §16.4's bound was hit, which says something about the response rather than about
    // whether the printer can be reached.
    const AttemptOutcome outcome =
        classify(network::HttpOutcome::ResponseTooLarge, 200, DecodeError::None);

    EXPECT_EQ(outcome.phase, ConnectionPhase::InvalidResponse);
    EXPECT_FALSE(outcome.usable);
    EXPECT_EQ(outcome.error, "response-too-large");
}

TEST(OutcomeTests, DecodeFailuresOnAGoodExchangeBecomeInvalidResponse) {
    EXPECT_EQ(classify(network::HttpOutcome::Ok, 200, DecodeError::Malformed).phase,
              ConnectionPhase::InvalidResponse);
    EXPECT_EQ(classify(network::HttpOutcome::Ok, 200, DecodeError::MissingObject).phase,
              ConnectionPhase::InvalidResponse);
    EXPECT_EQ(classify(network::HttpOutcome::Ok, 200, DecodeError::MissingField).phase,
              ConnectionPhase::InvalidResponse);
    EXPECT_EQ(classify(network::HttpOutcome::Ok, 200, DecodeError::UnusableField).phase,
              ConnectionPhase::InvalidResponse);

    // Syntax and schema keep distinct labels — the fault a maintainer chases differs.
    EXPECT_EQ(classify(network::HttpOutcome::Ok, 200, DecodeError::Malformed).error,
              "malformed-body");
    EXPECT_EQ(classify(network::HttpOutcome::Ok, 200, DecodeError::MissingField).error,
              "schema-mismatch");
}

TEST(OutcomeTests, TheDecodeResultIsIgnoredWhenTheStatusAlreadyFailed) {
    // An error page's body is not worth a schema verdict, and reporting one would send a
    // maintainer looking for a decoder bug that does not exist.
    for (const DecodeError decode : kEveryDecode) {
        EXPECT_EQ(classify(network::HttpOutcome::Ok, 401, decode).phase,
                  ConnectionPhase::Unauthorized);
        EXPECT_EQ(classify(network::HttpOutcome::Ok, 404, decode).phase,
                  ConnectionPhase::UnsupportedApi);
    }
}

TEST(OutcomeTests, NoInputCombinationProducesAPhaseThePollerCannotActOn) {
    // Exhaustive over every transport, a representative status per class, and every decode
    // result. Two properties: the function is total, and it never claims a phase that
    // means something other than "an attempt was made" — Unconfigured, WaitingForNetwork,
    // Resolving, Probing and Authenticating are set by the supervisor's own lifecycle,
    // never by an attempt's outcome.
    constexpr std::uint16_t kStatuses[] = {0, 200, 204, 302, 400, 401, 403, 404, 500, 503};

    for (const network::HttpOutcome transport : kEveryTransport) {
        for (const std::uint16_t status : kStatuses) {
            for (const DecodeError decode : kEveryDecode) {
                const AttemptOutcome outcome = classify(transport, status, decode);

                EXPECT_NE(outcome.phase, ConnectionPhase::Unconfigured);
                EXPECT_NE(outcome.phase, ConnectionPhase::WaitingForNetwork);
                EXPECT_NE(outcome.phase, ConnectionPhase::Resolving);
                EXPECT_NE(outcome.phase, ConnectionPhase::Probing);
                EXPECT_NE(outcome.phase, ConnectionPhase::Authenticating);
                EXPECT_NE(outcome.phase, ConnectionPhase::Degraded);

                // A usable result and a non-empty error are mutually exclusive by
                // construction; a caller reads one or the other and must not have to
                // reconcile them.
                EXPECT_EQ(outcome.usable, outcome.error.empty());
            }
        }
    }
}

TEST(OutcomeTests, EveryErrorLabelComesFromTheFixedSet) {
    // ⛔ THE REDACTION PROPERTY. §7.5's lastError reaches AppState and therefore the log
    // and the local interface. Proving the label is always one of a fixed set is what
    // rules out a value from the printer, the configuration or the credential store ever
    // arriving there — redaction by construction rather than by filtering.
    constexpr std::array<std::string_view, 12> kAllowed = {
        "unreachable",
        "timeout",
        "transport",
        "request-rejected",
        "response-too-large",
        "unauthorized",
        "endpoint-unsupported",
        "request-unsupported",
        "printer-error",
        "unexpected-status",
        "malformed-body",
        "schema-mismatch",
    };

    constexpr std::uint16_t kStatuses[] = {0,   100, 200, 204, 299, 300, 302, 400,
                                           401, 403, 404, 405, 418, 500, 503, 599};

    for (const network::HttpOutcome transport : kEveryTransport) {
        for (const std::uint16_t status : kStatuses) {
            for (const DecodeError decode : kEveryDecode) {
                const AttemptOutcome outcome = classify(transport, status, decode);

                if (outcome.error.empty()) {
                    EXPECT_TRUE(outcome.usable);
                    continue;
                }

                bool found = false;

                for (const std::string_view allowed : kAllowed) {
                    if (outcome.error == allowed) {
                        found = true;
                        break;
                    }
                }

                EXPECT_TRUE(found) << "unlisted label: " << outcome.error;
            }
        }
    }
}

TEST(NextDelayTests, RefusedConnectionsAreProbedInfrequently) {
    // §16.2: "Authentication errors and unsupported API errors are not transient network
    // failures and must not use aggressive retries." Hammering a printer that has already
    // refused us is the failure this rule prevents.
    EXPECT_GE(next_delay(ConnectionPhase::Unauthorized).count(), 30000);
    EXPECT_GE(next_delay(ConnectionPhase::UnsupportedApi).count(), 30000);

    // NEGATIVE CONTROL: the ordinary intervals are genuinely shorter, so the assertions
    // above are not passing because every phase waits a minute.
    EXPECT_LT(next_delay(ConnectionPhase::Connected).count(),
              next_delay(ConnectionPhase::Unauthorized).count());
    EXPECT_LT(next_delay(ConnectionPhase::PrinterUnavailable).count(),
              next_delay(ConnectionPhase::UnsupportedApi).count());
}

TEST(NextDelayTests, NoPhasePollsFasterThanTheAcceptedFloor) {
    // §11.1's fastest proposed interval is 2 s and is itself "subject to physical load
    // validation". Nothing here may poll a printer faster than that, and nothing may
    // busy-loop: a zero delay would be a request storm.
    constexpr ConnectionPhase kEveryPhase[] = {
        ConnectionPhase::Unconfigured,       ConnectionPhase::WaitingForNetwork,
        ConnectionPhase::Resolving,          ConnectionPhase::Probing,
        ConnectionPhase::Authenticating,     ConnectionPhase::Connected,
        ConnectionPhase::Degraded,           ConnectionPhase::Unauthorized,
        ConnectionPhase::PrinterUnavailable, ConnectionPhase::UnsupportedApi,
        ConnectionPhase::InvalidResponse,
    };

    for (const ConnectionPhase phase : kEveryPhase) {
        EXPECT_GE(next_delay(phase).count(), 2000);
        EXPECT_LE(next_delay(phase).count(), 60000);
    }
}

// --- Initial status pacing (audit item 7) ----------------------------------
//
// ⛔ THE DEFECT. run_polling() waited next_delay() after EVERY cycle, including the successful
// version probe that opens a run. §11.1 gives idle/ready 5 s, so the first `/api/v1/status` was
// issued roughly five seconds after the printer had already answered `/api/version`. §15.1 does not
// schedule it that way: step 19 probes the version, step 21 fetches info and status, and only step
// 22 "enter[s] normal polling".
//
// ⬅️ WHAT THESE COVER AND WHAT THEY DO NOT. poll_pause() is production code — worker.cpp calls
// exactly this function. What they do NOT cover is the worker's loop: that it alternates version and
// status, and that it re-checks supersession at the loop head before issuing the follow-up.
// worker.cpp includes FreeRTOS and a socket and cannot be compiled here, so that binding is READ,
// and lifecycle::should_exit() below is the seam it re-checks.

namespace {

// Built by assignment rather than by aggregate initialisation, so a future field added between
// `phase` and `usable` cannot silently repoint these tests at the wrong member.
[[nodiscard]] AttemptOutcome outcome_of(const ConnectionPhase phase, const bool usable) {
    AttemptOutcome outcome;
    outcome.phase = phase;
    outcome.usable = usable;
    return outcome;
}

constexpr ConnectionPhase kAllPhases[] = {
    ConnectionPhase::Unconfigured,       ConnectionPhase::WaitingForNetwork,
    ConnectionPhase::Resolving,          ConnectionPhase::Probing,
    ConnectionPhase::Authenticating,     ConnectionPhase::Connected,
    ConnectionPhase::Degraded,           ConnectionPhase::Unauthorized,
    ConnectionPhase::PrinterUnavailable, ConnectionPhase::UnsupportedApi,
    ConnectionPhase::InvalidResponse,
};

} // namespace

TEST(PollPauseTests, A_SUCCESSFUL_VERSION_PROBE_IS_FOLLOWED_BY_STATUS_IMMEDIATELY) {
    const AttemptOutcome connected = outcome_of(ConnectionPhase::Connected, true);

    // ⛔ THE REGRESSION, AS ONE COMPARISON. This is what the cycle used to be handed:
    EXPECT_EQ(next_delay(connected.phase).count(), 5000);

    // And this is what it is handed now, for the version probe only.
    EXPECT_EQ(poll_pause(PollRequest::VersionProbe, connected).count(), 0);
}

TEST(PollPauseTests, THE_FIRST_STATUS_THEN_TAKES_THE_ORDINARY_INTERVAL) {
    // The interval begins after the initial status outcome, which is §15.1's step 22.
    const AttemptOutcome connected = outcome_of(ConnectionPhase::Connected, true);

    EXPECT_EQ(poll_pause(PollRequest::Status, connected).count(), 5000);
    EXPECT_EQ(poll_pause(PollRequest::Status, connected), next_delay(connected.phase));
}

TEST(PollPauseTests, THE_EMITTED_PAUSE_SEQUENCE_IS_VERSION_THEN_STATUS_THEN_WAIT) {
    // ⬅️ THE ORDER, STATED AS THE PACING IT PRODUCES. The loop feeds poll_pause() one request kind
    // per cycle; driving the real function across the sequence a healthy run produces is what shows
    // that only the version→status hand-off is free of a wait. The loop's own alternation is the
    // read-verified binding named above.
    const AttemptOutcome connected = outcome_of(ConnectionPhase::Connected, true);

    constexpr PollRequest kCycles[] = {PollRequest::VersionProbe, PollRequest::Status,
                                       PollRequest::Status, PollRequest::Status};
    constexpr std::int64_t kExpected[] = {0, 5000, 5000, 5000};

    for (std::size_t index = 0; index < std::size(kCycles); ++index) {
        EXPECT_EQ(poll_pause(kCycles[index], connected).count(), kExpected[index])
            << "cycle " << index;
    }
}

TEST(PollPauseTests, A_FAILED_VERSION_PROBE_KEEPS_ITS_EXISTING_BACKOFF) {
    // ⛔ ONLY A SUCCESSFUL PROBE EARNS THE IMMEDIATE FOLLOW-UP. A probe that failed learned nothing
    // about the printer, so §11.3's pacing and §16.2's "must not use aggressive retries" still
    // apply — and an unauthorized probe in particular keeps its infrequent interval.
    for (const ConnectionPhase phase : kAllPhases) {
        const AttemptOutcome failed = outcome_of(phase, false);

        EXPECT_EQ(poll_pause(PollRequest::VersionProbe, failed), next_delay(phase))
            << app_model::log_label(phase);
        EXPECT_GE(poll_pause(PollRequest::VersionProbe, failed).count(), 2000)
            << app_model::log_label(phase);
    }
}

TEST(PollPauseTests, NO_PAUSE_OTHER_THAN_THE_SUCCESSFUL_PROBE_IS_EVER_ZERO) {
    // ⛔ A ZERO ANYWHERE ELSE WOULD BE A REQUEST STORM. Stated over the whole domain rather than as
    // a list of the cases that happen to be exercised above.
    for (const ConnectionPhase phase : kAllPhases) {
        for (const bool usable : {false, true}) {
            const AttemptOutcome outcome = outcome_of(phase, usable);

            EXPECT_GE(poll_pause(PollRequest::Status, outcome).count(), 2000)
                << app_model::log_label(phase);

            if (!usable) {
                EXPECT_GE(poll_pause(PollRequest::VersionProbe, outcome).count(), 2000)
                    << app_model::log_label(phase);
            }
        }
    }
}

TEST(PollPauseTests, RecurringConnectedPollingIsUnchanged) {
    // The steady state must be exactly what it was: §11.1's idle/ready interval, cycle after cycle.
    const AttemptOutcome connected = outcome_of(ConnectionPhase::Connected, true);

    for (int cycle = 0; cycle < 5; ++cycle) {
        EXPECT_EQ(poll_pause(PollRequest::Status, connected).count(), 5000) << "cycle " << cycle;
    }
}

TEST(PollPauseTests,
     SUPERSESSION_BETWEEN_THE_VERSION_RESPONSE_AND_THE_STATUS_REQUEST_ENDS_THE_RUN) {
    // ⬅️ THE CANCELLATION SEAM, ASSERTED WHERE IT LIVES. Removing the wait must not remove a
    // supersession check. wait() is a no-op for a zero pause, so control returns to the loop
    // condition — lifecycle::should_exit() against the current generation.
    constexpr lifecycle::Generation kMine = 7;

    EXPECT_FALSE(lifecycle::should_exit(kMine, kMine))
        << "an un-superseded worker must proceed to its first status";
    EXPECT_TRUE(lifecycle::should_exit(kMine, kMine + 1))
        << "a replaced worker must not issue the follow-up";
    EXPECT_TRUE(lifecycle::should_exit(kMine, lifecycle::kNoLease))
        << "a stopped worker must not issue the follow-up";

    // And the publication side, for a report already in flight.
    EXPECT_FALSE(lifecycle::may_publish(kMine, kMine + 1));
    EXPECT_TRUE(lifecycle::may_publish(kMine, kMine));
}

} // namespace
} // namespace prd::prusalink
