// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include "ota/release_discovery.hpp"

#include <algorithm>
#include <array>
#include <limits>

#include "json_reader/reader.hpp"
#include "ota/manifest.hpp"
#include "ota/release_policy.hpp"

namespace prd::ota {

std::string_view release_discovery_url() noexcept {
#ifdef PRD_OTA_LAN_MODE
    return PRD_OTA_LAN_ORIGIN "/releases/latest";
#else
    return "https://api.github.com/repos/" PRD_OTA_RELEASE_OWNER "/" PRD_OTA_RELEASE_NAME
           "/releases/latest";
#endif
}

namespace {

#ifdef PRD_OTA_LAN_MODE
constexpr std::string_view kReleaseApiPrefix = PRD_OTA_LAN_ORIGIN "/releases/";
#else
constexpr std::string_view kReleaseApiPrefix =
    "https://api.github.com/repos/" PRD_OTA_RELEASE_OWNER "/" PRD_OTA_RELEASE_NAME "/releases/";
#endif
constexpr std::uint64_t kMaximumImageAssetBytes = UINT64_C(0x680000);

template <std::size_t Size> std::string_view text_view(const std::array<char, Size>& text) {
    const auto end = std::find(text.begin(), text.end(), '\0');
    return {text.data(), static_cast<std::size_t>(end - text.begin())};
}

[[nodiscard]] bool unique(const json_reader::JsonObject& object, const std::string_view name,
                          json_reader::JsonField& field) {
    return json_reader::find_unique(object, name, field) && field.present();
}

[[nodiscard]] bool unsigned_member(const json_reader::JsonObject& object,
                                   const std::string_view name, std::uint64_t& output) {
    json_reader::JsonField field;
    return unique(object, name, field) && json_reader::read_unsigned(field, output);
}

[[nodiscard]] bool bool_member(const json_reader::JsonObject& object, const std::string_view name,
                               bool& output) {
    json_reader::JsonField field;
    if (!unique(object, name, field) || field.type != json_reader::JsonType::Bool) {
        return false;
    }
    output = field.boolean;
    return true;
}

template <std::size_t Size>
[[nodiscard]] bool text_member(const json_reader::JsonObject& object, const std::string_view name,
                               std::array<char, Size>& output) {
    json_reader::JsonField field;
    std::size_t length = 0;
    output = {};
    return unique(object, name, field) &&
           json_reader::read_ascii(field, output.data(), Size - 1, length) && length != 0;
}

[[nodiscard]] bool decimal_id(const std::string_view text, std::uint64_t& value) {
    value = 0;
    if (text.empty() || (text.size() > 1 && text.front() == '0')) {
        return false;
    }
    for (const char character : text) {
        if (character < '0' || character > '9') {
            return false;
        }
        const auto digit = static_cast<std::uint64_t>(character - '0');
        if (value > (std::numeric_limits<std::uint64_t>::max() - digit) / 10) {
            return false;
        }
        value = (value * 10) + digit;
    }
    return true;
}

[[nodiscard]] bool repository_identity(const std::string_view url, const std::uint64_t release_id) {
    if (url.size() <= kReleaseApiPrefix.size() ||
        url.compare(0, kReleaseApiPrefix.size(), kReleaseApiPrefix) != 0) {
        return false;
    }
    std::uint64_t url_id = 0;
    return decimal_id(url.substr(kReleaseApiPrefix.size()), url_id) && url_id == release_id;
}

enum class AssetDecode : std::uint8_t { Ignored, Manifest, Image, Invalid };

struct AssetPresence {
    bool manifest{false};
    bool image{false};
};

[[nodiscard]] AssetDecode decode_asset(const json_reader::JsonObject& object,
                                       ReleaseAsset& output) {
    std::array<char, 64> name{};
    if (!text_member(object, "name", name)) {
        return AssetDecode::Invalid;
    }
    const std::string_view asset_name = text_view(name);
    if (asset_name != kReleaseManifestAsset && asset_name != kReleaseImageAsset) {
        return AssetDecode::Ignored;
    }

    ReleaseAsset decoded;
    if (!unsigned_member(object, "id", decoded.id) || decoded.id == 0 ||
        !unsigned_member(object, "size", decoded.size) || decoded.size == 0 ||
        !text_member(object, "browser_download_url", decoded.url)) {
        return AssetDecode::Invalid;
    }

    const auto resource =
        asset_name == kReleaseManifestAsset ? ReleaseResource::Manifest : ReleaseResource::Image;
    if (!release_url_allowed(resource, decoded.url_view())) {
        return AssetDecode::Invalid;
    }
    if ((resource == ReleaseResource::Manifest && decoded.size > kManifestBodyLimit) ||
        (resource == ReleaseResource::Image && decoded.size > kMaximumImageAssetBytes)) {
        return AssetDecode::Invalid;
    }
    output = decoded;
    return resource == ReleaseResource::Manifest ? AssetDecode::Manifest : AssetDecode::Image;
}

[[nodiscard]] ReleaseStatus decode_release_header(const json_reader::JsonObject& root,
                                                  ReleaseCandidate& decoded) {
    bool draft = false;
    bool prerelease = false;
    bool immutable = false;
    std::array<char, 256> api_url{};
    if (!unsigned_member(root, "id", decoded.release_id) || decoded.release_id == 0 ||
        !text_member(root, "tag_name", decoded.tag) || !text_member(root, "url", api_url) ||
        !bool_member(root, "draft", draft) || !bool_member(root, "prerelease", prerelease) ||
        !bool_member(root, "immutable", immutable)) {
        return ReleaseStatus::InvalidField;
    }
    if (draft || prerelease) {
        return ReleaseStatus::NotStable;
    }
    if (!immutable) {
        return ReleaseStatus::NotImmutable;
    }
    if (!repository_identity(text_view(api_url), decoded.release_id)) {
        return ReleaseStatus::WrongRepository;
    }
    return ReleaseStatus::Decoded;
}

[[nodiscard]] ReleaseStatus store_asset(const AssetDecode kind, const ReleaseAsset& asset,
                                        ReleaseCandidate& decoded, AssetPresence& presence) {
    if (kind == AssetDecode::Invalid) {
        return ReleaseStatus::InvalidField;
    }
    if (kind == AssetDecode::Manifest) {
        if (presence.manifest) {
            return ReleaseStatus::DuplicateAsset;
        }
        decoded.manifest = asset;
        presence.manifest = true;
    } else if (kind == AssetDecode::Image) {
        if (presence.image) {
            return ReleaseStatus::DuplicateAsset;
        }
        decoded.image = asset;
        presence.image = true;
    }
    return ReleaseStatus::Decoded;
}

[[nodiscard]] ReleaseStatus decode_release_assets(const std::string_view body,
                                                  const json_reader::JsonObject& root,
                                                  ReleaseCandidate& decoded) {
    json_reader::JsonField assets_field;
    if (!unique(root, "assets", assets_field) ||
        assets_field.type != json_reader::JsonType::Array) {
        return ReleaseStatus::InvalidField;
    }
    const auto begin = static_cast<std::size_t>(assets_field.raw.data() - body.data());
    const json_reader::JsonArray assets{body, begin, begin + assets_field.raw.size()};

    AssetPresence presence;
    std::size_t count = 0;
    std::size_t cursor = 0;
    for (;;) {
        json_reader::JsonField field;
        const auto step = json_reader::next(assets, cursor, field);
        if (step == json_reader::ArrayStep::End) {
            break;
        }
        if (step != json_reader::ArrayStep::Item || field.type != json_reader::JsonType::Object) {
            return ReleaseStatus::Malformed;
        }
        if (++count > kMaximumReleaseAssets) {
            return ReleaseStatus::TooManyAssets;
        }

        json_reader::JsonObject object;
        if (json_reader::parse_object(field.raw, object) != json_reader::ScanStatus::Ok) {
            return ReleaseStatus::Malformed;
        }
        ReleaseAsset asset;
        const auto status = store_asset(decode_asset(object, asset), asset, decoded, presence);
        if (status != ReleaseStatus::Decoded) {
            return status;
        }
    }

    if (!presence.manifest || !presence.image) {
        return ReleaseStatus::MissingAsset;
    }
    if (decoded.manifest.id == decoded.image.id ||
        decoded.manifest.url_view() == decoded.image.url_view()) {
        return ReleaseStatus::InvalidField;
    }
    return ReleaseStatus::Decoded;
}

} // namespace

std::string_view ReleaseAsset::url_view() const {
    return text_view(url);
}

std::string_view ReleaseCandidate::tag_view() const {
    return text_view(tag);
}

std::string_view log_label(const ReleaseStatus status) {
    switch (status) {
    case ReleaseStatus::Decoded:
        return "decoded";
    case ReleaseStatus::TooLarge:
        return "too-large";
    case ReleaseStatus::Malformed:
        return "malformed";
    case ReleaseStatus::NotStable:
        return "not-stable";
    case ReleaseStatus::NotImmutable:
        return "not-immutable";
    case ReleaseStatus::WrongRepository:
        return "wrong-repository";
    case ReleaseStatus::InvalidField:
        return "invalid-field";
    case ReleaseStatus::MissingAsset:
        return "missing-asset";
    case ReleaseStatus::DuplicateAsset:
        return "duplicate-asset";
    case ReleaseStatus::TooManyAssets:
        return "too-many-assets";
    }
    return "unknown";
}

ReleaseStatus decode_release(const std::string_view body, ReleaseCandidate& output) {
    output = {};
    if (body.size() > kReleaseBodyLimit) {
        return ReleaseStatus::TooLarge;
    }

    json_reader::JsonObject root;
    if (json_reader::parse_object(body, root) != json_reader::ScanStatus::Ok) {
        return ReleaseStatus::Malformed;
    }

    ReleaseCandidate decoded;
    auto status = decode_release_header(root, decoded);
    if (status != ReleaseStatus::Decoded) {
        return status;
    }
    status = decode_release_assets(body, root, decoded);
    if (status != ReleaseStatus::Decoded) {
        return status;
    }
    output = decoded;
    return ReleaseStatus::Decoded;
}

} // namespace prd::ota
