// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "diagnostics/task_report.hpp"
#include "network/http.hpp"
#include "network/lan.hpp"
#include "prusalink/prusalink.hpp"

namespace {

using prd::app_model::PrinterValidationFailure;
using prd::app_model::PrinterValidationReason;
using prd::app_model::PrinterValidationStage;
using prd::network::HttpOutcome;
using prd::network::lan_policy::Resolution;
using prd::network::lan_policy::ResolveOutcome;
namespace prusalink = prd::prusalink;

constexpr std::string_view kVersion =
    R"({"api":"2.0.0","version":"2.1.2","printer":"1.3.1","text":"PrusaLink","firmware":"6.5.7"})";
constexpr std::string_view kHost = "test-printer.local";
constexpr Resolution kLanAddress{ResolveOutcome::Ok, {{192, 168, 204, 7}}};

struct Reply {
    HttpOutcome outcome;
    std::uint16_t status;
    std::string_view body{};
    bool stop_worker{false};
};

struct Script {
    Resolution resolution{kLanAddress};
    std::vector<Reply> replies;
    std::vector<std::string> paths;
    std::vector<prusalink::Report> reports;
};

Script script;

void receive(const prusalink::Report& report) {
    script.reports.push_back(report);
}

class WorkerValidationTests : public testing::Test {
  protected:
    prusalink::Target target;

    void SetUp() override {
        script = {};
        ASSERT_TRUE(target.host.assign(kHost));
        ASSERT_TRUE(target.api_key.assign("test-key-marker"));
        target.auth_method = prd::app_model::AuthMethod::ApiKey;
        target.transaction = {11, 3};
    }

    void validate(std::initializer_list<Reply> replies, Resolution resolution = kLanAddress) {
        script.resolution = resolution;
        script.replies.assign(replies);

        const auto started = prusalink::start(target, receive);
        ASSERT_TRUE(started.started);
        EXPECT_FALSE(prusalink::running());
        ASSERT_EQ(script.reports.size(), 1U);
        const auto& report = script.reports.front();
        EXPECT_EQ(report.generation, started.generation);
        EXPECT_EQ(report.transaction, target.transaction);
        EXPECT_EQ(report.verdict, prusalink::Verdict::Failed);
        EXPECT_FALSE(report.connection_present || report.status_present || report.identity_present);
    }
};

} // namespace

namespace prd::network {

std::string_view log_label(HttpOutcome) {
    return "scripted-http-outcome";
}

HttpResponse get(const HttpRequest& request, BodyBuffer& buffer) {
    const auto index = script.paths.size();
    EXPECT_LT(index, script.replies.size());
    if (index >= script.replies.size()) {
        return {};
    }
    constexpr std::string_view kAuthority = "http://192.168.204.7:80";
    EXPECT_EQ(request.url.substr(0, kAuthority.size()), kAuthority);
    script.paths.emplace_back(request.url.substr(kAuthority.size()));
    const auto& reply = script.replies[index];
    if (reply.stop_worker && request.abandon != nullptr) {
        EXPECT_FALSE(request.abandon(request.abandon_context));
        prusalink::stop();
        if (request.abandon(request.abandon_context)) {
            return {HttpOutcome::Cancelled, 0, 1, 0};
        }
    }
    EXPECT_FALSE(reply.stop_worker) << "The in-flight GET should observe supersession";
    EXPECT_TRUE(buffer.append(reply.body));
    return {reply.outcome, reply.status, 1, buffer.length()};
}

namespace lan_policy {
Resolution resolve(std::string_view host) {
    EXPECT_EQ(host, kHost);
    return script.resolution;
}

std::string_view log_label(ResolveOutcome) {
    return "scripted-resolution";
}
} // namespace lan_policy

} // namespace prd::network

void prd::diagnostics::log_task_stack(std::string_view, std::uint32_t) {}

namespace {

TEST_F(WorkerValidationTests, AddressFailureNeverCallsHttp) {
    ASSERT_NO_FATAL_FAILURE(validate({}, {ResolveOutcome::NotFound, {}}));
    EXPECT_TRUE(script.paths.empty());
    EXPECT_EQ(script.reports.front().validation_failure,
              (PrinterValidationFailure{PrinterValidationStage::AddressCheck,
                                        PrinterValidationReason::NameNotResolved, 0}));
}

TEST_F(WorkerValidationTests, RequestRejectionAfterVersionNamesRequestPreparation) {
    ASSERT_NO_FATAL_FAILURE(
        validate({{HttpOutcome::Ok, 200, kVersion}, {HttpOutcome::Rejected, 0}}));
    EXPECT_EQ(script.paths, (std::vector<std::string>{"/api/version", "/api/v1/status"}));
    EXPECT_EQ(script.reports.front().validation_failure,
              (PrinterValidationFailure{PrinterValidationStage::RequestPreparation,
                                        PrinterValidationReason::RequestRejected, 0}));
}

TEST_F(WorkerValidationTests, VersionBodyFailureKeepsDenialStatusAndSkipsStatusRequest) {
    ASSERT_NO_FATAL_FAILURE(validate({{HttpOutcome::TransportFailed, 403}}));
    EXPECT_EQ(script.paths, (std::vector<std::string>{"/api/version"}));
    EXPECT_EQ(script.reports.front().validation_failure,
              (PrinterValidationFailure{PrinterValidationStage::VersionProbe,
                                        PrinterValidationReason::TransportFailed, 403}));
}

TEST_F(WorkerValidationTests, OutOfRangeStatusIsNotPublishedAsHeaderEvidence) {
    for (const std::uint16_t status : {std::uint16_t{99}, std::uint16_t{600}}) {
        SCOPED_TRACE(status);
        script = {};
        ASSERT_NO_FATAL_FAILURE(validate({{HttpOutcome::TransportFailed, status}}));
        EXPECT_EQ(script.reports.front().validation_failure,
                  (PrinterValidationFailure{PrinterValidationStage::VersionProbe,
                                            PrinterValidationReason::TransportFailed, 0}));
    }
}

TEST_F(WorkerValidationTests, InvalidStatusOnCompletedResponseKeepsReasonWithoutCode) {
    ASSERT_NO_FATAL_FAILURE(validate({{HttpOutcome::Ok, 200, kVersion}, {HttpOutcome::Ok, 600}}));
    EXPECT_EQ(script.paths, (std::vector<std::string>{"/api/version", "/api/v1/status"}));
    EXPECT_EQ(script.reports.front().failure_phase,
              prd::app_model::ConnectionPhase::PrinterUnavailable);
    EXPECT_EQ(script.reports.front().validation_failure,
              (PrinterValidationFailure{PrinterValidationStage::Status,
                                        PrinterValidationReason::PrinterError, 0}));
}

TEST_F(WorkerValidationTests, ServerErrorAfterVersionNamesStatusAndKeepsCode) {
    ASSERT_NO_FATAL_FAILURE(validate({{HttpOutcome::Ok, 200, kVersion}, {HttpOutcome::Ok, 503}}));
    EXPECT_EQ(script.paths, (std::vector<std::string>{"/api/version", "/api/v1/status"}));
    EXPECT_EQ(script.reports.front().validation_failure,
              (PrinterValidationFailure{PrinterValidationStage::Status,
                                        PrinterValidationReason::PrinterError, 503}));
}

TEST_F(WorkerValidationTests, StopDuringVersionRequestCancelsAndDropsReport) {
    script.replies = {{HttpOutcome::Ok, 200, kVersion, true}};
    const auto started = prusalink::start(target, receive);
    ASSERT_TRUE(started.started);
    EXPECT_FALSE(prusalink::running());
    EXPECT_EQ(script.paths, (std::vector<std::string>{"/api/version"}));
    EXPECT_TRUE(script.reports.empty());
}

} // namespace
