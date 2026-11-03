// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include "scenes/access_scene.hpp"

#include <cmath>

#include "text/scene_text.hpp"
#include "text/text.hpp"

namespace prd::ui::scene {
namespace {

// Owner-approved activity indication only during Connecting; no power-policy timer.
constexpr std::uint32_t kDotStepMs = 500;
constexpr std::uint8_t kDotCount = 3;

} // namespace

bool AccessScene::build(lv_obj_t* parent) {
    if (!form_.build(parent)) {
        return false;
    }
    form_.set_acquisition(text::lookup(text::TextId::StateConnecting));
    return true;
}

void AccessScene::set_visible(bool visible) {
    if (!form_.built() || visible_ == visible) {
        return;
    }
    visible_ = visible;
    form_.show(visible);
    if (visible) {
        clock_.arm();
        running_ = true;
        paint(0);
    } else {
        running_ = false;
        icon_running_ = false;
        applied_ = false;
        clock_.clear();
    }
}

void AccessScene::apply(const app_model::Presentation& presentation) {
    auto entry_level = level_ < 0 ? 0 : level_;
    const bool changed = !applied_ || condition_ != presentation.access_condition;
    const bool entering = !applied_;
    condition_ = presentation.access_condition;
    applied_ = true;
    if (changed) {
        dots_ = 1;
        dots_anchored_ = false;
        switch (condition_) {
        case app_model::AccessCondition::None: {
            auto title = text::lookup(text::TextId::StateConnecting);
            form_.set_acquisition(title.substr(0, title.size() - kDotCount + dots_));
            break;
        }
        case app_model::AccessCondition::AuthenticationRequired:
            form_.set_statement(text::TextId::StateAuthenticationRequired,
                                text::TextId::AccessCheckKey, text::MessageTone::Warning,
                                "the authentication access text");
            break;
        case app_model::AccessCondition::UnsupportedInterface:
            form_.set_statement(text::TextId::StateUnsupportedInterface,
                                text::TextId::AccessUnsupported, text::MessageTone::Warning,
                                "the unsupported-interface access text");
            break;
        }
        const bool connecting = condition_ == app_model::AccessCondition::None;
        const double previous_target = icon_target_;
        icon_target_ = connecting ? 0.0 : 1.0;
        if (entering) {
            icon_mix_ = icon_target_;
            icon_running_ = false;
        } else if (icon_target_ != icon_mix_) {
            icon_from_ = icon_mix_;
            icon_clock_.arm();
            icon_running_ = true;
#ifdef PRD_OLD_ACCESS_STATIC
        } else if (false) {
#else
        } else if (icon_target_ == previous_target && !running_) {
#endif
            // ⛔ A RECLASSIFICATION BETWEEN TWO CLASSIFIED FAILURES MOVES NOTHING OTHERWISE, AND
            // THAT WAS THE GAP. Authentication required and Unsupported interface share a symbol,
            // an identity row and every measure of the composition, so the settled screen changed
            // two strings and stood still. The words are already the new ones — set above, before
            // this — so what the ramp carries in is the true statement and never the old one.
            //
            // ⬅️ ONLY FROM SETTLED. If a ramp is already running the clock is left alone: the
            // accepted rule for this scene is that an interruption keeps the painted appearance
            // coordinate and never replays from black, and re-arming here would do exactly that.
            entry_level = 0;
            clock_.arm();
            running_ = true;
        }
        // The outline morphs between the two semantic assets. No old telemetry survives;
        // the current title is applied immediately, including on reversal.
        // Keep the current appearance coordinate on interruption; never replay from black.
        level_ = -1;
    }
    const auto name =
        presentation.printer_name_known ? presentation.printer_name.view() : std::string_view{};
    form_.set_identity(name);
    paint(entry_level);
}

void AccessScene::paint(std::int32_t level) {
    level_ = level;
    form_.set_symbol(icon_mix_);
    form_.paint(level);
}

bool AccessScene::step_dots(bool panel_visible, std::uint32_t now) {
    if (!visible_ || !panel_visible || condition_ != app_model::AccessCondition::None) {
        dots_anchored_ = false;
        return false;
    }
    if (!dots_anchored_) {
        dots_at_ = now;
        dots_anchored_ = true;
        return false;
    }
    const auto steps = (now - dots_at_) / kDotStepMs;
    if (steps == 0) {
        return false;
    }
    dots_at_ += steps * kDotStepMs;
    const auto next = static_cast<std::uint8_t>(((dots_ - 1U + steps) % kDotCount) + 1U);
    if (next == dots_) {
        return false;
    }
    dots_ = next;
    const auto label = text::lookup(text::TextId::StateConnecting);
    form_.set_title_text(label.substr(0, label.size() - kDotCount + dots_));
    return true;
}

bool AccessScene::step(bool panel_visible, std::uint32_t now) {
    const bool dots_changed = step_dots(panel_visible, now);

    // ⛔ HELD, NOT MERELY START-GATED. EntryClock refuses to begin in the dark but carries on
    // through one; an entry or a morph dimmed at 300 ms was finished and settled before anybody
    // saw it. HoldClock stops at the frame it reached and resumes there. The accepted Connecting
    // endpoints, the dot cadence and its own separate pause are untouched — only the two clocks
    // that carry this scene's bounded one-shot motions changed.
    if (icon_running_) {
        icon_clock_.advance(panel_visible, now);
        const auto elapsed = icon_clock_.elapsed();
        if (panel_visible && elapsed >= theme::kOperationalMotionMs) {
            icon_mix_ = icon_target_;
            icon_running_ = false;
        } else {
            icon_mix_ = icon_from_ + ((icon_target_ - icon_from_) * entry_progress(elapsed));
        }
    }

    if (!running_) {
        paint(theme::opacity(theme::Opacity::Full));
        return icon_running_ || dots_changed;
    }

    clock_.advance(panel_visible, now);

    const auto elapsed = clock_.elapsed();
    if (panel_visible && elapsed >= theme::kOperationalMotionMs) {
        running_ = false;
        paint(theme::opacity(theme::Opacity::Full));
        return icon_running_ || dots_changed;
    }

    paint(static_cast<std::int32_t>(
        std::lround(entry_progress(elapsed) * theme::opacity(theme::Opacity::Full))));
    return true;
}

} // namespace prd::ui::scene
