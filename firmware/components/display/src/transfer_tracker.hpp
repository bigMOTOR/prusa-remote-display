// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstdint>

// Ownership bookkeeping for in-flight pixel transfers (pre-Phase F hardening).
//
// esp_lcd's SPI colour path is asynchronous: esp_lcd_panel_draw_bitmap() queues a
// transfer and returns while DMA is still reading the caller's buffer. Exactly one
// completion callback arrives per submitted draw, because esp_lcd marks its
// trans-done flag only on the last chunk of each draw.
//
// This class is the ownership state that follows from those two facts: how many
// transfers the panel still owns, and therefore whether the caller may touch its
// buffer. It is deliberately free of ESP-IDF and of FreeRTOS so the invariants can
// be exercised by the host test suite — "pending never underflows" and "a spurious
// completion is counted, not absorbed" are correctness properties, and a driver
// that miscounts them either corrupts pixels or deadlocks.
//
// Not thread-safe by itself. The adapter serialises access with a spinlock,
// because completions arrive in interrupt context.

namespace prd::display {

class TransferTracker {
  public:
    // A transfer has been handed to the panel. The submitted buffer now belongs to
    // DMA and must not be modified until the matching completion arrives.
    void on_submit();

    // Submission failed after on_submit() was optimistically recorded. Rolls the
    // count back, because a transfer that was never queued will never complete
    // and would otherwise strand the count above zero forever.
    void on_submit_failed();

    // A completion callback arrived. Returns false if no transfer was
    // outstanding, which means the completion was spurious: the count is left at
    // zero rather than wrapping, and the event is recorded.
    [[nodiscard]] bool on_complete();

    // Forgets all outstanding transfers. For teardown only, where the panel and
    // its bus are being destroyed and no further completion can arrive.
    void reset();

    // Transfers the panel still owns.
    [[nodiscard]] std::uint32_t pending() const {
        return pending_;
    }

    // True when the caller owns every buffer it has submitted.
    [[nodiscard]] bool idle() const {
        return pending_ == 0;
    }

    // Diagnostics. Both must stay at zero in normal operation.
    [[nodiscard]] std::uint32_t spurious_completions() const {
        return spurious_completions_;
    }
    [[nodiscard]] std::uint32_t peak_pending() const {
        return peak_pending_;
    }

  private:
    std::uint32_t pending_{0};
    std::uint32_t peak_pending_{0};
    std::uint32_t spurious_completions_{0};
};

} // namespace prd::display
