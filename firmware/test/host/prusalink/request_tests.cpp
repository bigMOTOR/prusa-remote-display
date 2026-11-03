// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include <string>

#include <gtest/gtest.h>

#include "outcome.hpp"
#include "request.hpp"

// Request construction and the credential boundary — the audit's B6.
//
// ⛔ THE CONTROL THAT USED TO BE HERE ASSERTED THE DEFECT. An earlier version of this file contained
// `EXPECT_TRUE(build_url("evil.example.com", 80, kStatusPath, ok))` as a "positive control": it
// asserted that a public DNS name was an acceptable destination for a request carrying the printer's
// API key. §13.5 forbids exactly that. It is replaced below by negative controls over the resolved
// destination, which is the only thing that can answer the question.
//
// ⬅️ AND THE SHAPE OF THE FIX IS VISIBLE IN THE SIGNATURE. build_url() no longer accepts a host
// string at all — it takes an address the policy has already approved. There is no overload through
// which an unapproved authority can even be expressed, which is a stronger guarantee than any test.

namespace prd::prusalink {
namespace {

using network::lan_policy::Ipv4;
using network::lan_policy::Resolution;
using network::lan_policy::ResolveOutcome;

[[nodiscard]] Ipv4 address_of(const char* text) {
    Ipv4 parsed;
    EXPECT_TRUE(network::lan_policy::parse_ipv4(text, parsed)) << text;
    return parsed;
}

[[nodiscard]] Resolution resolved(const char* text) {
    Resolution resolution;
    resolution.outcome = ResolveOutcome::Ok;
    resolution.address = address_of(text);
    return resolution;
}

[[nodiscard]] std::string url_for(const char* address, const std::uint16_t port,
                                  const std::string_view path) {
    app_model::BoundedString<kMaxUrl> out;

    if (!build_url(address_of(address), port, path, out)) {
        return {};
    }

    return std::string{out.view()};
}

// --- The credential boundary ----------------------------------------------

TEST(TargetVerdictTests, APrivateOrLinkLocalDestinationIsAccepted) {
    EXPECT_EQ(verdict_for(resolved("192.168.1.50")), TargetVerdict::Ok);
    EXPECT_EQ(verdict_for(resolved("10.0.0.7")), TargetVerdict::Ok);
    EXPECT_EQ(verdict_for(resolved("172.20.1.1")), TargetVerdict::Ok);
    EXPECT_EQ(verdict_for(resolved("169.254.3.4")), TargetVerdict::Ok);
}

TEST(TargetVerdictTests, ANameRESOLVINGToAPublicAddressIsRefused) {
    // ⛔ THE SCENARIO THE AUDIT NAMED. The host string was syntactically fine; what matters is where
    // it resolved to. This is the case the old syntax-only check waved through, and the credential
    // would have followed.
    for (const char* text : {"8.8.8.8", "93.184.216.34", "203.0.113.10", "100.64.0.1"}) {
        Resolution resolution;
        resolution.outcome = ResolveOutcome::Ok;
        resolution.address = address_of(text);

        EXPECT_EQ(verdict_for(resolution), TargetVerdict::NotOnLan) << text;
    }
}

TEST(TargetVerdictTests, LoopbackUnspecifiedAndMulticastAreRefusedToo) {
    for (const char* text : {"127.0.0.1", "0.0.0.0", "224.0.0.1", "255.255.255.255"}) {
        Resolution resolution;
        resolution.outcome = ResolveOutcome::Ok;
        resolution.address = address_of(text);

        EXPECT_EQ(verdict_for(resolution), TargetVerdict::NotOnLan) << text;
    }
}

TEST(TargetVerdictTests, ResolutionFailuresAreDistinctFromPolicyRefusals) {
    // A user whose printer name is misspelled and a user whose printer is on the internet need
    // different things said to them, so the two must not collapse into one verdict.
    Resolution refused;
    refused.outcome = ResolveOutcome::Refused;
    EXPECT_EQ(verdict_for(refused), TargetVerdict::HostUnusable);

    Resolution missing;
    missing.outcome = ResolveOutcome::NotFound;
    EXPECT_EQ(verdict_for(missing), TargetVerdict::NotResolved);

    Resolution failed;
    failed.outcome = ResolveOutcome::Failed;
    EXPECT_EQ(verdict_for(failed), TargetVerdict::NotResolved);

    // NEGATIVE CONTROL: none of them may read as Ok, whatever address happens to be in the struct.
    // A failed resolution leaves `address` at 0.0.0.0, and if the outcome were ignored that would
    // classify as Unspecified rather than as "we never found out".
    for (const ResolveOutcome outcome :
         {ResolveOutcome::Refused, ResolveOutcome::NotFound, ResolveOutcome::Failed}) {
        Resolution resolution;
        resolution.outcome = outcome;
        resolution.address = address_of("192.168.1.50");

        EXPECT_NE(verdict_for(resolution), TargetVerdict::Ok);
    }
}

TEST(TargetVerdictTests, ARefusedTargetIsClassifiedAsUnusableRatherThanUnreachable) {
    // Nothing was sent, so nothing about the printer was learned. Telling the user the printer is
    // unreachable would send them to check cables for a configuration problem.
    const AttemptOutcome off_lan = classify_target(TargetVerdict::NotOnLan);
    EXPECT_FALSE(off_lan.usable);
    EXPECT_EQ(off_lan.phase, app_model::ConnectionPhase::Unconfigured);
    EXPECT_EQ(off_lan.error, "printer-not-on-lan");

    const AttemptOutcome unusable = classify_target(TargetVerdict::HostUnusable);
    EXPECT_EQ(unusable.phase, app_model::ConnectionPhase::Unconfigured);

    // A name that does not resolve IS a reachability fact, and is retried like one.
    const AttemptOutcome unresolved = classify_target(TargetVerdict::NotResolved);
    EXPECT_EQ(unresolved.phase, app_model::ConnectionPhase::PrinterUnavailable);

    // POSITIVE CONTROL.
    EXPECT_TRUE(classify_target(TargetVerdict::Ok).usable);

    // And a refusal must not be retried in a storm: §16.2's rule covers it.
    EXPECT_GE(next_delay(off_lan.phase).count(), 30000);
}

// --- URL construction ------------------------------------------------------

TEST(RequestTests, BuildsTheDocumentedEndpointsFromAnApprovedAddress) {
    EXPECT_EQ(url_for("192.168.1.50", 80, kStatusPath), "http://192.168.1.50:80/api/v1/status");
    EXPECT_EQ(url_for("10.0.0.7", 80, kVersionPath), "http://10.0.0.7:80/api/version");
    EXPECT_EQ(url_for("192.168.1.50", 8080, kStatusPath), "http://192.168.1.50:8080/api/v1/status");
}

TEST(RequestTests, SubstitutesTheDefaultPortForAnUnsetOne) {
    // config::ConfigV1 stores 0 for "unset", the common case on a device provisioned without one.
    EXPECT_EQ(url_for("192.168.1.50", 0, kStatusPath), "http://192.168.1.50:80/api/v1/status");
}

TEST(RequestTests, NoHostStringCanReachAUrl) {
    // ⬅️ THE REBINDING FIX, AS A PROPERTY RATHER THAN A CHECK. The authority is rendered from four
    // octets that the policy approved, so there is no string — from the configuration, from a
    // response, or from a resolver's second answer — anywhere in the result. The longest possible
    // URL is therefore bounded by the address, not by a 64-character host.
    const std::string longest = url_for("255.255.255.255", 65535, kStatusPath);
    EXPECT_EQ(longest, "http://255.255.255.255:65535/api/v1/status");
    EXPECT_LE(longest.size(), kMaxUrl);
}

TEST(RequestTests, RefusesAPathThatIsNotAbsolute) {
    app_model::BoundedString<kMaxUrl> out;

    EXPECT_FALSE(build_url(address_of("192.168.1.50"), 80, "", out));
    EXPECT_TRUE(out.empty());

    // A relative path would concatenate onto the authority.
    EXPECT_FALSE(build_url(address_of("192.168.1.50"), 80, "api/v1/status", out));
    EXPECT_TRUE(out.empty());

    // POSITIVE CONTROL.
    EXPECT_TRUE(build_url(address_of("192.168.1.50"), 80, kStatusPath, out));
}

TEST(RequestTests, OnlyApiKeyAuthenticationIsAccepted) {
    // §10.3: API key for the first hardware slice, Digest kept in the architecture but not
    // implemented. A configuration recording Digest is refused rather than attempted without
    // credentials, which would present as an authentication failure the user cannot fix.
    EXPECT_TRUE(auth_supported(app_model::AuthMethod::ApiKey));
    EXPECT_FALSE(auth_supported(app_model::AuthMethod::Digest));
    EXPECT_FALSE(auth_supported(app_model::AuthMethod::None));
}

TEST(RequestTests, TheEndpointSurfaceIsExactlyTwoReadOnlyPaths) {
    // ⛔ The scope boundary, pinned. §19 places print control, uploads and deletion outside the MVP,
    // and this checkpoint is narrower still: no /api/v1/job and no /api/v1/info. A later change that
    // adds a path changes this test, which is where the scope decision is recorded.
    EXPECT_EQ(kVersionPath, "/api/version");
    EXPECT_EQ(kStatusPath, "/api/v1/status");

    // Plain HTTP, deliberately: §10.5 and §13.6 document that LAN traffic is unencrypted and that
    // the product is for trusted networks. A TLS scheme would imply a guarantee this build cannot
    // make — and the LAN policy above is what keeps that honest rather than reckless.
    EXPECT_EQ(kScheme, "http://");
    EXPECT_EQ(kApiKeyHeader, "X-Api-Key");
    EXPECT_EQ(kDefaultPort, 80U);
}

} // namespace
} // namespace prd::prusalink
