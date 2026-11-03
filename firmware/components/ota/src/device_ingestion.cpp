// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include "ota/installation.hpp"

#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#if !defined(CONFIG_SECURE_SIGNED_ON_UPDATE) || !CONFIG_SECURE_SIGNED_ON_UPDATE
#error "OTA device ingestion requires CONFIG_SECURE_SIGNED_ON_UPDATE"
#endif
#endif

#include <algorithm>
#include <cstring>
#include <limits>
#include <mutex>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wundef"
#include "esp_ota_ops.h"
#include "mbedtls/sha256.h"
#pragma GCC diagnostic pop

namespace prd::ota {
namespace {
class DeviceIngestion final : public IngestionPort, public InstallationPort {
  public:
    DeviceIngestion() {
        mbedtls_sha256_init(&hash_);
    }
    ~DeviceIngestion() override {
        abort();
        mbedtls_sha256_free(&hash_);
    }

    IngestionBegin begin(std::uint64_t image_size) override {
        const std::scoped_lock guard{mutex_};
        return lease_.valid() ? IngestionBegin::Refused : begin_impl(image_size);
    }
    bool write(const std::uint8_t* bytes, std::size_t size) override {
        const std::scoped_lock guard{mutex_};
        return !lease_.valid() && write_impl(bytes, size);
    }
    bool finish_digest(ImageDigest& digest) override {
        const std::scoped_lock guard{mutex_};
        return !lease_.valid() && digest_impl(digest);
    }
    bool finalize(ImageIdentity& identity) override {
        const std::scoped_lock guard{mutex_};
        return !lease_.valid() && finalize_impl(identity);
    }
    void abort() override {
        const std::scoped_lock guard{mutex_};
        if (!lease_.valid()) {
            abort_impl();
        }
    }

    InstallationLease reserve() override {
        const std::scoped_lock guard{mutex_};
        if (lease_.valid() || owned_ || generation_ == std::numeric_limits<std::uint64_t>::max()) {
            return {};
        }
        lease_ = lease_for(++generation_);
        verified_ = false;
        activation_locked_ = false;
        return lease_;
    }
    IngestionBegin begin(InstallationLease lease, std::uint64_t size) override {
        const std::scoped_lock guard{mutex_};
        if (!matches(lease) || activation_locked_ || verified_) {
            return IngestionBegin::Refused;
        }
        return begin_impl(size);
    }
    bool write(InstallationLease lease, const std::uint8_t* bytes, std::size_t size) override {
        const std::scoped_lock guard{mutex_};
        return matches(lease) && !activation_locked_ && write_impl(bytes, size);
    }
    bool finish_digest(InstallationLease lease, ImageDigest& digest) override {
        const std::scoped_lock guard{mutex_};
        return matches(lease) && !activation_locked_ && digest_impl(digest);
    }
    bool finalize(InstallationLease lease, ImageIdentity& identity) override {
        const std::scoped_lock guard{mutex_};
        if (!matches(lease) || activation_locked_ || verified_) {
            return false;
        }
        verified_ = finalize_impl(identity);
        if (verified_) {
            verified_identity_ = identity;
        }
        return verified_;
    }
    void abort(InstallationLease lease) override {
        const std::scoped_lock guard{mutex_};
        if (matches(lease) && !activation_locked_) {
            abort_impl();
        }
    }
    SelectionResult select(InstallationLease lease, const Candidate& approved) override {
        const std::scoped_lock guard{mutex_};
        if (!matches(lease) || activation_locked_ || !verified_ || owned_ || target_ == nullptr ||
            target_->address != target_snapshot_.address ||
            target_->size != target_snapshot_.size || target_->type != target_snapshot_.type ||
            target_->subtype != target_snapshot_.subtype || approved.image_size != expected_ ||
            approved.sha256.size() != 64 ||
            approved.board.size() >= verified_identity_.board.size() ||
            approved.version.size() >= verified_identity_.version.size()) {
            return SelectionResult::Rejected;
        }
        ImageIdentity expected;
        std::copy(approved.board.begin(), approved.board.end(), expected.board.begin());
        std::copy(approved.version.begin(), approved.version.end(), expected.version.begin());
        expected.min_bootloader_schema = approved.min_bootloader_schema;
        if (verified_identity_.magic != expected.magic ||
            verified_identity_.format != expected.format ||
            verified_identity_.board != expected.board ||
            verified_identity_.version != expected.version ||
            verified_identity_.min_bootloader_schema != expected.min_bootloader_schema ||
            verified_identity_.reserved != expected.reserved) {
            return SelectionResult::Rejected;
        }
        constexpr std::string_view hex = "0123456789abcdef";
        for (std::size_t index = 0; index < verified_digest_.size(); ++index) {
            const auto lower = [](char value) {
                return value >= 'A' && value <= 'F' ? static_cast<char>(value + ('a' - 'A'))
                                                    : value;
            };
            if (lower(approved.sha256[index * 2]) != hex[verified_digest_[index] >> 4] ||
                lower(approved.sha256[(index * 2) + 1]) != hex[verified_digest_[index] & 15]) {
                return SelectionResult::Rejected;
            }
        }
        // No writer can enter between authenticated binding and this exact SDK
        // selection call. IDF revalidates the image; this is not a reboot.
        const auto result = esp_ota_set_boot_partition(target_);
        if (result == ESP_ERR_OTA_VALIDATE_FAILED) {
            return SelectionResult::Rejected; // IDF returns before any metadata write.
        }
        activation_locked_ = true;
        // Other failures may follow an otadata erase/write. Do not erase/retry
        // the potentially selected target or pretend the old boot choice held.
        return result == ESP_OK ? SelectionResult::Selected : SelectionResult::Uncertain;
    }
    void release(InstallationLease lease) override {
        const std::scoped_lock guard{mutex_};
        if (matches(lease) && !activation_locked_) {
            abort_impl();
            lease_ = {};
        }
    }

  private:
    [[nodiscard]] bool matches(InstallationLease lease) const {
        return lease.valid() && lease == lease_;
    }

    IngestionBegin begin_impl(std::uint64_t image_size) {
        if (owned_) {
            return IngestionBegin::Refused;
        }
        if (image_size == 0 || image_size > std::numeric_limits<std::size_t>::max()) {
            return IngestionBegin::FailedNeedsAbort;
        }
        const auto* running = esp_ota_get_running_partition();
        const auto* target = esp_ota_get_next_update_partition(nullptr);
        if (running == nullptr || target == nullptr || target->type != ESP_PARTITION_TYPE_APP ||
            target->subtype < ESP_PARTITION_SUBTYPE_APP_OTA_0 ||
            target->subtype > ESP_PARTITION_SUBTYPE_APP_OTA_15 ||
            target->address == running->address || image_size > target->size) {
            return IngestionBegin::FailedNeedsAbort;
        }
        if (mbedtls_sha256_starts(&hash_, 0) != 0) {
            return IngestionBegin::FailedNeedsAbort;
        }
        // In pinned IDF 5.5.4 sequential APP begin has no failing operation after
        // handle allocation. Upfront erase mode does, with no invalid-handle
        // sentinel. Sequential erase failures instead arrive through write.
        if (esp_ota_begin(target, OTA_WITH_SEQUENTIAL_WRITES, &handle_) != ESP_OK) {
            return IngestionBegin::FailedNeedsAbort;
        }
        owned_ = true; // Handle zero is valid; ownership is not a numeric test.
        finished_ = false;
        expected_ = image_size;
        received_ = 0;
        target_ = target;
        target_snapshot_ = *target;
        return IngestionBegin::Started;
    }

    bool write_impl(const std::uint8_t* bytes, std::size_t size) {
        if (!owned_ || finished_ || size > expected_ - received_) {
            return false;
        }
        if (size == 0) {
            return true;
        }
        if (bytes == nullptr || esp_ota_write(handle_, bytes, size) != ESP_OK ||
            mbedtls_sha256_update(&hash_, bytes, size) != 0) {
            return false;
        }
        received_ += size;
        return true;
    }

    bool digest_impl(ImageDigest& digest) {
        if (!owned_ || finished_ || received_ != expected_) {
            return false;
        }
        if (mbedtls_sha256_finish(&hash_, digest.data()) != 0) {
            return false;
        }
        finished_ = true;
        verified_digest_ = digest;
        return true;
    }

    bool finalize_impl(ImageIdentity& identity) {
        if (!owned_) {
            return false;
        }
        if (!finished_ || received_ != expected_) {
            abort_impl();
            return false;
        }
        owned_ = false;
        // IDF consumes this handle for either success or validation failure.
        const auto result = esp_ota_end(handle_);
        if (result != ESP_OK) {
            return false;
        }
        constexpr std::size_t prefix_size = 24 + 8 + 256 + sizeof(ImageIdentity);
        std::array<std::uint8_t, prefix_size> prefix{};
        if (expected_ < prefix.size() ||
            esp_partition_read(target_, 0, prefix.data(), prefix.size()) != ESP_OK) {
            return false;
        }
        const auto word = [&prefix](std::size_t offset) {
            return static_cast<std::uint32_t>(prefix[offset]) |
                   (static_cast<std::uint32_t>(prefix[offset + 1]) << 8) |
                   (static_cast<std::uint32_t>(prefix[offset + 2]) << 16) |
                   (static_cast<std::uint32_t>(prefix[offset + 3]) << 24);
        };
        // Descriptor must lie within the authenticated first segment, not a
        // coincidental byte sequence elsewhere in the signed image.
        if (prefix[0] != 0xe9 || prefix[1] == 0 || word(28) < 256 + sizeof(ImageIdentity) ||
            word(28) > expected_ - 32 || word(32) != 0xabcd5432) {
            return false;
        }
        std::memcpy(&identity, prefix.data() + 288, sizeof(identity));
        constexpr std::string_view project = "prusa_remote_display";
        std::array<std::uint8_t, 32> expected_project{};
        std::copy(std::begin(project), std::end(project), expected_project.begin());
        return std::equal(expected_project.begin(), expected_project.end(), prefix.begin() + 80) &&
               std::equal(identity.version.begin(), identity.version.end(), prefix.begin() + 48);
    }

    void abort_impl() {
        if (owned_) {
            owned_ = false;
            (void)esp_ota_abort(handle_);
        }
        mbedtls_sha256_free(&hash_);
        mbedtls_sha256_init(&hash_);
        finished_ = false;
        expected_ = 0;
        received_ = 0;
        verified_ = false;
    }

    mbedtls_sha256_context hash_{};
    esp_ota_handle_t handle_{};
    const esp_partition_t* target_{nullptr};
    esp_partition_t target_snapshot_{};
    ImageIdentity verified_identity_{};
    ImageDigest verified_digest_{};
    std::mutex mutex_;
    InstallationLease lease_;
    std::uint64_t generation_{0};
    std::uint64_t expected_{0};
    std::uint64_t received_{0};
    bool owned_{false};
    bool finished_{false};
    bool verified_{false};
    bool activation_locked_{false};
};
DeviceIngestion& device_port() {
    static DeviceIngestion port;
    return port;
}
} // namespace

IngestionPort& device_ingestion_port() {
    return device_port();
}
InstallationPort& device_installation_port() {
    return device_port();
}

std::uint64_t device_inactive_slot_size() {
    const esp_partition_t* const target = esp_ota_get_next_update_partition(nullptr);
    return target == nullptr ? 0 : target->size;
}
} // namespace prd::ota
