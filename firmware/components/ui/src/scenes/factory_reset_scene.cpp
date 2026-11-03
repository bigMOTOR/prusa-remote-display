// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include "scenes/factory_reset_scene.hpp"

#include <cmath>
#include <string_view>

#include "diagnostics/log.hpp"
#include "diagnostics/tags.hpp"
#include "text/scene_text.hpp"
#include "text/text.hpp"
#include "theme/layout.hpp"
#include "theme/tokens.hpp"

namespace prd::ui::scene {
namespace {

constexpr std::string_view kTag = diagnostics::tag::kUi;

// One measure for the progress line, the full content width, left-aligned at the content inset.
constexpr std::int32_t kTextX = theme::kCanvasContent.x;
constexpr std::int32_t kTextWidth = theme::kCanvasContent.width;

// ⛔ THE PROGRESS TITLE DOES NOT MOVE — 2026-09-12, and the arithmetic below is kept intact for
// exactly that reason. The block height is still measured as a title over a Body instruction even
// though the instruction is gone from this scene, because that is what put the running line where
// the owner accepted it. The question moved to the shared message composition; the neutral
// endpoint the erase settles on did not, and re-deriving it from what is now drawn would move it
// by 27 px while looking like a simplification.
constexpr std::int32_t kBlockHeight = theme::kLineTitle + theme::kSpaceComponent + theme::kLineBody;
constexpr std::int32_t kTitleY =
    theme::kCanvasContent.y + ((theme::kCanvasContent.height - kBlockHeight) / 2);

static_assert(kTitleY >= theme::kCanvasContent.y, "the block must start inside the content area");
static_assert(kTitleY + theme::kLineTitle <= theme::kCanvasContent.bottom(),
              "the running line must fit inside the content area");

// ⛔ THE PROGRESS LINE IS ALPHABETIC, SO ITS ROLE MUST CARRY LETTERS. tools/generate-fonts.py
// builds the hero and display faces from a digits-only subset, and either would drop every letter
// silently. StateTitle is the largest face carrying the text subset — the same trap, and the same
// answer, as the provisioning scene's password row.
static_assert(theme::kFontSizeTitle > theme::kFontSizeBody,
              "the running line must be larger than the body role beneath it");

// ⛔ THE ACTIVITY CADENCE, AND IT IS THE CONNECTING ONE — owner ruling after physical review,
// 2026-09-12. Production UI §9's access refinement made the one-two-three dot loop a
// Connecting-only exception; this ruling extends it to FactoryResetting and to nothing else.
//
// ⬅️ THE VALUES ARE STATED HERE RATHER THAN SHARED WITH THE ACCESS SCENE, deliberately and with
// its cost named. Moving them into the token layer would mean editing access_scene.cpp, which is
// outside this ruling's narrow scope; a second definition is the price. What keeps the two from
// drifting is not this comment but a measurement: the renderer suite times BOTH loops in one run
// and requires the same boundaries, so a changed constant fails a test rather than passing review.
constexpr std::uint32_t kDotStepMs = 500;
constexpr std::uint8_t kDotCount = 3;

void set_label(lv_obj_t* const label, const std::string_view text) {
    lv_label_set_text_fmt(label, "%.*s", static_cast<int>(text.size()), text.data());
}

} // namespace

bool FactoryResetScene::build(lv_obj_t* const parent) {
    root_ = theme::bare_object(parent);
    if (root_ == nullptr) {
        diagnostics::log(diagnostics::LogLevel::Error, kTag, "factory reset scene root failed");
        return false;
    }

    theme::place(root_, theme::kCanvas);

    title_ = theme::bare_label(root_);
    if (title_ == nullptr) {
        diagnostics::log(diagnostics::LogLevel::Error, kTag, "factory reset title failed");
        return false;
    }

    theme::apply_text_style(title_, theme::TextRole::StateTitle, theme::Colour::Primary);
    lv_obj_set_width(title_, kTextWidth);
    lv_obj_set_style_text_align(title_, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN);
    lv_obj_set_pos(title_, kTextX, kTitleY);
    lv_label_set_text(title_, "");

    // The question, in the shared composition. Built after the running line so it draws over it
    // during the crossfade rather than under it — Production UI §11's layering order.
    if (!form_.build(root_)) {
        diagnostics::log(diagnostics::LogLevel::Error, kTag, "factory reset message form failed");
        return false;
    }

    // The warning symbol is the statement's settled endpoint. This scene has no acquisition
    // outline to travel from: nothing was being acquired, a question was asked.
    form_.set_symbol(1.0);

    lv_obj_add_flag(root_, LV_OBJ_FLAG_HIDDEN);
    return true;
}

void FactoryResetScene::apply(const app_model::Presentation& presentation) {
    if (title_ == nullptr) {
        return;
    }

    // The operation outranks the modal, which is UIAD 001 §12's own order: §12.2's exclusive
    // operation is above §12.5's modal surface. Once the erase is running the question is answered
    // and must not still be on screen.
    const bool progress =
        presentation.device_operation == app_model::DeviceOperation::FactoryResetting;

    const bool changed = !applied_ || progress != progress_;
    applied_ = true;
    progress_ = progress;

    if (progress_ && changed) {
        // ⛔ ONLY ON THE CHANGE. Writing the title on every apply would restart the visible dot
        // cycle from one whenever an unrelated field of the presentation moved — and would
        // invalidate the label on a screen nothing had changed about. The cadence belongs to
        // step(); apply() only says which half is being presented.
        dots_ = 1;
        dots_anchored_ = false;
        set_running_title();

        // ⬅️ NO INSTRUCTION HERE, DELIBERATELY. The erase is six key operations and is over in
        // milliseconds, and it is interruption-safe by construction — a confirmed reset that loses
        // power resumes on the next boot. "Do not unplug the display" was written and removed: it
        // would be a warning about a danger this implementation does not have, which is the same
        // class of untruth as naming a gesture that does not exist.
    } else if (!progress_) {
        // ⛔ WARNING, NOT ERROR, AND THE TITLE STAYS PRIMARY — owner decision, 2026-09-12. Warning
        // is what is waiting on the reader; Error is an operation that completed and failed.
        // Nothing has gone wrong here, so the symbol is the attention one and the question is set
        // in the same Primary as every other title in the family. What distinguishes it from a
        // failure is the words: it ends in a question mark and it names both outcomes, so the
        // screen still reads correctly in monochrome (Product Brief: never status by colour alone;
        // Production UI §12's colour-independence rule).
        form_.set_statement(text::kFactoryResetConfirmTitle, text::kFactoryResetConfirmMessage,
                            text::MessageTone::Warning, "the factory-reset question");
    }

    // ⛔ THE ANSWERED QUESTION IS RETIRED AT ONCE, NOT FADED OUT — root finding R2, 2026-09-12.
    // A crossfade left "A short press cancels" fully legible on the first frame after the erase
    // began, and by then it is FALSE: nothing cancels a confirmed reset, and the reader is being
    // told they can still stop something they cannot. UI Behaviour Decision 001's refusal to
    // present what is not true outranks Production UI §10's preference for a fade, so the half
    // that has stopped being true leaves on the same tick the operation changed.
    //
    // The motion is therefore the ENTRY of valid content, never the lingering of invalid content:
    // the running line appears on the shared 1100 ms clock and settles on the unchanged neutral
    // endpoint. No new timing, and no change to the reset lifecycle itself.
    if (changed) {
        clock_.arm();
        running_ = true;
        level_ = 0;
        paint(0);
        return;
    }

    paint(level_ < 0 ? 0 : level_);
}

void FactoryResetScene::paint(const std::int32_t level) {
    level_ = level;

    // Exactly one half is in the tree at any instant — not one at zero opacity beside the other.
    // An answered destructive question must not be findable at all: not by a reader of the panel,
    // and not by a reader of the object tree.
#ifdef PRD_OLD_RESET_CROSSFADE
    // The pre-2026-09-12 crossfade, restored for the negative control: the answered question and
    // its cancellation instruction stayed on the panel while the running line faded in.
    form_.show(true);
    form_.paint(level);
#else
    form_.show(!progress_);
#endif

    if (progress_) {
        theme::apply_text_level(title_, theme::Opacity::Full, level);
        return;
    }

    form_.paint(level);
    theme::apply_text_level(title_, theme::Opacity::Transparent, 0);
}

// The running title at its current dot count. The origin never moves: the label is left-aligned
// at the content inset and only its trailing dots change, so the words stay exactly where the
// physical review accepted them.
void FactoryResetScene::set_running_title() {
    const auto label = text::lookup(text::kFactoryResetProgressTitle);
#ifdef PRD_OLD_RESET_STATIC
    // The pre-ruling behaviour, restored for the negative control in
    // tools/test-message-controls.sh: one fixed title, no activity at all.
    set_label(title_, label);
#else
    set_label(title_, label.substr(0, label.size() - kDotCount + dots_));
#endif
}

// ⛔ THE SAME SHAPE AS THE ACCESS SCENE'S LOOP, INCLUDING ITS Off BEHAVIOUR. The anchor is dropped
// whenever the panel cannot be seen and re-taken on the next visible tick, so the cycle RESUMES at
// the phase it was at instead of catching up through the dark. It stops outright when this is not
// the running half, which covers leaving the scene, the question, and a reset that failed — all of
// which route elsewhere.
bool FactoryResetScene::step_dots(const bool panel_visible, const std::uint32_t now) {
    if (!visible_ || !panel_visible || !progress_ || title_ == nullptr) {
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
    set_running_title();
    return true;
}

bool FactoryResetScene::step(const bool panel_visible, const std::uint32_t now) {
    // ⬅️ THE ONLY WRITE BETWEEN DOT CHANGES IS THE DOT CHANGE. step_dots() returns false on every
    // tick that is not a boundary and touches no LVGL object then, so a settled erase screen
    // invalidates nothing for 500 ms at a time and the loop returns to its ordinary idle wait.
    const bool dots_changed = step_dots(panel_visible, now);

    if (!running_) {
        return dots_changed;
    }

    // Holds rather than merely refusing to start: an entry interrupted by a dim resumes at the
    // frame it reached instead of finishing in the dark. See HoldClock.
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

void FactoryResetScene::set_visible(const bool visible) {
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
    running_ = false;
    applied_ = false;
    level_ = -1;
    clock_.clear();

    // Leaving resets the cycle rather than pausing it: the next erase is a new operation and
    // starts at one dot, not wherever the last one happened to stop.
    dots_ = 1;
    dots_anchored_ = false;
}

} // namespace prd::ui::scene
