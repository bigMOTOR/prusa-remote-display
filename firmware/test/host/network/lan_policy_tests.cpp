// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include <array>
#include <string>

#include <gtest/gtest.h>

#include "network/lan.hpp"

// Which destinations may receive a credential — the audit's B6.
//
// ⛔ WHAT THIS PROTECTS. Architecture Spec §13.5: credentials must never be sent outside the LAN.
// The previous implementation checked only that a host string was made of letters, digits, dots and
// hyphens, which accepts `api.example.com` and `8.8.8.8` exactly as readily as `192.168.1.50` — and
// the test suite of the day contained a POSITIVE control asserting that `evil.example.com` was an
// acceptable printer host. That control is gone; these are its replacements, and they are negative.
//
// ⬅️ THE POLICY IS STATED OVER AN ADDRESS, NOT A NAME, BECAUSE A NAME CANNOT ANSWER THE QUESTION.
// A name is resolved once, the resulting address is classified here, and the request is issued to
// that address as a literal — so there is no second resolution to answer differently. These tests
// are the whole policy; the adapter that produces a Resolution is a thin wrapper around getaddrinfo.

namespace prd::network::lan_policy {
namespace {

[[nodiscard]] Ipv4 address_of(const std::string& text) {
    Ipv4 parsed;
    EXPECT_TRUE(parse_ipv4(text, parsed)) << text;
    return parsed;
}

TEST(LanPolicyTests, PrivateRangesArePermitted) {
    // RFC 1918, the ordinary case: a home or workshop LAN.
    for (const char* text : {"10.0.0.1", "10.255.255.254", "172.16.0.1", "172.31.255.254",
                             "192.168.0.1", "192.168.1.50", "192.168.255.254"}) {
        const Ipv4 address = address_of(text);
        EXPECT_EQ(classify(address), Destination::Private) << text;
        EXPECT_TRUE(permits(address)) << text;
    }
}

TEST(LanPolicyTests, LinkLocalIsPermitted) {
    // RFC 3927. What a printer and a display fall back to on a segment with no DHCP server, so
    // refusing it would break a legitimate local setup.
    const Ipv4 address = address_of("169.254.10.20");
    EXPECT_EQ(classify(address), Destination::LinkLocal);
    EXPECT_TRUE(permits(address));
}

TEST(LanPolicyTests, PublicAddressesAreRefused) {
    // ⛔ THE CORE NEGATIVE CONTROL. Each of these is somewhere an API key must never go.
    for (const char* text : {"8.8.8.8", "1.1.1.1", "93.184.216.34", "203.0.113.10", "198.51.100.7",
                             "192.0.2.1", "172.32.0.1", "172.15.255.254", "192.169.0.1", "11.0.0.1",
                             "100.64.0.1", "169.253.0.1", "169.255.0.1"}) {
        const Ipv4 address = address_of(text);
        EXPECT_EQ(classify(address), Destination::Public) << text;
        EXPECT_FALSE(permits(address)) << text;
    }
}

TEST(LanPolicyTests, TheBoundariesOfEveryPrivateRangeAreExact) {
    // An off-by-one on any of these admits globally routable space. Each pair straddles a boundary.
    EXPECT_EQ(classify(address_of("9.255.255.255")), Destination::Public);
    EXPECT_EQ(classify(address_of("10.0.0.0")), Destination::Private);
    EXPECT_EQ(classify(address_of("10.255.255.255")), Destination::Private);
    EXPECT_EQ(classify(address_of("11.0.0.0")), Destination::Public);

    EXPECT_EQ(classify(address_of("172.15.255.255")), Destination::Public);
    EXPECT_EQ(classify(address_of("172.16.0.0")), Destination::Private);
    EXPECT_EQ(classify(address_of("172.31.255.255")), Destination::Private);
    EXPECT_EQ(classify(address_of("172.32.0.0")), Destination::Public);

    EXPECT_EQ(classify(address_of("192.167.255.255")), Destination::Public);
    EXPECT_EQ(classify(address_of("192.168.0.0")), Destination::Private);
    EXPECT_EQ(classify(address_of("192.168.255.255")), Destination::Private);
    EXPECT_EQ(classify(address_of("192.169.0.0")), Destination::Public);

    EXPECT_EQ(classify(address_of("169.253.255.255")), Destination::Public);
    EXPECT_EQ(classify(address_of("169.254.0.0")), Destination::LinkLocal);
    EXPECT_EQ(classify(address_of("169.254.255.255")), Destination::LinkLocal);
    EXPECT_EQ(classify(address_of("169.255.0.0")), Destination::Public);
}

TEST(LanPolicyTests, LoopbackUnspecifiedAndMulticastAreRefused) {
    // Loopback is refused on purpose: nothing on this device serves PrusaLink, so a loopback
    // destination is either a misconfiguration or a local proxy that could forward the key onward.
    EXPECT_EQ(classify(address_of("127.0.0.1")), Destination::Loopback);
    EXPECT_EQ(classify(address_of("127.255.255.254")), Destination::Loopback);

    // 0.0.0.0/8 is what a failed or unset resolution looks like, and must never be a destination.
    EXPECT_EQ(classify(address_of("0.0.0.0")), Destination::Unspecified);
    EXPECT_EQ(classify(address_of("0.1.2.3")), Destination::Unspecified);

    EXPECT_EQ(classify(address_of("224.0.0.1")), Destination::Multicast);
    EXPECT_EQ(classify(address_of("239.255.255.255")), Destination::Multicast);
    EXPECT_EQ(classify(address_of("255.255.255.255")), Destination::Multicast);

    for (const char* text : {"127.0.0.1", "0.0.0.0", "224.0.0.1", "255.255.255.255"}) {
        EXPECT_FALSE(permits(address_of(text))) << text;
    }
}

TEST(LanPolicyTests, OnlyTwoDestinationClassesAreEverPermitted) {
    // Default-deny, stated exhaustively: a class nobody has taught the policy about must be refused
    // rather than allowed. Without this, adding a Destination value later could silently open a hole.
    constexpr std::array<Destination, 6> kAll{Destination::Private,   Destination::LinkLocal,
                                              Destination::Loopback,  Destination::Unspecified,
                                              Destination::Multicast, Destination::Public};

    int permitted = 0;

    for (const Destination destination : kAll) {
        if (permits(destination)) {
            ++permitted;
            EXPECT_TRUE(destination == Destination::Private ||
                        destination == Destination::LinkLocal);
        }

        EXPECT_FALSE(log_label(destination).empty());
    }

    EXPECT_EQ(permitted, 2);
}

TEST(LanPolicyTests, ParsingRefusesEverythingThatIsNotADottedQuad) {
    Ipv4 parsed;

    // A leading zero is the dangerous one: some resolvers read `010` as octal, so a value that
    // means two different things is a value no policy can be stated over.
    EXPECT_FALSE(parse_ipv4("010.0.0.1", parsed));
    EXPECT_FALSE(parse_ipv4("192.168.01.1", parsed));

    EXPECT_FALSE(parse_ipv4("192.168.1", parsed));
    EXPECT_FALSE(parse_ipv4("192.168.1.1.1", parsed));
    EXPECT_FALSE(parse_ipv4("192.168.1.256", parsed));
    EXPECT_FALSE(parse_ipv4("192.168.1.-1", parsed));
    EXPECT_FALSE(parse_ipv4("192.168.1.", parsed));
    EXPECT_FALSE(parse_ipv4(".192.168.1.1", parsed));
    EXPECT_FALSE(parse_ipv4("192.168..1", parsed));
    EXPECT_FALSE(parse_ipv4("192.168.1.1 ", parsed));
    EXPECT_FALSE(parse_ipv4(" 192.168.1.1", parsed));
    EXPECT_FALSE(parse_ipv4("printer.local", parsed));
    EXPECT_FALSE(parse_ipv4("0x c0.a8.1.1", parsed));
    EXPECT_FALSE(parse_ipv4("", parsed));

    // POSITIVE CONTROL: the canonical forms parse, so the table above discriminates rather than
    // reflecting a parser that refuses everything.
    EXPECT_TRUE(parse_ipv4("0.0.0.0", parsed));
    EXPECT_TRUE(parse_ipv4("192.168.1.1", parsed));
    EXPECT_TRUE(parse_ipv4("255.255.255.255", parsed));
    EXPECT_TRUE(parse_ipv4("10.0.0.0", parsed));
}

TEST(LanPolicyTests, RenderRoundTripsAndBoundsItself) {
    for (const char* text : {"0.0.0.0", "10.1.2.3", "192.168.1.50", "255.255.255.255"}) {
        std::array<char, 16> buffer{};
        const std::size_t length = render_ipv4(address_of(text), buffer.data(), buffer.size());

        ASSERT_NE(length, 0U) << text;
        EXPECT_EQ(std::string(buffer.data(), length), text);
    }

    // Refuses rather than truncates: a clipped authority is a request to somewhere else.
    std::array<char, 4> small{};
    EXPECT_EQ(render_ipv4(address_of("192.168.1.50"), small.data(), small.size()), 0U);
    EXPECT_EQ(render_ipv4(address_of("10.0.0.1"), nullptr, 16), 0U);
}

TEST(LanPolicyTests, ANameIsAcceptedForResolutionOnlyAndNeverAsADecision) {
    // These are names a resolver may be asked about. Being resolvable says NOTHING about whether the
    // answer may receive a credential — that is classify()'s job on the resulting address, and the
    // separation is the point: `printer.example.com` is a perfectly well-formed name that must still
    // be refused if it resolves off the LAN.
    EXPECT_TRUE(is_resolvable_name("printer.local"));
    EXPECT_TRUE(is_resolvable_name("prusa"));
    EXPECT_TRUE(is_resolvable_name("core-one.local"));
    EXPECT_TRUE(is_resolvable_name("printer.example.com"));

    // An address literal is not a name, and must not be handed to a resolver — a resolver could
    // answer differently for something that looks like an address.
    EXPECT_FALSE(is_resolvable_name("192.168.1.50"));

    // Shapes that could re-point an authority, or that a resolver and this code would read
    // differently.
    for (const char* host :
         {"", "evil.example.com/x", "user@evil.example.com", "printer.local:80",
          "http://printer.local", "printer .local", "printer\tlocal", "[fe80::1]", "printer_local",
          "printer%2elocal", ".printer.local", "printer.local.", "printer..local", "-printer.local",
          "printer.local-"}) {
        EXPECT_FALSE(is_resolvable_name(host)) << host;
    }
}

} // namespace
} // namespace prd::network::lan_policy
