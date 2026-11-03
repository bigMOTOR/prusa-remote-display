// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "app_model/reducer.hpp"
#include "app_store/store.hpp"

namespace prd::validation {

// Optional diagnostic continuation of PrintingTimeline. No renderer, clock read, network,
// hardware or model-state mutation: only the existing fixture/event dispatch boundary.
// Target CMake includes its translation unit only when PRINTING_LOSS_REVIEW is selected.
class LossReview {
  public:
    enum class Phase : std::uint8_t {
        Inactive,
        Seed,
        AwaitLoss,
        Loss,
        OffFade,
        Recovered,
        FinalReady,
        Failed
    };
    static constexpr std::size_t kChapterCount = 4;

    void begin(app_store::Store& store, app_model::Timestamp now, app_model::SnapshotOrder order,
               std::uint32_t prior_manual_controls = 0);
    void tick(app_store::Store& store, app_model::Timestamp now);
    void skip(app_store::Store& store, app_model::Timestamp now);
    void note_manual_control();

    [[nodiscard]] Phase phase() const {
        return phase_;
    }
    [[nodiscard]] std::size_t chapter() const {
        return chapter_;
    }
    [[nodiscard]] std::size_t recoveries() const {
        return recoveries_;
    }
    [[nodiscard]] std::uint32_t next_sequence() const {
        return order_.sequence;
    }
    [[nodiscard]] bool unattended() const {
        return manual_controls_ == 0;
    }

  private:
    void tick_seed(app_store::Store& store, app_model::Timestamp now);
    void tick_loss(app_store::Store& store, app_model::Timestamp now);
    bool publish(app_store::Store& store, app_model::Timestamp now, std::size_t fixture);
    void recover(app_store::Store& store, app_model::Timestamp now);
    void fail(std::string_view reason);
    void report(std::string_view event, app_model::Timestamp now) const;

    Phase phase_{Phase::Inactive};
    std::size_t chapter_{0}, recoveries_{0};
    std::uint32_t manual_controls_{0};
    app_model::SnapshotOrder order_{};
    app_model::Timestamp phase_at_{}, next_age_{}, next_refresh_{};
};

} // namespace prd::validation
