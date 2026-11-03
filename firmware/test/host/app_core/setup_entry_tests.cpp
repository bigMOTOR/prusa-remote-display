// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include "setup_entry.hpp"

#include <algorithm>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "surface.hpp"

// The order a setup session comes into existence in — Milestone 3.11 finding F-B.
//
// ⛔ WHY THIS FILE EXISTS. F-B was a sequencing defect in our own orchestration: the panel was
// handed a session before the address it derives had been published, so a deliberate re-entry
// showed a network and a password with no address, and the access point's own Origin was briefly
// absent from the allow-list. None of that depends on hardware. It was found by pressing a
// physical button, which is the wrong instrument for a deterministic ordering bug — this file is
// the right one.
//
// ⬅️ THE FAKE ADAPTER USES THE REAL Surfaces AND THE REAL URL DERIVATION. provisioning::Surfaces
// and setup_url_for() both compile on the host, so what is asserted below is the production
// membership rule and the production address string, not a restatement of either. Only the driver
// calls — bringing up a radio, binding a socket, drawing on a panel — are stood in for.

namespace {

using prd::app_core::setup_entry::begin;
using prd::app_core::setup_entry::Outcome;
using prd::app_core::setup_entry::Steps;
using prd::provisioning::permits_mutation;
using prd::provisioning::setup_url_for;
using prd::provisioning::Surface;
using prd::provisioning::Surfaces;

constexpr std::string_view kAccessPoint = "192.168.4.1";
constexpr std::string_view kStation = "192.168.1.42";
constexpr std::string_view kHostname = "display-0c01.local";

// Stands in for the composition root: the driver state the real adapters own, plus the real
// reachable set they publish into.
struct Bench {
    std::vector<std::string> calls;

    // Whether the radio actually raised the access point.
    bool access_point_up{false};
    bool access_point_start_succeeds{true};

    // Whether a server is listening. True on entry models the case that hid F-B: on a deliberate
    // re-entry the server is ALREADY running from boot, so provisioning::start() returns at its
    // `g_server != nullptr` guard and seeds nothing.
    bool server_already_running{false};
    bool server_start_succeeds{true};

    // The production reachable set, published into by step 2 and read by step 4.
    Surfaces surfaces;

    // What the panel was handed. Empty address is the F-B symptom.
    bool session_submitted{false};
    bool session_discarded{false};
    std::string submitted_address;
};

bool start_access_point(void* const context) {
    auto& bench = *static_cast<Bench*>(context);
    bench.calls.emplace_back("start_access_point");

    if (!bench.access_point_start_succeeds) {
        return false;
    }

    bench.access_point_up = true;
    return true;
}

// refresh_reachability()'s production behaviour: each slot is present only while genuinely
// active, read from its owner rather than remembered.
void publish_addresses(void* const context) {
    auto& bench = *static_cast<Bench*>(context);
    bench.calls.emplace_back("publish_addresses");

    bench.surfaces.set_access_point(bench.access_point_up ? kAccessPoint : std::string_view{});
    bench.surfaces.set_station(kStation);
    bench.surfaces.set_hostname(kHostname);
}

bool start_server(void* const context) {
    auto& bench = *static_cast<Bench*>(context);
    bench.calls.emplace_back("start_server");

    // The re-entry case, and the reason F-B survived first-time provisioning: an already-running
    // server starts nothing and therefore seeds no address.
    if (bench.server_already_running) {
        return true;
    }

    return bench.server_start_succeeds;
}

// provisioning::url()'s production behaviour, through the production derivation.
void submit_session(void* const context) {
    auto& bench = *static_cast<Bench*>(context);
    bench.calls.emplace_back("submit_session");

    bench.session_submitted = true;
    bench.submitted_address = setup_url_for(bench.surfaces.access_point());
}

void discard_session(void* const context) {
    auto& bench = *static_cast<Bench*>(context);
    bench.calls.emplace_back("discard_session");
    bench.session_discarded = true;
}

constexpr Steps kSteps{&start_access_point, &publish_addresses, &start_server, &submit_session,
                       &discard_session};

// The re-entry shape: a provisioned device, joined, with the local server already up from boot.
Bench reentry_bench() {
    Bench bench;
    bench.server_already_running = true;
    return bench;
}

} // namespace

// --- The ordering itself -----------------------------------------------------

TEST(SetupEntryTests, TheAddressIsPublishedBeforeTheSessionIsSubmitted) {
    // ⛔ THE F-B REGRESSION. Publication must precede submission, because the address the panel is
    // given is derived from what publication wrote.
    Bench bench = reentry_bench();

    EXPECT_EQ(begin(kSteps, &bench), Outcome::Started);

    const auto published = std::find(bench.calls.begin(), bench.calls.end(), "publish_addresses");
    const auto submitted = std::find(bench.calls.begin(), bench.calls.end(), "submit_session");

    ASSERT_NE(published, bench.calls.end());
    ASSERT_NE(submitted, bench.calls.end());
    EXPECT_LT(published, submitted) << "the panel was handed an address before one was published";
}

TEST(SetupEntryTests, TheFourStepsRunInTheOneOrderThatWorks) {
    Bench bench = reentry_bench();

    EXPECT_EQ(begin(kSteps, &bench), Outcome::Started);
    EXPECT_EQ(bench.calls, (std::vector<std::string>{"start_access_point", "publish_addresses",
                                                     "start_server", "submit_session"}));
    EXPECT_FALSE(bench.session_discarded);
}

// --- What must be true once the access point is up ---------------------------

TEST(SetupEntryTests, TheSetupSurfaceContainsTheAccessPointAddress) {
    Bench bench = reentry_bench();
    ASSERT_EQ(begin(kSteps, &bench), Outcome::Started);

    EXPECT_EQ(bench.surfaces.access_point(), kAccessPoint);
    EXPECT_TRUE(bench.surfaces.allows_origin("http://192.168.4.1"));
}

TEST(SetupEntryTests, TheDerivedUrlIsTheSetupAddress) {
    Bench bench = reentry_bench();
    ASSERT_EQ(begin(kSteps, &bench), Outcome::Started);

    const std::string url = setup_url_for(bench.surfaces.access_point());

    EXPECT_FALSE(url.empty());
    EXPECT_EQ(url, "http://192.168.4.1");
}

TEST(SetupEntryTests, TheSessionHandedToThePanelCarriesThatAddress) {
    // The visible half of F-B: this is the row that was missing on the physical panel.
    Bench bench = reentry_bench();
    ASSERT_EQ(begin(kSteps, &bench), Outcome::Started);

    EXPECT_TRUE(bench.session_submitted);
    EXPECT_FALSE(bench.submitted_address.empty()) << "this is exactly the absent Address row";
    EXPECT_EQ(bench.submitted_address, "http://192.168.4.1");
}

TEST(SetupEntryTests, TheSetupSurfaceIsMutationCapableAtThatPoint) {
    // ⛔ THE HALF THAT WAS NOT VISIBLE ON THE PANEL. Until the address is published the access
    // point classifies as StationLan, so a submission from the setup page — the only caller the
    // privileged surface exists for — is refused. The panel row was the symptom; this was the bug.
    Bench bench = reentry_bench();
    ASSERT_EQ(begin(kSteps, &bench), Outcome::Started);

    EXPECT_EQ(bench.surfaces.classify(kAccessPoint), Surface::SetupAccessPoint);
    EXPECT_TRUE(permits_mutation(bench.surfaces.classify(kAccessPoint)));

    // And the LAN half is still read-only, so the fix widened nothing.
    EXPECT_EQ(bench.surfaces.classify(kStation), Surface::StationLan);
    EXPECT_FALSE(permits_mutation(bench.surfaces.classify(kStation)));
}

// --- First-time provisioning, the shape that hid the defect ------------------

TEST(SetupEntryTests, FirstTimeProvisioningReachesTheSameStateByTheSamePath) {
    // No server running yet, so provisioning::start() genuinely starts one. This shape worked
    // before the fix, and it must still work after it — by the same route, not a special case.
    Bench bench;
    bench.server_already_running = false;

    EXPECT_EQ(begin(kSteps, &bench), Outcome::Started);
    EXPECT_EQ(bench.calls, (std::vector<std::string>{"start_access_point", "publish_addresses",
                                                     "start_server", "submit_session"}));
    EXPECT_EQ(bench.submitted_address, "http://192.168.4.1");
}

// --- Failure paths -----------------------------------------------------------

TEST(SetupEntryTests, AnAccessPointThatDidNotStartDiscardsTheSessionAndSubmitsNothing) {
    // The session's password exists nowhere but the panel, so a session that cannot be joined
    // must not be left drawn — and must not outlive the attempt.
    Bench bench = reentry_bench();
    bench.access_point_start_succeeds = false;

    EXPECT_EQ(begin(kSteps, &bench), Outcome::AccessPointUnavailable);
    EXPECT_EQ(bench.calls, (std::vector<std::string>{"start_access_point", "discard_session"}));
    EXPECT_TRUE(bench.session_discarded);
    EXPECT_FALSE(bench.session_submitted);
}

TEST(SetupEntryTests, AFailedServerStillYieldsAJoinableNetworkWithNoAddress) {
    // §14.2's honest degradation, unchanged by the fix: the access point is up and joinable, and
    // the panel shows no address rather than one that would refuse the connection.
    //
    // The address is still non-empty here because publication succeeded — what the real url()
    // withholds on this path is decided by the server's own state, which is provisioning's
    // concern and is covered where that state lives.
    Bench bench;
    bench.server_already_running = false;
    bench.server_start_succeeds = false;

    EXPECT_EQ(begin(kSteps, &bench), Outcome::StartedWithoutServer);
    EXPECT_TRUE(bench.session_submitted) << "the network is joinable, so the session is real";
    EXPECT_EQ(bench.calls.back(), "submit_session");
}

// --- The derivation on its own ------------------------------------------------

TEST(SetupEntryTests, AnUnpublishedAccessPointDerivesNoAddressAtAll) {
    // The state the panel was in during F-B, asserted directly: an empty slot yields an empty
    // address, which is why the row disappeared rather than showing something wrong.
    EXPECT_TRUE(setup_url_for("").empty());
    EXPECT_EQ(setup_url_for(kAccessPoint), "http://192.168.4.1");
}
