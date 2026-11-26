// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include "server_view.hpp"

#include "escaping.hpp"

namespace prd::provisioning {
namespace {

using app_model::PrinterValidationFailure;
using app_model::PrinterValidationReason;
using app_model::PrinterValidationStage;

[[nodiscard]] std::string_view diagnostic_stage(const PrinterValidationFailure& failure) {
    if (failure.reason == PrinterValidationReason::None ||
        failure.reason == PrinterValidationReason::Cancelled) {
        return {};
    }

    switch (failure.stage) {
    case PrinterValidationStage::None:
        return {};
    case PrinterValidationStage::AddressCheck:
        return "Printer address check";
    case PrinterValidationStage::RequestPreparation:
        return "PrusaLink request preparation";
    case PrinterValidationStage::VersionProbe:
        return "PrusaLink version check";
    case PrinterValidationStage::Status:
        return "PrusaLink status check";
    }

    return {};
}

[[nodiscard]] std::string_view diagnostic_summary(const PrinterValidationFailure& failure) {
    if (failure.http_status == 401 || failure.http_status == 403) {
        return "PrusaLink denied access.";
    }
    if (failure.http_status >= 500 && failure.http_status <= 599) {
        return "PrusaLink returned a server error.";
    }

    switch (failure.reason) {
    case PrinterValidationReason::Unreachable:
        return "Could not connect to PrusaLink.";
    case PrinterValidationReason::Timeout:
        return failure.has_http_status() ? "The request timed out while reading the response."
                                         : "No HTTP response arrived before the request timed out.";
    case PrinterValidationReason::TransportFailed:
        return failure.has_http_status()
                   ? "PrusaLink returned HTTP headers."
                   : "The connection failed before the response could be read.";
    case PrinterValidationReason::RequestRejected:
        return "The display could not send the PrusaLink request.";
    case PrinterValidationReason::ResponseTooLarge:
        return "The response was too large for the display.";
    case PrinterValidationReason::Unauthorized:
        return "PrusaLink denied access.";
    case PrinterValidationReason::EndpointUnsupported:
        return "The expected PrusaLink endpoint was missing.";
    case PrinterValidationReason::RequestUnsupported:
        return "The endpoint did not accept the PrusaLink request.";
    case PrinterValidationReason::PrinterError:
        return "The display could not use the PrusaLink response.";
    case PrinterValidationReason::UnexpectedStatus:
        return "The endpoint returned an unexpected HTTP status.";
    case PrinterValidationReason::MalformedBody:
        return "The display could not read the PrusaLink response.";
    case PrinterValidationReason::SchemaMismatch:
        return "The response did not contain the expected PrusaLink data.";
    case PrinterValidationReason::HostUnusable:
        return "The display could not use the printer address.";
    case PrinterValidationReason::NameNotResolved:
        return "The display could not resolve the printer name.";
    case PrinterValidationReason::NotOnLan:
        return "The printer address did not resolve to the local network.";
    case PrinterValidationReason::None:
    case PrinterValidationReason::Cancelled:
        return {};
    }

    return {};
}

} // namespace

// ⬅️ DEFINED HERE RATHER THAN IN server.cpp, WHICH IS WHY IT MOVED. The label is a pure switch over
// a public enumerator, and status_body() below needs it — so it has to live in a translation unit
// the host suite can compile. server.cpp includes esp_http_server.h and cannot be one.
std::string_view log_label(const SetupState state) {
    switch (state) {
    case SetupState::Editing:
        return "editing";

    case SetupState::Validating:
        return "validating";

    case SetupState::WifiValidated:
        return "wifi-validated";

    case SetupState::Succeeded:
        return "succeeded";

    case SetupState::Failed:
        return "failed";
    }

    return "editing";
}

bool admits_printer_stage(const Stage stage, const SetupState state) {
    // The Wi-Fi stage is always admissible: it is step one, and D-6 keeps the form editable after a
    // recoverable failure so another submission needs no reboot.
    if (stage == Stage::Wifi) {
        return true;
    }

    return state == SetupState::WifiValidated;
}

std::string status_body(const ServerSnapshot& snapshot, const Surface arrival,
                        const std::string_view version, const CsrfToken token) {
    const bool configurable = permits_mutation(arrival);

    std::string body;
    body.reserve(256);
    body += '{';
    append_json_string(body, FieldName{"product"}, "Prusa Remote Display");
    body += ',';
    append_json_string(body, FieldName{"version"}, version);
    body += ',';
    append_json_string(body, FieldName{"csrfToken"},
                       configurable ? token.text : std::string_view{});
    body += ',';
    append_json_bool(body, FieldName{"setupActive"}, true);
    body += ',';

    // ⬅️ WHAT THE PAGE NEEDS TO STOP OFFERING A CONTROL IT CANNOT HONOUR. A non-secret fact
    // about the caller's own connection, on the response the page already polls — owner ruling
    // D-3's mechanism, not a second one. It names a capability, not an interface or an
    // address: the page has no business knowing the device's network topology, and a label
    // like "station-lan" would tell every caller where the privileged surface is.
    append_json_bool(body, FieldName{"canConfigure"}, configurable);
    body += ',';

    // Owner ruling D-3: the minimum the page needs to render editing, validating, success
    // and a recoverable failure. Not one submitted byte, and no stored secret.
    append_json_string(body, FieldName{"setupState"}, log_label(snapshot.setup_state));
    body += ',';
    append_json_string(body, FieldName{"setupReason"}, snapshot.setup_reason.view());
    body += ',';
    append_json_string(body, FieldName{"updateVersion"}, snapshot.update_version.view());
    body += ',';
    append_json_bool(body, FieldName{"canInstall"},
                     configurable && !snapshot.update_version.empty() &&
                         !snapshot.update_installing);
    body += ',';
    append_json_bool(body, FieldName{"updateInstalling"}, snapshot.update_installing);
    if (configurable) {
        const auto& display = snapshot.display_settings;
        body += ',';
        append_json_bool(body, FieldName{"canAdjustDisplay"}, display.adjustable);
        body += ',';
        append_json_bool(body, FieldName{"canShowDemo"}, display.demo_available);
        body += ',';
        append_json_bool(body, FieldName{"demoRunning"}, display.demo_running);
        body += ',';
        append_json_bool(body, FieldName{"displaySettingsSaved"}, display.saved);
        body += ',';
        append_json_string(body, FieldName{"displaySettingsRevision"},
                           std::to_string(display.revision));
        body += ',';
        append_json_string(body, FieldName{"displayMode"},
                           std::to_string(static_cast<unsigned>(display.preferences.mode)));
        body += ',';
        append_json_string(body, FieldName{"backlight"},
                           std::to_string(static_cast<unsigned>(display.preferences.backlight)));
    }
    if (configurable && !token.text.empty() && snapshot.setup_state == SetupState::Failed &&
        !diagnostic_stage(snapshot.validation_failure).empty() &&
        !diagnostic_summary(snapshot.validation_failure).empty()) {
        body += ',';
        append_json_string(body, FieldName{"diagnosticReportId"},
                           std::to_string(snapshot.setup_revision));

        // This distinction is for truthful recovery copy. The status view still exposes
        // neither the HTTP status nor the more detailed manual report.
        if (snapshot.setup_reason.view() == "printer-unusable-response" &&
            snapshot.validation_failure.body_interrupted()) {
            body += ',';
            append_json_bool(body, FieldName{"printerResponseIncomplete"}, true);
        }
    }
    body += '}';

    return body;
}

std::string diagnostic_report(const ServerSnapshot& snapshot,
                              const std::string_view requested_revision,
                              const app_model::SemanticVersion& version) {
    if (snapshot.setup_state != SetupState::Failed ||
        requested_revision != std::to_string(snapshot.setup_revision)) {
        return {};
    }

    const auto& failure = snapshot.validation_failure;
    const std::string_view stage = diagnostic_stage(failure);
    const std::string_view summary = diagnostic_summary(failure);
    if (stage.empty() || summary.empty()) {
        return {};
    }

    std::string report{"Prusa Remote Display setup diagnostic\nFirmware: "};
    report += version.to_string();
    report += "\nStage: ";
    report += stage;
    report += "\nSummary: ";
    report += summary;
    if (failure.has_http_status()) {
        report += "\nHTTP status: ";
        report += std::to_string(failure.http_status);
        if (failure.reason == PrinterValidationReason::TransportFailed) {
            report += "\nResponse body: The connection failed while reading it.";
        }
    }

    return report;
}

SubmitAnswer submit_answer(const bool validated, const SubmissionHandoff handoff) {
    // ⛔ VALIDATION FIRST, AND IT SHORT-CIRCUITS. A body that did not validate never reached the
    // sink, so `handoff` describes nothing and must not be able to change the answer.
    if (!validated) {
        return SubmitAnswer::Report;
    }

    // ⛔ THE ONLY ROUTE TO Accepted. Every value of SubmissionHandoff other than Accepted lands on
    // Unavailable, so a future addition to that enum fails closed rather than silently claiming a
    // handoff that did not happen.
    return handoff == SubmissionHandoff::Accepted ? SubmitAnswer::Accepted
                                                  : SubmitAnswer::Unavailable;
}

} // namespace prd::provisioning
