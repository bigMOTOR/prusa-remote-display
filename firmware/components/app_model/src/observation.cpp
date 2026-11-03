// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include "app_model/observation.hpp"

namespace prd::app_model {

std::string_view log_label(const DataFreshness freshness) {
    switch (freshness) {
    case DataFreshness::Unknown:
        return "unknown";

    case DataFreshness::Fresh:
        return "fresh";

    case DataFreshness::Aging:
        return "aging";

    case DataFreshness::Stale:
        return "stale";

    case DataFreshness::Expired:
        return "expired";
    }

    return "unknown";
}

DataFreshness classify_freshness(const Timestamp age, const FreshnessPolicy& policy) {
    // Time appeared to move backwards. The reducer counts this as out-of-order
    // and clamps its own clock forward; here the safe reading is the youngest
    // one, because treating a negative age as very old would make a single bad
    // timestamp blank the screen.
    if (age < Timestamp::zero()) {
        return DataFreshness::Fresh;
    }

    if (age <= policy.fresh_within) {
        return DataFreshness::Fresh;
    }

    if (age <= policy.aging_within) {
        return DataFreshness::Aging;
    }

    if (age <= policy.stale_within) {
        return DataFreshness::Stale;
    }

    return DataFreshness::Expired;
}

} // namespace prd::app_model
