// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include "transfer_tracker.hpp"

namespace prd::display {

void TransferTracker::on_submit() {
    ++pending_;
    // clang-tidy suggests std::max. Declined: this is the Phase F ownership
    // bookkeeping validated on hardware, and the peak-tracking idiom reads more
    // plainly as a comparison than as an assignment from std::max.
    // NOLINTNEXTLINE(readability-use-std-min-max)
    if (pending_ > peak_pending_) {
        peak_pending_ = pending_;
    }
}

void TransferTracker::on_submit_failed() {
    if (pending_ != 0) {
        --pending_;
    }
}

bool TransferTracker::on_complete() {
    if (pending_ == 0) {
        ++spurious_completions_;
        return false;
    }

    --pending_;
    return true;
}

void TransferTracker::reset() {
    pending_ = 0;
}

} // namespace prd::display
