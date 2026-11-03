// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include "app_model/values.hpp"

namespace prd::app_model {

// Exact float comparison is correct here. These operators answer "is this the
// same stored observation", not "are these two measurements close": a value that
// arrived once and was never updated must compare equal to itself so that a
// repeated snapshot is recognised as no change.
bool operator==(const TemperatureValue& lhs, const TemperatureValue& rhs) {
    if (lhs.known != rhs.known) {
        return false;
    }

    // An unknown value's celsius field is not part of the value.
    return !lhs.known || lhs.celsius == rhs.celsius;
}

bool operator!=(const TemperatureValue& lhs, const TemperatureValue& rhs) {
    return !(lhs == rhs);
}

bool operator==(const ProgressValue& lhs, const ProgressValue& rhs) {
    if (lhs.known != rhs.known) {
        return false;
    }

    return !lhs.known || lhs.percent == rhs.percent;
}

bool operator!=(const ProgressValue& lhs, const ProgressValue& rhs) {
    return !(lhs == rhs);
}

bool operator==(const DurationValue& lhs, const DurationValue& rhs) {
    if (lhs.known != rhs.known) {
        return false;
    }

    return !lhs.known || lhs.seconds == rhs.seconds;
}

bool operator!=(const DurationValue& lhs, const DurationValue& rhs) {
    return !(lhs == rhs);
}

bool operator==(const LayerProgress& lhs, const LayerProgress& rhs) {
    if (lhs.known != rhs.known) {
        return false;
    }

    return !lhs.known || (lhs.current == rhs.current && lhs.total == rhs.total);
}

bool operator!=(const LayerProgress& lhs, const LayerProgress& rhs) {
    return !(lhs == rhs);
}

} // namespace prd::app_model
