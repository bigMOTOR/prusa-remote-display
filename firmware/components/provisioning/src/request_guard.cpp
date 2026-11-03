// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include "request_guard.hpp"

#include "session_token.hpp"

namespace prd::provisioning {

std::string_view log_label(const GateOutcome outcome) {
    switch (outcome) {
    case GateOutcome::Allow:
        return "allow";

    case GateOutcome::SurfaceNotPermitted:
        return "surface-not-permitted";

    case GateOutcome::OriginMissing:
        return "origin-missing";

    case GateOutcome::OriginMismatch:
        return "origin-mismatch";

    case GateOutcome::ContentTypeUnsupported:
        return "content-type-unsupported";

    case GateOutcome::BodyTooLarge:
        return "body-too-large";

    case GateOutcome::BodyMissing:
        return "body-missing";

    case GateOutcome::TokenMissing:
        return "token-missing";

    case GateOutcome::TokenMismatch:
        return "token-mismatch";

    case GateOutcome::TooManyRequests:
        return "too-many-requests";
    }

    return "origin-mismatch";
}

int status_for(const GateOutcome outcome) {
    switch (outcome) {
    case GateOutcome::Allow:
        return 200;

    // 403 rather than 400 for every CSRF rule, and deliberately the SAME status for all of
    // them: a caller probing the gate learns that it was refused and nothing about which
    // rule it broke. The distinction is in the log, which is ours, not in the response,
    // which is theirs.
    //
    // ⬅️ THE SURFACE REFUSAL JOINS THEM RATHER THAN GETTING ITS OWN STATUS. A distinct code
    // would tell a caller on the LAN that the endpoint exists and is refusing them
    // specifically for being on the LAN — which is a map of where to attack from. It is
    // refused, and that is all the response says.
    case GateOutcome::SurfaceNotPermitted:
    case GateOutcome::OriginMissing:
    case GateOutcome::OriginMismatch:
    case GateOutcome::TokenMissing:
    case GateOutcome::TokenMismatch:
        return 403;

    case GateOutcome::ContentTypeUnsupported:
        return 415;

    case GateOutcome::BodyTooLarge:
        return 413;

    case GateOutcome::BodyMissing:
        return 400;

    case GateOutcome::TooManyRequests:
        return 429;
    }

    return 403;
}

bool RateLimiter::allow(const std::uint32_t now_ms) {
    if (!started_) {
        started_ = true;
        last_refill_ms_ = now_ms;
    }

    // Unsigned subtraction, so a wrapping millisecond counter yields the true elapsed
    // interval rather than an enormous one. The counter wraps after 49 days; a bucket that
    // refilled itself completely at that moment would be a rate limit with a hole in it.
    const std::uint32_t elapsed = now_ms - last_refill_ms_;
    const std::uint32_t refills = elapsed / kRateRefillIntervalMs;

    if (refills != 0) {
        const std::uint32_t room = kRateBurst - tokens_;
        tokens_ += refills < room ? refills : room;
        last_refill_ms_ += refills * kRateRefillIntervalMs;
    }

    if (tokens_ == 0) {
        return false;
    }

    --tokens_;
    return true;
}

void RateLimiter::reset() {
    tokens_ = kRateBurst;
    last_refill_ms_ = 0;
    started_ = false;
}

GateOutcome evaluate(const RequestFacts& facts, const Surfaces& surfaces, const SessionToken& token,
                     RateLimiter& limiter, const std::uint32_t now_ms) {
    // ⬅️ OWNER RULING A, FIRST. R7 requires explicit authorization for any endpoint that
    // mutates sensitive configuration; until M3.10 builds it, the persistent LAN surface has
    // none to offer and the request is refused before anything else is examined.
    if (!permits_mutation(facts.arrival)) {
        return GateOutcome::SurfaceNotPermitted;
    }

    if (facts.origin.empty()) {
        return GateOutcome::OriginMissing;
    }

    // Exact membership over the origins the device actually owns (owner ruling B) — the
    // widening of D9's single-origin test, and not a relaxation of it.
    if (!surfaces.allows_origin(facts.origin)) {
        return GateOutcome::OriginMismatch;
    }

    // Exact match, not a prefix test. A prefix would accept
    // "application/jsonp-something"; a parameter-tolerant match is not needed because the
    // only client is the page this server itself served.
    if (facts.content_type != kJsonContentType) {
        return GateOutcome::ContentTypeUnsupported;
    }

    if (facts.content_length == 0) {
        return GateOutcome::BodyMissing;
    }

    // §14.4, on the DECLARED length — before a byte is read and before anything is
    // allocated for it.
    if (facts.content_length > kMaxBodyBytes) {
        return GateOutcome::BodyTooLarge;
    }

    if (facts.token.empty()) {
        return GateOutcome::TokenMissing;
    }

    if (!token.matches(facts.token)) {
        return GateOutcome::TokenMismatch;
    }

    // Last, so a refused request does not spend a token from the bucket. A caller that
    // cannot pass the gate should not be able to exhaust the budget of one who can.
    if (!limiter.allow(now_ms)) {
        return GateOutcome::TooManyRequests;
    }

    return GateOutcome::Allow;
}

} // namespace prd::provisioning
