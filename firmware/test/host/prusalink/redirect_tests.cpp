// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include <cstdint>

#include <gtest/gtest.h>

#include "outcome.hpp"
#include "redirect_policy.hpp"
#include "request.hpp"

#include "network/lan.hpp"

// Redirect containment — Milestone 4's first vertical slice.
//
// ⛔ THE HAZARD. Every PrusaLink request carries the printer's API key in an `X-Api-Key` header
// (API Research Report §10.2). If anything in the request path followed a 3xx, that header would
// be re-sent to whatever authority the `Location` header named — chosen by whatever answered on
// the printer's address, not by the owner. §13.5 forbids sending credentials outside the LAN and
// §13.6's threat model assumes an unencrypted local network, so a followed redirect is the one
// response that turns a local request into an exfiltration.
//
// ⬅️ FOUR BARRIERS, AND EACH IS CHECKED HERE OR NAMED WITH WHERE IT IS CHECKED.
//   1. `disable_auto_redirect` is derived from the policy under a static assertion in http.cpp, so a
//      build that flips it does not compile. (`max_redirection_count` is NOT a barrier: ESP-IDF
//      reads zero as "use the default of 10" — see the test below.)
//   2. Every 3xx classifies as a bounded, non-usable failure, proved below over the whole range.
//   3. The exchange is driven through esp_http_client_open()/read(), never perform() — the only
//      entry point that implements redirect chasing. That is a code-structure fact recorded in
//      http.cpp; there is no perform() call in the firmware.
//   4. No URL ever comes from a response. The only URL the client is given is one build_url()
//      constructed from the stored host, and build_url() refuses every host shape that could
//      re-point an authority — proved below and in request_tests.cpp.

namespace prd::prusalink {
namespace {

TEST(RedirectTests, ThePolicyForbidsFollowing) {
    EXPECT_FALSE(network::redirect_policy::kFollowRedirects);
}

TEST(RedirectTests, TheRedirectionCountSettingIsNotZeroBecauseZeroMeansTen) {
    // ⛔ THE CORRECTED FACT. An earlier version of this suite asserted that
    // `max_redirection_count == 0` forbade redirects. It does not: esp_http_client.c:522-523
    // replaces a zero with DEFAULT_MAX_REDIRECT, which is 10. So zero is the one value that must
    // NOT be written, and the policy carries a non-zero setting instead.
    //
    // This is not the barrier that contains a redirect — see the two below — but a false claim in a
    // security argument is worse than a missing one, so the value is pinned and explained.
    EXPECT_NE(network::redirect_policy::kMaxRedirectionSetting, 0U);
    EXPECT_LE(network::redirect_policy::kMaxRedirectionSetting, 1U);
}

TEST(RedirectTests, NoStatusWhatsoeverMayBeFollowed) {
    // Total over the whole status range rather than over the codes somebody remembered. A
    // may_follow() that returned true for one obscure code would be as bad as one that returned
    // true for 302.
    for (std::uint32_t status = 0; status <= 599; ++status) {
        EXPECT_FALSE(network::redirect_policy::may_follow(static_cast<std::uint16_t>(status)))
            << status;
    }
}

TEST(RedirectTests, TheRedirectRangeIsIdentifiedExactly) {
    EXPECT_FALSE(network::redirect_policy::is_redirect(299));
    EXPECT_TRUE(network::redirect_policy::is_redirect(300));
    EXPECT_TRUE(network::redirect_policy::is_redirect(301));
    EXPECT_TRUE(network::redirect_policy::is_redirect(302));
    EXPECT_TRUE(network::redirect_policy::is_redirect(307));
    EXPECT_TRUE(network::redirect_policy::is_redirect(308));
    EXPECT_TRUE(network::redirect_policy::is_redirect(399));
    EXPECT_FALSE(network::redirect_policy::is_redirect(400));
}

TEST(RedirectTests, EveryRedirectIsABoundedClassifiedFailure) {
    // ⬅️ THE EFFECTIVE CONTROL. Not merely "we do not follow" but "a redirect is reported as a
    // failure the caller cannot mistake for success": `usable` false, a phase that stops the
    // validation transaction, and a fixed non-secret label. A 3xx that classified as usable would
    // let a redirecting device pass validation and be promoted to active.
    for (std::uint32_t status = 300; status <= 399; ++status) {
        const AttemptOutcome outcome = classify(
            network::HttpOutcome::Ok, static_cast<std::uint16_t>(status), DecodeError::None);

        EXPECT_FALSE(outcome.usable) << status;
        EXPECT_EQ(outcome.phase, app_model::ConnectionPhase::UnsupportedApi) << status;
        EXPECT_FALSE(outcome.error.empty()) << status;

        // It must not read as a reachability problem either: a device that redirects is present
        // and answering, and telling the user the printer is unreachable would send them to
        // check cables instead of the address.
        EXPECT_NE(outcome.phase, app_model::ConnectionPhase::PrinterUnavailable) << status;
    }

    // POSITIVE CONTROL: a 2xx in the same call is usable, so the assertions above discriminate
    // redirects rather than reflecting a classifier that refuses everything.
    EXPECT_TRUE(classify(network::HttpOutcome::Ok, 200, DecodeError::None).usable);
}

TEST(RedirectTests, ARedirectCannotBeRetriedAggressivelyEither) {
    // §16.2: an unsupported-API outcome is not a transient network failure. So even the timing
    // consequence of a redirect is bounded — a device that redirects every request is probed
    // infrequently rather than hammered.
    const AttemptOutcome outcome = classify(network::HttpOutcome::Ok, 302, DecodeError::None);

    EXPECT_GE(next_delay(outcome.phase).count(), 30000);
}

TEST(RedirectTests, NoResponseCanNameTheNextDestination) {
    // ⬅️ THE BARRIER THAT ACTUALLY HOLDS, AS A PROPERTY OF THE SIGNATURE. build_url() takes an
    // approved Ipv4 and no host string, so a `Location` header — even if something read one, and
    // nothing does — has no way to express itself as an authority. The LAN policy decides which
    // addresses are reachable at all, and lan_policy_tests.cpp proves that exhaustively.
    //
    // The second barrier is structural and stated where it belongs: redirect following lives only in
    // esp_http_client_set_redirection(), reached only from perform()'s state machine, and this
    // firmware drives the client through open()/fetch_headers()/read().
    app_model::BoundedString<kMaxUrl> url;
    network::lan_policy::Ipv4 lan{};
    ASSERT_TRUE(network::lan_policy::parse_ipv4("192.168.1.50", lan));

    ASSERT_TRUE(build_url(lan, 80, kStatusPath, url));
    EXPECT_EQ(url.view(), "http://192.168.1.50:80/api/v1/status");

    // A redirect target that resolved off the LAN is refused by policy before any request is made.
    network::lan_policy::Resolution elsewhere;
    elsewhere.outcome = network::lan_policy::ResolveOutcome::Ok;
    ASSERT_TRUE(network::lan_policy::parse_ipv4("93.184.216.34", elsewhere.address));
    EXPECT_EQ(verdict_for(elsewhere), TargetVerdict::NotOnLan);
}

TEST(RedirectTests, TheSchemeIsFixedSoNoResponseCanUpgradeOrDowngradeIt) {
    // A redirect's other lever is the scheme. It is a compile-time constant, so a response
    // cannot move the request to a scheme — or a default port — the owner did not configure.
    EXPECT_EQ(kScheme, "http://");
    EXPECT_EQ(kDefaultPort, 80U);
}

} // namespace
} // namespace prd::prusalink
