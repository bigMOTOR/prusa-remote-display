// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only
#include "ota/manifest.hpp"
#include "json_reader/reader.hpp"
#include <algorithm>
#include <limits>

namespace prd::ota {
namespace {
using json_reader::JsonField;
using json_reader::JsonObject;

template <std::size_t Size> std::string_view bounded_text(const std::array<char, Size>& text) {
    const auto end = std::find(text.begin(), text.end(), '\0');
    return {text.data(), static_cast<std::size_t>(end - text.begin())};
}

bool object_member(const JsonObject& object, std::string_view name, JsonObject& output) {
    JsonField field;
    if (!json_reader::find_unique(object, name, field) ||
        field.type != json_reader::JsonType::Object) {
        return false;
    }
    return json_reader::parse_object(field.raw, output) == json_reader::ScanStatus::Ok;
}

bool integer_member(const JsonObject& object, std::string_view name, std::uint64_t& output) {
    JsonField field;
    return json_reader::find_unique(object, name, field) &&
           json_reader::read_unsigned(field, output);
}

template <std::size_t Size>
bool text_member(const JsonObject& object, std::string_view name, std::array<char, Size>& output) {
    JsonField field;
    std::size_t length = 0;
    return json_reader::find_unique(object, name, field) &&
           json_reader::read_ascii(field, output.data(), Size - 1, length) && length != 0;
}
} // namespace

Candidate DecodedManifest::candidate() const {
    return {schema,         bounded_text(board), bounded_text(chip),  bounded_text(version),
            minimum_schema, image_size,          bounded_text(sha256)};
}

ManifestStatus decode_manifest(std::string_view body, DecodedManifest& output) {
    output = {};
    if (body.size() > kManifestBodyLimit) {
        return ManifestStatus::TooLarge;
    }
    JsonObject root;
    if (json_reader::parse_object(body, root) != json_reader::ScanStatus::Ok) {
        return ManifestStatus::Malformed;
    }
    DecodedManifest decoded;
    JsonObject image;
    JsonObject compatibility;
    std::uint64_t schema = 0;
    std::uint64_t minimum_schema = 0;
    if (!object_member(root, "image", image) ||
        !object_member(root, "compatibility", compatibility) ||
        !integer_member(root, "schemaVersion", schema) ||
        !integer_member(compatibility, "minBootloaderSchema", minimum_schema) ||
        schema > std::numeric_limits<std::uint32_t>::max() ||
        minimum_schema > std::numeric_limits<std::uint32_t>::max() ||
        !integer_member(image, "sizeBytes", decoded.image_size) ||
        !text_member(root, "boardTarget", decoded.board) ||
        !text_member(root, "chipTarget", decoded.chip) ||
        !text_member(root, "productVersion", decoded.version) ||
        !text_member(image, "sha256", decoded.sha256) || decoded.sha256[63] == '\0') {
        return ManifestStatus::InvalidField;
    }
    decoded.schema = static_cast<std::uint32_t>(schema);
    decoded.minimum_schema = static_cast<std::uint32_t>(minimum_schema);
    output = decoded;
    return ManifestStatus::Decoded;
}
} // namespace prd::ota
