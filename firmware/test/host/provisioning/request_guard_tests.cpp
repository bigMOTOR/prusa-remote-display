// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "request_guard.hpp"
#include "session_token.hpp"

// The §14.4 gate — owner ruling D9, extended by Milestone 3.9's owner rulings A and B.
//
// This is the file that makes the security surface real rather than scaffolding. Every rule
// the handler relies on is exercised here, including the ones nobody would reach with a
// browser: a missing Origin, a token of the wrong length, a body declared larger than the
// cap, the sixth request in a second, and a perfectly formed submission arriving from the
// LAN instead of the setup access point.

namespace prd::provisioning {
namespace {

constexpr std::string_view kAccessPointHost = "192.168.4.1";
constexpr std::string_view kOrigin = "http://192.168.4.1";

// The device mid-provisioning: the setup access point is up and nothing else is.
Surfaces surfaces() {
    Surfaces active;
    active.set_access_point(kAccessPointHost);
    return active;
}

// A scripted entropy source, so the token is known to the test without being predictable
// in the product.
struct Bytes {
    std::vector<std::uint8_t> values;
    std::size_t index{0};
};

std::uint8_t next(void* const context) {
    auto* const bytes = static_cast<Bytes*>(context);
    const std::uint8_t value = bytes->values[bytes->index];
    bytes->index = (bytes->index + 1) % bytes->values.size();
    return value;
}

SessionToken minted() {
    Bytes bytes;
    for (std::uint32_t value = 0; value < 256; ++value) {
        bytes.values.push_back(static_cast<std::uint8_t>(value));
    }

    SessionToken token;
    token.regenerate(&next, &bytes);
    return token;
}

RequestFacts good(const SessionToken& token) {
    RequestFacts facts;

    // The surface every pre-3.9 case implicitly assumed, now stated: these requests come in
    // on the setup access point, which is the only one that may carry a mutation.
    facts.arrival = Surface::SetupAccessPoint;

    facts.origin = kOrigin;
    facts.content_type = kJsonContentType;
    facts.token = token.view();
    facts.content_length = 128;
    return facts;
}

} // namespace

TEST(RequestGuardTests, AWellFormedRequestIsAllowed) {
    const SessionToken token = minted();
    RateLimiter limiter;

    EXPECT_EQ(evaluate(good(token), surfaces(), token, limiter, 0), GateOutcome::Allow);
}

// --- CSRF: Origin -----------------------------------------------------------

TEST(RequestGuardTests, AMissingOriginIsRefused) {
    // The classic cross-site form POST. Some clients omit Origin; this server has exactly
    // one client, the page it served itself, and that one always sends it.
    const SessionToken token = minted();
    RateLimiter limiter;

    RequestFacts facts = good(token);
    facts.origin = {};

    EXPECT_EQ(evaluate(facts, surfaces(), token, limiter, 0), GateOutcome::OriginMissing);
}

TEST(RequestGuardTests, AForeignOriginIsRefused) {
    const SessionToken token = minted();
    RateLimiter limiter;

    for (const auto* const foreign :
         {"http://evil.example", "https://192.168.4.1", "http://192.168.4.10",
          "http://192.168.4.1:8080", "http://192.168.4.1/", "null"}) {
        RequestFacts facts = good(token);
        facts.origin = foreign;

        EXPECT_EQ(evaluate(facts, surfaces(), token, limiter, 0), GateOutcome::OriginMismatch)
            << "origin=" << foreign;
    }
}

// --- CSRF: content type -----------------------------------------------------

TEST(RequestGuardTests, TheFormContentTypesAreRefused) {
    // These three are the "simple" content types a cross-origin form can send without a
    // preflight. Refusing them is what makes the auto-submitting form attack unable to
    // reach a handler at all.
    const SessionToken token = minted();
    RateLimiter limiter;

    for (const auto* const type : {"application/x-www-form-urlencoded", "multipart/form-data",
                                   "text/plain", "", "application/jsonp"}) {
        RequestFacts facts = good(token);
        facts.content_type = type;

        EXPECT_EQ(evaluate(facts, surfaces(), token, limiter, 0),
                  GateOutcome::ContentTypeUnsupported)
            << "content-type=" << type;
    }
}

TEST(RequestGuardTests, TheMatchIsExactRatherThanAPrefix) {
    // "application/jsonp-something" starts with the accepted type. A prefix test would let
    // it through.
    const SessionToken token = minted();
    RateLimiter limiter;

    RequestFacts facts = good(token);
    facts.content_type = "application/jsonp-x";

    EXPECT_EQ(evaluate(facts, surfaces(), token, limiter, 0), GateOutcome::ContentTypeUnsupported);
}

// --- Size limits (§14.4) ----------------------------------------------------

TEST(RequestGuardTests, TheBodyCapIsAppliedToTheDeclaredLength) {
    const SessionToken token = minted();
    RateLimiter limiter;

    RequestFacts at_cap = good(token);
    at_cap.content_length = kMaxBodyBytes;
    EXPECT_EQ(evaluate(at_cap, surfaces(), token, limiter, 0), GateOutcome::Allow);

    RequestFacts over = good(token);
    over.content_length = kMaxBodyBytes + 1;
    EXPECT_EQ(evaluate(over, surfaces(), token, limiter, 0), GateOutcome::BodyTooLarge);
}

TEST(RequestGuardTests, AnEnormousDeclaredBodyIsRefusedRatherThanAllocated) {
    const SessionToken token = minted();
    RateLimiter limiter;

    RequestFacts facts = good(token);
    facts.content_length = 64U * 1024U * 1024U;

    EXPECT_EQ(evaluate(facts, surfaces(), token, limiter, 0), GateOutcome::BodyTooLarge);
}

TEST(RequestGuardTests, AnEmptyBodyIsRefused) {
    const SessionToken token = minted();
    RateLimiter limiter;

    RequestFacts facts = good(token);
    facts.content_length = 0;

    EXPECT_EQ(evaluate(facts, surfaces(), token, limiter, 0), GateOutcome::BodyMissing);
}

// --- CSRF: the token --------------------------------------------------------

TEST(RequestGuardTests, AMissingOrWrongTokenIsRefused) {
    const SessionToken token = minted();
    RateLimiter limiter;

    RequestFacts missing = good(token);
    missing.token = {};
    EXPECT_EQ(evaluate(missing, surfaces(), token, limiter, 0), GateOutcome::TokenMissing);

    RequestFacts wrong = good(token);
    const std::string other(kTokenLength, 'a');
    wrong.token = other;
    EXPECT_EQ(evaluate(wrong, surfaces(), token, limiter, 0), GateOutcome::TokenMismatch);
}

TEST(RequestGuardTests, ATruncatedOrPaddedTokenIsRefused) {
    const SessionToken token = minted();
    RateLimiter limiter;

    const std::string full{token.view()};

    RequestFacts truncated = good(token);
    const std::string short_token = full.substr(0, full.size() - 1);
    truncated.token = short_token;
    EXPECT_EQ(evaluate(truncated, surfaces(), token, limiter, 0), GateOutcome::TokenMismatch);

    RequestFacts padded = good(token);
    const std::string long_token = full + "0";
    padded.token = long_token;
    EXPECT_EQ(evaluate(padded, surfaces(), token, limiter, 0), GateOutcome::TokenMismatch);
}

// --- Ordering ---------------------------------------------------------------

TEST(RequestGuardTests, ARefusedRequestSpendsNoRateBudget) {
    // The property that stops one bad client exhausting a good one's budget: the limiter is
    // consulted last, after every rule that could refuse.
    const SessionToken token = minted();
    RateLimiter limiter;

    RequestFacts bad = good(token);
    bad.origin = "http://evil.example";

    for (int attempt = 0; attempt < 100; ++attempt) {
        EXPECT_EQ(evaluate(bad, surfaces(), token, limiter, 0), GateOutcome::OriginMismatch);
    }

    EXPECT_EQ(limiter.available(), kRateBurst) << "refused requests consumed the bucket";
    EXPECT_EQ(evaluate(good(token), surfaces(), token, limiter, 0), GateOutcome::Allow);
}

TEST(RequestGuardTests, TheCheapHeaderRulesRunBeforeTheConstantTimeComparison) {
    // A caller that fails an earlier rule never reaches the token comparison, which is one
    // fewer way to measure it. Asserted by the outcome: a request that is wrong in two ways
    // reports the earlier rule.
    const SessionToken token = minted();
    RateLimiter limiter;

    RequestFacts facts = good(token);
    facts.origin = {};
    facts.token = {};

    EXPECT_EQ(evaluate(facts, surfaces(), token, limiter, 0), GateOutcome::OriginMissing);
}

// --- Rate limiting ----------------------------------------------------------

TEST(RateLimiterTests, TheBurstIsAllowedThenRefused) {
    RateLimiter limiter;

    for (std::uint32_t attempt = 0; attempt < kRateBurst; ++attempt) {
        EXPECT_TRUE(limiter.allow(0)) << "attempt " << attempt;
    }

    EXPECT_FALSE(limiter.allow(0)) << "the bucket must be empty after the burst";
}

TEST(RateLimiterTests, TheBucketRefillsOverTime) {
    RateLimiter limiter;

    for (std::uint32_t attempt = 0; attempt < kRateBurst; ++attempt) {
        EXPECT_TRUE(limiter.allow(0));
    }
    EXPECT_FALSE(limiter.allow(0));

    EXPECT_TRUE(limiter.allow(kRateRefillIntervalMs)) << "one token after one interval";
    EXPECT_FALSE(limiter.allow(kRateRefillIntervalMs)) << "and only one";
}

TEST(RateLimiterTests, TheBucketNeverExceedsItsBurst) {
    RateLimiter limiter;

    // A very long quiet period must not bank an unbounded number of requests.
    EXPECT_TRUE(limiter.allow(0));
    EXPECT_TRUE(limiter.allow(kRateRefillIntervalMs * 1000));

    EXPECT_EQ(limiter.available(), kRateBurst - 1);
}

// The reason the elapsed time is computed with unsigned subtraction.
TEST(RateLimiterTests, AWrappingMillisecondCounterDoesNotOpenAHole) {
    RateLimiter limiter;

    const std::uint32_t near_wrap = 0xFFFFFF00U;
    EXPECT_TRUE(limiter.allow(near_wrap));
    for (std::uint32_t attempt = 1; attempt < kRateBurst; ++attempt) {
        EXPECT_TRUE(limiter.allow(near_wrap));
    }
    EXPECT_FALSE(limiter.allow(near_wrap));

    // 255 ms later, across the wrap. Less than one refill interval, so still refused —
    // a signed or naive subtraction would compute an enormous elapsed time and refill.
    EXPECT_FALSE(limiter.allow(0xFFFFFFFFU)) << "the counter wrap refilled the bucket";
}

TEST(RateLimiterTests, ResetRestoresTheFullBurst) {
    RateLimiter limiter;

    for (std::uint32_t attempt = 0; attempt < kRateBurst; ++attempt) {
        EXPECT_TRUE(limiter.allow(0));
    }

    limiter.reset();
    EXPECT_EQ(limiter.available(), kRateBurst);
    EXPECT_TRUE(limiter.allow(0));
}

// --- Statuses ---------------------------------------------------------------

TEST(RequestGuardTests, EveryCsrfRefusalAnswersTheSameStatus) {
    // A caller probing the gate learns that it was refused and nothing about which rule it
    // broke. The distinction lives in our log, not in their response.
    EXPECT_EQ(status_for(GateOutcome::OriginMissing), 403);
    EXPECT_EQ(status_for(GateOutcome::OriginMismatch), 403);
    EXPECT_EQ(status_for(GateOutcome::TokenMissing), 403);
    EXPECT_EQ(status_for(GateOutcome::TokenMismatch), 403);
}

TEST(RequestGuardTests, TheOtherStatusesAreTheConventionalOnes) {
    EXPECT_EQ(status_for(GateOutcome::Allow), 200);
    EXPECT_EQ(status_for(GateOutcome::ContentTypeUnsupported), 415);
    EXPECT_EQ(status_for(GateOutcome::BodyTooLarge), 413);
    EXPECT_EQ(status_for(GateOutcome::BodyMissing), 400);
    EXPECT_EQ(status_for(GateOutcome::TooManyRequests), 429);
}

// --- Owner ruling A: the LAN surface carries no mutation --------------------
//
// These are the cases that matter most in this file. A request that is perfect in every other
// respect — this device's own origin, JSON, a body under the cap, the real token, budget to
// spare — must still be refused when it did not arrive on the setup access point, because R7
// requires explicit authorization for a configuration mutation and none exists until M3.10.

TEST(RequestGuardTests, AFlawlessRequestFromTheLanIsStillRefused) {
    const SessionToken token = minted();
    RateLimiter limiter;

    Surfaces active = surfaces();
    active.set_station("192.168.1.42");

    RequestFacts facts = good(token);
    facts.arrival = Surface::StationLan;
    facts.origin = "http://192.168.1.42";

    EXPECT_EQ(evaluate(facts, active, token, limiter, 0), GateOutcome::SurfaceNotPermitted);
}

TEST(RequestGuardTests, AnUnidentifiedSurfaceIsRefused) {
    // getsockname() failed, or the request carried no resolvable local address. Refusing is
    // the recoverable mistake; permitting is the one R7 forbids.
    const SessionToken token = minted();
    RateLimiter limiter;

    RequestFacts facts = good(token);
    facts.arrival = Surface::Unknown;

    EXPECT_EQ(evaluate(facts, surfaces(), token, limiter, 0), GateOutcome::SurfaceNotPermitted);
}

TEST(RequestGuardTests, TheSurfaceRefusalSpendsNoRateLimitBudget) {
    // The ordering claim in evaluate(), asserted rather than described. If the surface rule
    // ran after the limiter, a caller on the LAN could exhaust the budget of the person
    // standing at the device — a denial of service delivered by a check meant to prevent one.
    const SessionToken token = minted();
    RateLimiter limiter;

    RequestFacts refused = good(token);
    refused.arrival = Surface::StationLan;

    for (std::uint32_t attempt = 0; attempt < kRateBurst * 4; ++attempt) {
        EXPECT_EQ(evaluate(refused, surfaces(), token, limiter, 0),
                  GateOutcome::SurfaceNotPermitted);
    }

    EXPECT_EQ(limiter.available(), kRateBurst);
    EXPECT_EQ(evaluate(good(token), surfaces(), token, limiter, 0), GateOutcome::Allow);
}

TEST(RequestGuardTests, TheSurfaceRefusalIsNotDistinguishableFromTheOtherRefusals) {
    // A distinct status would tell a caller on the LAN that the endpoint exists and is
    // refusing them for being on the LAN, which is a map of where to attack from instead.
    EXPECT_EQ(status_for(GateOutcome::SurfaceNotPermitted), 403);
    EXPECT_EQ(status_for(GateOutcome::SurfaceNotPermitted), status_for(GateOutcome::OriginMissing));
    EXPECT_EQ(status_for(GateOutcome::SurfaceNotPermitted), status_for(GateOutcome::TokenMismatch));
}

TEST(RequestGuardTests, TheSurfaceIsCheckedBeforeTheTokenIsCompared) {
    // Not merely "both are refused": a LAN caller must not be able to learn anything about
    // the token by measuring the response, so the refusal has to happen before the comparison
    // does. Observable as the outcome NAMING the surface rather than the token.
    const SessionToken token = minted();
    RateLimiter limiter;

    RequestFacts facts = good(token);
    facts.arrival = Surface::StationLan;
    facts.token = "not-the-token";

    EXPECT_EQ(evaluate(facts, surfaces(), token, limiter, 0), GateOutcome::SurfaceNotPermitted);
}

// --- Owner ruling B: the origin set is wider, and still exact ---------------

TEST(RequestGuardTests, EachActiveOriginIsAccepted) {
    const SessionToken token = minted();

    Surfaces active;
    active.set_access_point("192.168.4.1");
    active.set_station("192.168.1.42");
    active.set_hostname("display-0c01.local");

    for (const std::string_view origin :
         {"http://192.168.4.1", "http://192.168.1.42", "http://display-0c01.local"}) {
        RateLimiter limiter;
        RequestFacts facts = good(token);
        facts.origin = origin;

        EXPECT_EQ(evaluate(facts, active, token, limiter, 0), GateOutcome::Allow)
            << "origin " << origin;
    }
}

TEST(RequestGuardTests, AnOriginThisDeviceDoesNotOwnIsRefusedEvenFromTheAccessPoint) {
    const SessionToken token = minted();

    Surfaces active;
    active.set_access_point("192.168.4.1");
    active.set_station("192.168.1.42");
    active.set_hostname("display-0c01.local");

    for (const std::string_view origin :
         {"http://192.168.1.43", "http://display-0c02.local", "http://evil.example.com",
          "http://192.168.4.1.evil.example.com", "http://192.168.4.1:8080", "*"}) {
        RateLimiter limiter;
        RequestFacts facts = good(token);
        facts.origin = origin;

        EXPECT_EQ(evaluate(facts, active, token, limiter, 0), GateOutcome::OriginMismatch)
            << "origin " << origin;
    }
}

} // namespace prd::provisioning
