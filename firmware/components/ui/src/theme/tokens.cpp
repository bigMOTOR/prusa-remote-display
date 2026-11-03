// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include "theme/tokens.hpp"

namespace prd::ui::theme {

// The §4 Text Roles table. One switch per property rather than a struct array,
// so a role added to the enumeration fails to compile here until it is given a
// size, a line height and a weight — an unhandled enumerator is a warning the
// project treats as an error.

std::int32_t font_size(const TextRole role) {
    switch (role) {
    case TextRole::OutcomeTitle:
        return kFontSizeOutcome;
    case TextRole::StateTitle:
        return kFontSizeTitle;
    case TextRole::PrimaryValue:
        return kFontSizeHero;
    case TextRole::PrimaryUnit:
        return kFontSizeDisplay;
    case TextRole::Section:
        return kFontSizeSection;
    case TextRole::Body:
        return kFontSizeBody;
    case TextRole::Label:
        return kFontSizeCaption;
    case TextRole::Metadata:
        return kFontSizeMicro;
    }

    return kFontSizeBody;
}

std::int32_t line_height(const TextRole role) {
    switch (role) {
    case TextRole::OutcomeTitle:
        return kLineOutcome;
    case TextRole::StateTitle:
        return kLineTitle;
    case TextRole::PrimaryValue:
        return kLineHero;
    case TextRole::PrimaryUnit:
        return kLineDisplay;
    case TextRole::Section:
        return kLineSection;
    case TextRole::Body:
        return kLineBody;
    case TextRole::Label:
        return kLineCaption;
    case TextRole::Metadata:
        return kLineMicro;
    }

    return kLineBody;
}

std::int32_t font_weight(const TextRole role) {
    switch (role) {
    case TextRole::OutcomeTitle:
    case TextRole::StateTitle:
        return kFontWeightSemibold;
    case TextRole::PrimaryValue:
    case TextRole::PrimaryUnit:
        return kFontWeightBold;
    case TextRole::Section:
        return kFontWeightMedium;
    case TextRole::Body:
        return kFontWeightRegular;
    case TextRole::Label:
        return kFontWeightMedium;
    case TextRole::Metadata:
        return kFontWeightRegular;
    }

    return kFontWeightRegular;
}

} // namespace prd::ui::theme
