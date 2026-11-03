// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only
#include "ota/installation.hpp"
#include "app_model/events.hpp"

namespace prd::ota {
Installation::~Installation() {
    release();
}

bool Installation::request(std::string_view manifest, const DeviceCompatibility& device,
                           DowngradePolicy policy) {
    if (status_ != InstallationStatus::Idle) {
        return false;
    }
    if (decode_manifest(manifest, manifest_) != ManifestStatus::Decoded) {
        status_ = InstallationStatus::InvalidManifest;
        return false;
    }
    compatibility_ = check_compatibility(manifest_.candidate(), device, policy);
    if (compatibility_ != Compatibility::MetadataCompatible) {
        status_ = InstallationStatus::Incompatible;
        return false;
    }
    if (!admission_.acquire()) {
        status_ = InstallationStatus::Refused;
        return false;
    }
    admitted_ = true;
    lease_ = port_.reserve();
    if (!lease_.valid()) {
        status_ = InstallationStatus::Refused;
        release();
        return false;
    }
    observable_ = true;
    if (!ingestion_.begin(manifest_.candidate(), device, policy)) {
        if (begin_refused_) {
            status_ = InstallationStatus::Refused;
            observable_ = false;
            release();
            return false;
        }
        return fail();
    }
    status_ = InstallationStatus::Receiving;
    return true;
}

bool Installation::append(const std::uint8_t* bytes, std::size_t size) {
    if (status_ != InstallationStatus::Receiving) {
        return false;
    }
    return ingestion_.append(bytes, size) || fail();
}
bool Installation::finish() {
    if (status_ != InstallationStatus::Receiving) {
        return false;
    }
    if (!ingestion_.finish()) {
        return fail();
    }
    status_ = InstallationStatus::Verifying;
    return true;
}
bool Installation::verify() {
    if (status_ != InstallationStatus::Verifying) {
        return false;
    }
    if (!ingestion_.verify()) {
        return fail();
    }
    status_ = InstallationStatus::Installing;
    return true;
}
bool Installation::activate() {
    if (status_ != InstallationStatus::Installing) {
        return false;
    }
    const auto result = port_.select(lease_, manifest_.candidate());
    if (result == SelectionResult::Rejected) {
        return fail();
    }
    if (result != SelectionResult::Selected) {
        status_ = InstallationStatus::ActivationUncertain;
        return false; // Retain both reservations; no retry or success/reboot claim.
    }
    status_ = InstallationStatus::RebootRequired;
    // Neither destructor nor cancel may unlock the selected future boot image.
    return true;
}
void Installation::cancel() {
    if (status_ == InstallationStatus::Receiving || status_ == InstallationStatus::Verifying ||
        status_ == InstallationStatus::Installing) {
        status_ = InstallationStatus::Cancelled;
        release();
    }
}
void Installation::release() {
    ingestion_.cancel();
    if (lease_.valid()) {
        port_.release(lease_);
        lease_ = {};
    }
    if (admitted_) {
        admission_.release(status_ == InstallationStatus::RebootRequired ||
                           status_ == InstallationStatus::ActivationUncertain);
        admitted_ = false;
    }
}
bool Installation::fail() {
    status_ = InstallationStatus::Failed;
    release();
    return false;
}
IngestionBegin Installation::begin(std::uint64_t size) {
    const auto result = port_.begin(lease_, size);
    begin_refused_ = result == IngestionBegin::Refused;
    return result;
}
bool Installation::write(const std::uint8_t* bytes, std::size_t size) {
    return port_.write(lease_, bytes, size);
}
bool Installation::finish_digest(ImageDigest& digest) {
    return port_.finish_digest(lease_, digest);
}
bool Installation::finalize(ImageIdentity& identity) {
    return port_.finalize(lease_, identity);
}
void Installation::abort() {
    port_.abort(lease_);
}

bool Installation::observation(app_model::OtaObservation& output) const {
    output = {};
    if (!observable_) {
        return false;
    }
    output.available_version.assign(manifest_.candidate().version);
    switch (status_) {
    case InstallationStatus::Receiving:
        output.phase = app_model::OtaPhase::Downloading;
        break;
    case InstallationStatus::Verifying:
        output.phase = app_model::OtaPhase::Verifying;
        break;
    case InstallationStatus::Installing:
        output.phase = app_model::OtaPhase::Installing;
        break;
    case InstallationStatus::RebootRequired:
        output.phase = app_model::OtaPhase::RebootRequired;
        break;
    case InstallationStatus::Cancelled:
        output.phase = app_model::OtaPhase::Failed;
        output.error.assign("Update cancelled");
        return true;
    case InstallationStatus::Failed:
        output.phase = app_model::OtaPhase::Failed;
        output.error.assign("Update failed");
        return true;
    case InstallationStatus::ActivationUncertain:
        output.phase = app_model::OtaPhase::Failed;
        output.error.assign("Update state uncertain");
        return true;
    default:
        return false;
    }
    // Avoid multiplication overflow even for a permitted uint64 byte count.
    const auto total = manifest_.image_size;
    const auto received = ingestion_.received_bytes();
    std::uint8_t percent = 0;
    for (std::uint64_t value = 1; value <= 100; ++value) {
        const auto threshold = ((total / 100) * value) + ((((total % 100) * value) + 99) / 100);
        if (received < threshold) {
            break;
        }
        percent = static_cast<std::uint8_t>(value);
    }
    output.progress = {true, percent};
    return true;
}
} // namespace prd::ota
