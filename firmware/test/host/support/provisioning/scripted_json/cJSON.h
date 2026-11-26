// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "json_fixture.hpp"

struct cJSON {
    char* valuestring{nullptr};
    host_json::Document::Kind kind{host_json::Document::Kind::Invalid};
    std::string name;
    std::optional<std::string> text;
    std::vector<cJSON> children;
};

cJSON* cJSON_ParseWithLength(const char* value, std::size_t length);
cJSON* cJSON_ParseWithLengthOpts(const char* value, std::size_t length, const char** end,
                                 int require_null_terminated);
cJSON* cJSON_GetObjectItemCaseSensitive(const cJSON* root, const char* name);
int cJSON_IsObject(const cJSON* item);
int cJSON_IsString(const cJSON* item);
int cJSON_GetArraySize(const cJSON* item);
void cJSON_Delete(cJSON* item);
