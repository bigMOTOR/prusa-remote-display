// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include <string_view>

#include <gtest/gtest.h>

#include "surface.hpp"

// The device's own surfaces and the origins it will answer for — owner rulings A and B of
// Milestone 3.9.
//
// ⬅️ WHY THIS FILE IS WORTH MORE THAN IT LOOKS. Owner ruling B widened an accepted security
// rule: until Milestone 3.9 the Origin header was compared against one string, and now it is
// compared against a set. That is the kind of change whose failure mode is silent — a widening
// that quietly became a wildcard would still pass every existing test, because every existing
// test sends a legitimate origin. So most of what follows asserts what must NOT be accepted.

namespace prd::provisioning {
namespace {

constexpr std::string_view kAccessPoint = "192.168.4.1";
constexpr std::string_view kStation = "192.168.1.42";
constexpr std::string_view kHostname = "display-0c01.local";

Surfaces all_three() {
    Surfaces active;
    active.set_access_point(kAccessPoint);
    active.set_station(kStation);
    active.set_hostname(kHostname);
    return active;
}

} // namespace

// --- Ruling A: which surface may mutate -------------------------------------

TEST(SurfaceTests, OnlyTheSetupAccessPointPermitsAMutation) {
    EXPECT_TRUE(permits_mutation(Surface::SetupAccessPoint));
    EXPECT_FALSE(permits_mutation(Surface::StationLan));

    // The fail-closed half, and the reason Unknown exists at all: a surface that could not be
    // identified is not the access point, and the two mistakes are not symmetrical.
    EXPECT_FALSE(permits_mutation(Surface::Unknown));
}

TEST(SurfaceTests, TheAccessPointAddressIdentifiesTheSetupSurface) {
    EXPECT_EQ(all_three().classify(kAccessPoint), Surface::SetupAccessPoint);
}

TEST(SurfaceTests, EveryOtherLocalAddressIsTheUsersNetwork) {
    const Surfaces active = all_three();

    EXPECT_EQ(active.classify(kStation), Surface::StationLan);
    EXPECT_EQ(active.classify("10.0.0.7"), Surface::StationLan);

    // Not the access point's address merely because it looks like one. 192.168.4.2 is a
    // CLIENT of the setup network, not the device's own address on it.
    EXPECT_EQ(active.classify("192.168.4.2"), Surface::StationLan);
}

TEST(SurfaceTests, AnUnresolvableLocalAddressIsUnknownRatherThanPrivileged) {
    EXPECT_EQ(all_three().classify(""), Surface::Unknown);
}

TEST(SurfaceTests, WithNoAccessPointNothingClassifiesAsTheSetupSurface) {
    // A boot that is already provisioned: the server is up, there is no access point, and so
    // there is no surface on which a mutation could be authorised. Read-only by construction
    // rather than by a second rule somebody has to remember to write.
    Surfaces active;
    active.set_station(kStation);
    active.set_hostname(kHostname);

    EXPECT_EQ(active.classify(kStation), Surface::StationLan);
    EXPECT_EQ(active.classify(kAccessPoint), Surface::StationLan);
    EXPECT_FALSE(permits_mutation(active.classify(kStation)));
}

// --- Ruling B: exact membership over the active set -------------------------

TEST(SurfaceTests, EachActiveOriginIsAMember) {
    const Surfaces active = all_three();

    EXPECT_TRUE(active.allows_origin("http://192.168.4.1"));
    EXPECT_TRUE(active.allows_origin("http://192.168.1.42"));
    EXPECT_TRUE(active.allows_origin("http://display-0c01.local"));
}

TEST(SurfaceTests, AnInactiveSlotMatchesNothing) {
    // Ruling B allows each origin "while each is genuinely active". Before DHCP there is no
    // station origin, and after the access point stops there is no setup origin.
    Surfaces active;
    active.set_access_point(kAccessPoint);

    EXPECT_TRUE(active.allows_origin("http://192.168.4.1"));
    EXPECT_FALSE(active.allows_origin("http://192.168.1.42"));
    EXPECT_FALSE(active.allows_origin("http://display-0c01.local"));
}

TEST(SurfaceTests, ClearingASlotWithdrawsItsOrigin) {
    // The mechanism app_core relies on when the access point stops: the origin it answered on
    // stops being accepted in the same call that stops being able to serve it.
    Surfaces active = all_three();
    ASSERT_TRUE(active.allows_origin("http://192.168.4.1"));

    active.set_access_point("");

    EXPECT_FALSE(active.allows_origin("http://192.168.4.1"));
    EXPECT_TRUE(active.allows_origin("http://192.168.1.42"));
}

TEST(SurfaceTests, AnEmptySetMatchesNoOriginAtAll) {
    const Surfaces nothing;

    EXPECT_FALSE(nothing.allows_origin("http://192.168.4.1"));
    EXPECT_FALSE(nothing.allows_origin("http://display-0c01.local"));
    EXPECT_FALSE(nothing.allows_origin(""));
}

TEST(SurfaceTests, AnEmptyOriginIsNeverAMember) {
    // Including when a slot is itself empty: an absent Origin header must not be able to
    // satisfy an inactive slot by both being nothing.
    Surfaces active;
    active.set_access_point("");

    EXPECT_FALSE(active.allows_origin(""));
    EXPECT_FALSE(all_three().allows_origin(""));
}

// ⛔ THE WIDENING MUST NOT HAVE BECOME A WILDCARD. Every string below is one a relaxed
// implementation would let through: a prefix test would accept the suffixed hosts, a
// substring test the embedded ones, a scheme-insensitive test the wrong schemes, and anything
// that trusted `*` would accept the lot.
TEST(SurfaceTests, NoWildcardPrefixOrSubstringIsAccepted) {
    const Surfaces active = all_three();

    for (const std::string_view origin : {
             "*",
             "null",
             "http://*",
             "http://192.168.4.1.evil.example.com",
             "http://evil.example.com/http://192.168.4.1",
             "http://display-0c01.local.evil.example.com",
             "http://192.168.4.1:8080",
             "http://192.168.4.1/",
             "http://192.168.4.10",
             "https://192.168.4.1",
             "HTTP://192.168.4.1",
             "http://display-0C01.local",
             " http://192.168.4.1",
             "http://192.168.4.1 ",
             "192.168.4.1",
             "http://",
         }) {
        EXPECT_FALSE(active.allows_origin(origin)) << "accepted: " << origin;
    }
}

TEST(SurfaceTests, TheHostHalfMustMatchWhole) {
    EXPECT_TRUE(origin_matches_host("http://192.168.4.1", "192.168.4.1"));

    EXPECT_FALSE(origin_matches_host("http://192.168.4.1", "192.168.4."));
    EXPECT_FALSE(origin_matches_host("http://192.168.4.1", "92.168.4.1"));
    EXPECT_FALSE(origin_matches_host("http://192.168.4.1", ""));
    EXPECT_FALSE(origin_matches_host("", "192.168.4.1"));
    EXPECT_FALSE(origin_matches_host("", ""));
}

TEST(SurfaceTests, EveryLabelHasALogLabel) {
    // The log is where the distinction between refusals lives, since the response deliberately
    // does not carry it. A surface with no label would make that log unreadable.
    for (const Surface surface :
         {Surface::Unknown, Surface::SetupAccessPoint, Surface::StationLan}) {
        EXPECT_FALSE(log_label(surface).empty());
    }

    EXPECT_NE(log_label(Surface::SetupAccessPoint), log_label(Surface::StationLan));
    EXPECT_NE(log_label(Surface::Unknown), log_label(Surface::StationLan));
}

} // namespace prd::provisioning
