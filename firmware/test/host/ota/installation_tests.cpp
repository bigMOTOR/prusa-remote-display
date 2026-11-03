// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only
#include "app_model/events.hpp"
#include "exclusive_operation.hpp"
#include "ota/installation.hpp"
#include <algorithm>
#include <gtest/gtest.h>
#include <limits>
#include <string>
#include <vector>

namespace prd::ota {
namespace {
constexpr std::string_view digest_text =
    "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
std::string manifest(std::uint64_t size = 3, std::string_view version = "1.2.3") {
    return "{\"schemaVersion\":1,\"boardTarget\":\"board\",\"chipTarget\":\"esp32s3\","
           "\"productVersion\":\"" +
           std::string(version) +
           "\",\"compatibility\":{\"minBootloaderSchema\":1},\"image\":{\"sizeBytes\":" +
           std::to_string(size) + ",\"sha256\":\"" + std::string(digest_text) + "\"}}";
}
struct Port final : InstallationPort {
    std::vector<std::string> calls;
    InstallationLease current;
    unsigned sequence{0};
    bool refuse{false};
    bool activation_locked{false};
    bool verify_ok{true};
    bool digest_ok{true};
    bool write_ok{true};
    bool select_ok{true};
    bool select_uncertain{false};
    bool wrong_identity{false};
    bool wrong_digest{false};
    IngestionBegin begin_result{IngestionBegin::Started};
    ImageIdentity identity;
    Port() {
        std::copy_n("board", 5, identity.board.begin());
        std::copy_n("1.2.3", 5, identity.version.begin());
    }
    InstallationLease reserve() override {
        calls.emplace_back("reserve");
        if (refuse || current.valid()) {
            return {};
        }
        current = lease_for(++sequence);
        return current;
    }
    IngestionBegin begin(InstallationLease lease, std::uint64_t) override {
        EXPECT_TRUE(lease == current);
        calls.emplace_back("begin");
        return begin_result;
    }
    bool write(InstallationLease lease, const std::uint8_t*, std::size_t) override {
        EXPECT_TRUE(lease == current);
        calls.emplace_back("write");
        return write_ok;
    }
    bool finish_digest(InstallationLease lease, ImageDigest& result) override {
        EXPECT_TRUE(lease == current);
        calls.emplace_back("digest");
        constexpr std::string_view hex = "0123456789abcdef";
        for (std::size_t index = 0; index < result.size(); ++index) {
            result[index] = static_cast<std::uint8_t>((hex.find(digest_text[index * 2]) * 16) +
                                                      hex.find(digest_text[(index * 2) + 1]));
        }
        if (wrong_digest) {
            ++result[0];
        }
        return digest_ok;
    }
    bool finalize(InstallationLease lease, ImageIdentity& result) override {
        EXPECT_TRUE(lease == current);
        calls.emplace_back("verify");
        result = identity;
        if (wrong_identity) {
            result.board[0] = 'x';
        }
        return verify_ok;
    }
    void abort(InstallationLease lease) override {
        EXPECT_TRUE(lease == current);
        calls.emplace_back("abort");
    }
    SelectionResult select(InstallationLease lease, const Candidate& candidate) override {
        EXPECT_TRUE(lease == current);
        EXPECT_EQ(candidate.board, "board");
        EXPECT_EQ(candidate.sha256, digest_text);
        calls.emplace_back("select");
        activation_locked = select_ok || select_uncertain;
        return select_uncertain ? SelectionResult::Uncertain
               : select_ok      ? SelectionResult::Selected
                                : SelectionResult::Rejected;
    }
    void release(InstallationLease lease) override {
        EXPECT_TRUE(lease == current);
        calls.emplace_back("release");
        if (!activation_locked) {
            current = {};
        }
    }
    unsigned count(std::string_view value) const {
        return static_cast<unsigned>(std::count(calls.begin(), calls.end(), value));
    }
};
struct InstallationTests : ::testing::Test {
    app_core::exclusive::Conditions live;
    app_core::exclusive::FirmwareUpdateAdmission admission;
    Port port;
    DeviceCompatibility device{"board", "esp32s3", "1.2.2", 1,
                               std::numeric_limits<std::uint64_t>::max()};
    std::array<std::uint8_t, 3> bytes{'a', 'b', 'c'};
    bool start(Installation& transaction) {
        if (!admission.authorize(live)) {
            return false;
        }
        return transaction.request(manifest(), device, DowngradePolicy::Reject);
    }
    void receive(Installation& transaction) {
        ASSERT_TRUE(start(transaction));
        ASSERT_TRUE(transaction.append(bytes.data(), bytes.size()));
    }
};

TEST_F(InstallationTests, OwnsInputAndSelectsOnlyAfterExactOrderedVerification) {
    {
        Installation transaction{port, admission};
        auto input = manifest();
        ASSERT_TRUE(admission.authorize(live));
        ASSERT_TRUE(transaction.request(input, device, DowngradePolicy::Reject));
        input.assign(input.size(), 'x');
        app_model::OtaObservation output;
        ASSERT_TRUE(transaction.observation(output));
        EXPECT_EQ(output.phase, app_model::OtaPhase::Downloading);
        EXPECT_EQ(output.available_version.view(), "1.2.3");
        EXPECT_TRUE(admission.held());
        EXPECT_FALSE(transaction.activate());
        EXPECT_FALSE(transaction.verify());
        ASSERT_TRUE(transaction.append(bytes.data(), bytes.size()));
        ASSERT_TRUE(transaction.observation(output));
        EXPECT_EQ(output.progress.percent, 100);
        EXPECT_EQ(output.phase, app_model::OtaPhase::Downloading);
        ASSERT_TRUE(transaction.finish());
        ASSERT_TRUE(transaction.observation(output));
        EXPECT_EQ(output.phase, app_model::OtaPhase::Verifying);
        ASSERT_TRUE(transaction.verify());
        ASSERT_TRUE(transaction.observation(output));
        EXPECT_EQ(output.phase, app_model::OtaPhase::Installing);
        ASSERT_TRUE(transaction.activate());
        ASSERT_TRUE(transaction.observation(output));
        EXPECT_EQ(output.phase, app_model::OtaPhase::RebootRequired);
        EXPECT_FALSE(transaction.activate());
        EXPECT_FALSE(transaction.verify());
        EXPECT_FALSE(transaction.finish());
        EXPECT_FALSE(start(transaction));
        transaction.cancel();
        EXPECT_EQ(transaction.status(), InstallationStatus::RebootRequired);
        EXPECT_EQ(port.count("abort"), 0U);
    }
    EXPECT_EQ(port.calls, (std::vector<std::string>{"reserve", "begin", "write", "digest", "verify",
                                                    "select", "release"}));
    EXPECT_TRUE(port.current.valid());
    EXPECT_TRUE(admission.held());
    EXPECT_TRUE(admission.activation_locked());
    Installation next{port, admission};
    EXPECT_FALSE(start(next));
    EXPECT_EQ(port.count("reserve"), 1U);
}

TEST_F(InstallationTests, RefusedRequestCannotReleaseOrPublishOverIncumbent) {
    Installation incumbent{port, admission};
    ASSERT_TRUE(start(incumbent));
    {
        Installation other{port, admission};
        EXPECT_FALSE(other.request(manifest(), device, DowngradePolicy::Reject));
        EXPECT_EQ(other.status(), InstallationStatus::Refused);
        app_model::OtaObservation output;
        EXPECT_FALSE(other.observation(output));
        other.cancel();
    }
    EXPECT_TRUE(admission.held());
    EXPECT_EQ(port.count("reserve"), 1U);
    EXPECT_EQ(port.count("abort"), 0U);
    EXPECT_EQ(port.count("release"), 0U);
    ASSERT_TRUE(incumbent.append(bytes.data(), bytes.size()));
}

TEST_F(InstallationTests, UncertainActivationRetainsAdmissionWithoutSuccessOrRebootClaim) {
    {
        Installation transaction{port, admission};
        receive(transaction);
        ASSERT_TRUE(transaction.finish());
        ASSERT_TRUE(transaction.verify());
        port.select_uncertain = true;
        EXPECT_FALSE(transaction.activate());
        EXPECT_EQ(transaction.status(), InstallationStatus::ActivationUncertain);
        EXPECT_FALSE(transaction.activate());
        transaction.cancel();
        EXPECT_EQ(transaction.status(), InstallationStatus::ActivationUncertain);
        app_model::OtaObservation observation;
        ASSERT_TRUE(transaction.observation(observation));
        EXPECT_EQ(observation.phase, app_model::OtaPhase::Failed);
        EXPECT_TRUE(admission.held());
        // Failed does not globally become exclusive. The actual owner feeds the
        // retained admission into the same guards, independently of observation.
        live.firmware_update_active = admission.held();
        EXPECT_FALSE(app_core::exclusive::firmware_update_permitted(live));
        EXPECT_FALSE(app_core::exclusive::provisioning_work_permitted(live));
        EXPECT_FALSE(app_core::exclusive::candidate_activation_permitted(live));
    }
    EXPECT_TRUE(admission.held());
    EXPECT_TRUE(admission.activation_locked());
    EXPECT_EQ(port.count("select"), 1U);
    EXPECT_TRUE(port.current.valid());
    EXPECT_EQ(port.count("abort"), 0U);
    Installation next{port, admission};
    EXPECT_FALSE(start(next));
}

TEST_F(InstallationTests, InvalidOrIncompatibleMetadataNeverTouchesAdmissionOrPort) {
    for (auto input : {std::string("{}"), manifest(0), manifest(3, "0.1.0")}) {
        Installation transaction{port, admission};
        EXPECT_FALSE(transaction.request(input, device, DowngradePolicy::Reject));
        app_model::OtaObservation output;
        EXPECT_FALSE(transaction.observation(output));
        EXPECT_FALSE(admission.held());
    }
    EXPECT_TRUE(port.calls.empty());
}

TEST_F(InstallationTests, EveryFailureReleasesAndNeverSelectsExceptItsOwnSelectionAttempt) {
    for (unsigned scenario = 0; scenario < 9; ++scenario) {
        Port local;
        local.begin_result = scenario == 0   ? IngestionBegin::Refused
                             : scenario == 1 ? IngestionBegin::FailedNeedsAbort
                                             : IngestionBegin::Started;
        local.write_ok = scenario != 2;
        local.digest_ok = scenario != 3;
        local.wrong_digest = scenario == 4;
        local.verify_ok = scenario != 5;
        local.wrong_identity = scenario == 6;
        local.select_ok = scenario != 7;
        {
            Installation transaction{local, admission};
            if (start(transaction) && transaction.append(bytes.data(), scenario == 8 ? 2 : 3) &&
                transaction.finish() && transaction.verify()) {
                EXPECT_FALSE(transaction.activate());
            }
            EXPECT_EQ(transaction.status(),
                      scenario == 0 ? InstallationStatus::Refused : InstallationStatus::Failed);
            app_model::OtaObservation output;
            EXPECT_EQ(transaction.observation(output), scenario != 0);
            if (scenario != 0) {
                EXPECT_EQ(output.phase, app_model::OtaPhase::Failed);
                EXPECT_FALSE(output.error.empty());
            }
            transaction.cancel();
        }
        EXPECT_FALSE(admission.held());
        EXPECT_FALSE(local.current.valid());
        EXPECT_EQ(local.count("release"), 1U);
        EXPECT_EQ(local.count("select"), scenario == 7 ? 1U : 0U);
        EXPECT_EQ(local.count("abort"), scenario >= 1 && scenario <= 4 ? 1U
                                        : scenario == 8                ? 1U
                                                                       : 0U);
    }
}

TEST_F(InstallationTests, CancellationAndDestructionBeforeSelectionReleaseExactlyOnce) {
    for (unsigned step = 0; step < 3; ++step) {
        for (bool cancel : {false, true}) {
            Port local;
            {
                Installation transaction{local, admission};
                receive(transaction);
                if (step >= 1) {
                    ASSERT_TRUE(transaction.finish());
                }
                if (step >= 2) {
                    ASSERT_TRUE(transaction.verify());
                }
                if (cancel) {
                    transaction.cancel();
                    transaction.cancel();
                }
            }
            EXPECT_FALSE(admission.held());
            EXPECT_FALSE(local.current.valid());
            EXPECT_EQ(local.count("select"), 0U);
            EXPECT_EQ(local.count("release"), 1U);
            EXPECT_EQ(local.count("abort"), step < 2 ? 1U : 0U);
        }
    }
}

TEST_F(InstallationTests, PortReservationRefusalReleasesOnlyItsAdmissionAndHasNoObservation) {
    port.refuse = true;
    {
        Installation transaction{port, admission};
        EXPECT_FALSE(start(transaction));
        app_model::OtaObservation output;
        EXPECT_FALSE(transaction.observation(output));
    }
    EXPECT_FALSE(admission.held());
    EXPECT_EQ(port.calls, (std::vector<std::string>{"reserve"}));
}

TEST_F(InstallationTests, FullSupportedVersionIsNotTruncatedInObservation) {
    const std::string version = "1.2.3+" + std::string(25, 'a');
    ASSERT_EQ(version.size(), 31U);
    Installation transaction{port, admission};
    ASSERT_TRUE(admission.authorize(live));
    ASSERT_TRUE(transaction.request(manifest(3, version), device, DowngradePolicy::Reject));
    app_model::OtaObservation output;
    ASSERT_TRUE(transaction.observation(output));
    EXPECT_EQ(output.available_version.view(), version);
    EXPECT_FALSE(output.available_version.truncated());
}

TEST_F(InstallationTests, ProgressIsBoundedForTinyAndMaximumSizes) {
    for (const auto size :
         {std::uint64_t{1}, std::uint64_t{3}, std::numeric_limits<std::uint64_t>::max()}) {
        Installation transaction{port, admission};
        ASSERT_TRUE(admission.authorize(live));
        ASSERT_TRUE(transaction.request(manifest(size), device, DowngradePolicy::Reject));
        app_model::OtaObservation output;
        ASSERT_TRUE(transaction.observation(output));
        EXPECT_EQ(output.progress.percent, 0);
        // Mock port deliberately does not dereference; tests uint64 accounting,
        // not a real UINT64_MAX-byte buffer or target partition.
        ASSERT_TRUE(transaction.append(bytes.data(), static_cast<std::size_t>(size - 1)));
        ASSERT_TRUE(transaction.observation(output));
        EXPECT_EQ(output.progress.percent, size == 1 ? 0 : size == 3 ? 66 : 99);
        ASSERT_TRUE(transaction.append(bytes.data(), 1));
        ASSERT_TRUE(transaction.observation(output));
        EXPECT_EQ(output.progress.percent, 100);
        EXPECT_EQ(output.phase, app_model::OtaPhase::Downloading);
    }
}
} // namespace
} // namespace prd::ota
