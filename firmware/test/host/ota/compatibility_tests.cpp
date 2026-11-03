// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include "ota/compatibility.hpp"

#include <gtest/gtest.h>

#include <array>
#include <limits>
#include <string>

namespace {
using namespace prd::ota;

TEST(OtaCompatibilityDefaultTests, ValueInitializationIsNotAnAcceptance) {
    const Compatibility result{};
    EXPECT_EQ(result, Compatibility::NotChecked);
    EXPECT_NE(result, Compatibility::MetadataCompatible);
}

class OtaCompatibilityTests : public ::testing::Test {
  protected:
    Candidate candidate{1,
                        "board-a",
                        "esp32s3",
                        "1.2.3",
                        1,
                        100,
                        "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"};
    DeviceCompatibility device{"board-a", "esp32s3", "1.2.2", 1, 100};

    Compatibility check(const DowngradePolicy policy = DowngradePolicy::Reject) const {
        return check_compatibility(candidate, device, policy);
    }
};

TEST_F(OtaCompatibilityTests, MatchingMetadataAndExactCapacityAreCompatible) {
    EXPECT_EQ(check(), Compatibility::MetadataCompatible);
    device.version = candidate.version;
    EXPECT_EQ(check(), Compatibility::MetadataCompatible);
}

TEST_F(OtaCompatibilityTests, RejectsUnknownManifestSchema) {
    for (const auto schema : {0U, 2U, std::numeric_limits<unsigned int>::max()}) {
        candidate.manifest_schema = schema;
        EXPECT_EQ(check(), Compatibility::UnsupportedManifestSchema);
    }
}

TEST_F(OtaCompatibilityTests, ExactBoardAndChipIdentityRequired) {
    candidate.board = "board-b";
    EXPECT_EQ(check(), Compatibility::WrongBoard);
    candidate.board = "";
    EXPECT_EQ(check(), Compatibility::WrongBoard);
    candidate.board = device.board;
    candidate.chip = "ESP32S3";
    EXPECT_EQ(check(), Compatibility::WrongChip);
    candidate.chip = "";
    EXPECT_EQ(check(), Compatibility::WrongChip);
}

TEST_F(OtaCompatibilityTests, RequiresKnownLocalCapabilities) {
    const auto original = device;
    device.board = "";
    EXPECT_EQ(check(), Compatibility::InvalidLocalIdentity);
    device = original;
    device.chip = "";
    EXPECT_EQ(check(), Compatibility::InvalidLocalIdentity);
    device = original;
    device.bootloader_schema = 0;
    EXPECT_EQ(check(), Compatibility::InvalidLocalIdentity);
    device = original;
    device.inactive_slot_size = 0;
    EXPECT_EQ(check(), Compatibility::InvalidLocalIdentity);
    device = original;
    device.version = "not-known";
    EXPECT_EQ(check(DowngradePolicy::ExplicitRecovery), Compatibility::InvalidLocalIdentity);
}

TEST_F(OtaCompatibilityTests, BootloaderRequirementMustBeKnownAndLocallySupported) {
    candidate.min_bootloader_schema = 0;
    EXPECT_EQ(check(), Compatibility::UnsupportedBootloaderSchema);
    candidate.min_bootloader_schema = 2;
    EXPECT_EQ(check(), Compatibility::UnsupportedBootloaderSchema);
    device.bootloader_schema = 3;
    EXPECT_EQ(check(), Compatibility::MetadataCompatible);
}

TEST_F(OtaCompatibilityTests, LocalSlotCapacityIsAuthoritative) {
    candidate.image_size = 0;
    EXPECT_EQ(check(), Compatibility::InvalidImageSize);
    candidate.image_size = 101;
    EXPECT_EQ(check(DowngradePolicy::ExplicitRecovery), Compatibility::InvalidImageSize);
    candidate.image_size = std::numeric_limits<std::uint64_t>::max();
    EXPECT_EQ(check(), Compatibility::InvalidImageSize);
    candidate.image_size = 99;
    EXPECT_EQ(check(), Compatibility::MetadataCompatible);
}

TEST_F(OtaCompatibilityTests, DigestIsSyntaxOnlyNotImageVerification) {
    for (const auto& digest : {std::string{}, std::string(63, 'a'), std::string(65, 'a'),
                               std::string(64, 'g'), std::string(64, '\0')}) {
        candidate.sha256 = digest;
        EXPECT_EQ(check(), Compatibility::InvalidDigestDeclaration);
    }
    const std::string syntactically_valid(64, '0');
    candidate.sha256 = syntactically_valid;
    EXPECT_EQ(check(), Compatibility::MetadataCompatible);
    const std::string upper_case(64, 'F');
    candidate.sha256 = upper_case;
    EXPECT_EQ(check(), Compatibility::MetadataCompatible);
}

TEST_F(OtaCompatibilityTests, RejectsMalformedVersionsEvenInRecovery) {
    for (const auto* value :
         {"", "1.2", "1.2.3.4", "01.2.3", "1.02.3", "1.2.03", "1.2.3-", "1.2.3-a..b", "1.2.3-a.",
          "1.2.3-01", "1.2.3-a_", "1.2.3+", "1.2.3+x+y", "1.2.3+x.", "1.2.3+..", "4294967296.0.0",
          " 1.2.3", "1.2.3\n"}) {
        SCOPED_TRACE(value);
        candidate.version = value;
        EXPECT_EQ(check(DowngradePolicy::ExplicitRecovery), Compatibility::InvalidCandidateVersion);
    }
}

TEST_F(OtaCompatibilityTests, NumericCoreIsNotLexicographic) {
    device.version = "1.9.0";
    candidate.version = "1.10.0";
    EXPECT_EQ(check(), Compatibility::MetadataCompatible);
    device.version = "2.0.0";
    EXPECT_EQ(check(), Compatibility::Downgrade);
    EXPECT_EQ(check(DowngradePolicy::ExplicitRecovery), Compatibility::MetadataCompatible);
    candidate.board = "wrong";
    EXPECT_EQ(check(DowngradePolicy::ExplicitRecovery), Compatibility::WrongBoard);
}

TEST_F(OtaCompatibilityTests, SemverPrereleasePrecedenceInBothDirections) {
    constexpr std::array versions{"1.0.0-alpha",  "1.0.0-alpha.1", "1.0.0-alpha.beta", "1.0.0-beta",
                                  "1.0.0-beta.2", "1.0.0-beta.11", "1.0.0-rc.1",       "1.0.0"};
    for (std::size_t current = 0; current < versions.size(); ++current) {
        for (std::size_t proposed = 0; proposed < versions.size(); ++proposed) {
            device.version = versions[current];
            candidate.version = versions[proposed];
            EXPECT_EQ(check(), proposed < current ? Compatibility::Downgrade
                                                  : Compatibility::MetadataCompatible);
        }
    }
}

TEST_F(OtaCompatibilityTests, MetadataIgnoredAndLargePrereleaseNumbersDoNotOverflow) {
    device.version = "1.2.3+build.001";
    candidate.version = "1.2.3+build.999";
    EXPECT_EQ(check(), Compatibility::MetadataCompatible);
    device.version = "1.2.3-99999999999999999999999999999999";
    candidate.version = "1.2.3-100000000000000000000000000000000";
    EXPECT_EQ(check(), Compatibility::MetadataCompatible);
    device.version = "1.2.3-z";
    EXPECT_EQ(check(), Compatibility::Downgrade);
}

} // namespace
