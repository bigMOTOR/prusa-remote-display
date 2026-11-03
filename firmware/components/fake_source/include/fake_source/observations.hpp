// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstdint>

#include "app_model/events.hpp"

// Observation builders shared by the fixtures and the scenarios.
//
// These construct event *payloads* — the facts a real adapter would report — with
// fixed, deterministic values. Nothing here knows about PrusaLink: there is no
// endpoint, no JSON field name, no HTTP status. A PrusaLink adapter in Milestone 4
// will build the same payloads from a decoded response, and the reducer will not be
// able to tell the difference. That is the point of the phase.
//
// Everything is pure: no clock, no allocation, no I/O. Timestamps arrive from the
// caller, always.

namespace prd::fake_source {

// The reference device, matching the physically validated hardware so a log read
// on the bench says what the board actually is.
[[nodiscard]] app_model::HardwareProbeObservation healthy_hardware();
[[nodiscard]] app_model::HardwareProbeObservation mismatched_hardware();

// A configuration that names a printer to reach. `credential_generation` is what
// distinguishes a corrected API key from a re-report of the same one (§13.3), so
// the authentication-recovery scenario needs to vary it.
[[nodiscard]] app_model::ConfigurationObservation
valid_configuration(std::uint32_t credential_generation = 0);

// No configuration at all: the first-run condition (§14.1).
[[nodiscard]] app_model::ConfigurationObservation missing_configuration();

[[nodiscard]] app_model::NetworkObservation network_connected();
[[nodiscard]] app_model::NetworkObservation network_connecting();

// A connection phase report. Endpoint, capability tier and latency are filled with
// plausible fixed values so a log line looks like the real thing.
[[nodiscard]] app_model::ConnectionObservation connection(app_model::ConnectionPhase phase);

// A printer status snapshot. Temperatures follow the reported state — a printer
// that is Ready is at temperature, one that is Preparing is on its way up — so a
// fixture is internally coherent rather than merely well-typed.
[[nodiscard]] app_model::PrinterSnapshotObservation snapshot(app_model::PrinterState state,
                                                             std::uint32_t sequence);

// The printer's own identity, as the info endpoint would later report it.
[[nodiscard]] app_model::PrinterIdentity printer_identity();

// How far through a job is: the two figures the canonical Printing composition shows
// moving. Grouped rather than passed as loose integers, because three adjacent
// integer parameters are trivially transposable at a call site, and a swapped
// percentage and layer count would produce a fixture that still looked plausible.
struct JobProgress {
    std::uint8_t percent{0};
    std::uint32_t layer{0};
};

// A job at a given point in its life.
[[nodiscard]] app_model::JobObservation job(std::uint32_t sequence, JobProgress progress);

// A job whose remaining time is an estimate rather than a figure to trust (§17.7).
[[nodiscard]] app_model::JobObservation job_with_rough_estimate(std::uint32_t sequence,
                                                                JobProgress progress);

[[nodiscard]] app_model::AlertObservation attention_alert();
[[nodiscard]] app_model::AlertObservation error_alert();

[[nodiscard]] app_model::OtaObservation ota(app_model::OtaPhase phase, std::uint8_t percent);

} // namespace prd::fake_source
