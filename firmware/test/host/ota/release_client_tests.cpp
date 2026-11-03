// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include "ota/release_client.hpp"

#include <gtest/gtest.h>

#include <string>

#include "ota/release_policy.hpp"

namespace prd::ota {
namespace {

struct FakeExchange {
    std::string body;
    network::PublicHttpsOutcome outcome{network::PublicHttpsOutcome::Ok};
    std::uint16_t status{200};
    bool saw_abandon{false};
    bool shape_valid{false};
    bool mismatch_length{false};
};

bool abandon(void* context) {
    *static_cast<bool*>(context) = true;
    return false;
}

network::PublicHttpsResponse exchange(void* context, const network::PublicHttpsRequest& request) {
    auto& fake = *static_cast<FakeExchange*>(context);
    fake.shape_valid =
        request.url == release_discovery_url() &&
        request.media == network::PublicMedia::GithubJson &&
        request.maximum_body_bytes == kReleaseBodyLimit && request.total_budget_ms == 15000 &&
        request.slice_ms == 1000 && request.connect_budget_ms == 5000 &&
        request.maximum_redirects == 0 && request.url_policy != nullptr &&
        request.url_policy_context != nullptr && request.sink != nullptr &&
        request.sink_context != nullptr &&
        request.url_policy(request.url_policy_context, {}, request.url, 0) &&
        !request.url_policy(request.url_policy_context, {}, "https://example.invalid/latest", 0);
    if (request.abandon != nullptr) {
        (void)request.abandon(request.abandon_context);
        fake.saw_abandon = true;
    }

    network::PublicHttpsResponse response;
    response.outcome = fake.outcome;
    response.status = fake.status;
    if (response.outcome == network::PublicHttpsOutcome::Ok) {
        const auto half = fake.body.size() / 2;
        if (!request.sink(request.sink_context,
                          reinterpret_cast<const std::uint8_t*>(fake.body.data()), half) ||
            !request.sink(request.sink_context,
                          reinterpret_cast<const std::uint8_t*>(fake.body.data() + half),
                          fake.body.size() - half)) {
            response.outcome = network::PublicHttpsOutcome::SinkRejected;
        }
        response.body_length = fake.body.size() + (fake.mismatch_length ? 1U : 0U);
    }
    return response;
}

std::string valid_release(const bool immutable = true) {
    return std::string{"{\"id\":42,\"url\":\"https://api.github.com/repos/bigMOTOR/"
                       "prusa-remote-display/releases/42\",\"tag_name\":\"v1.2.3\","
                       "\"draft\":false,\"prerelease\":false,\"immutable\":"} +
           (immutable ? "true" : "false") +
           ",\"assets\":[{\"id\":10,\"name\":\"manifest.json\",\"size\":1024,"
           "\"browser_download_url\":\"https://github.com/bigMOTOR/prusa-remote-display/"
           "releases/download/v1.2.3/manifest.json\"},{\"id\":11,"
           "\"name\":\"prusa_remote_display.bin\",\"size\":1280000,"
           "\"browser_download_url\":\"https://github.com/bigMOTOR/prusa-remote-display/"
           "releases/download/v1.2.3/prusa_remote_display.bin\"}]}";
}

TEST(ReleaseClientTests, BindsTheExactPublicRequestStreamAndImmutableDecoder) {
    FakeExchange fake{valid_release()};
    ReleaseBodyBuffer body;
    bool abandon_checked = false;
    const auto result = discover_latest_release(exchange, &fake, body, abandon, &abandon_checked);

    EXPECT_TRUE(fake.shape_valid);
    EXPECT_TRUE(fake.saw_abandon);
    EXPECT_TRUE(abandon_checked);
    EXPECT_EQ(result.status, ReleaseFetchStatus::Found);
    EXPECT_EQ(result.transport.outcome, network::PublicHttpsOutcome::Ok);
    EXPECT_EQ(result.release, ReleaseStatus::Decoded);
    EXPECT_EQ(result.candidate.release_id, 42U);
    EXPECT_EQ(result.candidate.manifest.id, 10U);
    EXPECT_EQ(result.candidate.image.id, 11U);
    EXPECT_EQ(body.length, fake.body.size());
}

TEST(ReleaseClientTests, MutableReleaseIsRejectedAfterASuccessfulHttpsExchange) {
    FakeExchange fake{valid_release(false)};
    ReleaseBodyBuffer body;
    const auto result = discover_latest_release(exchange, &fake, body);

    EXPECT_TRUE(fake.shape_valid);
    EXPECT_EQ(result.transport.outcome, network::PublicHttpsOutcome::Ok);
    EXPECT_EQ(result.status, ReleaseFetchStatus::ReleaseRejected);
    EXPECT_EQ(result.release, ReleaseStatus::NotImmutable);
    EXPECT_EQ(result.candidate.release_id, 0U);
}

TEST(ReleaseClientTests, TransportFailureAndAccountingMismatchNeverReachDiscovery) {
    FakeExchange failed{valid_release(), network::PublicHttpsOutcome::Timeout, 0};
    ReleaseBodyBuffer body;
    auto result = discover_latest_release(exchange, &failed, body);
    EXPECT_EQ(result.status, ReleaseFetchStatus::TransportFailed);
    EXPECT_EQ(result.release, ReleaseStatus::InvalidField);
    EXPECT_EQ(body.length, 0U);

    FakeExchange mismatch{valid_release()};
    mismatch.mismatch_length = true;
    result = discover_latest_release(exchange, &mismatch, body);
    EXPECT_EQ(result.status, ReleaseFetchStatus::TransportFailed);
    EXPECT_EQ(result.release, ReleaseStatus::InvalidField);
    EXPECT_EQ(result.candidate.release_id, 0U);
}

TEST(ReleaseClientTests, NullTransportIsAClosedFailureAndClearsTheBuffer) {
    ReleaseBodyBuffer body;
    body.length = 17;
    const auto result = discover_latest_release(nullptr, nullptr, body);
    EXPECT_EQ(result.status, ReleaseFetchStatus::TransportFailed);
    EXPECT_EQ(result.transport.outcome, network::PublicHttpsOutcome::Rejected);
    EXPECT_EQ(body.length, 0U);
}

} // namespace
} // namespace prd::ota
