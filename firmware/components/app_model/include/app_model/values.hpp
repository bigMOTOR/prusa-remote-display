// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstdint>

// Small optional-valued carriers used throughout the application state.
//
// std::optional would be the obvious choice, and app_model uses it elsewhere
// (hardware_check.hpp) where the value is not part of AppState. These are used
// inside the state instead for two reasons: they are trivially copyable with a
// stable layout, which keeps a state snapshot a memcpy rather than a walk over
// engaged-ness checks; and at a use site `known` says what the distinction is
// for, which is Architecture Spec §9.3's rule that absent must be
// distinguishable from zero. A bed at 0 °C and a bed that did not report are
// different facts, and the UI is required to render them differently — absence
// stays absent (UI Design Decision 003) rather than becoming a placeholder.

namespace prd::app_model {

struct TemperatureValue {
    bool known{false};
    float celsius{0.0F};
};

[[nodiscard]] bool operator==(const TemperatureValue& lhs, const TemperatureValue& rhs);
[[nodiscard]] bool operator!=(const TemperatureValue& lhs, const TemperatureValue& rhs);

struct ProgressValue {
    bool known{false};

    // §7.8: "progress is clamped to 0...100 only for display". Stored as
    // observed; the clamp is applied by the projection, and an out-of-range
    // original is counted in DiagnosticsSummary rather than silently corrected.
    std::uint8_t percent{0};
};

[[nodiscard]] bool operator==(const ProgressValue& lhs, const ProgressValue& rhs);
[[nodiscard]] bool operator!=(const ProgressValue& lhs, const ProgressValue& rhs);

struct DurationValue {
    bool known{false};
    std::uint32_t seconds{0};
};

[[nodiscard]] bool operator==(const DurationValue& lhs, const DurationValue& rhs);
[[nodiscard]] bool operator!=(const DurationValue& lhs, const DurationValue& rhs);

// Layer progress.
//
// §7.8's PrintJob does not list layers, but Production UI Specification §9
// requires "Layer information" in the canonical Printing composition and every
// accepted artwork panel shows it. The model has to carry it. Recorded as an
// implementation note rather than a conflict: one source requires a field the
// other simply does not mention.
struct LayerProgress {
    bool known{false};
    std::uint32_t current{0};
    std::uint32_t total{0};
};

[[nodiscard]] bool operator==(const LayerProgress& lhs, const LayerProgress& rhs);
[[nodiscard]] bool operator!=(const LayerProgress& lhs, const LayerProgress& rhs);

} // namespace prd::app_model
