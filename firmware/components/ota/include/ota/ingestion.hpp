// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "ota/compatibility.hpp"
#include "ota/image_identity.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace prd::ota {

using ImageDigest = std::array<std::uint8_t, 32>;

enum class IngestionBegin : std::uint8_t {
    Started,
    Refused, // No resources acquired; caller must not abort an incumbent session.
    FailedNeedsAbort,
};

// A single, exclusively borrowed staging/hash session. Implementations must hash
// exactly the bytes they store. No method may select a boot partition or reboot.
// FailedNeedsAbort may partially acquire resources; abort must release those.
// Refused acquires nothing and must leave any incumbent session untouched.
// finish_digest finalizes only SHA-256, NOT an OTA image (not esp_ota_end), and
// leaves staging abortable. abort releases resources without activating the image.
class IngestionPort {
  public:
    virtual ~IngestionPort() = default;
    [[nodiscard]] virtual IngestionBegin begin(std::uint64_t image_size) = 0;
    [[nodiscard]] virtual bool write(const std::uint8_t* bytes, std::size_t size) = 0;
    [[nodiscard]] virtual bool finish_digest(ImageDigest& digest) = 0;
    // Consumes the staging session on success AND failure. Reads identity only
    // after SDK signature/image verification. Never selects a boot partition.
    [[nodiscard]] virtual bool finalize(ImageIdentity& identity) = 0;
    virtual void abort() = 0;
};

// Device-only, exclusively borrowed port. Stages into the inactive application
// slot; finalization verifies but never activates it. SDK begin may invalidate inactive OTA
// metadata under rollback configuration; abort does not restore erased bytes.
[[nodiscard]] IngestionPort& device_ingestion_port();

enum class IngestionStatus : std::uint8_t {
    Idle,
    Receiving,
    DigestMatched,
    ImageVerified,
    IdentityTooLong,
    VerificationFailed,
    IdentityMismatch,
    CompatibilityRejected,
    BeginFailed,
    InvalidChunk,
    TooManyBytes,
    WriteFailed,
    Truncated,
    DigestFailed,
    DigestMismatch,
    Cancelled,
};

// One-shot, allocation-free byte/digest validation, not a complete installer.
// The port must outlive this object and must not be used externally during it.
// DigestMatched authenticates neither metadata nor image signature and grants no
// installation authority. verify consumes staging, including on failure; before
// verify, destruction aborts. ImageVerified is historical evidence for this session,
// not a durable activation capability. Reboot or later writes require fresh verification.
// Calls are serialized by the caller; this object is not thread-safe.
class ImageIngestion {
  public:
    explicit ImageIngestion(IngestionPort& port) : port_(port) {}
    ~ImageIngestion();
    ImageIngestion(const ImageIngestion&) = delete;
    ImageIngestion& operator=(const ImageIngestion&) = delete;
    ImageIngestion(ImageIngestion&&) = delete;
    ImageIngestion& operator=(ImageIngestion&&) = delete;

    [[nodiscard]] bool begin(const Candidate& candidate, const DeviceCompatibility& device,
                             DowngradePolicy policy);
    [[nodiscard]] bool append(const std::uint8_t* bytes, std::size_t size);
    [[nodiscard]] bool finish();
    // Session-local evidence only, invalid after reboot or any later slot write.
    // No activation token or durable verified authority is returned.
    [[nodiscard]] bool verify();
    void cancel();

    [[nodiscard]] IngestionStatus status() const {
        return status_;
    }
    [[nodiscard]] Compatibility compatibility() const {
        return compatibility_;
    }
    [[nodiscard]] std::uint64_t received_bytes() const {
        return received_;
    }

  private:
    void release();
    [[nodiscard]] bool fail(IngestionStatus status);

    IngestionPort& port_;
    IngestionStatus status_{IngestionStatus::Idle};
    Compatibility compatibility_{Compatibility::NotChecked};
    ImageDigest expected_digest_{};
    ImageIdentity expected_identity_{};
    std::uint64_t expected_size_{0};
    std::uint64_t received_{0};
    bool owns_session_{false};
};

} // namespace prd::ota
