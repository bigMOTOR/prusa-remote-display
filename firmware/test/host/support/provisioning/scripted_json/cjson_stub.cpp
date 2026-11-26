// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include "cJSON.h"

#include <algorithm>
#include <memory>

#include "gtest/gtest.h"

namespace {

std::string_view submitted_body;
const host_json::Document* parsed_document = nullptr;

cJSON* parse(const char* value, std::size_t length, const char** end) {
    if (parsed_document == nullptr) {
        ADD_FAILURE() << "Parser called before the fixture read a request body";
        return nullptr;
    }
    EXPECT_EQ(std::string_view(value, length), submitted_body);

    if (parsed_document->kind == host_json::Document::Kind::Invalid) {
        return nullptr;
    }

    if (end != nullptr) {
        *end = value + parsed_document->end.value_or(length);
    }

    auto root = std::make_unique<cJSON>();
    root->kind = parsed_document->kind;
    root->children.resize(parsed_document->fields.size());
    for (std::size_t index = 0; index < root->children.size(); ++index) {
        auto& child = root->children[index];
        child.name = parsed_document->fields[index].name;
        child.text = parsed_document->fields[index].text;
        if (child.text.has_value()) {
            child.valuestring = child.text->data();
        }
    }
    return root.release();
}

} // namespace

namespace host_json {

void set_document(std::string_view body, const Document& document) {
    submitted_body = body;
    parsed_document = &document;
}

void clear_document() {
    submitted_body = {};
    parsed_document = nullptr;
}

} // namespace host_json

cJSON* cJSON_ParseWithLength(const char* value, std::size_t length) {
    return parse(value, length, nullptr);
}

cJSON* cJSON_ParseWithLengthOpts(const char* value, std::size_t length, const char** end,
                                 int require_null_terminated) {
    if (length == 0) {
        ADD_FAILURE() << "Strict parse did not receive a terminator";
        return nullptr;
    }
    EXPECT_NE(require_null_terminated, 0);
    EXPECT_NE(end, nullptr);
    EXPECT_EQ(value[length - 1], '\0');
    return parse(value, length - 1, end);
}

cJSON* cJSON_GetObjectItemCaseSensitive(const cJSON* root, const char* name) {
    if (!cJSON_IsObject(root)) {
        return nullptr;
    }
    // Like cJSON, return the first matching child, including for duplicate keys.
    auto& children = const_cast<cJSON*>(root)->children;
    const auto child = std::find_if(children.begin(), children.end(),
                                    [name](const cJSON& item) { return item.name == name; });
    return child == children.end() ? nullptr : &*child;
}

int cJSON_IsObject(const cJSON* item) {
    return item != nullptr && item->kind == host_json::Document::Kind::Object;
}

int cJSON_IsString(const cJSON* item) {
    return item != nullptr && item->valuestring != nullptr;
}

int cJSON_GetArraySize(const cJSON* item) {
    return item == nullptr ? 0 : static_cast<int>(item->children.size());
}

void cJSON_Delete(cJSON* item) {
    delete item;
}
