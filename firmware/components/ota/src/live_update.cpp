// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only
#include "ota/live_update.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <string>
#include <string_view>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wundef"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#pragma GCC diagnostic pop

#include "diagnostics/log.hpp"
#include "diagnostics/tags.hpp"
#include "diagnostics/task_report.hpp"
#include "network/public_https.hpp"
#include "ota/release_bundle.hpp"
#include "ota/release_client.hpp"
#include "ota/release_policy.hpp"
#ifdef PRD_OTA_LAN_MODE
#include "ota_validation_ca.hpp"
#endif

namespace prd::ota {
namespace {

constexpr char kTaskName[] = "prd_ota";
constexpr std::uint32_t kTaskStackBytes = 12288;
constexpr UBaseType_t kTaskPriority = 4;
constexpr UBaseType_t kCommandSlots = 2;
constexpr UBaseType_t kReportSlots = 8;
constexpr std::uint32_t kAssetBudgetMs = 5 * 60 * 1000;

enum class CommandKind : std::uint8_t { Check, Install };
struct Command {
    CommandKind kind{CommandKind::Check};
    std::uint32_t generation{0};
};

TaskHandle_t g_task = nullptr;
QueueHandle_t g_commands = nullptr;
QueueHandle_t g_reports = nullptr;
LiveConfig g_config{};
ReleaseBodyBuffer g_release_body;
std::array<std::uint8_t, kManifestBodyLimit> g_manifest_body{};
std::size_t g_manifest_length{0};
ReleaseCandidate g_release;
DecodedManifest g_manifest;
std::uint32_t g_candidate_generation{0};
std::uint32_t g_next_generation{0};
std::uint32_t g_report_queue_saturations{0};

std::string_view log_label(const ReleaseFetchStatus status) {
    switch (status) {
    case ReleaseFetchStatus::Found:
        return "found";
    case ReleaseFetchStatus::TransportFailed:
        return "transport-failed";
    case ReleaseFetchStatus::ReleaseRejected:
        return "release-rejected";
    }
    return "unknown";
}

std::string_view log_label(const ReleaseBundleStatus status) {
    switch (status) {
    case ReleaseBundleStatus::Accepted:
        return "accepted";
    case ReleaseBundleStatus::ManifestLengthMismatch:
        return "manifest-length-mismatch";
    case ReleaseBundleStatus::ManifestInvalid:
        return "manifest-invalid";
    case ReleaseBundleStatus::AssetLengthMismatch:
        return "asset-length-mismatch";
    case ReleaseBundleStatus::TagMismatch:
        return "tag-mismatch";
    case ReleaseBundleStatus::NotNewer:
        return "not-newer";
    case ReleaseBundleStatus::Incompatible:
        return "incompatible";
    }
    return "unknown";
}

std::string_view log_label(const InstallationStatus status) {
    switch (status) {
    case InstallationStatus::Idle:
        return "idle";
    case InstallationStatus::Refused:
        return "refused";
    case InstallationStatus::InvalidManifest:
        return "invalid-manifest";
    case InstallationStatus::Incompatible:
        return "incompatible";
    case InstallationStatus::Receiving:
        return "receiving";
    case InstallationStatus::Verifying:
        return "verifying";
    case InstallationStatus::Installing:
        return "installing";
    case InstallationStatus::RebootRequired:
        return "reboot-required";
    case InstallationStatus::ActivationUncertain:
        return "activation-uncertain";
    case InstallationStatus::Failed:
        return "failed";
    case InstallationStatus::Cancelled:
        return "cancelled";
    }
    return "unknown";
}

std::string_view log_label(const Compatibility compatibility) {
    switch (compatibility) {
    case Compatibility::NotChecked:
        return "not-checked";
    case Compatibility::MetadataCompatible:
        return "metadata-compatible";
    case Compatibility::InvalidLocalIdentity:
        return "invalid-local-identity";
    case Compatibility::UnsupportedManifestSchema:
        return "unsupported-manifest-schema";
    case Compatibility::WrongBoard:
        return "wrong-board";
    case Compatibility::WrongChip:
        return "wrong-chip";
    case Compatibility::UnsupportedBootloaderSchema:
        return "unsupported-bootloader-schema";
    case Compatibility::InvalidImageSize:
        return "invalid-image-size";
    case Compatibility::InvalidDigestDeclaration:
        return "invalid-digest-declaration";
    case Compatibility::InvalidCandidateVersion:
        return "invalid-candidate-version";
    case Compatibility::Downgrade:
        return "downgrade";
    }
    return "unknown";
}

std::string_view log_label(const IngestionStatus status) {
    switch (status) {
    case IngestionStatus::Idle:
        return "idle";
    case IngestionStatus::Receiving:
        return "receiving";
    case IngestionStatus::DigestMatched:
        return "digest-matched";
    case IngestionStatus::ImageVerified:
        return "image-verified";
    case IngestionStatus::IdentityTooLong:
        return "identity-too-long";
    case IngestionStatus::VerificationFailed:
        return "verification-failed";
    case IngestionStatus::IdentityMismatch:
        return "identity-mismatch";
    case IngestionStatus::CompatibilityRejected:
        return "compatibility-rejected";
    case IngestionStatus::BeginFailed:
        return "begin-failed";
    case IngestionStatus::InvalidChunk:
        return "invalid-chunk";
    case IngestionStatus::TooManyBytes:
        return "too-many-bytes";
    case IngestionStatus::WriteFailed:
        return "write-failed";
    case IngestionStatus::Truncated:
        return "truncated";
    case IngestionStatus::DigestFailed:
        return "digest-failed";
    case IngestionStatus::DigestMismatch:
        return "digest-mismatch";
    case IngestionStatus::Cancelled:
        return "cancelled";
    }
    return "unknown";
}

void log_install_result(const diagnostics::LogLevel level, const std::string_view stage,
                        const Installation& installation) {
    diagnostics::log(level, diagnostics::tag::kApp,
                     "OTA install stage=" + std::string{stage} +
                         " status=" + std::string{log_label(installation.status())} +
                         " compatibility=" + std::string{log_label(installation.compatibility())} +
                         " ingestion=" + std::string{log_label(installation.ingestion_status())});
}

bool send_report(const LiveReport& report) {
    if (g_reports == nullptr) {
        return false;
    }
    if (xQueueSend(g_reports, &report, 0) == pdTRUE) {
        return true;
    }

    ++g_report_queue_saturations;
    const bool coalescible_progress =
        saturation_policy(report) == LiveReportSaturationPolicy::Coalesce;
    diagnostics::log(
        diagnostics::LogLevel::Warning, diagnostics::tag::kApp,
        std::string{"OTA report queue full; "} +
            (coalescible_progress ? "coalesced download progress" : "preserving critical report") +
            " total_saturations=" + std::to_string(g_report_queue_saturations));
    if (coalescible_progress) {
        return true;
    }

    // Availability and phase/terminal transitions are critical §10.7 events. Backpressure is the
    // controlled recovery: the worker cannot advance past one until the serialized app task has
    // made room, while high-frequency byte progress above remains explicitly coalescible.
    return xQueueSend(g_reports, &report, portMAX_DELAY) == pdTRUE;
}

void report_install_failure(const std::uint32_t generation) {
    if (generation == 0) {
        return;
    }
    const LiveReport report = failed_live_install(generation, g_manifest.candidate().version);
    static_cast<void>(send_report(report));
}

void observe(const std::uint32_t generation, const Installation& installation) {
    LiveReport report;
    report.generation = generation;
    report.kind = LiveReportKind::Observation;
    if (installation.observation(report.observation)) {
        static_cast<void>(send_report(report));
    }
}

struct FixedBody {
    std::uint8_t* bytes{nullptr};
    std::size_t capacity{0};
    std::size_t length{0};
};

bool collect(void* const context, const std::uint8_t* const bytes, const std::size_t size) {
    auto* const body = static_cast<FixedBody*>(context);
    if (body == nullptr || bytes == nullptr || size > body->capacity - body->length) {
        return false;
    }
    std::copy_n(bytes, size, body->bytes + body->length);
    body->length += size;
    return true;
}

network::PublicHttpsResponse fetch_asset(const ReleaseAsset& asset, const ReleaseResource resource,
                                         network::PublicChunkSink sink, void* const sink_context,
                                         const std::size_t maximum) {
    ReleaseRoute route{resource};
    network::PublicHttpsRequest request;
    request.url = asset.url_view();
    request.media = resource == ReleaseResource::Image ? network::PublicMedia::Binary
                                                       : network::PublicMedia::Json;
    request.maximum_body_bytes = maximum;
    request.total_budget_ms = kAssetBudgetMs;
    request.connect_budget_ms = 5000;
    request.slice_ms = 1000;
#ifdef PRD_OTA_LAN_MODE
    request.maximum_redirects = 0;
    request.certificate_pem = kPrdOtaValidationCaPem;
#else
    request.maximum_redirects = 1;
#endif
    request.url_policy = release_url_policy;
    request.url_policy_context = &route;
    request.sink = sink;
    request.sink_context = sink_context;
    return network::get_public_https(request);
}

void check_release(const std::uint32_t generation) {
    const ReleaseFetchResult discovered = discover_latest_release(g_release_body);
    if (discovered.status != ReleaseFetchStatus::Found) {
        std::string line =
            "OTA discovery rejected metadata status=" + std::string{log_label(discovered.status)} +
            " transport=" + std::string{network::log_label(discovered.transport.outcome)};
        if (discovered.status == ReleaseFetchStatus::ReleaseRejected) {
            line += " release=" + std::string{log_label(discovered.release)};
        } else if (discovered.transport.outcome == network::PublicHttpsOutcome::Ok) {
            line += " accounting=length-mismatch";
        }
        diagnostics::log(diagnostics::LogLevel::Warning, diagnostics::tag::kApp, line);
        return;
    }
    if (discovered.candidate.manifest.size == 0 ||
        discovered.candidate.manifest.size > g_manifest_body.size()) {
        diagnostics::log(diagnostics::LogLevel::Warning, diagnostics::tag::kApp,
                         "OTA discovery rejected manifest-size");
        return;
    }

    FixedBody body{g_manifest_body.data(), g_manifest_body.size(), 0};
    const auto response = fetch_asset(discovered.candidate.manifest, ReleaseResource::Manifest,
                                      collect, &body, g_manifest_body.size());
    if (response.outcome != network::PublicHttpsOutcome::Ok ||
        response.body_length != body.length) {
        std::string line = "OTA discovery rejected manifest-transfer transport=" +
                           std::string{network::log_label(response.outcome)};
        if (response.outcome == network::PublicHttpsOutcome::Ok) {
            line += " accounting=length-mismatch";
        }
        diagnostics::log(diagnostics::LogLevel::Warning, diagnostics::tag::kApp, line);
        return;
    }

    const std::string_view manifest_text{reinterpret_cast<const char*>(body.bytes), body.length};
    DecodedManifest manifest;
    const ReleaseBundleStatus bundle =
        validate_release_bundle(discovered.candidate, manifest_text, g_config.device, manifest);
    if (bundle != ReleaseBundleStatus::Accepted) {
        diagnostics::log(diagnostics::LogLevel::Warning, diagnostics::tag::kApp,
                         "OTA discovery rejected bundle=" + std::string{log_label(bundle)});
        return;
    }

    g_release = discovered.candidate;
    g_manifest = manifest;
    g_manifest_length = body.length;
    g_candidate_generation = generation;

    LiveReport report;
    report.generation = generation;
    report.kind = LiveReportKind::Available;
    report.observation.phase = app_model::OtaPhase::Available;
    report.observation.available_version.assign(g_manifest.candidate().version);
    static_cast<void>(send_report(report));
}

struct ImageSink {
    Installation* installation{nullptr};
    std::uint32_t generation{0};
    std::uint8_t reported_percent{0};
};

bool append_image(void* const context, const std::uint8_t* const bytes, const std::size_t size) {
    auto* const sink = static_cast<ImageSink*>(context);
    if (sink == nullptr || sink->installation == nullptr ||
        !sink->installation->append(bytes, size)) {
        return false;
    }
    app_model::OtaObservation observation;
    if (sink->installation->observation(observation) &&
        observation.progress.percent >= static_cast<std::uint8_t>(sink->reported_percent + 5U)) {
        sink->reported_percent = observation.progress.percent;
        LiveReport report{sink->generation, LiveReportKind::Observation, observation};
        return send_report(report);
    }
    return true;
}

void install(const std::uint32_t generation) {
    if (generation == 0 || generation != g_candidate_generation || g_config.port == nullptr ||
        g_config.admission == nullptr) {
        const std::string_view binding = generation == 0 ? "invalid-generation"
                                         : generation != g_candidate_generation ? "stale-generation"
                                         : g_config.port == nullptr ? "missing-port"
                                                                    : "missing-admission";
        diagnostics::log(diagnostics::LogLevel::Warning, diagnostics::tag::kApp,
                         "OTA install rejected request binding=" + std::string{binding});
        if (g_config.admission != nullptr) {
            g_config.admission->release(false);
        }
        report_install_failure(generation);
        return;
    }

    Installation installation{*g_config.port, *g_config.admission};
    const std::string_view manifest_text{reinterpret_cast<const char*>(g_manifest_body.data()),
                                         g_manifest_length};
    if (!installation.request(manifest_text, g_config.device, DowngradePolicy::Reject)) {
        log_install_result(diagnostics::LogLevel::Warning, "request", installation);
        g_config.admission->release(false);
        report_install_failure(generation);
        return;
    }
    observe(generation, installation);

    ImageSink sink{&installation, generation, 0};
    const auto response = fetch_asset(g_release.image, ReleaseResource::Image, append_image, &sink,
                                      static_cast<std::size_t>(g_release.image.size));
    const bool received = response.outcome == network::PublicHttpsOutcome::Ok &&
                          response.body_length == g_release.image.size;
    if (!received || !installation.finish()) {
        // If the transport failed, this is the first finish() call and deliberately forces a
        // terminal installation state so destruction releases admission. Otherwise finish()
        // already made the state terminal and this second call is a harmless no-op.
        static_cast<void>(installation.finish());
        diagnostics::log(diagnostics::LogLevel::Warning, diagnostics::tag::kApp,
                         "OTA install image-transfer transport=" +
                             std::string{network::log_label(response.outcome)});
        log_install_result(diagnostics::LogLevel::Warning, "receive", installation);
        observe(generation, installation);
        return;
    }
    observe(generation, installation);
    if (!installation.verify()) {
        log_install_result(diagnostics::LogLevel::Warning, "verify", installation);
        observe(generation, installation);
        return;
    }
    observe(generation, installation);
    const bool activated = installation.activate();
    log_install_result(activated ? diagnostics::LogLevel::Info : diagnostics::LogLevel::Warning,
                       "activate", installation);
    observe(generation, installation);
}

void worker(void*) {
    while (true) {
        Command command;
        if (xQueueReceive(g_commands, &command, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (command.kind == CommandKind::Check) {
            check_release(command.generation);
        } else {
            install(command.generation);
        }
        // The high-water mark is lifetime-wide, so this post-operation report captures the
        // deepest path reached by discovery or installation rather than an optimistic startup
        // value. Physical OTA evidence can now bind the real worker's stack headroom to the
        // operation that exercised it.
        diagnostics::log_task_stack(kTaskName, kTaskStackBytes);
    }
}

} // namespace

bool start_live_update(const LiveConfig& config) {
    if (g_task != nullptr) {
        return true;
    }
    if (config.port == nullptr || config.admission == nullptr || config.device.board.empty() ||
        config.device.chip.empty() || config.device.version.empty() ||
        config.device.inactive_slot_size == 0) {
        diagnostics::log(diagnostics::LogLevel::Error, diagnostics::tag::kApp,
                         "live OTA invalid startup configuration");
        return false;
    }
    g_config = config;
    g_commands = xQueueCreate(kCommandSlots, sizeof(Command));
    g_reports = xQueueCreate(kReportSlots, sizeof(LiveReport));
    if (g_commands == nullptr || g_reports == nullptr) {
        diagnostics::log(diagnostics::LogLevel::Error, diagnostics::tag::kApp,
                         "live OTA queue allocation failed");
        if (g_commands != nullptr) {
            vQueueDelete(g_commands);
            g_commands = nullptr;
        }
        if (g_reports != nullptr) {
            vQueueDelete(g_reports);
            g_reports = nullptr;
        }
        return false;
    }
    if (xTaskCreate(worker, kTaskName, kTaskStackBytes, nullptr, kTaskPriority, &g_task) !=
        pdPASS) {
        diagnostics::log(diagnostics::LogLevel::Error, diagnostics::tag::kApp,
                         "live OTA task allocation failed");
        vQueueDelete(g_commands);
        vQueueDelete(g_reports);
        g_commands = nullptr;
        g_reports = nullptr;
        g_task = nullptr;
        return false;
    }
    return true;
}

std::uint32_t request_release_check() {
    if (g_commands == nullptr || g_next_generation == std::numeric_limits<std::uint32_t>::max()) {
        return 0;
    }
    const Command command{CommandKind::Check, ++g_next_generation};
    return xQueueSend(g_commands, &command, 0) == pdTRUE ? command.generation : 0;
}

bool request_live_install(const std::uint32_t generation) {
    const Command command{CommandKind::Install, generation};
    return g_commands != nullptr && xQueueSend(g_commands, &command, 0) == pdTRUE;
}

bool take_live_report(LiveReport& report) {
    return g_reports != nullptr && xQueueReceive(g_reports, &report, 0) == pdTRUE;
}

} // namespace prd::ota
