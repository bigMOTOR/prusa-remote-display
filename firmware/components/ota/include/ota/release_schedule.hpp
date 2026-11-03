// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstdint>

namespace prd::ota {

inline constexpr std::uint64_t kReleaseCheckIntervalMs = UINT64_C(24) * 60 * 60 * 1000;
inline constexpr std::uint64_t kReleaseCheckJitterWindowMs = UINT64_C(60) * 60 * 1000;

struct ReleaseCheckTiming {
    std::uint64_t now_ms{0};
    std::uint32_t entropy{0};
};

// Pure monotonic scheduler. It never reads wall time or randomness itself. An
// adapter supplies entropy and the monotonic millisecond reading, which makes the
// same cadence deterministic in host tests. Automatic repeat checks cannot occur
// sooner than 24 h; the one-hour positive jitter prevents a fleet synchronising.
class ReleaseCheckSchedule {
  public:
    // Boot/start spread only. A caller may instead perform an explicit/manual
    // check immediately and call checked() afterwards.
    void start(ReleaseCheckTiming timing);
    void checked(ReleaseCheckTiming timing);
    void reset();

    [[nodiscard]] bool armed() const {
        return armed_;
    }
    [[nodiscard]] bool due(std::uint64_t now_ms) const {
        return armed_ && now_ms >= due_ms_;
    }
    [[nodiscard]] std::uint64_t due_ms() const {
        return due_ms_;
    }

  private:
    std::uint64_t due_ms_{0};
    bool armed_{false};
};

} // namespace prd::ota
