// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include "diagnostics/log.hpp"

#include <string>

// ESP-IDF headers are not -Wundef clean. Suppress locally (per architecture
// spec §12.3) so the project -Wundef policy still applies to our own code.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wundef"
#include "esp_log.h"
#pragma GCC diagnostic pop

namespace prd::diagnostics {

namespace {

// `TL|` is the grep handle. One prefix, one grammar, whole timeline in one pass.
void emit_timeline(const std::string_view tag, const std::string& body) {
    const std::string tag_string{tag};
    ESP_LOGI(tag_string.c_str(), "TL|%s", body.c_str());
}

} // namespace

void timeline(const std::string_view tag, const std::string_view event) {
    emit_timeline(tag, "ev=" + std::string{event});
}

void timeline(const std::string_view tag, const std::string_view event, const std::string_view name,
              const std::int64_t value) {
    emit_timeline(tag, "ev=" + std::string{event} + " " + std::string{name} + "=" +
                           std::to_string(value));
}

void timeline(const std::string_view tag, const std::string_view event,
              const std::string_view first_name, const std::int64_t first_value,
              const std::string_view second_name, const std::int64_t second_value) {
    emit_timeline(tag, "ev=" + std::string{event} + " " + std::string{first_name} + "=" +
                           std::to_string(first_value) + " " + std::string{second_name} + "=" +
                           std::to_string(second_value));
}

void log(const LogLevel level, const std::string_view tag, const std::string_view message) {
    // The public API accepts string_view, but ESP-IDF logging needs a
    // null-terminated tag; materialize the tag and bound the (possibly
    // non-terminated) message with a precision specifier. Diagnostic logging
    // is not on a hot path.
    const std::string tag_string{tag};
    const auto length = static_cast<int>(message.size());
    const char* const data = message.data();

    switch (level) {
    case LogLevel::Debug:
        ESP_LOGD(tag_string.c_str(), "%.*s", length, data);
        break;
    case LogLevel::Info:
        ESP_LOGI(tag_string.c_str(), "%.*s", length, data);
        break;
    case LogLevel::Warning:
        ESP_LOGW(tag_string.c_str(), "%.*s", length, data);
        break;
    case LogLevel::Error:
        ESP_LOGE(tag_string.c_str(), "%.*s", length, data);
        break;
    }
}

void set_external_tag_level(const std::string_view tag, const LogLevel level) {
    const std::string tag_string{tag};

    auto esp_level = ESP_LOG_INFO;
    switch (level) {
    case LogLevel::Debug:
        esp_level = ESP_LOG_DEBUG;
        break;
    case LogLevel::Info:
        esp_level = ESP_LOG_INFO;
        break;
    case LogLevel::Warning:
        esp_level = ESP_LOG_WARN;
        break;
    case LogLevel::Error:
        esp_level = ESP_LOG_ERROR;
        break;
    }

    esp_log_level_set(tag_string.c_str(), esp_level);
}

} // namespace prd::diagnostics
