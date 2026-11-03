// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include "exclusive_operation.hpp"

namespace prd::app_core::exclusive {

std::string_view log_label(const Owner owner) {
    switch (owner) {
    case Owner::None:
        return "none";
    case Owner::FactoryReset:
        return "factory-reset";
    case Owner::FirmwareUpdate:
        return "firmware-update";
    case Owner::DeviceFault:
        return "device-fault";
    }

    return "unknown";
}

bool fault_owns_device(const app_model::DeviceFaultReason reason) {
    // ⬅️ ONE LIST, IN app_model. The reducer needs the same answer for its precedence rule — a fault
    // that holds the device must not be silently replaced by a later observation — and two copies of
    // "which faults hold the device" is exactly the duplication that lets one drift.
    return app_model::fault_holds_device(reason);
}

Owner owner_of(const Conditions& conditions) {
    // ⬅️ EITHER TERM IS ENOUGH. A confirmed reset is exclusive from the moment it is confirmed until
    // the device restarts, and the scheduled restart is a second, independent witness to the same
    // interval so the barrier does not rest on one flag remaining set.
    if (conditions.factory_resetting || conditions.reset_restart_scheduled) {
        return Owner::FactoryReset;
    }

    // ⛔ AND THE LATCH IS A THIRD WITNESS NO LATER EVENT CAN CLEAR — audit item 5. It reports the
    // reset as the owner, because that is what it is: a confirmed reset executed and did not finish.
    // Unlike the reason below, this cannot be overwritten by a queued observation.
    if (conditions.factory_reset_failed) {
        return Owner::FactoryReset;
    }

    // ⛔ CHECKED LAST, AND IT IS WHAT HOLDS THE BOOT — audit item 5. A resume that failed at boot sets
    // neither of the terms above: the reducer's `factory_resetting` comes only from a confirmed
    // press, and no restart is ever scheduled on a failure. Without this the boot released
    // everything.
    if (fault_owns_device(conditions.device_fault)) {
        return Owner::DeviceFault;
    }

    if (conditions.firmware_update_active) {
        return Owner::FirmwareUpdate;
    }

    return Owner::None;
}

bool firmware_update_permitted(const Conditions& conditions) {
    return !active(conditions) && !conditions.factory_reset_confirmation_armed &&
           conditions.device_fault == app_model::DeviceFaultReason::None &&
           (conditions.provisioning == app_model::ProvisioningPhase::Inactive ||
            conditions.firmware_update_authorized_surface) &&
           !conditions.configuration_in_flight && !conditions.first_boot_pending;
}

bool station_work_permitted(const Conditions& conditions) {
    const auto owner = owner_of(conditions);
    return owner == Owner::None || owner == Owner::FirmwareUpdate;
}

bool FirmwareUpdateAdmission::authorize(const Conditions& live) {
    if (!firmware_update_permitted(live) || held_.exchange(true, std::memory_order_acq_rel)) {
        return false;
    }
    authorized_.store(true, std::memory_order_release);
    return true;
}

bool FirmwareUpdateAdmission::acquire() {
    return held_.load(std::memory_order_acquire) &&
           authorized_.exchange(false, std::memory_order_acq_rel);
}

void FirmwareUpdateAdmission::release(bool retain_until_restart) {
    authorized_.store(false, std::memory_order_release);
    if (!held_.load(std::memory_order_acquire)) {
        return;
    }
    if (retain_until_restart) {
        activation_locked_.store(true, std::memory_order_release);
    }
    if (!activation_locked_.load(std::memory_order_acquire)) {
        held_.store(false, std::memory_order_release);
    }
}

bool active(const Conditions& conditions) {
    return owner_of(conditions) != Owner::None;
}

bool real_printer_source_permitted(const Conditions& conditions) {
    return !conditions.synthetic_status_source;
}

bool printer_reports_permitted(const Conditions& conditions) {
    // ⛔ THE DIAGNOSTIC SOURCE TERM COMES FIRST AND IS UNCONDITIONAL. A report can only have come
    // from a real worker, and while a synthetic source owns the printer data there must be no
    // second writer — including one that was launched before the instrument started.
    if (!real_printer_source_permitted(conditions)) {
        return false;
    }

    return !active(conditions);
}

bool candidate_activation_permitted(const Conditions& conditions) {
    return !active(conditions);
}

bool provisioning_work_permitted(const Conditions& conditions) {
    return !active(conditions);
}

bool printer_worker_permitted(const Conditions& conditions) {
    // Structurally first, for the same reason as in printer_reports_permitted(): no real worker may
    // be launched while a diagnostic status source is driving the store.
    if (!real_printer_source_permitted(conditions)) {
        return false;
    }

    if (active(conditions)) {
        return false;
    }

    // Each of these is an existing lifecycle fact rather than a new one: provisioning owns the radio
    // while it runs, and a station that is not associated has no route to the printer.
    //
    // ⛔ has_printer_configuration IS TRUE ONLY FOR AN ACTIVE, VALIDATED RECORD. §14.3 step 10 and
    // §13.3 step 6 both place activation after the connection test, so a pending candidate is never
    // the source of operational printer data. printer_configuration_pending is deliberately not part
    // of this condition.
    return conditions.provisioning == app_model::ProvisioningPhase::Inactive &&
           conditions.network == app_model::NetworkPhase::Connected &&
           conditions.has_printer_configuration;
}

} // namespace prd::app_core::exclusive
