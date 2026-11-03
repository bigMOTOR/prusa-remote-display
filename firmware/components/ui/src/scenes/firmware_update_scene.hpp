// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstdint>

#include "app_model/presentation.hpp"
#include "components/download_mark.hpp"
#include "scenes/unavailable_motion.hpp"
#include "theme/theme.hpp"

namespace prd::ui::scene {

// Production UI §9's firmware-update family. It never borrows the operational ring: only the
// transfer phase has a measured percentage, while verifying/installing/restarting state activity
// with the product's shared dot cadence.
class FirmwareUpdateScene {
  public:
    [[nodiscard]] bool build(lv_obj_t* parent);
    void apply(const app_model::Presentation& presentation);
    void set_visible(bool visible);
    [[nodiscard]] bool step(bool panel_visible, std::uint32_t now);

  private:
    enum class Mode : std::uint8_t { None, Downloading, Process };

    void paint(std::int32_t level);
    void set_process_title();
    [[nodiscard]] bool step_dots(bool panel_visible, std::uint32_t now);

    lv_obj_t* root_{nullptr};
    component::DownloadMark download_track_{};
    lv_obj_t* download_fill_clip_{nullptr};
    component::DownloadMark download_fill_{};
    lv_obj_t* version_{nullptr};
    lv_obj_t* value_{nullptr};
    lv_obj_t* unit_{nullptr};
    lv_obj_t* title_{nullptr};
    lv_obj_t* message_{nullptr};
    lv_obj_t* process_{nullptr};

    HoldClock clock_{};
    app_model::OtaPhase phase_{app_model::OtaPhase::Idle};
    Mode mode_{Mode::None};
    bool progress_known_{false};
    bool version_known_{false};
    std::uint8_t progress_percent_{0};
    bool applied_{false};
    bool visible_{false};
    bool running_{false};
    std::int32_t level_{-1};

    std::uint32_t dots_at_{0};
    std::uint8_t dots_{1};
    bool dots_anchored_{false};
};

} // namespace prd::ui::scene
