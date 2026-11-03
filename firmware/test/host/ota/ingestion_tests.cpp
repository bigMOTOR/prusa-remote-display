// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include "ota/ingestion.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <limits>
#include <string>
#include <type_traits>
#include <vector>

namespace {
using namespace prd::ota;

// Deliberately injected digest, not a SHA-256 implementation or crypto test.
// The component under test owns byte-count and digest-comparison decisions.
class RecordingPort final : public IngestionPort {
  public:
    IngestionBegin begin(std::uint64_t size) override {
        ++begins;
        declared_size = size;
        return begin_result;
    }
    bool write(const std::uint8_t* bytes, std::size_t size) override {
        ++writes;
        chunks.push_back(size);
        if (capture_bytes) {
            stored.insert(stored.end(), bytes, bytes + size);
        }
        return write_ok;
    }
    bool finish_digest(ImageDigest& output) override {
        ++finishes;
        output = digest;
        return digest_ok;
    }
    void abort() override {
        ++aborts;
    }
    bool finalize(ImageIdentity& output) override {
        ++verifications;
        output = identity;
        return verification_ok;
    }

    ImageIdentity identity{};
    bool verification_ok{true};
    int verifications{0};

    IngestionBegin begin_result{IngestionBegin::Started};
    bool write_ok{true};
    bool digest_ok{true};
    bool capture_bytes{true};
    int begins{0};
    int writes{0};
    int finishes{0};
    int aborts{0};
    std::uint64_t declared_size{0};
    ImageDigest digest{};
    std::vector<std::uint8_t> stored;
    std::vector<std::size_t> chunks;
};

class OtaIngestionTests : public ::testing::Test {
  protected:
    static constexpr std::array<std::uint8_t, 3> bytes{0x00, 0xff, 0x42};
    std::string digest_text = std::string(64, '0');
    Candidate candidate{1, "board-a", "esp32s3", "1.2.3", 1, bytes.size(), digest_text};
    DeviceCompatibility device{"board-a", "esp32s3", "1.2.2", 1, bytes.size()};
    RecordingPort port;
    ImageIngestion ingestion{port};

    bool begin() {
        return ingestion.begin(candidate, device, DowngradePolicy::Reject);
    }
};

TEST_F(OtaIngestionTests, RefusedBeginNeverAbortsAnIncumbentSession) {
    port.begin_result = IngestionBegin::Refused;
    {
        ImageIngestion refused{port};
        EXPECT_FALSE(refused.begin(candidate, device, DowngradePolicy::Reject));
        EXPECT_EQ(refused.status(), IngestionStatus::BeginFailed);
        refused.cancel();
        EXPECT_EQ(port.aborts, 0);
    }
    EXPECT_EQ(port.begins, 1);
    EXPECT_EQ(port.aborts, 0);
}

TEST_F(OtaIngestionTests, VerificationConsumesSuccessAndFailureWithoutAbort) {
    std::copy(candidate.board.begin(), candidate.board.end(), port.identity.board.begin());
    std::copy(candidate.version.begin(), candidate.version.end(), port.identity.version.begin());
    ASSERT_TRUE(begin());
    ASSERT_TRUE(ingestion.append(bytes.data(), bytes.size()));
    ASSERT_TRUE(ingestion.finish());
    ASSERT_TRUE(ingestion.verify());
    EXPECT_EQ(ingestion.status(), IngestionStatus::ImageVerified);
    EXPECT_FALSE(ingestion.verify());
    ingestion.cancel();
    EXPECT_EQ(port.verifications, 1);
    EXPECT_EQ(port.aborts, 0);
}

TEST_F(OtaIngestionTests, FailedVerificationNeverAbortsConsumedHandle) {
    port.verification_ok = false;
    ASSERT_TRUE(begin());
    ASSERT_TRUE(ingestion.append(bytes.data(), bytes.size()));
    ASSERT_TRUE(ingestion.finish());
    EXPECT_FALSE(ingestion.verify());
    EXPECT_EQ(ingestion.status(), IngestionStatus::VerificationFailed);
    EXPECT_FALSE(ingestion.verify());
    ingestion.cancel();
    EXPECT_EQ(port.verifications, 1);
    EXPECT_EQ(port.aborts, 0);
}

TEST_F(OtaIngestionTests, DestructionNeverAbortsAfterConsumingVerification) {
    for (const bool success : {false, true}) {
        RecordingPort trial_port;
        trial_port.verification_ok = success;
        std::copy(candidate.board.begin(), candidate.board.end(),
                  trial_port.identity.board.begin());
        std::copy(candidate.version.begin(), candidate.version.end(),
                  trial_port.identity.version.begin());
        {
            ImageIngestion trial{trial_port};
            ASSERT_TRUE(trial.begin(candidate, device, DowngradePolicy::Reject));
            ASSERT_TRUE(trial.append(bytes.data(), bytes.size()));
            ASSERT_TRUE(trial.finish());
            EXPECT_EQ(trial.verify(), success);
        }
        EXPECT_EQ(trial_port.verifications, 1);
        EXPECT_EQ(trial_port.aborts, 0);
    }
}

TEST_F(OtaIngestionTests, RetainedManifestCannotBeMutatedToAuthorizeOlderImage) {
    std::string version = "9.0.0";
    candidate.version = version;
    ASSERT_TRUE(begin());
    version = "1.0.0";
    std::copy(candidate.board.begin(), candidate.board.end(), port.identity.board.begin());
    std::copy(version.begin(), version.end(), port.identity.version.begin());
    ASSERT_TRUE(ingestion.append(bytes.data(), bytes.size()));
    ASSERT_TRUE(ingestion.finish());
    EXPECT_FALSE(ingestion.verify());
    EXPECT_EQ(ingestion.status(), IngestionStatus::IdentityMismatch);
    EXPECT_EQ(port.aborts, 0);
}

TEST_F(OtaIngestionTests, OverlongIdentityNeverStarts) {
    std::string board(32, 'a');
    candidate.board = board;
    device.board = board;
    EXPECT_FALSE(begin());
    EXPECT_EQ(ingestion.status(), IngestionStatus::IdentityTooLong);
    EXPECT_EQ(port.begins, 0);
}

TEST_F(OtaIngestionTests, OverlongSemverNeverStarts) {
    const std::string version = "1.2.3+" + std::string(26, 'a');
    candidate.version = version;
    EXPECT_FALSE(begin());
    EXPECT_EQ(ingestion.status(), IngestionStatus::IdentityTooLong);
    EXPECT_EQ(port.begins, 0);
}

TEST_F(OtaIngestionTests, DefaultsAndOutOfOrderCallsCannotOpenOrAcceptAnything) {
    EXPECT_EQ(ingestion.status(), IngestionStatus::Idle);
    EXPECT_EQ(ingestion.compatibility(), Compatibility::NotChecked);
    EXPECT_EQ(ingestion.received_bytes(), 0);
    EXPECT_FALSE(ingestion.append(bytes.data(), bytes.size()));
    EXPECT_FALSE(ingestion.finish());
    EXPECT_EQ(port.begins, 0);
    EXPECT_EQ(port.writes, 0);
    EXPECT_EQ(port.finishes, 0);
    EXPECT_EQ(port.aborts, 0);
    static_assert(!std::is_copy_constructible_v<ImageIngestion>);
    static_assert(!std::is_move_constructible_v<ImageIngestion>);
}

TEST_F(OtaIngestionTests, CompatibilityRejectionNeverOpensStaging) {
    candidate.board = "wrong-board";
    EXPECT_FALSE(begin());
    EXPECT_EQ(ingestion.compatibility(), Compatibility::WrongBoard);
    EXPECT_EQ(ingestion.status(), IngestionStatus::CompatibilityRejected);
    EXPECT_EQ(port.begins, 0);
    EXPECT_EQ(port.aborts, 0);
    candidate.board = device.board;
    EXPECT_FALSE(begin());
    EXPECT_FALSE(ingestion.finish());
    EXPECT_FALSE(ingestion.append(bytes.data(), bytes.size()));
    ingestion.cancel();
    EXPECT_EQ(ingestion.status(), IngestionStatus::CompatibilityRejected);
}

TEST_F(OtaIngestionTests, InvalidSizeAndDigestNeverOpenStaging) {
    for (const auto size : {std::uint64_t{0}, device.inactive_slot_size + 1}) {
        ImageIngestion trial(port);
        candidate.image_size = size;
        EXPECT_FALSE(trial.begin(candidate, device, DowngradePolicy::Reject));
        EXPECT_EQ(trial.compatibility(), Compatibility::InvalidImageSize);
    }
    candidate.image_size = bytes.size();
    for (const auto* invalid : {"", "not a hash"}) {
        ImageIngestion trial(port);
        candidate.sha256 = invalid;
        EXPECT_FALSE(trial.begin(candidate, device, DowngradePolicy::Reject));
        EXPECT_EQ(trial.compatibility(), Compatibility::InvalidDigestDeclaration);
    }
    EXPECT_EQ(port.begins, 0);
    EXPECT_EQ(port.aborts, 0);
}

TEST_F(OtaIngestionTests, BeginFailureReleasesPartialAcquisitionExactlyOnce) {
    port.begin_result = IngestionBegin::FailedNeedsAbort;
    {
        ImageIngestion trial(port);
        EXPECT_FALSE(trial.begin(candidate, device, DowngradePolicy::Reject));
        EXPECT_EQ(trial.status(), IngestionStatus::BeginFailed);
        EXPECT_EQ(port.aborts, 1);
        trial.cancel();
        EXPECT_FALSE(trial.finish());
        EXPECT_FALSE(trial.begin(candidate, device, DowngradePolicy::Reject));
    }
    EXPECT_EQ(port.begins, 1);
    EXPECT_EQ(port.aborts, 1);
}

TEST_F(OtaIngestionTests, ArbitraryChunksPassExactBytesInOrderAndMatchDigest) {
    ASSERT_TRUE(begin());
    EXPECT_EQ(port.declared_size, bytes.size());
    ASSERT_TRUE(ingestion.append(bytes.data(), 1));
    ASSERT_TRUE(ingestion.append(bytes.data() + 1, 2));
    EXPECT_EQ(port.chunks, (std::vector<std::size_t>{1, 2}));
    EXPECT_EQ(port.stored, (std::vector<std::uint8_t>{0x00, 0xff, 0x42}));
    EXPECT_EQ(ingestion.received_bytes(), bytes.size());
    ASSERT_TRUE(ingestion.finish());
    EXPECT_EQ(ingestion.status(), IngestionStatus::DigestMatched);
    EXPECT_EQ(port.finishes, 1);
    EXPECT_EQ(port.aborts, 0);
    EXPECT_FALSE(ingestion.finish());
    EXPECT_FALSE(ingestion.append(bytes.data(), 1));
    EXPECT_FALSE(begin());
    EXPECT_EQ(port.finishes, 1);
    EXPECT_EQ(port.writes, 2);
}

TEST_F(OtaIngestionTests, EmptyChunksAreNoOpsButCannotMakeAnImageComplete) {
    ASSERT_TRUE(begin());
    EXPECT_TRUE(ingestion.append(nullptr, 0));
    EXPECT_EQ(port.writes, 0);
    EXPECT_FALSE(ingestion.finish());
    EXPECT_EQ(ingestion.status(), IngestionStatus::Truncated);
    EXPECT_EQ(port.finishes, 0);
    EXPECT_EQ(port.aborts, 1);
}

TEST_F(OtaIngestionTests, NullNonemptyChunkAbortsBeforeWrite) {
    ASSERT_TRUE(begin());
    EXPECT_FALSE(ingestion.append(nullptr, 1));
    EXPECT_EQ(ingestion.status(), IngestionStatus::InvalidChunk);
    EXPECT_EQ(port.writes, 0);
    EXPECT_EQ(port.aborts, 1);
}

TEST_F(OtaIngestionTests, OversizedNullChunkReportsSizeBeforePointerFailure) {
    {
        ImageIngestion trial(port);
        ASSERT_TRUE(trial.begin(candidate, device, DowngradePolicy::Reject));
        EXPECT_FALSE(trial.append(nullptr, bytes.size() + 1));
        EXPECT_EQ(trial.status(), IngestionStatus::TooManyBytes);
        EXPECT_EQ(trial.received_bytes(), 0);
        EXPECT_EQ(port.writes, 0);
        EXPECT_EQ(port.aborts, 1);
        trial.cancel();
    }
    EXPECT_EQ(port.aborts, 1);
}

TEST_F(OtaIngestionTests, OversizedChunkIsRejectedWholeBeforeWrite) {
    ASSERT_TRUE(begin());
    ASSERT_TRUE(ingestion.append(bytes.data(), 2));
    EXPECT_FALSE(ingestion.append(bytes.data(), 2));
    EXPECT_EQ(ingestion.status(), IngestionStatus::TooManyBytes);
    EXPECT_EQ(ingestion.received_bytes(), 2);
    EXPECT_EQ(port.writes, 1);
    EXPECT_EQ(port.stored.size(), 2);
    EXPECT_EQ(port.aborts, 1);
    EXPECT_FALSE(ingestion.finish());
    EXPECT_FALSE(ingestion.append(bytes.data(), 1));
}

TEST_F(OtaIngestionTests, TrailingByteAfterExactSizeStillAborts) {
    ASSERT_TRUE(begin());
    ASSERT_TRUE(ingestion.append(bytes.data(), bytes.size()));
    EXPECT_FALSE(ingestion.append(bytes.data(), 1));
    EXPECT_EQ(ingestion.status(), IngestionStatus::TooManyBytes);
    EXPECT_EQ(port.writes, 1);
    EXPECT_EQ(port.finishes, 0);
    EXPECT_EQ(port.aborts, 1);
}

TEST_F(OtaIngestionTests, WriteFailureDoesNotCountPartialOrFailedChunk) {
    ASSERT_TRUE(begin());
    ASSERT_TRUE(ingestion.append(bytes.data(), 1));
    port.write_ok = false;
    EXPECT_FALSE(ingestion.append(bytes.data() + 1, 2));
    EXPECT_EQ(ingestion.status(), IngestionStatus::WriteFailed);
    EXPECT_EQ(ingestion.received_bytes(), 1);
    // The port simulates a write that stored bytes before reporting failure.
    EXPECT_EQ(port.stored.size(), bytes.size());
    EXPECT_EQ(port.aborts, 1);
    EXPECT_FALSE(ingestion.finish());
    EXPECT_EQ(port.finishes, 0);
}

TEST_F(OtaIngestionTests, TruncationDoesNotFinalizeDigest) {
    ASSERT_TRUE(begin());
    ASSERT_TRUE(ingestion.append(bytes.data(), 2));
    EXPECT_FALSE(ingestion.finish());
    EXPECT_EQ(ingestion.status(), IngestionStatus::Truncated);
    EXPECT_EQ(port.finishes, 0);
    EXPECT_EQ(port.aborts, 1);
}

TEST_F(OtaIngestionTests, FailedDigestCannotAcceptEvenMatchingOutput) {
    ASSERT_TRUE(begin());
    ASSERT_TRUE(ingestion.append(bytes.data(), bytes.size()));
    port.digest_ok = false;
    EXPECT_FALSE(ingestion.finish());
    EXPECT_EQ(ingestion.status(), IngestionStatus::DigestFailed);
    EXPECT_EQ(port.aborts, 1);
}

TEST_F(OtaIngestionTests, EveryDigestByteParticipatesInComparison) {
    for (std::size_t index = 0; index < port.digest.size(); ++index) {
        SCOPED_TRACE(index);
        RecordingPort altered;
        altered.digest[index] = 1;
        {
            ImageIngestion trial(altered);
            ASSERT_TRUE(trial.begin(candidate, device, DowngradePolicy::Reject));
            ASSERT_TRUE(trial.append(bytes.data(), bytes.size()));
            EXPECT_FALSE(trial.finish());
            EXPECT_EQ(trial.status(), IngestionStatus::DigestMismatch);
            trial.cancel();
        }
        EXPECT_EQ(altered.aborts, 1);
    }
}

TEST_F(OtaIngestionTests, DigestHexAcceptsBothCasesAndAllNibbles) {
    constexpr auto hex = "0123456789aBcDeF";
    for (std::size_t index = 0; index < digest_text.size(); ++index) {
        digest_text[index] = hex[index % 16];
    }
    for (std::size_t index = 0; index < port.digest.size(); ++index) {
        port.digest[index] =
            static_cast<std::uint8_t>((index * 2 % 16) * 16 + (index * 2 + 1) % 16);
    }
    ASSERT_TRUE(begin());
    ASSERT_TRUE(ingestion.append(bytes.data(), bytes.size()));
    EXPECT_TRUE(ingestion.finish());
}

TEST_F(OtaIngestionTests, MetadataStorageMayChangeAfterBegin) {
    ASSERT_TRUE(begin());
    std::fill(digest_text.begin(), digest_text.end(), 'f');
    candidate.image_size = 1;
    candidate.sha256 = "gone";
    ASSERT_TRUE(ingestion.append(bytes.data(), bytes.size()));
    EXPECT_TRUE(ingestion.finish());
    EXPECT_EQ(port.declared_size, bytes.size());
}

TEST_F(OtaIngestionTests, CancelAndDestructionReleaseEachOwnedSessionOnce) {
    for (const bool finish_first : {false, true}) {
        SCOPED_TRACE(finish_first);
        RecordingPort trial_port;
        {
            ImageIngestion trial(trial_port);
            ASSERT_TRUE(trial.begin(candidate, device, DowngradePolicy::Reject));
            if (finish_first) {
                ASSERT_TRUE(trial.append(bytes.data(), bytes.size()));
                ASSERT_TRUE(trial.finish());
            }
            trial.cancel();
            trial.cancel();
            EXPECT_EQ(trial.status(), IngestionStatus::Cancelled);
            EXPECT_EQ(trial_port.aborts, 1);
            EXPECT_FALSE(trial.append(bytes.data(), bytes.size()));
            EXPECT_FALSE(trial.finish());
        }
        EXPECT_EQ(trial_port.aborts, 1);
    }
}

TEST_F(OtaIngestionTests, DestructionAloneReleasesReceivingAndDigestMatchedSessions) {
    for (const bool finish_first : {false, true}) {
        RecordingPort trial_port;
        {
            ImageIngestion trial(trial_port);
            ASSERT_TRUE(trial.begin(candidate, device, DowngradePolicy::Reject));
            if (finish_first) {
                ASSERT_TRUE(trial.append(bytes.data(), bytes.size()));
                ASSERT_TRUE(trial.finish());
            }
            EXPECT_EQ(trial_port.aborts, 0);
        }
        EXPECT_EQ(trial_port.aborts, 1);
    }
}

TEST_F(OtaIngestionTests, CancellationBeforeBeginNeverOpensOrAborts) {
    ingestion.cancel();
    EXPECT_FALSE(begin());
    EXPECT_EQ(ingestion.status(), IngestionStatus::Cancelled);
    EXPECT_EQ(port.begins, 0);
    EXPECT_EQ(port.aborts, 0);
}

TEST_F(OtaIngestionTests, CounterBoundaryCannotWrapIntoAnAcceptedChunk) {
    if constexpr (sizeof(std::size_t) < sizeof(std::uint64_t)) {
        GTEST_SKIP() << "64-bit host required for synthetic UINT64_MAX chunk";
    }
    candidate.image_size = std::numeric_limits<std::uint64_t>::max();
    device.inactive_slot_size = candidate.image_size;
    port.capture_bytes = false; // Exercise counting without claiming a UINT64_MAX allocation.
    ASSERT_TRUE(begin());
    ASSERT_TRUE(ingestion.append(bytes.data(), 1));
    EXPECT_FALSE(ingestion.append(bytes.data(), std::numeric_limits<std::size_t>::max()));
    EXPECT_EQ(ingestion.status(), IngestionStatus::TooManyBytes);
    EXPECT_EQ(ingestion.received_bytes(), 1);
    EXPECT_EQ(port.writes, 1);
    EXPECT_EQ(port.aborts, 1);
}

} // namespace
