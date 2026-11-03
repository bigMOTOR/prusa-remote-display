// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include <array>
#include <cstdint>

#include <gtest/gtest.h>

#include "discovery_identity.hpp"
#include "session_credential.hpp"

// The device's stable local-network identity — owner ruling D of Milestone 3.9.
//
// The ruling is specific enough to be testable: `display-<suffix>.local`, the suffix being the
// four-hex-character device suffix already used for setup identity, with an advertised service
// instance of `Remote Display <SUFFIX>`, brand-neutral, and not coupled to the product brand.

namespace prd::network {
namespace {

// The MAC behind the worked example in the ruling: display-0c01.local.
constexpr std::array<std::uint8_t, 6> kMac{0x7C, 0xDF, 0xA1, 0x00, 0x0C, 0x01};

} // namespace

TEST(DiscoveryIdentityTests, TheHostnameIsTheOneTheRulingNames) {
    EXPECT_EQ(discovery_identity::hostname_for(device_suffix(kMac).view()).view(), "display-0c01");
}

TEST(DiscoveryIdentityTests, TheLocalNameIsTheHostnamePlusTheLocalDomain) {
    EXPECT_EQ(discovery_identity::local_name_for(device_suffix(kMac).view()).view(),
              "display-0c01.local");
}

TEST(DiscoveryIdentityTests, TheServiceInstanceIsTheOneTheRulingNames) {
    EXPECT_EQ(discovery_identity::service_instance_for(device_suffix(kMac).view()).view(),
              "Remote Display 0C01");
}

// ⬅️ ONE DERIVATION, TWO IDENTITIES. The point of reusing device_suffix() is that the setup
// network a person joined and the name they reach the device at afterwards identify the same
// board. A second derivation could drift, and nothing about the two names would reveal it.
TEST(DiscoveryIdentityTests, TheSetupSsidAndTheHostnameCarryTheSameSuffix) {
    const auto suffix = device_suffix(kMac);
    const auto ssid = session_ssid(kMac);
    const auto hostname = discovery_identity::hostname_for(suffix.view());

    EXPECT_EQ(ssid.view(), "PRD-Setup-0C01");
    EXPECT_EQ(hostname.view(), "display-0c01");

    // The same four characters, differing only in case — which is the ONLY difference the
    // ruling asks for.
    EXPECT_NE(ssid.view().find(suffix.view()), std::string_view::npos);
    EXPECT_EQ(suffix.view(), "0C01");
}

TEST(DiscoveryIdentityTests, TheSuffixComesFromTheLastTwoMacBytes) {
    // The first three bytes are the OUI and identical across every ESP32 module, so a suffix
    // taken from them would name every device the same. Two MACs sharing an OUI must still
    // produce different names.
    constexpr std::array<std::uint8_t, 6> other{0x7C, 0xDF, 0xA1, 0x00, 0xFE, 0xDC};

    EXPECT_EQ(device_suffix(other).view(), "FEDC");
    EXPECT_EQ(discovery_identity::hostname_for(device_suffix(other).view()).view(), "display-fedc");
    EXPECT_NE(discovery_identity::hostname_for(device_suffix(kMac).view()).view(),
              discovery_identity::hostname_for(device_suffix(other).view()).view());
}

TEST(DiscoveryIdentityTests, AZeroMacStillProducesAUsableName) {
    // esp_read_mac() failing is logged as a warning rather than refused: a device that
    // advertises a non-unique name is still reachable, and refusing to be reachable is worse.
    // What must not happen is an empty or malformed hostname.
    constexpr std::array<std::uint8_t, 6> zeros{};

    EXPECT_EQ(device_suffix(zeros).view(), "0000");
    EXPECT_EQ(discovery_identity::hostname_for(device_suffix(zeros).view()).view(), "display-0000");
    EXPECT_EQ(discovery_identity::local_name_for(device_suffix(zeros).view()).view(),
              "display-0000.local");
}

// ⛔ BRAND-NEUTRAL, BY RULING D. LICENSING.md reserves the project and product names, logos
// and wordmarks as all rights reserved, so a hostname coupled to a brand would be a product
// decision made by a firmware constant.
TEST(DiscoveryIdentityTests, NoNameCarriesTheReservedBrand) {
    const auto suffix = device_suffix(kMac);

    // ⛔ THE THREE NAMES ARE NAMED, AND THAT IS NOT A STYLE CHOICE. Each of these functions returns a
    // BoundedString BY VALUE, so `f(...).view()` is a view into a temporary. In C++17 a range-based
    // for over a braced list extends the lifetime of the initializer_list ARRAY but not of the
    // temporaries materialised inside the initializer: they die at the end of the init-statement,
    // leaving three dangling views the loop body then reads. AddressSanitizer reported it as
    // stack-use-after-scope, and it read the correct bytes without one — which is exactly why it
    // survived. C++23's P2718R0 extends those temporaries too, so this is a language-version trap
    // rather than a mistake in what the test is checking; naming them fixes it under C++17 without
    // touching the standard.
    const auto hostname = discovery_identity::hostname_for(suffix.view());
    const auto local_name = discovery_identity::local_name_for(suffix.view());
    const auto service_instance = discovery_identity::service_instance_for(suffix.view());

    for (const std::string_view name :
         {hostname.view(), local_name.view(), service_instance.view()}) {
        EXPECT_EQ(name.find("prusa"), std::string_view::npos) << name;
        EXPECT_EQ(name.find("Prusa"), std::string_view::npos) << name;
        EXPECT_EQ(name.find("PRUSA"), std::string_view::npos) << name;
    }

    // POSITIVE CONTROL: the loop really did see three non-empty names, so the three EXPECT_EQs above
    // cannot be passing because every view was empty.
    EXPECT_FALSE(hostname.view().empty());
    EXPECT_FALSE(local_name.view().empty());
    EXPECT_FALSE(service_instance.view().empty());
}

// The hostname is what a browser resolves, so it must be a legal DNS label: lowercase letters,
// digits and hyphens, never starting or ending with one.
TEST(DiscoveryIdentityTests, TheHostnameIsALegalDnsLabel) {
    constexpr std::array<std::uint8_t, 4> bytes{0x00, 0x0C, 0xFF, 0xA5};

    for (const std::uint8_t high : bytes) {
        for (const std::uint8_t low : bytes) {
            const std::array<std::uint8_t, 6> mac{0, 0, 0, 0, high, low};
            const auto hostname = discovery_identity::hostname_for(device_suffix(mac).view());
            const std::string_view text = hostname.view();

            ASSERT_FALSE(text.empty());
            EXPECT_NE(text.front(), '-') << text;
            EXPECT_NE(text.back(), '-') << text;

            // A single label, so no dot may appear before local_name_for() adds the domain.
            EXPECT_EQ(text.find('.'), std::string_view::npos) << text;

            for (const char character : text) {
                const bool legal = (character >= 'a' && character <= 'z') ||
                                   (character >= '0' && character <= '9') || character == '-';
                EXPECT_TRUE(legal) << "illegal character in " << text;
            }

            // 63 is the DNS label limit; this is far inside it, and the bounded string must
            // not have truncated on the way.
            EXPECT_FALSE(hostname.truncated());
            EXPECT_LE(text.size(), 63U);
        }
    }
}

TEST(DiscoveryIdentityTests, NoNameIsTruncatedByItsBoundedString) {
    // A truncated hostname would still resolve — to the wrong device, or to nothing — and a
    // truncated `.local` name would silently stop matching the origin the browser sends.
    const auto suffix = device_suffix(kMac);

    EXPECT_FALSE(discovery_identity::hostname_for(suffix.view()).truncated());
    EXPECT_FALSE(discovery_identity::local_name_for(suffix.view()).truncated());
    EXPECT_FALSE(discovery_identity::service_instance_for(suffix.view()).truncated());
}

} // namespace prd::network
