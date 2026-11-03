// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "ota/ingestion.hpp"
#include "ota/manifest.hpp"

namespace prd::app_model {
struct OtaObservation;
}

namespace prd::ota {

// Internal, non-persistent reservation identity. Installation never exports it.
class InstallationLease {
  public:
    InstallationLease() = default;
    [[nodiscard]] bool valid() const {
        return generation_ != 0;
    }
    [[nodiscard]] bool operator==(const InstallationLease& other) const {
        return generation_ == other.generation_;
    }

  private:
    explicit InstallationLease(std::uint64_t generation) : generation_(generation) {}
    std::uint64_t generation_{0};
    friend class InstallationPort;
};

// A reservation is independent of the consumed SDK write handle. Every entry
// checks the lease; the device implementation also serializes legacy writers.
// Successful selection latches ownership UNTIL REBOOT, including after release.
// No operation here reboots or treats JSON signing claims as authentication.
enum class SelectionResult : std::uint8_t { Rejected, Selected, Uncertain };

class InstallationPort {
  public:
    virtual ~InstallationPort() = default;
    [[nodiscard]] virtual InstallationLease reserve() = 0;
    [[nodiscard]] virtual IngestionBegin begin(InstallationLease, std::uint64_t) = 0;
    [[nodiscard]] virtual bool write(InstallationLease, const std::uint8_t*, std::size_t) = 0;
    [[nodiscard]] virtual bool finish_digest(InstallationLease, ImageDigest&) = 0;
    [[nodiscard]] virtual bool finalize(InstallationLease, ImageIdentity&) = 0;
    virtual void abort(InstallationLease) = 0;
    // Rechecks the approved metadata against this lease's actual digest/identity
    // before selecting its exact target. Never accepts a caller-supplied verified flag.
    // Only Rejected proves selection was not attempted/changed. Uncertain may
    // have changed boot metadata and MUST retain the reservation until restart.
    [[nodiscard]] virtual SelectionResult select(InstallationLease, const Candidate&) = 0;
    virtual void release(InstallationLease) = 0;

  protected:
    [[nodiscard]] static InstallationLease lease_for(std::uint64_t generation) {
        return InstallationLease{generation};
    }
};

[[nodiscard]] InstallationPort& device_installation_port();

// The application task supplies the existing exclusive-operation owner. Calls
// are serialized there; worker observations cannot acquire or release admission.
class InstallationAdmission {
  public:
    virtual ~InstallationAdmission() = default;
    [[nodiscard]] virtual bool acquire() = 0;
    virtual void release(bool retain_until_restart) = 0;
};

enum class InstallationStatus : std::uint8_t {
    Idle,
    Refused,
    InvalidManifest,
    Incompatible,
    Receiving,
    Verifying,
    Installing,
    RebootRequired,
    ActivationUncertain,
    Failed,
    Cancelled,
};

// Offline, one-shot transaction. The caller supplies an explicitly authorized
// install request and bytes; no trigger, downloader, scheduling or reboot lives
// here. Ports outlive this object. Serialized calls, no borrowed input lifetime.
class Installation final : private IngestionPort {
  public:
    Installation(InstallationPort& port, InstallationAdmission& admission)
        : port_(port), admission_(admission), ingestion_(*this) {}
    ~Installation() override;
    Installation(const Installation&) = delete;
    Installation& operator=(const Installation&) = delete;
    Installation(Installation&&) = delete;
    Installation& operator=(Installation&&) = delete;
    [[nodiscard]] bool request(std::string_view manifest, const DeviceCompatibility& device,
                               DowngradePolicy policy);
    [[nodiscard]] bool append(const std::uint8_t* bytes, std::size_t size);
    // Three explicit, ordered worker steps allow observations between blocking
    // operations without inventing a visual dwell. 100% received is NOT installed.
    [[nodiscard]] bool finish();
    [[nodiscard]] bool verify();
    [[nodiscard]] bool activate();
    void cancel();
    [[nodiscard]] InstallationStatus status() const {
        return status_;
    }
    [[nodiscard]] IngestionStatus ingestion_status() const {
        return ingestion_.status();
    }
    [[nodiscard]] Compatibility compatibility() const {
        return compatibility_;
    }
    // Refused/malformed/incompatible requests have no observation authority;
    // caller must not overwrite an incumbent operation with their result.
    // Returned snapshots (including failures) are historical data, not an ingress
    // capability. A future worker binding must filter by its live request/session
    // before dispatch, so a completed old request cannot overwrite a new one.
    [[nodiscard]] bool observation(app_model::OtaObservation& output) const;

  private:
    [[nodiscard]] IngestionBegin begin(std::uint64_t size) override;
    [[nodiscard]] bool write(const std::uint8_t* bytes, std::size_t size) override;
    [[nodiscard]] bool finish_digest(ImageDigest& digest) override;
    [[nodiscard]] bool finalize(ImageIdentity& identity) override;
    void abort() override;
    void release();
    [[nodiscard]] bool fail();
    InstallationPort& port_;
    InstallationAdmission& admission_;
    DecodedManifest manifest_{};
    InstallationLease lease_{};
    ImageIngestion ingestion_;
    Compatibility compatibility_{Compatibility::NotChecked};
    InstallationStatus status_{InstallationStatus::Idle};
    bool admitted_{false};
    bool observable_{false};
    bool begin_refused_{false};
};
} // namespace prd::ota
