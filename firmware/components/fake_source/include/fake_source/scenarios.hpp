// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "app_model/bounded_string.hpp"
#include "app_model/events.hpp"
#include "app_model/observation.hpp"

// Deterministic scenario sequences: the transitions between states, as opposed to
// the states themselves.
//
// A fixture answers "what does Paused look like". A scenario answers "does the
// product get from Printing to Paused and back without losing the job" — which is
// where the interesting defects live, and which no single state can show.
//
// Each scenario is an indexed list of steps. A step is one event plus a short label
// for the log. The step's timestamp is the scenario base plus a fixed offset, so:
//
//   - two runs from the same base produce byte-identical events;
//   - the offsets can jump minutes ahead where a scenario needs them to, which is
//     how confidence decay is exercised without anything waiting a minute;
//   - nothing reads a clock.
//
// Indexed rather than streamed, because the validation flow has to be able to
// pause, single-step and restart, and a callback stream cannot be stopped halfway.

namespace prd::fake_source {

enum class Scenario {
    // Cold boot through to a printer reporting Ready.
    ColdBootToReady,

    // First run: no configuration, provisioning, then acquisition and Ready.
    FirstSetupToReady,

    // The whole job lifecycle: ready → preparing → printing → paused → printing →
    // completed → ready.
    FullPrintCycle,

    // A print interrupted: printing → stopped → ready.
    PrintingToStopped,

    // Confidence decaying with no news: current → degraded → unavailable.
    ConfidenceDecay,

    // Authentication refused, corrected, then acquisition and Ready (§16.2).
    AuthenticationRecovery,

    // Check, download, verify, install, reboot required (§18.4).
    FirmwareUpdateLifecycle,

    // A recoverable device condition, then an unrecoverable one.
    RecoveryAndDeviceFault,

    // A modal surface opened and dismissed over a live operational subject
    // (UIAD 001 §9).
    ModalOverOperationalSubject,
};

constexpr std::size_t kScenarioCount = 9;

[[nodiscard]] std::string_view name(Scenario scenario);

[[nodiscard]] Scenario scenario_at(std::size_t index);

// The catalogue position of a scenario — the inverse of scenario_at(). Lets a driver
// walk the catalogue without keeping a second copy of the ordering.
[[nodiscard]] std::size_t scenario_index(Scenario scenario);

using StepLabel = app_model::BoundedString<40>;

struct Step {
    app_model::Event event;

    // What this step is, for the validation log. Empty when the index is out of
    // range, which is the only way step_at() reports that.
    StepLabel label;
};

[[nodiscard]] std::size_t step_count(Scenario scenario);

// The step at `index`, with its event timestamped at `base` plus the step's fixed
// offset.
//
// `sequence_base` is added to every observation sequence in the scenario. A replay
// needs it: the reducer rejects an observation whose sequence is not greater than
// the last accepted one (§8.1), so a second run through the same scenario against
// the same store would have every snapshot thrown away as stale. Offsetting the
// sequences makes a replay a genuine re-run rather than a rewind — which is also
// what a real device does, since its history only ever moves forward.
[[nodiscard]] Step step_at(Scenario scenario, std::size_t index, app_model::Timestamp base,
                           std::uint32_t sequence_base);

} // namespace prd::fake_source
