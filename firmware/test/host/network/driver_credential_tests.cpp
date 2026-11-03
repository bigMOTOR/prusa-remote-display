// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

#include <gtest/gtest.h>

#include "driver_credential.hpp"

// The station credential's driver-facing representation — the 2026-08-23 credential-boundary
// checkpoint, audit item 8.
//
// ⛔ THE DEFECT, IN ONE SENTENCE. Both call sites in wifi.cpp copied
// `size < sizeof(field) ? size : sizeof(field) - 1`, which at exactly 32 bytes takes the false
// branch and copies 31 — so a legal maximum-length SSID that provisioning had just accepted was
// silently shortened by one byte and could never join.
//
// ⬅️ WHY THE PROOF LIVES HERE AND WHAT IT DOES NOT COVER. wifi.cpp includes esp_wifi.h and cannot be
// compiled by this suite, so these tests exercise the pure unit the adapter calls, not the adapter.
// The two things this file cannot prove are pinned elsewhere, deliberately:
//
//   - that the field really is 32 bytes with no length companion — static_asserts in wifi.cpp
//     against sizeof(wifi_sta_config_t::ssid), where the real header is in scope;
//   - that both call sites go through this unit — configure_station() is the single construction
//     site in wifi.cpp, which must be READ. Extraction does not verify the binding.
//
// No real network name appears below. Every value is a generated pattern.

namespace prd::network::driver_credential {
namespace {

// A sentinel the functions must not disturb when they refuse.
constexpr std::uint8_t kUntouched = 0xAB;

// Distinguishable, deterministic, and nothing anyone's router is called.
[[nodiscard]] std::string pattern(const std::size_t length) {
    std::string text;
    text.reserve(length);
    for (std::size_t index = 0; index < length; ++index) {
        text.push_back(static_cast<char>('a' + static_cast<char>(index % 26)));
    }
    return text;
}

// A field pre-filled with the sentinel, so "left untouched" is checkable rather than assumed.
using SsidField = std::array<std::uint8_t, kStationSsidBytes>;
using PasswordField = std::array<std::uint8_t, kStationPasswordBytes>;

template <std::size_t Size> [[nodiscard]] std::array<std::uint8_t, Size> sentinel_field() {
    std::array<std::uint8_t, Size> field{};
    field.fill(kUntouched);
    return field;
}

template <std::size_t Size> void expect_untouched(const std::array<std::uint8_t, Size>& field) {
    for (std::size_t index = 0; index < Size; ++index) {
        ASSERT_EQ(field[index], kUntouched) << "byte " << index << " was written on a refusal";
    }
}

} // namespace

TEST(DriverCredentialTests, THE_FULL_32_BYTE_SSID_REACHES_THE_DRIVER_FIELD) {
    // ⛔ THE REGRESSION. This is the exact length the old expression truncated.
    const std::string ssid = pattern(32);
    ASSERT_EQ(ssid.size(), kStationSsidBytes);

    SsidField field = sentinel_field<kStationSsidBytes>();
    ASSERT_TRUE(copy_ssid(ssid, field.data(), field.size()));

    // Every one of the 32 bytes, named individually so a failure says which.
    for (std::size_t index = 0; index < kStationSsidBytes; ++index) {
        EXPECT_EQ(field[index], static_cast<std::uint8_t>(ssid[index])) << "byte " << index;
    }

    // ⛔ AND THE LAST BYTE IS THE NAME, NOT A TERMINATOR. wifi_sta_config_t has no ssid_len, so a
    // 32-byte array IS a 32-byte SSID. The old code wrote nothing here; asserting it is not zero is
    // what makes the truncation impossible to reintroduce.
    EXPECT_EQ(field[kStationSsidBytes - 1], static_cast<std::uint8_t>(ssid[31]));
    EXPECT_NE(field[kStationSsidBytes - 1], 0U);
}

TEST(DriverCredentialTests, A_31_BYTE_SSID_IS_COPIED_WHOLE_AND_THE_TAIL_IS_ZEROED) {
    const std::string ssid = pattern(31);

    SsidField field = sentinel_field<kStationSsidBytes>();
    ASSERT_TRUE(copy_ssid(ssid, field.data(), field.size()));

    for (std::size_t index = 0; index < 31; ++index) {
        EXPECT_EQ(field[index], static_cast<std::uint8_t>(ssid[index])) << "byte " << index;
    }

    // The one spare byte is zeroed rather than left holding the sentinel — a field reused for a
    // second configuration must not keep the tail of the first name.
    EXPECT_EQ(field[31], 0U);
}

TEST(DriverCredentialTests, A_33_BYTE_SSID_IS_REFUSED_AND_NOT_TRUNCATED) {
    // ⛔ REFUSED, WHICH IS THE OTHER HALF OF THE CORRECTION. request_validation caps the SSID at 32,
    // so 33 cannot arrive from the product — and if it ever could, a truncated name is a DIFFERENT
    // network and joining it would be worse than failing.
    const std::string ssid = pattern(33);

    SsidField field = sentinel_field<kStationSsidBytes>();
    EXPECT_FALSE(copy_ssid(ssid, field.data(), field.size()));
    expect_untouched(field);
}

TEST(DriverCredentialTests, AnEmptySsidIsRefused) {
    // network::start() already treats an absent SSID as the un-provisioned device before it reaches
    // a configuration; this is the same answer one level down.
    SsidField field = sentinel_field<kStationSsidBytes>();
    EXPECT_FALSE(copy_ssid("", field.data(), field.size()));
    expect_untouched(field);
}

TEST(DriverCredentialTests, ANullFieldIsRefusedRatherThanWritten) {
    EXPECT_FALSE(copy_ssid(pattern(8), nullptr, kStationSsidBytes));
    EXPECT_FALSE(copy_password(pattern(8), nullptr, kStationPasswordBytes));
}

TEST(DriverCredentialTests, EveryLengthUpToTheFieldIsAcceptedAndExact) {
    // The whole domain, so no single boundary is the only one checked.
    for (std::size_t length = 1; length <= kStationSsidBytes; ++length) {
        const std::string ssid = pattern(length);
        SsidField field = sentinel_field<kStationSsidBytes>();

        ASSERT_TRUE(copy_ssid(ssid, field.data(), field.size())) << "length " << length;

        for (std::size_t index = 0; index < length; ++index) {
            ASSERT_EQ(field[index], static_cast<std::uint8_t>(ssid[index]))
                << "length " << length << " byte " << index;
        }
        for (std::size_t index = length; index < kStationSsidBytes; ++index) {
            ASSERT_EQ(field[index], 0U) << "length " << length << " tail byte " << index;
        }
    }
}

TEST(DriverCredentialTests, THE_PASSWORD_KEEPS_ITS_EXISTING_VALID_HANDLING) {
    // ⬅️ THE 63-BYTE MAXIMUM request_validation ALLOWS, WHICH MUST BE UNCHANGED. The previous inline
    // expression copied all 63 and left byte 63 zero; so does this.
    const std::string password = pattern(63);
    ASSERT_EQ(password.size(), kStationPasswordBytes - 1);

    PasswordField field = sentinel_field<kStationPasswordBytes>();
    ASSERT_TRUE(copy_password(password, field.data(), field.size()));

    for (std::size_t index = 0; index < 63; ++index) {
        EXPECT_EQ(field[index], static_cast<std::uint8_t>(password[index])) << "byte " << index;
    }

    // ⛔ THE TERMINATOR THE SSID FIELD DOES NOT GET. This is the asymmetry the header explains.
    EXPECT_EQ(field[kStationPasswordBytes - 1], 0U);
}

TEST(DriverCredentialTests, AnEmptyPasswordIsAnOpenNetworkAndYieldsAnAllZeroField) {
    PasswordField field = sentinel_field<kStationPasswordBytes>();
    ASSERT_TRUE(copy_password("", field.data(), field.size()));

    for (std::size_t index = 0; index < kStationPasswordBytes; ++index) {
        ASSERT_EQ(field[index], 0U) << "byte " << index;
    }
}

TEST(DriverCredentialTests, A_PASSWORD_THAT_LEAVES_NO_ROOM_FOR_THE_TERMINATOR_IS_REFUSED) {
    // ⬅️ THE ONE DELIBERATE BEHAVIOUR CHANGE, ON AN INPUT THE PRODUCT CANNOT PRODUCE.
    // request_validation caps the passphrase at 63, so 64 is unreachable; the old expression would
    // have truncated it to 63 and attempted a join with a passphrase that is not the user's.
    PasswordField field = sentinel_field<kStationPasswordBytes>();
    EXPECT_FALSE(copy_password(pattern(64), field.data(), field.size()));
    expect_untouched(field);
}

TEST(DriverCredentialTests, TheBoundsMatchTheEspIdfStationStruct) {
    // wifi.cpp static_asserts both against sizeof(wifi_sta_config_t::ssid) and
    // sizeof(wifi_sta_config_t::password), where the pinned header is in scope. Restating them here
    // means a change shows up as a named failure in this suite too, rather than only as a firmware
    // build error on a machine with the toolchain installed.
    EXPECT_EQ(kStationSsidBytes, 32U);
    EXPECT_EQ(kStationPasswordBytes, 64U);
}

} // namespace prd::network::driver_credential
