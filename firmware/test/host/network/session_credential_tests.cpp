// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include <array>
#include <cstddef>
#include <cstdint>
#include <set>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "session_credential.hpp"

// The provisioning session credential — owner ruling R1.
//
// The point of injecting the entropy source is that everything worth asserting about a
// random password is assertable: that it is drawn uniformly, that a biased byte stream
// cannot skew it, that its length and alphabet are what WPA2 and a 2.8-inch panel both
// need, and that clearing the session actually overwrites the characters.

namespace prd::network {
namespace {

using app_model::BoundedString;

// A scripted entropy source. Yields the given bytes in order, then repeats.
struct Bytes {
    std::vector<std::uint8_t> values;
    std::size_t index{0};
    std::size_t draws{0};
};

std::uint8_t next(void* const context) {
    auto* const bytes = static_cast<Bytes*>(context);
    ++bytes->draws;
    const std::uint8_t value = bytes->values[bytes->index];
    bytes->index = (bytes->index + 1) % bytes->values.size();
    return value;
}

constexpr std::array<std::uint8_t, 6> kMac{0x24, 0x6F, 0x28, 0x11, 0x3F, 0x7A};

Bytes counting() {
    Bytes bytes;
    for (std::uint32_t value = 0; value < 256; ++value) {
        bytes.values.push_back(static_cast<std::uint8_t>(value));
    }
    return bytes;
}

} // namespace

TEST(SessionCredentialTests, TheSsidCarriesTheLastTwoMacBytesInUppercaseHex) {
    const BoundedString<32> ssid = session_ssid(kMac);

    EXPECT_EQ(ssid.view(), "PRD-Setup-3F7A");
}

// The first three bytes are the OUI and are the same on every ESP32 module, so a suffix
// taken from them would give every device the same setup network name.
TEST(SessionCredentialTests, TwoDevicesSharingAnOuiStillGetDifferentSsids) {
    const std::array<std::uint8_t, 6> first{0x24, 0x6F, 0x28, 0x11, 0x3F, 0x7A};
    const std::array<std::uint8_t, 6> second{0x24, 0x6F, 0x28, 0x11, 0xA0, 0x0B};

    EXPECT_NE(session_ssid(first).view(), session_ssid(second).view());
}

TEST(SessionCredentialTests, TheSsidFitsTheDriverAndTheState) {
    // 32 is both BoundedString's capacity here and wifi_ap_config_t::ssid's size.
    EXPECT_LE(session_ssid(kMac).view().size(), 32U);
    EXPECT_FALSE(session_ssid(kMac).truncated());
}

TEST(SessionCredentialTests, ThePasswordIsTheDeclaredLength) {
    Bytes bytes = counting();
    const setup_ap::Session session = make_session(kMac, &next, &bytes);

    EXPECT_EQ(session.password.view().size(), setup_ap::kPasswordLength);
}

// WPA2-PSK accepts 8-63 characters. Below eight the driver would silently produce an open
// network, which is the one outcome a setup AP must never have.
TEST(SessionCredentialTests, ThePasswordIsWithinTheWpa2Range) {
    EXPECT_GE(setup_ap::kPasswordLength, 8U);
    EXPECT_LE(setup_ap::kPasswordLength, 63U);
}

TEST(SessionCredentialTests, EveryPasswordCharacterComesFromTheAlphabet) {
    Bytes bytes = counting();
    const setup_ap::Session session = make_session(kMac, &next, &bytes);

    for (const char character : session.password.view()) {
        EXPECT_NE(kPasswordAlphabet.find(character), std::string_view::npos)
            << "character '" << character << "' is not in the alphabet";
    }
}

// The glyphs a user misreads off a panel. There is no second channel to check against —
// owner ruling D1 removed serial exposure entirely — so a misread password is unrecoverable
// until the device is rebooted into a new session.
TEST(SessionCredentialTests, TheAlphabetExcludesAmbiguousGlyphs) {
    for (const char excluded : std::string{"0O1lI"}) {
        EXPECT_EQ(kPasswordAlphabet.find(excluded), std::string_view::npos)
            << "'" << excluded << "' is ambiguous on a panel and must not be drawable";
    }
}

TEST(SessionCredentialTests, TheAlphabetHasNoRepeatedCharacter) {
    const std::set<char> unique{kPasswordAlphabet.begin(), kPasswordAlphabet.end()};

    EXPECT_EQ(unique.size(), kPasswordAlphabet.size());
}

// The property rejection sampling exists for. A byte at or above the largest multiple of
// the alphabet size is discarded, so `byte % size` never favours the alphabet's start.
TEST(SessionCredentialTests, OutOfRangeBytesAreRedrawnRatherThanFolded) {
    const auto size = static_cast<std::uint32_t>(kPasswordAlphabet.size());
    const std::uint32_t limit = 256U - (256U % size);

    // The first byte is above the limit and must be discarded; the second is used.
    Bytes bytes;
    bytes.values = {static_cast<std::uint8_t>(limit), 0};

    const char drawn = draw_password_character(&next, &bytes);

    EXPECT_EQ(drawn, kPasswordAlphabet[0]);
    EXPECT_EQ(bytes.draws, 2U) << "the out-of-range byte was folded instead of redrawn";
}

// If the fold were `byte % size`, a stream of exactly the out-of-range bytes would still
// produce characters — and they would all come from the alphabet's first 28 entries.
TEST(SessionCredentialTests, AStreamOfOutOfRangeBytesNeverYieldsABiasedCharacter) {
    const auto size = static_cast<std::uint32_t>(kPasswordAlphabet.size());
    const std::uint32_t limit = 256U - (256U % size);

    Bytes bytes;
    for (std::uint32_t value = limit; value < 256; ++value) {
        bytes.values.push_back(static_cast<std::uint8_t>(value));
    }
    // One usable byte at the end so the draw can terminate.
    bytes.values.push_back(static_cast<std::uint8_t>(size - 1));

    const char drawn = draw_password_character(&next, &bytes);

    EXPECT_EQ(drawn, kPasswordAlphabet[size - 1]);
    EXPECT_EQ(bytes.draws, bytes.values.size());
}

// Over a full sweep of usable bytes, every alphabet character is reachable and none is
// reachable more often than another.
TEST(SessionCredentialTests, TheDrawIsUniformOverTheAlphabet) {
    const auto size = static_cast<std::uint32_t>(kPasswordAlphabet.size());
    const std::uint32_t limit = 256U - (256U % size);

    std::vector<std::size_t> counts(size, 0);

    for (std::uint32_t value = 0; value < limit; ++value) {
        Bytes bytes;
        bytes.values = {static_cast<std::uint8_t>(value)};
        const char drawn = draw_password_character(&next, &bytes);
        counts[kPasswordAlphabet.find(drawn)] += 1;
    }

    const std::size_t expected = limit / size;
    for (std::size_t index = 0; index < counts.size(); ++index) {
        EXPECT_EQ(counts[index], expected)
            << "character '" << kPasswordAlphabet[index] << "' is not equally likely";
    }
}

// R1: a new password every session, not one derived from device identity.
TEST(SessionCredentialTests, TheSameDeviceGetsADifferentPasswordFromDifferentEntropy) {
    Bytes first;
    first.values = {0};
    Bytes second;
    second.values = {1};

    const setup_ap::Session one = make_session(kMac, &next, &first);
    const setup_ap::Session two = make_session(kMac, &next, &second);

    EXPECT_NE(one.password.view(), two.password.view());
    EXPECT_EQ(one.ssid.view(), two.ssid.view())
        << "the SSID identifies the device, not the session";
}

TEST(SessionCredentialTests, TheSessionCarriesNoTruncatedValue) {
    Bytes bytes = counting();
    const setup_ap::Session session = make_session(kMac, &next, &bytes);

    EXPECT_FALSE(session.ssid.truncated());
    EXPECT_FALSE(session.password.truncated());
}

// §13.5's "embedded in crash dumps where avoidable". Clearing must overwrite the
// characters, not merely reset the length — c_str() still addresses the whole buffer.
TEST(SessionCredentialTests, ClearingOverwritesThePasswordRatherThanForgettingIt) {
    Bytes bytes = counting();
    setup_ap::Session session = make_session(kMac, &next, &bytes);

    const std::size_t length = session.password.view().size();
    ASSERT_GT(length, 0U);

    const char* const storage = session.password.c_str();
    session.clear();

    for (std::size_t index = 0; index < length; ++index) {
        EXPECT_EQ(storage[index], '\0') << "byte " << index << " of the password survived clear()";
    }

    EXPECT_TRUE(session.password.empty());
    EXPECT_TRUE(session.ssid.empty());
}

} // namespace prd::network
