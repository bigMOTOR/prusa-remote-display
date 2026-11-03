// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstddef>
#include <string_view>

#include "app_model/app_state.hpp"
#include "app_model/events.hpp"
#include "app_model/observation.hpp"

// Complete application-state fixtures, one per MVP presentation.
//
// A FIXTURE IS A LIST OF EVENTS, NOT A STATE. That is the whole design. A
// hand-assembled AppState can express a state the product cannot actually reach —
// Printing with Unavailable confidence and a live remaining time, say — and a scene
// built against one would be built against a fiction. Every fixture here is
// defined as the events a real adapter would have reported, and its state is
// obtained by folding them through the real reducer.
//
// Consequently: if the reducer's rules change, the fixtures change with them, and
// the test that asserts a fixture reaches its intended presentation fails loudly
// instead of quietly describing something obsolete.
//
// Pure: no ESP-IDF, no clock, no allocation. Timestamps arrive from the caller.

namespace prd::fake_source {

// The MVP presentations, in the order UIAD 001 §13's hierarchy walks them:
// lifecycle, provisioning, subject states, conditions, device-owned operations,
// modal surfaces.
enum class Fixture {
    // System lifecycle (UIAD 001 §4).
    BootStarting,

    // Provisioning (§5).
    SetupRequired,
    SetupActive,
    ValidatingConfiguration,
    SetupFailed,

    // Subject states (§6).
    AcquiringPrinterState,
    Ready,
    Preparing,
    Printing,
    Paused,
    Completed,
    Stopped,

    // Three presentations the six above cannot reach. Two were added for hardware
    // validation in Milestone 2 Phase E.6, the third in Phase G.1.
    //
    // All are ordinary subject presentations rather than a new kind of thing: two
    // vary confidence, one varies which values the machine has supplied. They exist
    // because the composition each produces has never been seen on the panel, and
    // none is reachable from the six family fixtures — the only other fixture that
    // ages anything ages it past expiry, and every Printing script carries a job.
    //
    // NONE OF THEM IS A SEVENTH MEMBER OF THE OPERATIONAL FAMILY. UI Design Decision
    // 004 §2.2 names six energies. Two of these are Printing at Printing's energy and
    // one is Ready at Ready's; what differs is confidence (§10, orthogonal to energy)
    // and presence (§7).
    PrintingDegraded,

    // The same reduced confidence at the resting end of the axis. §10's durable /
    // decayed split has to be legible at more than one energy to be judged as a rule
    // rather than as a Printing behaviour — and this is the one composition where the
    // explicit stale indicator and the resting horizon are known to meet, which is a
    // measurement Phase G.6 cannot take without it.
    ReadyDegraded,

    PrintingUnknownProgress,

    // Confidence (§7) and access conditions (§8).
    PrinterUnavailable,
    AuthenticationRequired,
    UnsupportedInterface,

    // Maintenance and recovery (§10).
    FirmwareUpdating,
    FactoryResetting,
    RecoveryMode,
    DeviceFault,

    // User-invoked surfaces (§9).
    // ⬅️ Fixture::SettingsModal WAS REMOVED AT MILESTONE 3.10. Owner ruling 1 unassigned Settings
    // from the button, so no gesture reaches that surface and the fixture could not produce the
    // state it named. FactoryResetConfirmation below demonstrates the same thing the fixture
    // existed for — a modal drawing over a live operational composition — with the gesture that
    // still opens one. A fixture that cannot reach its own state is scaffolding that lies.
    FactoryResetConfirmation,
};

constexpr std::size_t kFixtureCount = 24;

[[nodiscard]] std::string_view name(Fixture fixture);

// Index-addressable, so a test or a validation flow can walk all of them without
// repeating the list and without being able to miss one.
[[nodiscard]] Fixture fixture_at(std::size_t index);

// Receives each event of a script, in order.
using EventSink = void (*)(const app_model::Event& event, void* context);

// Emits the fixture's script. Every event's timestamp is `base` plus a fixed
// offset, so two calls with the same base emit byte-identical events.
//
// A sink rather than a returned array: the longest script is ten events and an
// Event is 360 bytes, so returning one by value would put nearly 4 kB on the stack
// of whatever asked — on a device where the tasks that would ask have 8 kB in
// total.
void emit_script(Fixture fixture, app_model::Timestamp base, EventSink sink, void* context);

// The state the script produces, folded through the real reducer.
[[nodiscard]] app_model::AppState
state_for(Fixture fixture, app_model::Timestamp base = app_model::Timestamp::zero());

} // namespace prd::fake_source
