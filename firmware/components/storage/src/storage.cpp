// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include "storage/storage.hpp"

#include <string>

// ESP-IDF headers are not -Wundef clean. Suppress locally (per architecture
// spec §12.3) so the project -Wundef policy still applies to our own code.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wundef"
#include "esp_err.h"
#include "nvs.h"
#include "nvs_flash.h"
#pragma GCC diagnostic pop

#include "bring_up.hpp"
#include "diagnostics/log.hpp"
#include "diagnostics/tags.hpp"

// The NVS adapter — the only translation unit in the project that includes nvs.h.
//
// IT CONTAINS NO POLICY. Every ordering rule and the mapping onto StorageStatus live in
// bring_up.cpp, where the host suite can reach them. What is here is an
// argument-for-argument translation of two operations plus the boot log line.
//
// ⛔ nvs_flash_init() IS NEVER CALLED, AND MUST NEVER BE. This is the correction of
// 2026-08-14 and it is a hardware-safety rule, not a style preference.
//
// Under CONFIG_NVS_ENCRYPTION, ESP-IDF v5.5.4's nvs_flash_init() (nvs_api.cpp:136) reads
// the NVS security config, and on failure calls nvs_flash_generate_keys_v2(), which under
// the HMAC scheme reaches esp_efuse_write_key(EFUSE_BLK_KEY0,
// ESP_EFUSE_KEY_PURPOSE_HMAC_UP, ...) at nvs_sec_provider.c:181 — AN IRREVERSIBLE eFUSE
// BURN on a virgin device. The scheme registers itself through an ESP_SYSTEM_INIT_FN that
// ESP-IDF force-links, so calling nvs_flash_init() is by itself sufficient to trigger it.
// An earlier revision of this file called it and described it as a "plain bring-up",
// which was wrong; that mistake is recorded in docs/development/session-notes.md.
//
// nvs_flash_init_partition() (nvs_api.cpp:124) goes straight to
// NVSPartitionManager::init_partition() with no security path, no key derivation and no
// eFuse access, whatever the Kconfig says. It is what this file calls.
//
// TWO INDEPENDENT GUARDS BACK THAT UP, because one call site is not an invariant:
// CONFIG_NVS_ENCRYPTION is off in sdkconfig.defaults, which removes generate_keys_hmac
// from the image entirely; and tools/check-efuse-safety.sh fails the build if any
// project source names nvs_flash_init, the key-generation APIs or esp_efuse_write_key.
//
// PLAINTEXT NVS IS THE PRODUCT'S CURRENT STORAGE POLICY IN EVERY PROFILE, INCLUDING
// RELEASE (owner ruling, 2026-08-14). There is deliberately no development flag and no
// release #error here: encrypted NVS is deferred production-security hardening with no
// scheduled milestone, not a thing this build is temporarily standing in for.

namespace prd::storage {
namespace {

// Cross-check of the bound namespaces.cpp asserts against, which cannot include this
// header. If ESP-IDF ever changes the field, this fails to compile here rather than
// silently letting a name through that the flash cannot hold.
static_assert(NVS_NS_NAME_MAX_SIZE == 16,
              "namespaces.cpp assumes NVS stores 15 characters plus a terminator");

Report g_report{};
bool g_attempted = false;

[[nodiscard]] BackendResult classify_init(const esp_err_t error) {
    switch (error) {
    case ESP_OK:
        return BackendResult::Ok;

    // "NVS partition doesn't contain any empty pages. This may happen if NVS partition
    // was truncated." Deliberately NOT followed by an erase — see bring_up.cpp.
    case ESP_ERR_NVS_NO_FREE_PAGES:
        return BackendResult::NoFreePages;

    // "NVS partition contains data in new format and cannot be recognized by this
    // version of code" — a downgrade, with the user's credentials intact behind it.
    case ESP_ERR_NVS_NEW_VERSION_FOUND:
        return BackendResult::VersionAhead;

    default:
        return BackendResult::Failed;
    }
}

// The production backend. Holds no state: NVS keeps its own, and a handle is opened and
// closed inside probe() so none is retained anywhere for §5.10 to leak.
class NvsBackend final : public Backend {
  public:
    [[nodiscard]] BackendResult initialise() override {
        // The default partition by name, through the API with no security path. See the
        // file header for why this is not nvs_flash_init().
        return classify_init(nvs_flash_init_partition(NVS_DEFAULT_PART_NAME));
    }

    [[nodiscard]] BackendResult probe(const Namespace space) override {
        const std::string namespace_name{name(space)};

        // NVS_READWRITE rather than NVS_READONLY on purpose. A namespace that has never
        // been written does not exist yet, and read-only would report that as
        // ESP_ERR_NVS_NOT_FOUND — a failure reading of a perfectly healthy first boot.
        // Read-write creates it, which is both the honest probe of "can this be used"
        // and a one-time write rather than a per-boot one.
        nvs_handle_t handle = 0;
        if (nvs_open(namespace_name.c_str(), NVS_READWRITE, &handle) != ESP_OK) {
            return BackendResult::Failed;
        }

        nvs_close(handle);
        return BackendResult::Ok;
    }
};

void log_report(const Report& report) {
    std::string message = "storage status=" + std::string{app_model::log_label(report.status)} +
                          " outcome=" + std::string{log_label(report.outcome)} +
                          " namespaces=" + std::to_string(kNamespaceCount) + " encryption=off";

    if (report.outcome == Outcome::NamespaceUnusable) {
        message += " failed_namespace=" + std::string{log_label(report.failed_namespace)};
    }

    if (recovery_required(report.outcome)) {
        // Stated in the log because the remedy differs from every other failure: the
        // partition is intact and may hold the user's configuration and credentials, so
        // it was deliberately left alone. Milestone 3.10 owns the consented recovery.
        message += " preserved=yes recovery_required=yes";
    }

    const auto level = report.status == app_model::StorageStatus::Ready
                           ? diagnostics::LogLevel::Info
                           : diagnostics::LogLevel::Warning;

    diagnostics::log(level, diagnostics::tag::kStorage, message);
}

} // namespace

Report initialise() {
    // Idempotent. Returning the first conclusion keeps a caller that is wired up twice
    // from re-running the probe writes, and it is what makes report() and the value
    // returned here provably the same fact.
    if (g_attempted) {
        return g_report;
    }

    g_attempted = true;

    NvsBackend backend;
    g_report = bring_up(backend);
    log_report(g_report);

    return g_report;
}

Report report() {
    return g_report;
}

} // namespace prd::storage
