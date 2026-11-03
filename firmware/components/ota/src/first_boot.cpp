// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include "ota/first_boot.hpp"

namespace prd::ota {

void FirstBoot::begin(BootPort& port) {
    if (begun_) {
        return;
    }
    begun_ = true;
    const auto image = port.running_image();
    pending_ = image == BootImage::Pending;
    if (pending_) {
        health_ = BootHealth::Waiting;
    } else if (image == BootImage::Ordinary) {
        health_ = BootHealth::Inactive;
    } else {
        health_ = BootHealth::Failed;
    }
}

void FirstBoot::tick(BootPort& port, const std::uint32_t now_ms, const Readiness readiness) {
    if (health_ != BootHealth::Waiting) {
        return;
    }
    if (readiness.fatal_fault) {
        health_ = BootHealth::Failed;
        return;
    }
    if (!readiness.application || !readiness.display || !readiness.storage) {
        timing_ = false;
        return;
    }
    // Modular subtraction permits normal 32-bit uptime wrap. A backward sample
    // must not turn into a very large healthy interval and accept immediately.
    if (!timing_ || now_ms - last_ > UINT32_MAX / 2U) {
        timing_ = true;
        since_ = now_ms;
    }
    last_ = now_ms;
    if (now_ms - since_ < kHealthyWindowMs) {
        return;
    }
    // At most one attempt. A failed metadata write never becomes confirmation
    // through a later healthy tick; the bootloader's pending state remains owed.
    health_ = port.confirm_running_image() ? BootHealth::Confirmed : BootHealth::Failed;
    if (health_ == BootHealth::Confirmed) {
        pending_ = false;
    }
}

} // namespace prd::ota
