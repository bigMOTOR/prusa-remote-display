// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include "scenes/provisioning_scene.hpp"

#include <algorithm>
#include <cmath>
#include <string>

#include "diagnostics/log.hpp"
#include "diagnostics/tags.hpp"
#include "text/scene_text.hpp"
#include "text/text.hpp"
#include "wifi_qr.hpp"

namespace prd::ui::scene {
namespace {

constexpr std::string_view kTag = diagnostics::tag::kUi;

// THIS SCENE OWNS NO USER-VISIBLE STRING. The title and the three captions are requested
// from the text resource boundary by id; the network name and the password are runtime
// values from the session channel. firmware/tools/check-tokens.sh rejects a literal here.

// --- The composition, in tokens ---------------------------------------------
//
// Every figure below is §3 token arithmetic or QR geometry. No bare pixel appears, which
// is what makes the token check meaningful rather than decorative.
//
// THE SHAPE, AND WHY IT IS THIS SHAPE. A text column on the left, the QR block on the
// right, and the password full width beneath both. It is not a composition preference —
// every alternative was ruled out by measurement, and the measurements are worth keeping
// because the next person to move something will hit the same walls:
//
//   - The password cannot share a line with anything. Twelve characters of the Milestone
//     3.5 alphabet at StateTitle reach 244.5 px in the worst case ('W' is in that
//     alphabet), against 296 px of content width. Any side column would clip it, and a
//     clipped password is unrecoverable for the person copying it.
//   - The QR cannot shrink below its module grid. The 44-byte payload needs QR version 4 —
//     33 modules — which was measured against the pinned qrcodegen rather than read off a
//     capacity table, because the table answer (version 3) is wrong.
//   - A full-width StateTitle row does not fit beside a 164 px block and a 40 px password
//     row: 24 + 164 + 40 exceeds the 216 px of content height. The title therefore joins
//     the text column, at Body, and that is the one presentation the QR cost.
constexpr std::int32_t kQrModules = 33;
constexpr std::int32_t kQrScale = 4;
constexpr std::int32_t kQrQuietModules = 4;

constexpr std::int32_t kQrCode = kQrModules * kQrScale;
constexpr std::int32_t kQrQuiet = kQrQuietModules * kQrScale;
constexpr std::int32_t kQrBlock = kQrCode + (2 * kQrQuiet);

// THE QUIET ZONE IS OURS, NOT LVGL'S. lv_qrcode's own quiet zone is two modules per side;
// the specification asks four, and owner ruling 2026-08-16 makes scan reliability this
// checkpoint's primary purpose. So the widget is sized to the code exactly and the light
// field around it is this panel, which happens to land on `space.4` precisely.
static_assert(kQrQuiet == theme::kSpaceRegion,
              "the four-module quiet zone coincides with space.4 at this scale");

constexpr theme::Rect kQrBlockRect{theme::kCanvasContent.right() - kQrBlock,
                                   theme::kCanvasContent.y, kQrBlock, kQrBlock};

// The text column takes what the QR block and the gutter leave.
//
// ⬅️ THE GUTTER IS space.1, AND IT WAS space.2 UNTIL 2026-08-16. That cost four pixels of
// column, and the address needs 125 of them: at 124 the string `http://192.168.4.1`
// overflowed by ONE pixel and LVGL wrapped its final character onto a second line, which
// the owner caught on the panel. Measured from the committed face the way LVGL sums it —
// each glyph's advance rounded to a whole pixel, class kerning included — not by summing
// the 8.4 fixed-point advances and dividing once, which is the model that produced the
// wrong number and let this through.
//
// The gutter can be this small because the QR block's own quiet zone is a full space.4 of
// light on that edge: the gap a reader sees is 20 px, not 4.
constexpr std::int32_t kColumnWidth = theme::kCanvasContent.width - kQrBlock - theme::kSpace1;
constexpr std::int32_t kColumnX = theme::kCanvasContent.x;

// A secondary row is its caption over a Body value; the password row is its caption over a
// StateTitle value. The two boxes differ because the roles differ.
constexpr std::int32_t kSecondaryRow = theme::kLineCaption + theme::kLineBody;
constexpr std::int32_t kPasswordRow = theme::kLineCaption + theme::kLineTitle;

constexpr std::int32_t kColumnHeight =
    theme::kLineBody + theme::kSpaceRegion + kSecondaryRow + theme::kSpaceRegion + kSecondaryRow;

// The column is centred against the QR block rather than aligned to its top: the block is
// the heavier object and the column reads as belonging to it.
constexpr std::int32_t kColumnY = theme::kCanvasContent.y + ((kQrBlock - kColumnHeight) / 2);

constexpr std::int32_t kTitleY = kColumnY;
constexpr std::int32_t kNetworkY = kTitleY + theme::kLineBody + theme::kSpaceRegion;
constexpr std::int32_t kAddressY = kNetworkY + kSecondaryRow + theme::kSpaceRegion;
constexpr std::int32_t kPasswordY = theme::kCanvasContent.y + kQrBlock + theme::kSpaceComponent;

// Everything must fit the canvas with every element present. Checked here rather than
// discovered on a panel.
static_assert(kColumnHeight <= kQrBlock, "the text column is taller than the QR block");
static_assert(kAddressY + kSecondaryRow <= kQrBlockRect.bottom(),
              "the text column overflows the QR block it is centred against");
static_assert(kPasswordY + kPasswordRow <= theme::kCanvasContent.bottom(),
              "the password row overflows the canvas content area");
static_assert(kColumnX + kColumnWidth + theme::kSpace1 == kQrBlockRect.x,
              "the column, the gutter and the QR block must tile the content width");

// The password must not be the smallest thing a person has to read most carefully. It is
// the one value on this screen that is transcribed character by character.
static_assert(theme::kFontSizeTitle > theme::kFontSizeBody,
              "the password's role must be larger than the rows beside it");

// Where each row's box sits and how its text is set within it. The two secondary rows live
// in the left column beside the QR; the password owns the full width beneath both.
struct RowBox {
    std::int32_t x;
    std::int32_t y;
    std::int32_t width;

    // One line of this row's value role. A rendered label taller than this has wrapped,
    // which is how set_row() detects an overflow without predicting one.
    std::int32_t value_line;

    lv_text_align_t align;
};

// ⬅️ THE STATEMENT LEFT THIS SCENE'S GEOMETRY — 2026-09-12. Until then a statement borrowed the
// session title, promoted it to StateTitle, widened it to the full canvas, and drew a Status
// Message at a baseline of its own; every other path then had to restore the role and the width
// before drawing anything. The shared composition — component::MessageForm — has its own objects
// at its own accepted geometry, so nothing below is touched by a fault or by recovery, and owner
// ruling R3's frozen QR and session measures cannot be disturbed by a state that merely passes
// through. kMessageY, kRecoveryWidth and kMessageLines are gone with it; theme/layout.hpp carries
// the message composition's budget, and its assertions are the ones that hold it.

constexpr std::array<RowBox, 3> kRowBoxes{{
    {kColumnX, kNetworkY, kColumnWidth, theme::kLineBody, LV_TEXT_ALIGN_LEFT},
    {theme::kCanvasContent.x, kPasswordY, theme::kCanvasContent.width, theme::kLineTitle,
     LV_TEXT_ALIGN_CENTER},
    {kColumnX, kAddressY, kColumnWidth, theme::kLineBody, LV_TEXT_ALIGN_LEFT},
}};

static_assert(SetupSession{}.password.capacity() + 1 == 64,
              "the scene-owned password label must hold the complete setup credential");

// lv_label_set_text_fmt copies, and the length is explicit — so a std::string_view that
// is not NUL-terminated is safe and nothing allocates on the caller's side. The same
// idiom the telemetry column uses.
void set_label(lv_obj_t* const label, const std::string_view text) {
    lv_label_set_text_fmt(label, "%.*s", static_cast<int>(text.size()), text.data());
}

} // namespace

bool ProvisioningScene::build(lv_obj_t* const parent) {
    root_ = theme::bare_object(parent);
    if (root_ == nullptr) {
        diagnostics::log(diagnostics::LogLevel::Error, kTag, "provisioning scene root failed");
        return false;
    }

    // Absolute placement, not a layout container. Production UI §7: "layout does not
    // reflow", and a flex parent is precisely a thing that reflows when a child is hidden.
    theme::place(root_, theme::kCanvas);

    title_ = theme::bare_label(root_);
    if (title_ == nullptr) {
        diagnostics::log(diagnostics::LogLevel::Error, kTag, "provisioning title failed");
        return false;
    }

    theme::apply_text_style(title_, theme::TextRole::Body, theme::Colour::Primary);
    lv_obj_set_width(title_, kColumnWidth);
    lv_obj_set_style_text_align(title_, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN);
    lv_obj_set_pos(title_, kColumnX, kTitleY);
    lv_label_set_text(title_, "");

    static_assert(kRowBoxes.size() == kRowCount, "every row needs a box and every box a row");

    for (std::size_t index = 0; index < kRowCount; ++index) {
        Row& current = rows_[index];
        const RowBox& box = kRowBoxes[index];

        current.caption = theme::bare_label(root_);
        current.value = theme::bare_label(root_);

        if (current.caption == nullptr || current.value == nullptr) {
            diagnostics::log(diagnostics::LogLevel::Error, kTag, "provisioning row failed");
            return false;
        }

        // The caption is quieter than its value: Production UI §5 establishes hierarchy
        // "primarily through luminance", and the value is what the reader is here for.
        theme::apply_text_style(current.caption, theme::TextRole::Label, theme::Colour::Tertiary);
        lv_obj_set_width(current.caption, box.width);
        lv_obj_set_style_text_align(current.caption, box.align, LV_PART_MAIN);
        lv_obj_set_pos(current.caption, box.x, box.y);
        set_label(current.caption, text::lookup(text::kSetupFieldLabels[index]));

        theme::apply_text_style(current.value, kValueRoles[index], kValueColours[index]);
        lv_obj_set_width(current.value, box.width);
        lv_obj_set_style_text_align(current.value, box.align, LV_PART_MAIN);
        lv_obj_set_pos(current.value, box.x, box.y + theme::kLineCaption);
        if (index == static_cast<std::size_t>(RowIndex::Password)) {
            lv_label_set_text_static(current.value, password_label_text_.data());
        } else {
            lv_label_set_text(current.value, "");
        }

        // Every row starts absent. A row appears only when its value arrives.
        lv_obj_add_flag(current.caption, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(current.value, LV_OBJ_FLAG_HIDDEN);
    }

    // The QR block: a light field carrying the four-module quiet zone, with the code
    // centred in it. Built last so it draws over nothing, and hidden until a session
    // arrives — the same presence rule the rows obey.
    qr_panel_ = theme::bare_object(root_);
    if (qr_panel_ == nullptr) {
        diagnostics::log(diagnostics::LogLevel::Error, kTag, "provisioning qr panel failed");
        return false;
    }

    theme::place(qr_panel_, kQrBlockRect);
    lv_obj_set_style_bg_color(qr_panel_, theme::colour(theme::Colour::Primary), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(qr_panel_, theme::opacity(theme::Opacity::Full), LV_PART_MAIN);

    qr_ = lv_qrcode_create(qr_panel_);
    if (qr_ == nullptr) {
        diagnostics::log(diagnostics::LogLevel::Error, kTag, "provisioning qr failed");
        return false;
    }

    // ⛔ DARK MODULES ON A LIGHT FIELD, NOT THE OTHER WAY ROUND. An inverted code is not
    // reliably readable by a phone camera, so the palette is not a stylistic choice here.
    // Both values are §5 tokens: no literal colour is introduced for this (owner ruling A,
    // 2026-08-16), and check-tokens would reject one.
    lv_qrcode_set_size(qr_, kQrCode);
    lv_qrcode_set_dark_color(qr_, theme::colour(theme::Colour::Background));
    lv_qrcode_set_light_color(qr_, theme::colour(theme::Colour::Primary));

    // The widget's own quiet zone is two modules; this scene supplies four through the
    // panel behind it, so the widget is sized to the code exactly and offset into place.
    lv_qrcode_set_quiet_zone(qr_, false);
    lv_obj_set_pos(qr_, kQrQuiet, kQrQuiet);
    lv_obj_add_flag(qr_panel_, LV_OBJ_FLAG_HIDDEN);

    // The shared message composition. Built last so it draws over the session composition it
    // replaces rather than under it, and hidden on the same presence rule as everything above.
    if (!form_.build(root_)) {
        diagnostics::log(diagnostics::LogLevel::Error, kTag, "provisioning message form failed");
        return false;
    }

    set_visible(false);
    return true;
}

void ProvisioningScene::apply_qr() {
    if (qr_ == nullptr || qr_panel_ == nullptr) {
        return;
    }

    wifi_qr::Payload payload;

    // ⛔ session_drawable_ IS NOT has_session_, AND THE DIFFERENCE IS THE WHOLE POINT. A statement
    // can be reached while the setup access point is still up — the DriverStalled ruling keeps it
    // physically alive so a browser mid-flow is not stranded — so "there is a session" no longer
    // implies "the session is what the panel shows". Drawing the code from a live session under a
    // full-canvas statement is what this prevents, and clearing it is what keeps the modules from
    // outliving their screen: they encode the password as readably as the row did.
#ifdef PRD_OLD_SETUP_EARLY_SESSION
    const bool encoded = session_drawable_ && has_session_ &&
#else
    const bool encoded = session_drawable_ && offering_network_ && has_session_ &&
#endif
                         wifi_qr::build(session_.ssid.view(), session_.password.view(), payload);

    bool drawn = false;
    if (encoded) {
        const std::string_view bytes = payload.view();
        drawn = lv_qrcode_update(qr_, bytes.data(), static_cast<std::uint32_t>(bytes.size())) ==
                LV_RESULT_OK;
    }

    // ⛔ THE PAYLOAD CARRIES THE PASSWORD, so it is wiped on every path out of here —
    // including the ones where nothing was drawn.
    wifi_qr::wipe(payload);

    if (drawn) {
        lv_obj_remove_flag(qr_panel_, LV_OBJ_FLAG_HIDDEN);

        // PRESENCE ONLY, AND THAT IS THE WHOLE LINE. No payload, no SSID, no password, no
        // length — §13.5 and owner ruling D1 apply to the encoded form exactly as they do
        // to the password itself. What it buys is that a code which failed to draw stops
        // being indistinguishable from one nobody scanned.
        if (!was_drawn_) {
            diagnostics::log(diagnostics::LogLevel::Info, kTag, "setup qr drawn");
        }

        was_drawn_ = true;
        return;
    }

    // Re-encoding an empty string clears the canvas. Hiding the panel alone would not: the
    // modules of the previous code still encode the password, and that canvas buffer sits
    // in the coredump region — §13.5's "embedded in crash dumps where avoidable" is the
    // same obligation the scene-owned password label buffer's wipe in set_row() answers.
    (void)lv_qrcode_update(qr_, "", 0);
    lv_obj_add_flag(qr_panel_, LV_OBJ_FLAG_HIDDEN);

    // Only a session that exists, is the thing on the panel, and still could not be encoded is a
    // fault. Clearing the code at the end of a session reaches here too, and that is ordinary —
    // and so does a statement taking the canvas from a session that is still up.
    if (session_drawable_ && offering_network_ && has_session_) {
        diagnostics::log(diagnostics::LogLevel::Warning, kTag,
                         "setup qr not drawn; the panel still shows the network name, "
                         "password and address as text");
    } else if (was_drawn_) {
        diagnostics::log(diagnostics::LogLevel::Info, kTag, "setup qr cleared");
    }

    was_drawn_ = false;
}

void ProvisioningScene::set_row(const RowIndex index, const std::string_view value) {
    Row& target = row(index);
    if (target.caption == nullptr || target.value == nullptr) {
        return;
    }

    if (index == RowIndex::Password && std::string_view{password_label_text_.data()} != value) {
        // The label borrows this fixed buffer. Wipe all of it before the old credential
        // is withdrawn or replaced, including bytes past the new terminator. In contrast,
        // lv_label_set_text[_fmt] frees its old allocation before writing the replacement.
#ifndef PRD_OLD_SETUP_LABEL_SCRUB
        auto* const bytes = reinterpret_cast<volatile unsigned char*>(password_label_text_.data());
        for (std::size_t offset = 0; offset < password_label_text_.size(); ++offset) {
            bytes[offset] = 0;
        }
#endif
        if (value.size() >= password_label_text_.size()) {
            // Every current caller supplies BoundedString<63>, but fail closed if a future
            // caller violates that contract: the old credential is already wiped above.
            lv_label_set_text_static(target.value, password_label_text_.data());
            lv_obj_add_flag(target.caption, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(target.value, LV_OBJ_FLAG_HIDDEN);
            return;
        }
        if (!value.empty()) {
            std::copy(value.begin(), value.end(), password_label_text_.begin());
        }
        password_label_text_[value.size()] = '\0';
        lv_label_set_text_static(target.value, password_label_text_.data());
    }

    if (value.empty()) {
        lv_obj_add_flag(target.caption, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(target.value, LV_OBJ_FLAG_HIDDEN);
        if (index != RowIndex::Password) {
            lv_label_set_text(target.value, "");
        }
        return;
    }

    if (index != RowIndex::Password) {
        set_label(target.value, value);
    }
    lv_obj_remove_flag(target.caption, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(target.value, LV_OBJ_FLAG_HIDDEN);

    // ⬅️ A WRAPPED VALUE IS REPORTED, NOT LEFT TO BE NOTICED ON A PANEL.
    //
    // The address broke across two lines on 2026-08-16 — `http://192.168.4.1` needs 125 px
    // and the column was 124 — and nothing in the firmware knew. The column is wider now,
    // but no column this layout can offer is wide enough for every address the access point
    // could report: 132 px is the most the QR block leaves, and a nineteen-character
    // address already needs 134. So the width is sized for the value the device actually
    // shows, and this check covers the rest.
    //
    // IT MEASURES THE CONSEQUENCE RATHER THAN PREDICTING IT. The label auto-sizes its
    // height, so a value that wrapped is taller than one line of its own role — asked of
    // LVGL, after LVGL has laid it out, using the same metrics it will draw with. A
    // threshold computed anywhere else is what produced the defect: the first width model
    // summed fixed-point advances and divided once, where LVGL rounds every glyph
    // separately and applies kerning, and it was wrong by exactly the pixel that mattered.
    //
    // Clipping instead would be worse than wrapping: a truncated address looks like a
    // complete one.
    lv_obj_update_layout(target.value);
    if (lv_obj_get_height(target.value) > kRowBoxes[static_cast<std::size_t>(index)].value_line) {
        diagnostics::log(diagnostics::LogLevel::Warning, kTag,
                         "a provisioning value is too wide for its column and has wrapped; "
                         "the composition needs re-measuring against the committed face");
    }
}

// The shared full-screen message composition — the one every family that states a condition in
// words now draws. Recovery, the four device faults this scene has copy for, and nothing else.
//
// ⬅️ EXTRACTED TWICE. It was recovery's alone until checkpoint 3c, when a second caller wanting
// the same geometry made it this method; the cross-screen audit of 2026-09-12 found a third and a
// fourth arrangement elsewhere in the product and moved the drawing itself into a §8 component.
// What remains here is the routing: which pair of words, which semantic tone, and whose budget the
// overflow diagnostic names.
void ProvisioningScene::draw_statement(const text::TextId title, const text::TextId message,
                                       const text::MessageTone tone,
                                       const std::string_view subject) {
    // ⛔ A DIFFERENT STATEMENT IS A CHANGE, NOT A CONTINUATION — root finding R4, 2026-09-12. The
    // first version armed the clock only when the scene was not already showing a statement, so a
    // settled recovery statement displaced by a device fault — reachable, since §12.1 outranks
    // §12.3 — swapped its words with no motion at all. Keyed on the CONTENT rather than on the
    // scene, so every reachable replacement enters.
#ifdef PRD_OLD_STATEMENT_SWAP
    // The pre-2026-09-12 rule, restored for the negative control: a statement replacing another
    // was not a change, so the settled screen swapped its words with no motion.
    const bool changed = !statement_;
#else
    const bool changed = !statement_ || title != statement_title_ ||
                         message != statement_message_ || tone != statement_tone_;
#endif

    // ⬅️ AND AN UNCHANGED STATEMENT IS LEFT ALONE ENTIRELY. Every poll that resolves to the same
    // fault arrives here; rewriting the labels would invalidate them on each one, which is
    // Production UI §11's "only components affected by state changes shall be redrawn" broken by
    // a composition that never changes. The motion, if any is owed, belongs to step().
    if (!changed) {
        return;
    }

    hide_session();

    form_.set_statement(title, message, tone, subject);
    form_.set_symbol(1.0);
    form_.show(true);

    statement_ = true;
    statement_title_ = title;
    statement_message_ = message;
    statement_tone_ = tone;

    // ⛔ ENTERING A STATEMENT IS A SCENE CHANGE EVEN WHEN THE SCENE DOES NOT CHANGE. A device fault
    // raised while the setup composition is on the panel replaces everything on it, and Production
    // UI §10 requires motion to communicate that. The clock is ARMED, not started: step() begins it
    // on the first tick the panel can actually be seen, so nothing runs behind a dark backlight —
    // and it HOLDS if the panel goes dark part-way, rather than finishing unseen.
    //
    // ⬅️ THE WORDS ARE ALREADY THE NEW ONES WHEN THE RAMP BEGINS. The displaced statement is gone
    // from the first frame; what the ramp carries in is the true one. That is the same ordering
    // root finding R2 required of the reset, and for the same reason.
    // ⬅️ ONLY FROM SETTLED, the same rule the access scene follows: a ramp already in flight keeps
    // its painted appearance coordinate rather than replaying from black. The words are the new
    // ones either way — they were written above, before this.
    if (!running_) {
        clock_.arm();
        running_ = true;
        level_ = 0;
    }

    paint(level_ < 0 ? 0 : level_);
}

// Puts the session composition back after a statement has had the canvas, from the session that
// was retained throughout. The rows were not merely hidden — hide_session() overwrote the
// credential in LVGL's own buffer — so they are written again here rather than revealed.
void ProvisioningScene::show_session() {
    if (session_drawable_) {
        return;
    }

    session_drawable_ = true;
    lv_obj_remove_flag(title_, LV_OBJ_FLAG_HIDDEN);

    if (has_session_ && offering_network_) {
        set_row(RowIndex::Network, session_.ssid.view());
        set_row(RowIndex::Password, session_.password.view());
        set_row(RowIndex::Address, session_.url.view());
    }

    apply_qr();
}

void ProvisioningScene::hide_session() {
    // Once is enough: the rows are already empty and the code already cleared, and re-running
    // this on every poll would invalidate them again for no change.
    if (!session_drawable_) {
        return;
    }

    // The credential first. set_row() wipes the scene-owned text buffer as well as hiding it,
    // and session_drawable_ makes apply_qr() clear the encoded copy rather than redraw it.
    session_drawable_ = false;
    session_running_ = false;
    session_clock_.clear();
#if !defined(PRD_OLD_SETUP_STALE_OFFER) && !defined(PRD_OLD_SESSION_ROWS)
    // A statement supersedes the offer on the panel. If it later clears into SetupRequired,
    // show_session() runs before apply() processes that phase; the old offer must not cause a
    // password and QR to be populated only to be cleared again in the same call.
    offering_network_ = false;
#endif

    set_row(RowIndex::Network, {});
    set_row(RowIndex::Password, {});
    set_row(RowIndex::Address, {});
    apply_qr();

    lv_obj_add_flag(title_, LV_OBJ_FLAG_HIDDEN);
}

void ProvisioningScene::apply(const app_model::Presentation& presentation) {
    if (title_ == nullptr) {
        return;
    }

    // Only two phases reach this scene; scene_selection.cpp sends the rest elsewhere.
    // The rows follow the title, so a device that has not raised its access point yet
    // shows the state and nothing it cannot honour.
    // Milestone 3.8 adds two more phases to this scene. The rows follow the same presence
    // rule throughout: what is shown is what the device can honour right now.
    //
    //   SetupActive              the network, the code, the password, the address
    //   ValidatingConfiguration  the same, while the submitted network is being tried —
    //                            the setup network is still up and still joinable, so
    //                            removing it here would strand a browser mid-flow
    //   SetupFailed              the same again (owner ruling D-6: recoverable, the form
    //                            stays available and another submission is accepted)
    //   SetupRequired            the title alone
    // ⬅️ RECOVERY FIRST, AND IT RETURNS — Milestone 3.10, owner Decision 3 and ruling B. A device
    // whose stored configuration cannot be used enters this automatically; it has no session and no
    // access point, so every row and the code stay absent and the one thing drawn beneath the title
    // is §8's Status Message.
    //
    // ⛔ THE INSTRUCTION NAMES ONLY WHAT THIS FIRMWARE DOES. Holding the button really does start
    // setup from this checkpoint onward (owner ruling 1). It promises no repair, no Settings and no
    // recovery action that does not exist, and it says that nothing was erased — which is true:
    // §14.1's "do not automatically erase configuration" and ruling R10 both hold on this path.
    // ⛔ §12.1's DEVICE FAULT FIRST — checkpoint 3c, and it outranks the recovery branch below for
    // the same reason UIAD 001 ranks the tiers that way: a local fault that stops the device
    // participating at all is not a configuration problem, and it is not something a lower tier may
    // draw over. The condition is read from the TYPED device-fault reason, never from
    // NetworkState::last_error's words and never from the recovery state.
    //
    // ⬅️ IT SHARES RECOVERY'S DRAWING AND NOTHING ELSE. Production UI §8 lists a Status Message for
    // both Unavailable and Failure, §9 specifies no screen for either condition, and a second
    // one-title-one-message composition would be the redesign owner ruling B forbids. What is NOT
    // shared is the meaning: the words say the Wi-Fi is unavailable and the display must be
    // restarted, and they promise no repair the firmware cannot perform.
    // A live download/install failure uses the exact same owner-approved update statement as the
    // pending-image validation fault. Reuse this scene's already-built shared form rather than
    // duplicating its nine LVGL objects in FirmwareUpdateScene: the production pool is 64 KiB.
    if (presentation.update_notice) {
        draw_statement(text::kUpdateAvailableTitle, text::kUpdateHoldForSettings,
                       text::MessageTone::Warning, "the firmware-update availability text");
        return;
    }

    if (presentation.ota_phase == app_model::OtaPhase::Failed) {
        draw_statement(text::kOtaValidationFailureTitle, text::kOtaValidationFailureMessage,
                       text::MessageTone::Error, "the firmware-update failure text");
        return;
    }

    if (presentation.device_fault == app_model::DeviceFaultReason::WifiUnavailable) {
        draw_statement(text::kWifiUnavailableTitle, text::kWifiUnavailableMessage,
                       text::MessageTone::Error, "the wi-fi device-fault text");
        return;
    }

    // ⛔ AUDIT ITEM 5'S TWO FAULTS, AND THEY ARE TWO BECAUSE THE SENTENCES DIFFER. A reset that was
    // proved to have been authorised and could not finish can truthfully say so. A store that would
    // not answer cannot: on that route the journal was never read, so no reset is proved to exist
    // and "Reset could not finish" would assert an operation the user may never have requested.
    //
    // ⬅️ NEITHER MAKES A PERSISTENCE CLAIM. The recovery branch below says nothing has been erased,
    // which is true on ITS route and false on the first of these — records really are gone once the
    // removals began. That is why these return before it rather than falling through.
    if (presentation.device_fault == app_model::DeviceFaultReason::FactoryResetIncomplete) {
        draw_statement(text::kFactoryResetIncompleteTitle, text::kFactoryResetIncompleteMessage,
                       text::MessageTone::Error, "the incomplete-reset device-fault text");
        return;
    }

    if (presentation.device_fault == app_model::DeviceFaultReason::StorageFailure) {
        draw_statement(text::kStorageUnavailableTitle, text::kStorageUnavailableMessage,
                       text::MessageTone::Error, "the storage device-fault text");
        return;
    }

    // ⬅️ THE PENDING IMAGE DID NOT VALIDATE — 2026-09-12, and it is a fourth cause rather than a
    // fourth screen. It reaches this scene for the same reason the three above it do: UIAD 001
    // §12.1 ranks a device fault first, the scene keys on the TYPED reason, and Production UI §9
    // specifies no screen of its own for it.
    //
    // ⛔ AND THE SENTENCE PROMISES NOTHING ABOUT THE OLD IMAGE. Rolling back to it is intended
    // bootloader behaviour for a pending image, not a result this firmware has observed, so the
    // copy names only the action that works. The first-boot health and rollback logic is not
    // touched by this: what changed is that the fault it can raise is now visible.
    if (presentation.device_fault == app_model::DeviceFaultReason::OtaValidationFailure) {
        draw_statement(text::kOtaValidationFailureTitle, text::kOtaValidationFailureMessage,
                       text::MessageTone::Error, "the update-failed device-fault text");
        return;
    }

    // ⬅️ RECOVERY SECOND, AND IT RETURNS — Milestone 3.10, owner Decision 3 and ruling B. A device
    // whose stored configuration cannot be used enters this automatically; it has no session and no
    // access point, so every row and the code stay absent and the one thing drawn beneath the title
    // is §8's Status Message.
    //
    // ⛔ THE INSTRUCTION NAMES ONLY WHAT THIS FIRMWARE DOES. Holding the button really does start
    // setup from this checkpoint onward (owner ruling 1). It promises no repair, no Settings and no
    // recovery action that does not exist, and it says that nothing was erased — which is true:
    // §14.1's "do not automatically erase configuration" and ruling R10 both hold on this path.
    if (presentation.device_operation == app_model::DeviceOperation::RecoveryMode) {
        draw_statement(text::kRecoveryTitle, text::kRecoveryMessage, text::MessageTone::Warning,
                       "the recovery text");
        return;
    }

    // Not a statement: the message composition goes away and the session composition comes back,
    // before anything else is drawn. The two can never share the canvas — this one covers it.
    //
    // ⬅️ AND NOTHING IS RESTORED HERE ANY MORE. Until 2026-09-12 this had to put `title_` back to
    // Body at the column measure, because a statement had promoted and widened it in place. The
    // statement has its own objects now, so the session composition's geometry is never disturbed
    // and there is nothing to undo — which is also why a revert cannot silently reintroduce the
    // wrap this scene's comments spend so long measuring.
    if (statement_) {
        statement_ = false;
        form_.show(false);
        show_session();

        // ⛔ AND COMING BACK TO THE SESSION IS AN ENTRY. A fault clearing, or a withdrawn reset
        // question, puts the setup composition on a panel that was showing something else
        // entirely; it used to reappear fully drawn in one frame. Owner ruling R3 freezes its
        // GEOMETRY, and none of it moves — what fades is each object's own opacity, and the
        // settled pixels are identical.
#ifndef PRD_OLD_SETUP_STATIC
        if (!running_) {
            clock_.arm();
            running_ = true;
            level_ = 0;
        }
#endif

        paint(level_ < 0 ? 0 : level_);
    } else {
        show_session();
    }

    const app_model::ProvisioningPhase phase = presentation.provisioning;
    const bool previously_offering_network =
        phase_seen_ && (phase_ == app_model::ProvisioningPhase::SetupActive ||
                        phase_ == app_model::ProvisioningPhase::ValidatingConfiguration ||
                        phase_ == app_model::ProvisioningPhase::SetupFailed);

    // These are changes INSIDE the same scene, so the controller's scene-entry clock does not
    // see them. The new status is written now, not after the animation; only its luminance
    // resolves. In particular the network/password/address and QR stay put and readable while
    // a submitted configuration is checked or a failure returns to the form.
#ifndef PRD_OLD_SETUP_PHASE_STATIC
    if (phase_seen_ && phase != phase_ && !running_) {
        phase_from_level_ = phase_running_ ? phase_level_ : theme::opacity(theme::Opacity::Medium);
        phase_level_ = phase_from_level_;
        phase_clock_.arm();
        phase_running_ = true;
    }
#endif
    phase_ = phase;
    phase_seen_ = true;

    const bool offering_network = phase == app_model::ProvisioningPhase::SetupActive ||
                                  phase == app_model::ProvisioningPhase::ValidatingConfiguration ||
                                  phase == app_model::ProvisioningPhase::SetupFailed;
    offering_network_ = offering_network;

    text::TextId title = text::kSetupRequiredTitle;
    if (phase == app_model::ProvisioningPhase::ValidatingConfiguration) {
        title = text::TextId::StateValidatingConfiguration;
    } else if (phase == app_model::ProvisioningPhase::SetupFailed) {
        title = text::TextId::StateSetupFailed;
    } else if (phase == app_model::ProvisioningPhase::SetupActive) {
        title = text::kSetupActiveTitle;
    }

    set_label(title_, text::lookup(title));

    if (phase_running_) {
        paint(level_ < 0 ? theme::opacity(theme::Opacity::Full) : level_);
    }

    const bool active = offering_network;

    // All three active phases expose the same session. A phase-only title change must not
    // regenerate an identical password label or QR matrix: LVGL treats even an equal-value QR
    // update as an invalidation, and §11 permits only the changing title to be redrawn.
    if (active && previously_offering_network && has_session_ && was_drawn_) {
        return;
    }

    if (!active || !has_session_) {
        session_running_ = false;
        session_clock_.clear();
        set_row(RowIndex::Network, {});
        set_row(RowIndex::Password, {});
        set_row(RowIndex::Address, {});
        apply_qr();
        return;
    }

    set_row(RowIndex::Network, session_.ssid.view());
    set_row(RowIndex::Password, session_.password.view());

    // Empty until Milestone 3.7's server is listening, so this hides the row until then.
    set_row(RowIndex::Address, session_.url.view());

    // ⬅️ THE CODE AND THE TEXT ARE DRAWN FROM THE SAME SESSION, in the same call. They
    // cannot disagree about which network they describe, which is the failure that would
    // make the QR worse than useless: a code that joins something the panel does not name.
    apply_qr();
    if (!previously_offering_network) {
        start_session_reveal();
        paint(level_ < 0 ? theme::opacity(theme::Opacity::Full) : level_);
    }
}

void ProvisioningScene::apply_session(const SetupSession& session) {
    // The one-slot renderer mailbox can coalesce a clear followed immediately by a new
    // session. A different network credential must therefore take the same retirement path
    // as an explicit clear: scrub the old LVGL label and QR canvas before drawing the new
    // session, and give the replacement its own visible entry. An identical resubmission
    // must not redraw a QR or restart motion on an otherwise idle panel.
    if (has_session_ && session_.ssid == session.ssid && session_.password == session.password &&
        session_.url == session.url) {
#ifndef PRD_OLD_SETUP_SESSION_RESUBMIT
        return;
#endif
    }
#ifndef PRD_OLD_SETUP_SESSION_REPLACE
    if (has_session_ && (session_.ssid != session.ssid || session_.password != session.password)) {
        clear_session();
    }
#endif
    const bool had_session = has_session_;
    session_ = session;
    has_session_ = true;

    // The session and phase arrive on separate queues. Retain the session immediately, but
    // populate LVGL only while an offering phase owns the panel; either order then produces
    // the same visible result without drawing a credential beneath SetupRequired.
    if (title_ == nullptr) {
        return;
    }

    // ⛔ UNLESS A STATEMENT IS WHAT THE PANEL IS SHOWING — root finding R3, 2026-09-12. The setup
    // access point stays up under a device fault (the DriverStalled ruling keeps it alive so a
    // browser mid-flow is not stranded), so a session can arrive, change or be re-submitted while
    // a full-canvas statement is drawn. Writing the rows then put the network name and the
    // PASSWORD back on top of it. The session is still retained — the return path below redraws it
    // from session_ — but nothing of it reaches the panel while it is not what the panel is about.
#ifndef PRD_OLD_SESSION_ROWS
    if (!session_drawable_) {
        return;
    }
#endif
#ifndef PRD_OLD_SETUP_EARLY_SESSION
    if (!offering_network_) {
        return;
    }
#endif

    set_row(RowIndex::Network, session_.ssid.view());
    set_row(RowIndex::Password, session_.password.view());
    set_row(RowIndex::Address, session_.url.view());
    apply_qr();
    if (!had_session) {
        start_session_reveal();
        paint(level_ < 0 ? theme::opacity(theme::Opacity::Full) : level_);
    }
}

void ProvisioningScene::clear_session() {
    session_running_ = false;
    session_clock_.clear();
    // The password row's borrowed buffer is wiped by set_row() below, before the old
    // session is replaced. LVGL's heap-backed text setter cannot provide that guarantee:
    // it frees the old allocation before creating a new one, regardless of length.

    // Assigning a value-initialised session overwrites the whole of every BoundedString's
    // storage, not just its length — the copy assignment copies the entire buffer. That is
    // the difference between a secret that is gone and one that is merely unaddressed, and
    // it is the same property setup_ap::Session::clear() has on the network side.
    session_ = SetupSession{};
    has_session_ = false;

    set_row(RowIndex::Network, {});
    set_row(RowIndex::Password, {});
    set_row(RowIndex::Address, {});

    // Clears the canvas as well as hiding it — see apply_qr(). A drawn code outlives the
    // session it belongs to otherwise, and it is as readable as the password was.
    apply_qr();
}

void ProvisioningScene::set_visible(const bool visible) {
    if (root_ == nullptr) {
        return;
    }

    if (visible) {
        lv_obj_remove_flag(root_, LV_OBJ_FLAG_HIDDEN);
#ifndef PRD_OLD_SETUP_STATIC
        clock_.arm();
        running_ = true;
        level_ = 0;
        paint(0);
#endif
        return;
    }

    lv_obj_add_flag(root_, LV_OBJ_FLAG_HIDDEN);

    // ⬅️ LEAVING FORGETS THE STATEMENT, WHICH IS WHAT MAKES RE-ENTRY AN ENTRY. The scene is
    // hidden and shown around a change of scene, and a fault that comes back after the panel
    // showed something else is a new appearance, not a continuation of the old one. apply() then
    // arms a fresh clock rather than finding statement_ already true and drawing at full level.
    statement_ = false;
    running_ = false;
    level_ = -1;
    clock_.clear();
    phase_seen_ = false;
    phase_running_ = false;
    phase_clock_.clear();
    session_running_ = false;
    session_clock_.clear();
    offering_network_ = false;
    form_.show(false);

    // ⛔ RESTORE THE SESSION OBJECTS ON THE WAY OUT, NOT ON THE WAY BACK IN. The scene is hidden
    // and shown around every scene change, and a session can arrive through its own queue while
    // hidden. The offering gate stays closed until the next presentation selects the phase, so
    // no retained credential is populated in a hidden SetupRequired composition.
    show_session();
}

// ⛔ PER-OBJECT OPACITY, NOT A SUBTREE ONE. lv_obj_set_style_opa() on the scene root would make
// LVGL composite the whole canvas through an intermediate layer — at 320 x 240 in RGB565 that is
// 150 kB against a 64 KiB pool, so it is not a budget question but an impossible one. Every object
// here is faded by the style its own draw already honours: text_opa on the labels, bg_opa on the
// quiet-zone panel, image_opa on the code itself. All three are applied during that object's own
// draw rather than through a composited layer, which the render suite measures rather than
// assumes — it compares lv_mem's high-water mark across the fade and requires it not to rise.
//
// ⬅️ THAT IS A HIGH-WATER MEASUREMENT, NOT AN ALLOCATION TRACE. It proves the fade never needed
// more pool than drawing the same composition already did — which is what rules out a layer for
// this block, since one would be tens of kilobytes against a 64 KiB pool. It does not claim zero
// allocation calls, and nothing here depends on that stronger property.
//
// ⬅️ AND THE ENDPOINT IS UNTOUCHED BY CONSTRUCTION. At full level every one of them is the value
// it was built with, so the settled composition is the same pixels owner ruling R3 froze. The
// render suite asserts that too, byte for byte, rather than trusting the arithmetic.
void ProvisioningScene::paint_session(const std::int32_t level) {
    const auto session_level = session_running_ ? std::min(level, session_level_) : level;
    const auto alpha = static_cast<lv_opa_t>(session_level < 0 ? 0 : session_level);

    const auto title_level = phase_running_ ? std::min(level, phase_level_) : level;
    theme::apply_text_level(title_, theme::Opacity::Full, title_level);

    for (auto& current : rows_) {
        theme::apply_text_level(current.caption, theme::Opacity::Full, session_level);
        theme::apply_text_level(current.value, theme::Opacity::Full, session_level);
    }

    if (qr_panel_ != nullptr) {
#ifdef PRD_OLD_SETUP_QR_REDRAW
        lv_obj_set_style_bg_opa(qr_panel_, alpha, LV_PART_MAIN);
#else
        if (lv_obj_get_style_bg_opa(qr_panel_, LV_PART_MAIN) != alpha) {
            lv_obj_set_style_bg_opa(qr_panel_, alpha, LV_PART_MAIN);
        }
#endif
    }

    if (qr_ != nullptr) {
#ifdef PRD_OLD_SETUP_QR_REDRAW
        lv_obj_set_style_image_opa(qr_, alpha, LV_PART_MAIN);
#else
        if (lv_obj_get_style_image_opa(qr_, LV_PART_MAIN) != alpha) {
            lv_obj_set_style_image_opa(qr_, alpha, LV_PART_MAIN);
        }
#endif
    }
}

void ProvisioningScene::start_session_reveal() {
#ifndef PRD_OLD_SETUP_SESSION_REVEAL
    session_clock_.arm();
    session_running_ = true;
    session_level_ = 0;
#endif
}

void ProvisioningScene::paint(const std::int32_t level) {
    level_ = level;

    if (statement_) {
        form_.paint(level);
        return;
    }

    paint_session(level);
}

bool ProvisioningScene::step(const bool panel_visible, const std::uint32_t now) {
    if (!running_ && !phase_running_ && !session_running_) {
        return false;
    }

    // Until the panel can be seen the clock does not advance and the composition is held at its
    // first frame, so the first VISIBLE frame is the first frame of the motion rather than
    // whatever the ramp reached behind a dark backlight.
    if (running_) {
        clock_.advance(panel_visible, now);

        const auto elapsed = clock_.elapsed();
        if (panel_visible && elapsed >= theme::kOperationalMotionMs) {
            running_ = false;
            level_ = theme::opacity(theme::Opacity::Full);
        } else {
            level_ = static_cast<std::int32_t>(
                std::lround(entry_progress(elapsed) * theme::opacity(theme::Opacity::Full)));
        }
    }

    if (phase_running_) {
        phase_clock_.advance(panel_visible, now);
        const auto elapsed = phase_clock_.elapsed();
        if (panel_visible && elapsed >= theme::kOperationalMotionMs) {
            phase_running_ = false;
            phase_level_ = theme::opacity(theme::Opacity::Full);
        } else {
            const auto span = theme::opacity(theme::Opacity::Full) - phase_from_level_;
            phase_level_ = phase_from_level_ +
                           static_cast<std::int32_t>(std::lround(entry_progress(elapsed) * span));
        }
    }

    if (session_running_) {
        session_clock_.advance(panel_visible, now);
        const auto elapsed = session_clock_.elapsed();
        if (panel_visible && elapsed >= theme::kOperationalMotionMs) {
            session_running_ = false;
            session_level_ = theme::opacity(theme::Opacity::Full);
        } else {
            session_level_ = static_cast<std::int32_t>(
                std::lround(entry_progress(elapsed) * theme::opacity(theme::Opacity::Full)));
        }
    }

    paint(level_ < 0 ? theme::opacity(theme::Opacity::Full) : level_);
    return running_ || phase_running_ || session_running_;
}

} // namespace prd::ui::scene
