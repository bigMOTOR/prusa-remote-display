// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include "server_view.hpp"

#include "escaping.hpp"

namespace prd::provisioning {

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
    body += '}';

    return body;
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
