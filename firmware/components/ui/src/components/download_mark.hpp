// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstdint>

#include "theme/theme.hpp"

namespace prd::ui::component {

// One download glyph used at two scales: the firmware-update composition and Ready's compact
// availability mark. Geometry scales from the supplied token-owned box; colour and stroke remain
// shared, so the two screens cannot drift into separate icon languages.
class DownloadMark {
  public:
    [[nodiscard]] bool build(lv_obj_t* parent, const theme::Rect& bounds,
                             theme::Colour colour = theme::Colour::Accent);
    void show(bool visible);
    void paint(std::int32_t level);

  private:
    static void draw(lv_event_t* event);

    lv_obj_t* root_{nullptr};
    theme::Colour colour_{theme::Colour::Accent};
    std::int32_t width_{0};
    std::int32_t height_{0};
    std::int32_t level_{-1};
};

} // namespace prd::ui::component
