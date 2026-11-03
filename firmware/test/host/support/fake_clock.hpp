// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <chrono>

// Controllable monotonic clock for host tests (Bootstrap Spec §11.6).
//
// The seam is deliberate: production code that needs the time takes a
// std::chrono::microseconds argument rather than calling
// platform::monotonic_time() itself. That keeps timing logic testable without
// mocking ESP-IDF and without any test ever sleeping — a test suite that waits
// eight seconds to check a very-long press is a test suite nobody runs.
//
// Matches platform::monotonic_time() in units and in its guarantee: monotonic,
// never decreasing.

namespace prd::test {

class FakeClock {
  public:
    [[nodiscard]] std::chrono::microseconds now() const {
        return now_;
    }

    void advance(const std::chrono::microseconds delta) {
        now_ += delta;
    }
    void advance(const std::chrono::milliseconds delta) {
        now_ += std::chrono::duration_cast<std::chrono::microseconds>(delta);
    }

  private:
    std::chrono::microseconds now_{0};
};

} // namespace prd::test
