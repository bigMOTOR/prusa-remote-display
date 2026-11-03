// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <atomic>

#include <cstdint>
#include <string_view>

#include "app_model/device_data.hpp"
#include "app_model/state_layers.hpp"
#include "ota/installation.hpp"

// WHICH OPERATION OWNS THE PRODUCT STATE, AND WHAT THAT FORBIDS EVERYTHING ELSE.
//
// ⛔ THE DEFECT THIS CLOSES, AS AN EXECUTION PATH. The application loop runs local_interface_tick()
// — and therefore the factory-reset executor — BEFORE printer_supervise(); and printer_supervise()
// returned immediately while a Validate session was armed, before it ever looked at
// factory_resetting. So:
//
//   1. a Validate session is armed and its worker is running;
//   2. the confirmed reset erases the configuration and the credentials;
//   3. printer_supervise() returns early and does not supersede the worker;
//   4. printer_ingest() consumes a queued validation SUCCESS;
//   5. complete_printer_validation() writes the recovery record and both activation pointers —
//      AFTER the erase;
//   6. the device restarts carrying storage state instead of true first-boot state.
//
// ⬅️ AND WHY REORDERING THE LOOP WOULD NOT HAVE BEEN ENOUGH. The erase takes milliseconds and the
// PrusaLink worker task runs concurrently on another core, so a report can be published DURING
// factory_reset::run() whatever order the loop uses. The fix has to be a barrier that disarms
// everything before the first destructive step, plus a standing rule that outranks session identity
// for as long as the reset owns the device. Both are here: app_core's quiesce runs synchronously
// before the erase, and this decision is consulted by report ingress, worker launch and the
// activation boundary.
//
// PURE, SO IT IS PROVED WITHOUT FreeRTOS. app_core.cpp cannot be host-compiled; this can, and
// production calls exactly these functions rather than a copy of their conditions.

namespace prd::app_core::exclusive {

// What owns the product state.
enum class Owner : std::uint8_t {
    // Ordinary operation. Printer and provisioning work may proceed on their own merits.
    None,

    // A user-confirmed factory reset is armed, erasing, or holding its post-erase presentation
    // before the restart. It outranks every printer and provisioning activity for that whole
    // interval — including the two-second hold, during which the panel is describing a device whose
    // configuration is gone.
    FactoryReset,

    FirmwareUpdate,

    // ⛔ A DEVICE FAULT THAT MUST NOT RELEASE THE DEVICE — audit item 5, 2026-08-28. Two reasons and
    // only two.
    //
    // FactoryResetIncomplete: a confirmed reset executed and did not finish. On the route where its
    // journal WAS written, one may still be armed and a device released into provisioning would have
    // that new configuration erased by the next boot's resume. On the route where the journal could
    // not be written, nothing was removed and nothing is owed — but the store has just refused a
    // write, so it is not trusted for further mutation either. ⛔ Both hold the device; only one of
    // them owes a job, and this barrier does not need to know which.
    //
    // StorageFailure: the store would not answer, and nothing about the device's stored state has
    // been established.
    //
    // ⛔ IT IS NOT "ANY DEVICE FAULT". WifiUnavailable must NOT reach here: the accepted 3c ruling
    // keeps the setup access point and its server physically alive under that fault so a browser
    // mid-flow is not stranded, and making it exclusive would refuse the very submissions that
    // surface is up to accept. fault_owns_device() below decides that per reason, exhaustively.
    //
    // ⬅️ AND IT EXISTS BECAUSE THE BOOT NEEDS AN OWNER THAT AppState::factory_resetting CANNOT GIVE.
    // A resume that fails at boot has no confirmed reset in the reducer — the flag is set only by
    // the second very-long press — so before this there was no term that could hold the device on
    // the one path where it mattered most.
    DeviceFault,
};

[[nodiscard]] std::string_view log_label(Owner owner);

// Everything the decision needs. All of it already exists: the first two come from AppState, the
// third is app_core's own post-erase restart deadline, and the last three are the polling
// eligibility that printer_supervise() and printer_launch() used to spell out separately.
struct Conditions {
    // AppState::factory_resetting — set by the reducer on the second deliberate very-long press and
    // by nothing else.
    bool factory_resetting{false};

    // AppState::modal_surface == FactoryResetConfirmation. The prompt is on the panel and the next
    // press could confirm; nothing destructive has happened yet.
    bool factory_reset_confirmation_armed{false};

    // Whether app_core has a post-erase restart scheduled. Belt and braces: factory_resetting is
    // still true throughout the hold, so this only matters if that ever stopped being true while a
    // restart was pending — and a barrier that depends on one flag staying set is weaker than one
    // that does not.
    bool reset_restart_scheduled{false};

    // AppState::device_fault. The TYPED reason, not a boolean: which faults hold the device is a
    // decision, and it is made once, here, rather than at each call site.
    app_model::DeviceFaultReason device_fault{app_model::DeviceFaultReason::None};

    // ⛔ AppState::factory_reset_failed — audit item 5's fifth finding. A THIRD, INDEPENDENT WITNESS
    // to the same interval, and the only one that does not rest on a mutable presentation field.
    // `factory_resetting` carries the runtime case and `device_fault` carries the boot case; this is
    // set on both, by the reducer, when FactoryResetIncomplete is raised — and cleared only by an
    // actual boot. A queued observation arriving after the failure cannot release the barrier
    // through it, which is precisely what it could do through the reason alone.
    bool factory_reset_failed{false};

    app_model::ProvisioningPhase provisioning{app_model::ProvisioningPhase::Inactive};
    app_model::NetworkPhase network{app_model::NetworkPhase::Disabled};
    bool has_printer_configuration{false};

    // ⛔ A DIAGNOSTIC STATUS SOURCE IS DRIVING THE STORE, so the real printer must not.
    //
    // ⬅️ WHY THIS IS A BARRIER AND NOT A CONVENTION. The Printing-timeline instrument runs a
    // fake_source prelude that makes the network and the printer configuration eligible — that is
    // what makes an operational composition reachable at all — and the ordinary application loop
    // then reaches printer_launch() and would start a REAL worker against the board's persisted
    // printer pair. Real readings would race the synthetic ones, or overwrite them, and a reviewer
    // could not tell which they were looking at.
    //
    // ⛔ AND IT MUST NOT REST ON THE PRINTER BEING OFF, ON DNS FAILING, OR ON A DEVELOPMENT
    // CONFIGURATION BEING EMPTY. Every one of those is a property of the bench that day, not of
    // the firmware, and each has already been the reason a validation run had to be discarded in
    // this project. This term is compiled from the Kconfig gate, so in a product image it is
    // constant false and the whole barrier folds away.
    //
    // It blocks BOTH launch and report ingress, so a worker that somehow already existed still
    // contributes nothing.
    bool synthetic_status_source{false};

    // Live facts assembled by the serialized application owner. The reservation
    // latch below is independent of queued observation/presentation fields.
    bool firmware_update_active{false};
    bool first_boot_pending{false};
    bool configuration_in_flight{false};

    // True only for the one application-task decision consuming POST /api/update from the
    // physically opened setup surface. It permits closing that otherwise idle surface as part of
    // acquiring OTA; it never permits an in-flight configuration transaction.
    bool firmware_update_authorized_surface{false};
};

[[nodiscard]] bool firmware_update_permitted(const Conditions& conditions);
[[nodiscard]] bool station_work_permitted(const Conditions& conditions);

// One shared object per application owner, not per request. The application task evaluates fresh
// live Conditions immediately before authorization, without intervening work/dispatch; all
// admission and mutation decisions run on that serialized task. held() is included in that owner's
// guards, rather than waiting for an asynchronous OtaObserved to acquire exclusivity.
class FirmwareUpdateAdmission final : public ota::InstallationAdmission {
  public:
    // Called only by the serialized application task. It closes the exclusivity barrier before a
    // worker can receive the command, rather than waiting for that worker's first observation.
    [[nodiscard]] bool authorize(const Conditions& live);
    [[nodiscard]] bool acquire() override;
    void release(bool retain_until_restart) override;
    [[nodiscard]] bool held() const {
        return held_.load(std::memory_order_acquire);
    }
    [[nodiscard]] bool activation_locked() const {
        return activation_locked_.load(std::memory_order_acquire);
    }

  private:
    std::atomic<bool> authorized_{false};
    std::atomic<bool> held_{false};
    std::atomic<bool> activation_locked_{false};
};

// Whether a device fault holds the product state exclusively. Exhaustive over DeviceFaultReason
// with no permissive default, so a new reason cannot be added without deciding this.
[[nodiscard]] bool fault_owns_device(app_model::DeviceFaultReason reason);

[[nodiscard]] Owner owner_of(const Conditions& conditions);

// Whether an exclusive operation holds the device right now.
[[nodiscard]] bool active(const Conditions& conditions);

// ⛔ WHETHER A PRINTER REPORT MAY BE ACTED ON AT ALL. False while a reset owns the state, whatever the
// report says and whatever session was armed — which is what stops a queued validation success from
// activating a candidate after the erase, and a late Poll cycle from refreshing observed_at on a
// device whose configuration no longer exists.
[[nodiscard]] bool printer_reports_permitted(const Conditions& conditions);

// Whether a real PrusaLink status source may exist at all — i.e. whether a diagnostic source has
// taken the printer data exclusively. Named separately from the two permissions below because it
// answers a different question: not "is now a good time", but "is there another writer".
[[nodiscard]] bool real_printer_source_permitted(const Conditions& conditions);

// ⛔ WHETHER A CANDIDATE MAY BE ACTIVATED. The last check before the activation boundary writes
// anything, so a report that somehow reached settlement still cannot move a pointer.
[[nodiscard]] bool candidate_activation_permitted(const Conditions& conditions);

// ⛔ WHETHER PROVISIONING MAY DO ANYTHING AT ALL. False while a reset owns the device, which covers
// consuming a submission, beginning or advancing a transaction, opening or closing a dwell, settling
// a verdict, retaining a credential, and stopping the access point on a deadline.
//
// ⬅️ WHY THIS IS A SEPARATE PERMISSION AND NOT THE PRINTER ONE. The two are consulted from different
// places for different reasons — the printer permission also carries the polling eligibility terms,
// which say nothing about provisioning. Sharing it would mean a device that is merely disconnected
// could not run setup, which is the opposite of what setup is for.
[[nodiscard]] bool provisioning_work_permitted(const Conditions& conditions);

// Whether a polling worker may be launched. The exclusivity term plus the eligibility that was
// duplicated between printer_supervise() and printer_launch() — one expression, one place.
//
// ⬅️ THE CONFIRMATION PROMPT DOES NOT STOP POLLING, AND THAT IS DELIBERATE. An armed prompt is a
// question, not an operation: it may expire or be cancelled, and blanking the panel's printer data
// while the user reads a question would present a device as disconnected because they were asked
// something. Only a CONFIRMED reset takes the device.
[[nodiscard]] bool printer_worker_permitted(const Conditions& conditions);

} // namespace prd::app_core::exclusive
