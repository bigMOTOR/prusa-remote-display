// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include "app_model/app_state.hpp"

namespace prd::app_model {

bool operator==(const LossEpisode& lhs, const LossEpisode& rhs) {
    return lhs.active == rhs.active && lhs.automatic_spent == rhs.automatic_spent &&
           lhs.presenting == rhs.presenting && lhs.origin == rhs.origin;
}

bool operator!=(const LossEpisode& lhs, const LossEpisode& rhs) {
    return !(lhs == rhs);
}

bool content_equals(const AppState& lhs, const AppState& rhs) {
    // Device data.
    if (lhs.boot != rhs.boot || lhs.configuration != rhs.configuration ||
        lhs.network != rhs.network || lhs.display != rhs.display || lhs.ota != rhs.ota) {
        return false;
    }

    // Device-owned flows.
    if (lhs.provisioning != rhs.provisioning || lhs.modal_surface != rhs.modal_surface ||
        lhs.device_fault != rhs.device_fault || lhs.recovery_mode != rhs.recovery_mode ||
        lhs.factory_resetting != rhs.factory_resetting ||
        lhs.factory_reset_failed != rhs.factory_reset_failed ||
        lhs.device_operation != rhs.device_operation) {
        return false;
    }

    // ⛔ factory_reset_failed IS CONTENT, AND COMPARING IT IS NOT OPTIONAL — audit item 5. It decides
    // whether the destructive executor may run again and whether the reset still owns the device, so
    // a store that treated a state differing only in this flag as unchanged would be reporting that
    // nothing had happened at the exact moment a reset became terminal. It is a fact about the
    // device, not bookkeeping about when something was observed.
    //
    // ⬅️ factory_reset_armed_at IS STILL DELIBERATELY NOT COMPARED, and the two are not the same
    // kind of thing. That one is the confirmation window's internal timing: the transition a viewer
    // must see is modal_surface becoming FactoryResetConfirmation, which is compared above, and
    // including the moment would bump the revision and re-project the whole tree for a value nothing
    // draws. The latch above changes what the device is ALLOWED TO DO, which nothing else records.

    // Printer data.
    if (lhs.printer_connection != rhs.printer_connection ||
        lhs.printer_identity != rhs.printer_identity || lhs.printer != rhs.printer ||
        lhs.temperatures != rhs.temperatures || lhs.current_job != rhs.current_job ||
        lhs.last_completed_job != rhs.last_completed_job || lhs.alert != rhs.alert) {
        return false;
    }

    // Subject, conditions, confidence.
    //
    // ⛔ THE EPISODE IS CONTENT. It decides whether the acknowledgement is on the panel, so a store
    // that treated two states differing only in it as unchanged would swallow both the
    // acknowledgement appearing and its retirement.
    return lhs.subject == rhs.subject && lhs.subject_condition == rhs.subject_condition &&
           lhs.confidence == rhs.confidence && lhs.access_condition == rhs.access_condition &&
           lhs.loss_episode == rhs.loss_episode &&
           lhs.loss_presentation_generation == rhs.loss_presentation_generation;
}

bool operator==(const AppState& lhs, const AppState& rhs) {
    return lhs.revision == rhs.revision && lhs.last_change_at == rhs.last_change_at &&
           lhs.last_event_at == rhs.last_event_at &&
           lhs.last_printer_sequence == rhs.last_printer_sequence &&
           lhs.last_job_sequence == rhs.last_job_sequence && lhs.diagnostics == rhs.diagnostics &&
           content_equals(lhs, rhs);
}

bool operator!=(const AppState& lhs, const AppState& rhs) {
    return !(lhs == rhs);
}

} // namespace prd::app_model
