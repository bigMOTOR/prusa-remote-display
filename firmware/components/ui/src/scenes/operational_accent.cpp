// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include "scenes/operational_accent.hpp"

#include "theme/tokens.hpp"

namespace prd::ui::scene {

std::uint32_t accent() {
    // ⛔ THE TOKEN ITSELF, UNMODIFIED. No scale, no level, no channel arithmetic — §5.1 as ruled on
    // 2026-09-01. If this ever becomes an expression, the rule has been broken.
    return theme::kColorAccent;
}

} // namespace prd::ui::scene
