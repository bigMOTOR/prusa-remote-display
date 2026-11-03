// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include "scenes/firmware_update_scene.hpp"

#include <cmath>
#include <string_view>

#include "diagnostics/log.hpp"
#include "diagnostics/tags.hpp"
#include "fonts/motion_fonts.hpp"
#include "formatters.hpp"
#include "text/scene_text.hpp"
#include "text/text.hpp"
#include "theme/layout.hpp"
#include "theme/tokens.hpp"

namespace prd::ui::scene {
namespace {

constexpr std::string_view kTag = diagnostics::tag::kUi;
constexpr std::uint32_t kDotStepMs = 500;
constexpr std::uint8_t kDotCount = 3;

void show(lv_obj_t* const object, const bool visible) {
    if (visible) {
        lv_obj_remove_flag(object, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(object, LV_OBJ_FLAG_HIDDEN);
    }
}

void set_label(lv_obj_t* const label, const std::string_view value) {
    lv_label_set_text_fmt(label, "%.*s", static_cast<int>(value.size()), value.data());
}

} // namespace

bool FirmwareUpdateScene::build(lv_obj_t* const parent) {
    root_ = theme::bare_object(parent);
    if (root_ == nullptr) {
        diagnostics::log(diagnostics::LogLevel::Error, kTag, "firmware update root failed");
        return false;
    }
    theme::place(root_, theme::kCanvas);

    if (!download_track_.build(root_, theme::kUpdateDownloadMark, theme::Colour::Tertiary)) {
        diagnostics::log(diagnostics::LogLevel::Error, kTag, "firmware update mark failed");
        return false;
    }

    download_fill_clip_ = theme::bare_object(root_);
    if (download_fill_clip_ == nullptr) {
        diagnostics::log(diagnostics::LogLevel::Error, kTag, "firmware update fill clip failed");
        return false;
    }
    theme::place(download_fill_clip_, theme::kUpdateDownloadMark);
    if (!download_fill_.build(download_fill_clip_, theme::kUpdateDownloadMarkLocal)) {
        diagnostics::log(diagnostics::LogLevel::Error, kTag, "firmware update fill failed");
        return false;
    }

    version_ = theme::bare_label(root_);
    value_ = theme::bare_label(root_);
    unit_ = theme::bare_label(root_);
    title_ = theme::bare_label(root_);
    message_ = theme::bare_label(root_);
    process_ = theme::bare_label(root_);
    if (version_ == nullptr || value_ == nullptr || unit_ == nullptr || title_ == nullptr ||
        message_ == nullptr || process_ == nullptr) {
        diagnostics::log(diagnostics::LogLevel::Error, kTag, "firmware update labels failed");
        return false;
    }

    theme::apply_text_style(version_, theme::TextRole::Body, theme::Colour::Tertiary);
    lv_obj_set_width(version_, theme::kCanvasContent.width);
    lv_obj_set_pos(version_, theme::kCanvasContent.x, theme::kUpdateVersionY);

    theme::apply_text_style(value_, theme::TextRole::PrimaryValue, theme::Colour::Primary);
    lv_obj_set_style_text_font(value_, fonts::motion_value(theme::kUpdateValueSize), LV_PART_MAIN);
    lv_obj_set_width(value_, LV_SIZE_CONTENT);
    lv_obj_set_style_text_align(value_, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN);
    lv_obj_set_pos(value_, theme::kUpdateValueX, theme::kUpdateValueY);

    theme::apply_text_style(unit_, theme::TextRole::PrimaryUnit, theme::Colour::Primary);
    lv_obj_set_style_text_font(unit_, fonts::motion_unit(theme::kUpdateUnitSize), LV_PART_MAIN);
    lv_obj_set_width(unit_, theme::kUpdateUnitWidth);
    lv_obj_set_pos(unit_, theme::kUpdateValueX, theme::kUpdateUnitY);
    set_label(unit_, text::lookup(text::TextId::UnitPercent));

    theme::apply_text_style(title_, theme::TextRole::StateTitle, theme::Colour::Primary);
    lv_obj_set_width(title_, theme::kCanvasContent.width);
    lv_obj_set_pos(title_, theme::kCanvasContent.x, theme::kUpdateTitleY);
    set_label(title_, text::lookup(text::kUpdateDownloadingTitle));

    theme::apply_text_style(message_, theme::TextRole::Body, theme::Colour::Secondary);
    lv_obj_set_width(message_, theme::kCanvasContent.width);
    lv_obj_set_pos(message_, theme::kCanvasContent.x, theme::kUpdateMessageY);
    set_label(message_, text::lookup(text::kUpdateKeepPowered));

    theme::apply_text_style(process_, theme::TextRole::StateTitle, theme::Colour::Primary);
    lv_obj_set_width(process_, theme::kCanvasContent.width);
    lv_obj_set_style_text_align(process_, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN);
    lv_obj_set_pos(process_, theme::kCanvasContent.x, theme::kUpdateProcessTitleY);

    lv_obj_add_flag(root_, LV_OBJ_FLAG_HIDDEN);
    paint(0);
    return true;
}

void FirmwareUpdateScene::apply(const app_model::Presentation& presentation) {
    if (root_ == nullptr) {
        return;
    }

    Mode next = Mode::None;
    switch (presentation.ota_phase) {
    case app_model::OtaPhase::Downloading:
        next = Mode::Downloading;
        break;
    case app_model::OtaPhase::Verifying:
    case app_model::OtaPhase::Installing:
    case app_model::OtaPhase::RebootRequired:
        next = Mode::Process;
        break;
    case app_model::OtaPhase::Idle:
    case app_model::OtaPhase::Checking:
    case app_model::OtaPhase::Available:
    case app_model::OtaPhase::Validating:
    case app_model::OtaPhase::Failed:
        next = Mode::None;
        break;
    }

    const bool phase_changed = !applied_ || phase_ != presentation.ota_phase;
    phase_ = presentation.ota_phase;
    mode_ = next;
    applied_ = true;

    if (mode_ == Mode::Downloading) {
        progress_known_ = presentation.update_progress.known;
        progress_percent_ = presentation.update_progress.percent;
        const auto progress = format::percent(presentation.update_progress);
        set_label(value_, progress.view());
        lv_obj_update_layout(value_);
        lv_obj_set_x(unit_, theme::kUpdateValueX + lv_obj_get_width(value_));

        const auto version = format::firmware_version(presentation.update_version);
        version_known_ = !version.empty();
        set_label(version_, version.view());
        const auto fill_height =
            static_cast<std::int32_t>(theme::kUpdateDownloadMark.height * progress_percent_ / 100U);
        lv_obj_set_height(download_fill_clip_, fill_height);
    } else {
        progress_known_ = false;
        version_known_ = false;
        progress_percent_ = 0;
    }

    if (mode_ == Mode::Process && phase_changed) {
        dots_ = 1;
        dots_anchored_ = false;
        set_process_title();
    }

    if (phase_changed) {
        clock_.arm();
        running_ = true;
        level_ = 0;
        paint(0);
        return;
    }

    paint(level_ < 0 ? 0 : level_);
}

void FirmwareUpdateScene::paint(const std::int32_t level) {
    level_ = level;
    const bool downloading = mode_ == Mode::Downloading;
    const bool process = mode_ == Mode::Process;

    download_track_.show(downloading);
    download_track_.paint(level);
    const bool filled = downloading && progress_known_ && progress_percent_ > 0;
    show(download_fill_clip_, filled);
    download_fill_.show(filled);
    download_fill_.paint(level);
    show(version_, downloading && version_known_);
    show(value_, downloading && progress_known_);
    show(unit_, downloading && progress_known_);
    show(title_, downloading);
    show(message_, downloading);
    show(process_, process);

    theme::apply_text_level(value_, theme::Opacity::Full, level);
    theme::apply_text_level(version_, theme::Opacity::Full, level);
    theme::apply_text_level(unit_, theme::Opacity::Full, level);
    theme::apply_text_level(title_, theme::Opacity::Full, level);
    theme::apply_text_level(message_, theme::Opacity::High, level);
    theme::apply_text_level(process_, theme::Opacity::Full, level);
}

void FirmwareUpdateScene::set_process_title() {
    text::TextId id = text::kUpdateVerifyingTitle;
    switch (phase_) {
    case app_model::OtaPhase::Verifying:
        id = text::kUpdateVerifyingTitle;
        break;
    case app_model::OtaPhase::Installing:
        id = text::kUpdateInstallingTitle;
        break;
    case app_model::OtaPhase::RebootRequired:
        id = text::kUpdateRestartingTitle;
        break;
    case app_model::OtaPhase::Idle:
    case app_model::OtaPhase::Checking:
    case app_model::OtaPhase::Available:
    case app_model::OtaPhase::Downloading:
    case app_model::OtaPhase::Validating:
    case app_model::OtaPhase::Failed:
        break;
    }

    const auto full = text::lookup(id);
    set_label(process_, full.substr(0, full.size() - kDotCount + dots_));
}

bool FirmwareUpdateScene::step_dots(const bool panel_visible, const std::uint32_t now) {
    if (!visible_ || !panel_visible || mode_ != Mode::Process) {
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
    set_process_title();
    return true;
}

bool FirmwareUpdateScene::step(const bool panel_visible, const std::uint32_t now) {
    const bool dots_changed = step_dots(panel_visible, now);
    if (!running_) {
        return dots_changed;
    }

    clock_.advance(panel_visible, now);
    const auto elapsed = clock_.elapsed();
    if (panel_visible && elapsed >= theme::kOperationalMotionMs) {
        running_ = false;
        paint(theme::opacity(theme::Opacity::Full));
        return dots_changed;
    }

    paint(static_cast<std::int32_t>(
        std::lround(entry_progress(elapsed) * theme::opacity(theme::Opacity::Full))));
    return true;
}

void FirmwareUpdateScene::set_visible(const bool visible) {
    if (root_ == nullptr) {
        return;
    }
    visible_ = visible;
    if (visible) {
        lv_obj_remove_flag(root_, LV_OBJ_FLAG_HIDDEN);
        clock_.arm();
        running_ = true;
        level_ = 0;
        paint(0);
        return;
    }

    lv_obj_add_flag(root_, LV_OBJ_FLAG_HIDDEN);
    clock_.clear();
    applied_ = false;
    running_ = false;
    level_ = -1;
    dots_ = 1;
    dots_anchored_ = false;
}

} // namespace prd::ui::scene
