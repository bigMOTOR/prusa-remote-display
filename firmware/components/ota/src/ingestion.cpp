// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include "ota/ingestion.hpp"

#include <algorithm>

namespace prd::ota {
namespace {

// Called only after check_compatibility has validated all 64 hex characters.
std::uint8_t hex_value(const char value) {
    if (value >= '0' && value <= '9') {
        return static_cast<std::uint8_t>(value - '0');
    }
    if (value >= 'a' && value <= 'f') {
        return static_cast<std::uint8_t>(value - 'a' + 10);
    }
    return static_cast<std::uint8_t>(value - 'A' + 10);
}

} // namespace

ImageIngestion::~ImageIngestion() {
    release();
}

bool ImageIngestion::begin(const Candidate& candidate, const DeviceCompatibility& device,
                           const DowngradePolicy policy) {
    if (status_ != IngestionStatus::Idle) {
        return false;
    }
    compatibility_ = check_compatibility(candidate, device, policy);
    if (compatibility_ != Compatibility::MetadataCompatible) {
        return fail(IngestionStatus::CompatibilityRejected);
    }
    // Retain no views into caller-owned manifest storage beyond this call.
    if (candidate.board.size() >= expected_identity_.board.size() ||
        candidate.version.size() >= expected_identity_.version.size() ||
        candidate.board.find('\0') != std::string_view::npos) {
        return fail(IngestionStatus::IdentityTooLong);
    }
    std::copy(candidate.board.begin(), candidate.board.end(), expected_identity_.board.begin());
    std::copy(candidate.version.begin(), candidate.version.end(),
              expected_identity_.version.begin());
    expected_identity_.min_bootloader_schema = candidate.min_bootloader_schema;
    expected_size_ = candidate.image_size;
    for (std::size_t index = 0; index < expected_digest_.size(); ++index) {
        expected_digest_[index] =
            static_cast<std::uint8_t>((hex_value(candidate.sha256[index * 2]) * 16) +
                                      hex_value(candidate.sha256[(index * 2) + 1]));
    }
    const auto result = port_.begin(expected_size_);
    owns_session_ = result != IngestionBegin::Refused;
    if (result != IngestionBegin::Started) {
        return fail(IngestionStatus::BeginFailed);
    }
    status_ = IngestionStatus::Receiving;
    return true;
}

bool ImageIngestion::append(const std::uint8_t* bytes, const std::size_t size) {
    if (status_ != IngestionStatus::Receiving) {
        return false;
    }
    // Subtraction is safe by invariant; addition could wrap for an oversized input.
    if (size > expected_size_ - received_) {
        return fail(IngestionStatus::TooManyBytes);
    }
    if (size == 0) {
        return true;
    }
    if (bytes == nullptr) {
        return fail(IngestionStatus::InvalidChunk);
    }
    if (!port_.write(bytes, size)) {
        return fail(IngestionStatus::WriteFailed);
    }
    received_ += size;
    return true;
}

bool ImageIngestion::finish() {
    if (status_ != IngestionStatus::Receiving) {
        return false;
    }
    if (received_ != expected_size_) {
        return fail(IngestionStatus::Truncated);
    }
    ImageDigest actual{};
    if (!port_.finish_digest(actual)) {
        return fail(IngestionStatus::DigestFailed);
    }
    if (actual != expected_digest_) {
        return fail(IngestionStatus::DigestMismatch);
    }
    status_ = IngestionStatus::DigestMatched;
    return true;
}

bool ImageIngestion::verify() {
    if (status_ != IngestionStatus::DigestMatched) {
        return false;
    }
    ImageIdentity identity{};
    owns_session_ = false; // finalize consumes even when verification fails.
    if (!port_.finalize(identity)) {
        return fail(IngestionStatus::VerificationFailed);
    }
    if (identity.magic != expected_identity_.magic || identity.format != 1 ||
        identity.min_bootloader_schema != expected_identity_.min_bootloader_schema ||
        identity.board != expected_identity_.board ||
        identity.version != expected_identity_.version ||
        identity.reserved != expected_identity_.reserved) {
        return fail(IngestionStatus::IdentityMismatch);
    }
    status_ = IngestionStatus::ImageVerified;
    return true;
}

void ImageIngestion::cancel() {
    if (status_ == IngestionStatus::Idle || status_ == IngestionStatus::Receiving ||
        status_ == IngestionStatus::DigestMatched) {
        status_ = IngestionStatus::Cancelled;
        release();
    }
}

void ImageIngestion::release() {
    if (owns_session_) {
        owns_session_ = false;
        port_.abort();
    }
}

bool ImageIngestion::fail(const IngestionStatus status) {
    status_ = status;
    release();
    return false;
}

} // namespace prd::ota
