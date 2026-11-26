// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include <algorithm>
#include <cstring>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "app_model/build_info.hpp"
#include "diagnostics/log.hpp"
#include "esp_http_server.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "json_fixture.hpp"
#include "json_reader/reader.hpp"
#include "lwip/sockets.h"
#include "provisioning/server.hpp"
#include "gtest/gtest.h"

namespace {
using namespace prd::provisioning;

constexpr auto kAccessPoint = "192.0.2.1";
constexpr auto kLan = "192.0.2.2";
constexpr auto kOrigin = "http://192.0.2.1";
constexpr auto kReport = "/api/diagnostic-report";
constexpr auto kSetup = "/api/setup";
constexpr auto kUpdate = "/api/update";
constexpr auto kDisplay = "/api/display";
using host_json::Document;

struct Body {
    std::string text;
    Document document;
};

const Body kWifi{R"({"stage":"wifi","wifiSsid":"Fixture Network","wifiPassword":""})",
                 {Document::Kind::Object,
                  {{"stage", "wifi"}, {"wifiSsid", "Fixture Network"}, {"wifiPassword", ""}},
                  {}}};
const Body kInvalidWifi{R"({"stage":"wifi","wifiSsid":""})",
                        {Document::Kind::Object, {{"stage", "wifi"}, {"wifiSsid", ""}}, {}}};
const Body kUpdateBody{"{}", {}};

std::string response_header(const std::map<std::string, std::string>& headers, const char* name) {
    const auto it = headers.find(name);
    return it == headers.end() ? std::string{} : it->second;
}

struct HttpExchange {
    std::string local_address{kAccessPoint};
    std::map<std::string, std::string> request_headers;
    Body request_body;
    std::function<void()> during_read;
    bool short_read{false};
    int read_calls{0};
    std::string content_type;
    int status{200};
    std::map<std::string, std::string> response_headers;
    std::string response_body;
};

struct Queue {
    std::vector<unsigned char> bytes;
    bool written{false};
};

Queue snapshot_queue;
std::map<std::pair<httpd_method_t, std::string>, httpd_uri_t> routes;
HttpExchange* exchange_in_progress = nullptr;
httpd_config_t config;
std::int64_t time_ms = 0;
std::uint32_t next_random = 0;

HttpExchange& from_request(httpd_req_t* request) {
    return *static_cast<HttpExchange*>(request->fixture);
}

std::string json_value(const std::string& body, const char* key) {
    using namespace prd::json_reader;
    JsonObject root;
    if (parse_object(body, root) != ScanStatus::Ok) {
        ADD_FAILURE() << "Expected a response JSON object";
        return {};
    }
    const auto field = find(root, key);
    if (!field.present()) {
        return {};
    }
    std::string value(field.raw.size(), '\0');
    std::size_t length = 0;
    EXPECT_TRUE(read_string(field, value.data(), value.size(), length));
    value.resize(length);
    return value;
}

Body report_body(const std::string& id) {
    return {"{\"stage\":\"diagnostic-report\",\"reportId\":\"" + id + "\"}",
            {Document::Kind::Object, {{"stage", "diagnostic-report"}, {"reportId", id}}, {}}};
}

void check_report_headers(const HttpExchange& result) {
    EXPECT_EQ(result.content_type, "application/json");
    EXPECT_EQ(response_header(result.response_headers, "Cache-Control"), "no-store");
    EXPECT_EQ(response_header(result.response_headers, "X-Content-Type-Options"), "nosniff");
}

class ProvisioningServerAdmission : public testing::Test {
  protected:
    struct SinkState {
        SubmissionHandoff handoff{SubmissionHandoff::Accepted};
        std::vector<Submitted> submissions;
        std::vector<DisplayRequest> display_actions;
        int updates{0};
    } sink;

    void SetUp() override {
        time_ms = 12000;
        next_random = 0;
        ASSERT_TRUE(start_session());
    }

    void TearDown() override {
        stop();
    }

    bool start_session() {
        return start({kAccessPoint, &accept_submission, &accept_update, &sink, &accept_display});
    }

    static SubmissionHandoff accept_submission(const Submitted& submission, void* context) {
        auto& state = *static_cast<SinkState*>(context);
        state.submissions.push_back(submission);
        return state.handoff;
    }

    static bool accept_display(const DisplayRequest& request, void* context) {
        static_cast<SinkState*>(context)->display_actions.push_back(request);
        return true;
    }

    static UpdateHandoff accept_update(void* context) {
        ++static_cast<SinkState*>(context)->updates;
        return UpdateHandoff::Accepted;
    }

    HttpExchange dispatch(httpd_method_t method, const char* path, HttpExchange exchange) {
        const auto route = routes.find({method, path});
        if (route == routes.end()) {
            ADD_FAILURE() << "Expected registered route " << path;
            return exchange;
        }
        EXPECT_EQ(exchange_in_progress, nullptr);
        exchange_in_progress = &exchange;
        httpd_req_t request{exchange.request_body.text.size(), &exchange};
        EXPECT_EQ(route->second.handler(&request), ESP_OK);
#ifdef PRD_SCRIPTED_CJSON
        host_json::clear_document();
#endif
        exchange_in_progress = nullptr;
        return exchange;
    }

    HttpExchange status() {
        auto response = dispatch(HTTP_GET, "/api/status", {});
        EXPECT_EQ(response.status, 200);
        return response;
    }

    HttpExchange make_post(Body body) {
        HttpExchange exchange;
        exchange.request_headers = {
            {"Origin", kOrigin},
            {"Content-Type", "application/json"},
            {"X-PRD-Setup-Token", json_value(status().response_body, "csrfToken")}};
        exchange.request_body = std::move(body);
        return exchange;
    }

    HttpExchange post(const char* path, Body body) {
        return dispatch(HTTP_POST, path, make_post(std::move(body)));
    }

    std::string publish_failure() {
        using prd::app_model::PrinterValidationReason;
        using prd::app_model::PrinterValidationStage;
        publish(SetupState::Failed, "printer-unauthorized",
                {PrinterValidationStage::VersionProbe, PrinterValidationReason::Unauthorized, 401});
        return json_value(status().response_body, "diagnosticReportId");
    }

    void refill() {
        time_ms += 1000;
    }
};

TEST_F(ProvisioningServerAdmission, DisplayActionsRequireActiveSetupSurfaceAndStrictBody) {
    const Body demo{R"("demo")", {}};
    const Body save{"[1,2]", {}};
    DisplaySettings settings;
    settings.adjustable = true;
    settings.demo_available = true;
    publish_display_settings(settings);

    auto lan = make_post(demo);
    lan.local_address = kLan;
    EXPECT_EQ(dispatch(HTTP_POST, kDisplay, std::move(lan)).status, 403);

    auto no_token = make_post(demo);
    no_token.request_headers.erase("X-PRD-Setup-Token");
    EXPECT_EQ(dispatch(HTTP_POST, kDisplay, std::move(no_token)).status, 403);

    auto changed_surface = make_post(demo);
    changed_surface.during_read = [] { publish_addresses({{}, kLan, {}}); };
    EXPECT_EQ(dispatch(HTTP_POST, kDisplay, std::move(changed_surface)).status, 503);
    publish_addresses({kAccessPoint, kLan, {}});
    EXPECT_TRUE(sink.display_actions.empty());

    EXPECT_EQ(post(kDisplay, demo).response_body, R"({"accepted":true})");
    EXPECT_EQ(post(kDisplay, save).response_body, R"({"accepted":true})");
    ASSERT_EQ(sink.display_actions.size(), 2U);
    EXPECT_EQ(sink.display_actions[0].action, DisplayAction::Demo);
    EXPECT_EQ(sink.display_actions[1].action, DisplayAction::Save);
    EXPECT_EQ(sink.display_actions[1].preferences.mode, prd::app_model::DisplayMode::PrinterStatus);
    EXPECT_EQ(sink.display_actions[1].preferences.backlight, prd::app_model::Backlight::High);

    refill();
    EXPECT_EQ(post(kDisplay, Body{"[1,9]", {}}).status, 400);
    refill();
    settings.demo_available = false;
    settings.adjustable = false;
    settings.demo_running = true;
    publish_display_settings(settings);
    EXPECT_EQ(post(kDisplay, demo).status, 503);
    EXPECT_EQ(sink.display_actions.size(), 2U);
}

TEST_F(ProvisioningServerAdmission, StartPublishesDisplaySettingsSeededBeforeServerExists) {
    stop();
    DisplaySettings initial;
    initial.preferences = {prd::app_model::DisplayMode::PrinterStatus,
                           prd::app_model::Backlight::Low};
    publish_display_settings(initial);
    ASSERT_TRUE(start_session());
    const auto snapshot = status().response_body;
    EXPECT_EQ(json_value(snapshot, "displayMode"), "1");
    EXPECT_EQ(json_value(snapshot, "backlight"), "0");
    EXPECT_NE(snapshot.find(R"("canShowDemo":false)"), std::string::npos);
}

TEST_F(ProvisioningServerAdmission, RegisteredAssetsSendExactBytesAndHeaders) {
    struct Asset {
        const char* route;
        const char* file;
        const char* type;
    };
    for (const auto& asset : {Asset{"/", "index.html", "text/html; charset=utf-8"},
                              Asset{"/style.css", "style.css", "text/css; charset=utf-8"},
                              Asset{"/app.js", "app.js", "text/javascript; charset=utf-8"}}) {
        SCOPED_TRACE(asset.route);
        std::ifstream file(std::string{PRD_PROVISIONING_ASSETS_DIR} + "/" + asset.file,
                           std::ios::binary);
        ASSERT_TRUE(file.is_open());
        const std::string expected((std::istreambuf_iterator<char>(file)), {});
        ASSERT_FALSE(expected.empty());
        const auto result = dispatch(HTTP_GET, asset.route, {});
        EXPECT_EQ(result.status, 200);
        EXPECT_EQ(result.content_type, asset.type);
        EXPECT_EQ(response_header(result.response_headers, "Cache-Control"), "no-store");
        EXPECT_EQ(response_header(result.response_headers, "X-Content-Type-Options"), "nosniff");
        EXPECT_EQ(result.response_body.size(), expected.size());
        EXPECT_TRUE(result.response_body == expected)
            << "Embedded response bytes differ from the file";
    }
}

TEST_F(ProvisioningServerAdmission, ReportUsesCurrentRevisionAndDoesNotHandOffSettings) {
    const auto id = publish_failure();
    ASSERT_FALSE(id.empty());
    auto result = post(kReport, report_body(id));
    ASSERT_EQ(result.status, 200);
    check_report_headers(result);
    EXPECT_EQ(result.read_calls, 1);
    EXPECT_NE(json_value(result.response_body, "diagnosticReport").find("HTTP status: 401"),
              std::string::npos);
    EXPECT_EQ(json_value(status().response_body, "diagnosticReportId"), id);
    EXPECT_TRUE(sink.submissions.empty());
    EXPECT_EQ(sink.updates, 0);

    refill();
    result = post(kReport, report_body("0"));
    EXPECT_EQ(result.status, 409);
    EXPECT_EQ(result.response_body, "{}");
    check_report_headers(result);
}

TEST_F(ProvisioningServerAdmission, WithdrawalDuringBodyReadClosesReportAdmission) {
    const auto id = publish_failure();
    ASSERT_FALSE(id.empty());
    auto exchange = make_post(report_body(id));
    exchange.during_read = [] { publish_addresses({"", kLan, ""}); };
    const auto result = dispatch(HTTP_POST, kReport, std::move(exchange));
    EXPECT_EQ(result.read_calls, 1);
    EXPECT_EQ(result.status, 409);
    EXPECT_EQ(result.response_body, "{}");
    check_report_headers(result);
    EXPECT_TRUE(sink.submissions.empty());
}

TEST_F(ProvisioningServerAdmission, RevisionChangeDuringBodyReadClosesReportAdmission) {
    const auto id = publish_failure();
    ASSERT_FALSE(id.empty());
    auto exchange = make_post(report_body(std::to_string(std::stoul(id) + 1)));
    exchange.during_read = [&] {
        using prd::app_model::PrinterValidationReason;
        using prd::app_model::PrinterValidationStage;
        publish(SetupState::Failed, "printer-unauthorized",
                {PrinterValidationStage::VersionProbe, PrinterValidationReason::Unauthorized, 401});
    };
    const auto result = dispatch(HTTP_POST, kReport, std::move(exchange));
    EXPECT_EQ(result.read_calls, 1);
    EXPECT_EQ(result.status, 409);
    EXPECT_EQ(result.response_body, "{}");
    check_report_headers(result);
    const auto newer_id = json_value(status().response_body, "diagnosticReportId");
    EXPECT_EQ(newer_id, std::to_string(std::stoul(id) + 1));
    refill();
    EXPECT_EQ(post(kReport, report_body(newer_id)).status, 200);
}

TEST_F(ProvisioningServerAdmission, ExactReportShapeAndNoFallbackToSettings) {
    const auto id = publish_failure();
    ASSERT_FALSE(id.empty());
    const auto base = report_body(id);
    const auto action = host_json::Field{"stage", "diagnostic-report"};
    auto extra_field = base;
    extra_field.text.insert(extra_field.text.size() - 1, R"(,"wifiSsid":"Fixture Network")");
    extra_field.document.fields.push_back({"wifiSsid", "Fixture Network"});

    // The scripted build delivers the DOM shape or parser end position that the
    // raw bytes produce with cJSON. The real-parser build uses the raw bytes alone.
    const std::vector<Body> wrong_shape{
        extra_field,
        {R"({"stage":"diagnostic-report","reportId":1})",
         {Document::Kind::Object, {action, {"reportId", std::nullopt}}, {}}},
        {R"({"stage":"diagnostic-report","diagnosticId":"1"})",
         {Document::Kind::Object, {action, {"diagnosticId", "1"}}, {}}},
        {R"({"stage":"diagnostic-report","reportId":""})",
         {Document::Kind::Object, {action, {"reportId", ""}}, {}}},
        {R"({"stage":"diagnostic-report","stage":"diagnostic-report"})",
         {Document::Kind::Object, {action, action}, {}}},
        {R"(["diagnostic-report","1"])", {Document::Kind::Array, {}, {}}},
        {base.text + " {}", {Document::Kind::Invalid, {}, {}}},
        {base.text + '\0' + "{}", {Document::Kind::Object, base.document.fields, base.text.size()}},
        {base.text.substr(0, base.text.size() - 2) + R"(\u0000extra"})", base.document},
        kWifi,
    };
    for (const auto& body : wrong_shape) {
        SCOPED_TRACE(body.text);
        const auto rejected = post(kReport, body);
        EXPECT_EQ(rejected.status, 400);
        EXPECT_EQ(rejected.read_calls, 1);
        check_report_headers(rejected);
        refill();
    }
    auto padded = base;
    padded.text = " \n\t" + base.text + "\r\n";
    const auto valid = post(kReport, std::move(padded));
    EXPECT_EQ(valid.status, 200);
    const auto legacy = post(kSetup, extra_field);
    EXPECT_EQ(legacy.status, 400);
    EXPECT_EQ(legacy.read_calls, 1);
    EXPECT_TRUE(sink.submissions.empty());
    EXPECT_EQ(sink.updates, 0);
}

TEST_F(ProvisioningServerAdmission, RetiresOnlyAcceptedHandoffAndResetsAtServerStart) {
    const auto id = publish_failure();
    ASSERT_EQ(id, "1");

    EXPECT_EQ(post(kSetup, kInvalidWifi).status, 200);
    EXPECT_TRUE(sink.submissions.empty());
    EXPECT_EQ(post(kReport, report_body(id)).status, 200);
    refill();

    sink.handoff = SubmissionHandoff::Unavailable;
    EXPECT_EQ(post(kSetup, kWifi).status, 503);
    ASSERT_EQ(sink.submissions.size(), 1u);
    EXPECT_EQ(post(kReport, report_body(id)).status, 200);
    refill();

    sink.handoff = SubmissionHandoff::Accepted;
    const auto accepted = post(kSetup, kWifi);
    EXPECT_EQ(accepted.status, 200);
    EXPECT_NE(accepted.response_body.find("\"accepted\":true"), std::string::npos);
    ASSERT_EQ(sink.submissions.size(), 2u);
    EXPECT_EQ(sink.submissions.back().stage, SubmissionStage::Wifi);
    EXPECT_EQ(sink.submissions.back().wifi_ssid.view(), "Fixture Network");
    EXPECT_TRUE(json_value(status().response_body, "diagnosticReportId").empty());
    const auto retired = post(kReport, report_body(id));
    EXPECT_EQ(retired.status, 409);
    check_report_headers(retired);

    const auto old_session = make_post(report_body(id));
    stop();
    ASSERT_TRUE(start_session());
    ASSERT_EQ(publish_failure(), id);
    EXPECT_EQ(dispatch(HTTP_POST, kReport, old_session).status, 403);
    EXPECT_EQ(post(kReport, report_body(id)).status, 200);
    EXPECT_EQ(sink.submissions.size(), 2u);
}

TEST_F(ProvisioningServerAdmission, BothRoutesEnforceThePreBodyGateAndReadLimits) {
    ASSERT_EQ(config.recv_wait_timeout, 5);
    ASSERT_EQ(config.send_wait_timeout, 5);
    const auto id = publish_failure();
    ASSERT_FALSE(id.empty());
    for (const auto* route : {kReport, kSetup}) {
        SCOPED_TRACE(route);
        auto request = make_post(report_body(id));
        publish_addresses({kAccessPoint, kLan, ""});
        request.local_address = kLan;
        auto result = dispatch(HTTP_POST, route, request);
        EXPECT_EQ(result.status, 403);
        EXPECT_EQ(result.read_calls, 0);

        request.local_address.clear();
        result = dispatch(HTTP_POST, route, request);
        EXPECT_EQ(result.status, 403);
        EXPECT_EQ(result.read_calls, 0);

        request = make_post(report_body(id));
        request.request_headers["Origin"] = "http://192.0.2.99";
        result = dispatch(HTTP_POST, route, request);
        EXPECT_EQ(result.status, 403);
        EXPECT_EQ(result.read_calls, 0);

        request = make_post(report_body(id));
        request.request_headers["Content-Type"] = "text/plain";
        result = dispatch(HTTP_POST, route, request);
        EXPECT_EQ(result.status, 415);
        EXPECT_EQ(result.read_calls, 0);

        request = make_post(report_body(id));
        request.request_headers["X-PRD-Setup-Token"] = "fixture-invalid";
        result = dispatch(HTTP_POST, route, request);
        EXPECT_EQ(result.status, 403);
        EXPECT_EQ(result.read_calls, 0);

        request = make_post({std::string(1025, ' '), {}});
        result = dispatch(HTTP_POST, route, request);
        EXPECT_EQ(result.status, 413);
        EXPECT_EQ(result.read_calls, 0);

        request = make_post({});
        result = dispatch(HTTP_POST, route, request);
        EXPECT_EQ(result.status, 400);
        EXPECT_EQ(result.read_calls, 0);

        const auto valid = route == kReport ? report_body(id) : kInvalidWifi;
        request = make_post(valid);
        request.request_body.text.resize(1024, ' ');
        result = dispatch(HTTP_POST, route, request);
        EXPECT_EQ(result.status, 200);
        EXPECT_EQ(result.read_calls, 1);
        refill();

        request.short_read = true;
        result = dispatch(HTTP_POST, route, request);
        EXPECT_EQ(result.status, 400);
        EXPECT_EQ(result.read_calls, 1);
    }
    EXPECT_TRUE(sink.submissions.empty());
}

TEST_F(ProvisioningServerAdmission, ReportQuotaIsIndependentOfSharedSettingsAndUpdateQuota) {
    const auto id = publish_failure();
    ASSERT_FALSE(id.empty());
    publish_update("1.2.0", false);
    ASSERT_EQ(post(kUpdate, kUpdateBody).status, 200);
    for (int i = 0; i < 3; ++i) {
        ASSERT_EQ(post(kSetup, kInvalidWifi).status, 200);
    }

    // One settings slot remains. Reading the report cannot spend it.
    ASSERT_EQ(post(kReport, report_body(id)).status, 200);
    auto rejected = post(kReport, report_body(id));
    EXPECT_EQ(rejected.status, 429);
    EXPECT_EQ(rejected.read_calls, 0);
    EXPECT_EQ(response_header(rejected.response_headers, "Retry-After"), "1");
    check_report_headers(rejected);

    time_ms += 999;
    EXPECT_EQ(post(kReport, report_body(id)).status, 429);
    auto accepted = post(kSetup, kWifi);
    EXPECT_EQ(accepted.status, 200);
    ASSERT_EQ(sink.submissions.size(), 1u);
    EXPECT_EQ(sink.submissions.front().stage, SubmissionStage::Wifi);

    rejected = post(kSetup, kWifi);
    EXPECT_EQ(rejected.status, 429);
    EXPECT_EQ(rejected.read_calls, 0);
    EXPECT_EQ(response_header(rejected.response_headers, "Retry-After"), "1");
    rejected = post(kUpdate, kUpdateBody);
    EXPECT_EQ(rejected.status, 429);
    EXPECT_EQ(rejected.read_calls, 0);
    EXPECT_EQ(sink.updates, 1);

    ++time_ms;
    // The new report slot works despite the retired old report and the shared
    // budget being used by an update at this very same fake tick.
    EXPECT_EQ(post(kUpdate, kUpdateBody).status, 200);
    const auto newer_id = publish_failure();
    ASSERT_FALSE(newer_id.empty());
    EXPECT_EQ(post(kReport, report_body(newer_id)).status, 200);
    EXPECT_EQ(post(kSetup, kWifi).status, 429);
    EXPECT_EQ(post(kReport, report_body(newer_id)).status, 429);
    EXPECT_EQ(sink.updates, 2);
    EXPECT_EQ(sink.submissions.size(), 1u);
}
} // namespace

esp_err_t httpd_start(httpd_handle_t* handle, const httpd_config_t* settings) {
    config = *settings;
    *handle = &routes;
    return ESP_OK;
}

esp_err_t httpd_stop(httpd_handle_t) {
    routes.clear();
    return ESP_OK;
}

esp_err_t httpd_register_uri_handler(httpd_handle_t, const httpd_uri_t* route) {
    return routes.emplace(std::make_pair(route->method, route->uri), *route).second ? ESP_OK
                                                                                    : ESP_FAIL;
}

int httpd_req_to_sockfd(httpd_req_t*) {
    return 42;
}

int fixture_getsockname(int, sockaddr* address, socklen_t* length) {
    sockaddr_in local{};
    local.sin_family = AF_INET;
    if (exchange_in_progress == nullptr || *length < sizeof(local) ||
        inet_pton(AF_INET, exchange_in_progress->local_address.c_str(), &local.sin_addr) != 1) {
        return -1;
    }
    std::memcpy(address, &local, sizeof(local));
    *length = sizeof(local);
    return 0;
}

std::size_t httpd_req_get_hdr_value_len(httpd_req_t* request, const char* name) {
    auto& headers = from_request(request).request_headers;
    const auto entry = headers.find(name);
    return entry == headers.end() ? 0 : entry->second.size();
}

esp_err_t httpd_req_get_hdr_value_str(httpd_req_t* request, const char* name, char* target,
                                      std::size_t size) {
    const auto& value = from_request(request).request_headers.at(name);
    if (value.size() >= size) {
        return ESP_FAIL;
    }
    std::memcpy(target, value.c_str(), value.size() + 1);
    return ESP_OK;
}

int httpd_req_recv(httpd_req_t* request, char* target, std::size_t size) {
    auto& exchange = from_request(request);
    ++exchange.read_calls;
    if (exchange.during_read) {
        exchange.during_read();
    }
    const std::size_t received = std::min(size, exchange.request_body.text.size());
    std::memcpy(target, exchange.request_body.text.data(), received);
#ifdef PRD_SCRIPTED_CJSON
    host_json::set_document(exchange.request_body.text, exchange.request_body.document);
#endif
    return static_cast<int>(exchange.short_read && received != 0 ? received - 1 : received);
}

esp_err_t httpd_resp_set_type(httpd_req_t* request, const char* value) {
    from_request(request).content_type = value;
    return ESP_OK;
}

esp_err_t httpd_resp_set_status(httpd_req_t* request, const char* value) {
    from_request(request).status = std::stoi(value);
    return ESP_OK;
}

esp_err_t httpd_resp_set_hdr(httpd_req_t* request, const char* key, const char* value) {
    from_request(request).response_headers[key] = value;
    return ESP_OK;
}

esp_err_t httpd_resp_send(httpd_req_t* request, const char* value, ssize_t length) {
    from_request(request).response_body.assign(value, length == HTTPD_RESP_USE_STRLEN
                                                          ? std::strlen(value)
                                                          : static_cast<std::size_t>(length));
    return ESP_OK;
}

std::int64_t esp_timer_get_time() {
    return time_ms * 1000;
}

std::uint32_t esp_random() {
    return next_random++;
}

QueueHandle_t xQueueCreate(std::size_t count, std::size_t bytes) {
    EXPECT_EQ(count, 1u);
    snapshot_queue.bytes.resize(bytes);
    return &snapshot_queue;
}

int xQueuePeek(QueueHandle_t handle, void* target, unsigned) {
    const auto& queue = *static_cast<Queue*>(handle);
    if (!queue.written) {
        return 0;
    }
    std::memcpy(target, queue.bytes.data(), queue.bytes.size());
    return pdTRUE;
}

int xQueueOverwrite(QueueHandle_t handle, const void* source) {
    auto& queue = *static_cast<Queue*>(handle);
    std::memcpy(queue.bytes.data(), source, queue.bytes.size());
    queue.written = true;
    return pdTRUE;
}

namespace prd::diagnostics {
void log(LogLevel, std::string_view, std::string_view) {}
void timeline(std::string_view, std::string_view, std::string_view, std::int64_t) {}
} // namespace prd::diagnostics

namespace prd::app_model {
BuildInfo current_build_info() {
    BuildInfo info;
    info.version = SemanticVersion::parse("1.2.0").value();
    return info;
}
} // namespace prd::app_model
