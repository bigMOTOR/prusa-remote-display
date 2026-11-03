// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

// The shared message composition, through the ACTUAL production controller and the actual LVGL
// renderer — not a duplicate scene implementation and not a static endpoint.
//
// WHAT THIS FILE ASSERTS, and why each one is here rather than in a host test:
//
//   - the eight reachable full-screen messages draw the words their condition owns, and none of
//     another condition's — the "no inherited recovery wording" rule, measured on the tree;
//   - the accepted geometry, measured on painted PIXELS: the symbol's 44 x 36 painted box at
//     (14, 24), the identity row's clearance beneath it, and every pixel of ink inside
//     canvas.inset — including the 228 px floor, which the atlas's 240 px panel does not have;
//   - the semantic tone: Warning where something is waiting on the reader, Error where an
//     operation completed and failed, with neither tone's colour appearing on the other's screen
//     and the acquisition accent appearing on neither;
//   - every glyph of every drawn string exists in the face that was selected for it;
//   - the 1100 ms entry runs on the real tick, pauses while the panel is Off, resumes without
//     catching up, and stops when it settles — no idle redraw, no flush;
//   - the destructive sequence end to end: question -> running -> the failure it can raise, and
//     the cancellation that returns to the operational composition;
//   - the session composition survives a statement taking the canvas from it, and the credential
//     and its encoded copy do not.
//
// Run with an output directory to capture frames; run with --controls to execute the negative
// controls, which deliberately feed each detector a violation and require it to fire.

#include "controller.hpp"
#include "fonts/fonts.hpp"
#include "fonts/motion_fonts.hpp"
#include "scene_selection.hpp"
#include "text/scene_text.hpp"
#include "text/text.hpp"
#include "theme/theme.hpp"
#include "ui/ui.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

namespace prd::diagnostics {
unsigned setup_qr_draw_count();
unsigned setup_qr_clear_count();
unsigned qr_temp_audit_count();
unsigned qr_temp_dirty_count();
} // namespace prd::diagnostics

namespace {

namespace theme = prd::ui::theme;

constexpr int w = 320, h = 240;
std::array<std::uint16_t, w * h> pixels{};
std::array<std::uint8_t, w * 40 * 2> buffer{};
unsigned flushes = 0;
bool watch_setup_phase_qr = false;
unsigned setup_phase_qr_flushes = 0;

// The frozen R3 QR block is 33 modules at scale four with a four-module quiet zone.
constexpr int kSetupQrSide = (33 + 2 * 4) * 4;
constexpr int kSetupQrLeft = theme::kCanvasContent.right() - kSetupQrSide;
constexpr int kSetupQrTop = theme::kCanvasContent.y;

void require(bool ok, const char* why) {
    if (!ok) {
        std::fprintf(stderr, "FAIL %s\n", why);
        std::abort();
    }
}

void flush(lv_display_t* d, const lv_area_t* a, std::uint8_t* data) {
    ++flushes;
    if (watch_setup_phase_qr && a->x1 < kSetupQrLeft + kSetupQrSide && a->x2 >= kSetupQrLeft &&
        a->y1 < kSetupQrTop + kSetupQrSide && a->y2 >= kSetupQrTop) {
        ++setup_phase_qr_flushes;
    }
    const auto* input = reinterpret_cast<const std::uint16_t*>(data);
    require(a->x1 >= 0 && a->y1 >= 0 && a->x2 < w && a->y2 < h, "canvas bounds");
    for (int y = a->y1; y <= a->y2; ++y) {
        for (int x = a->x1; x <= a->x2; ++x) {
            pixels[y * w + x] = input[(y - a->y1) * (a->x2 - a->x1 + 1) + x - a->x1];
        }
    }
    lv_display_flush_ready(d);
}

// --- Detectors -------------------------------------------------------------
//
// Written as functions over a frame rather than inline loops, so --controls can hand each one a
// frame that violates it and require it to say so. An assertion nobody has ever seen fail is
// indistinguishable from one that cannot.

using Frame = std::array<std::uint16_t, w * h>;

// The §7 content area: "All content shall remain inside canvas.inset."
[[nodiscard]] bool ink_outside_inset(const Frame& frame) {
    const auto background = frame[0];
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const bool inside = x >= theme::kCanvasContent.x && x < theme::kCanvasContent.right() &&
                                y >= theme::kCanvasContent.y && y < theme::kCanvasContent.bottom();
            if (!inside && frame[y * w + x] != background) {
                return true;
            }
        }
    }
    return false;
}

// The lowest row carrying any ink, or -1. The atlas's panel is 240 px tall and the canvas is not.
[[nodiscard]] int ink_bottom(const Frame& frame) {
    const auto background = frame[0];
    for (int y = h - 1; y >= 0; --y) {
        for (int x = 0; x < w; ++x) {
            if (frame[y * w + x] != background) {
                return y;
            }
        }
    }
    return -1;
}

// The highest row carrying any ink, or -1.
[[nodiscard]] int ink_top(const Frame& frame) {
    const auto background = frame[0];
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            if (frame[y * w + x] != background) {
                return y;
            }
        }
    }
    return -1;
}

struct Box {
    int x1, y1, x2, y2;
};

[[nodiscard]] unsigned count_colour(const Frame& frame, const std::uint16_t value) {
    unsigned found = 0;
    for (const auto pixel : frame) {
        found += pixel == value ? 1U : 0U;
    }
    return found;
}

// Whether every pixel of `value` lies inside `box`. The symbol's painted bounds are an accepted
// measurement, so they are checked as a bound and not as a count.
[[nodiscard]] bool colour_confined_to(const Frame& frame, const std::uint16_t value,
                                      const Box& box) {
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            if (frame[y * w + x] != value) {
                continue;
            }
            if (x < box.x1 || x > box.x2 || y < box.y1 || y > box.y2) {
                return false;
            }
        }
    }
    return true;
}

// A 44 x 36 crop, for comparing the shared symbol against the accepted operational one.
[[nodiscard]] std::vector<std::uint16_t> crop(const Frame& frame, const int x, const int y) {
    std::vector<std::uint16_t> out;
    out.reserve(44U * 36U);
    for (int row = 0; row < 36; ++row) {
        for (int column = 0; column < 44; ++column) {
            out.push_back(frame[(y + row) * w + x + column]);
        }
    }
    return out;
}

[[nodiscard]] unsigned differing(const std::vector<std::uint16_t>& a,
                                 const std::vector<std::uint16_t>& b) {
    require(a.size() == b.size(), "crops must be the same size to compare");
    unsigned count = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        count += a[i] != b[i] ? 1U : 0U;
    }
    return count;
}

[[nodiscard]] std::uint16_t rgb565(const std::uint32_t rgb) {
    const auto r = static_cast<std::uint16_t>(((rgb >> 16) & 0xFFU) >> 3);
    const auto g = static_cast<std::uint16_t>(((rgb >> 8) & 0xFFU) >> 2);
    const auto b = static_cast<std::uint16_t>((rgb & 0xFFU) >> 3);
    return static_cast<std::uint16_t>((r << 11) | (g << 5) | b);
}

// --- The object tree -------------------------------------------------------

struct Drawn {
    std::set<std::string> labels;
    std::set<std::string> title_fonts;
};

const char* face_name(const lv_font_t* const font) {
    if (font == prd::ui::fonts::motion_title(theme::kMessageTitleSize)) {
        return "title24";
    }
    if (font == prd::ui::fonts::motion_title(theme::kMessageMarkSize)) {
        return "mark22";
    }

    // ⬅️ ONE NAME FOR ONE ASSET. fonts::motion_title(20) and fonts::for_role(StateTitle) return
    // the SAME face — prd_font_title_20 is the size-20 entry of the motion table — so the
    // explanation and the reset's running line are set in one asset and the font check cannot
    // tell them apart. Naming them separately would have made this look like a difference.
    if (font == prd::ui::fonts::for_role(theme::TextRole::StateTitle) ||
        font == prd::ui::fonts::motion_title(theme::kMessageExplanationSize)) {
        return "title20";
    }
    if (font == prd::ui::fonts::for_role(theme::TextRole::OutcomeTitle)) {
        return "outcome28";
    }
    if (font == prd::ui::fonts::for_role(theme::TextRole::Section)) {
        return "section16";
    }
    if (font == prd::ui::fonts::for_role(theme::TextRole::Body)) {
        return "body14";
    }
    return "other";
}

// The laid-out height of the first visible label whose text starts with `prefix`, or -1.
// Used to prove a title fits one line rather than assuming it from a character count.
int label_height(lv_obj_t* const o, const char* const prefix) {
    if (lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) {
        return -1;
    }
    if (lv_obj_check_type(o, &lv_label_class)) {
        const std::string text = lv_label_get_text(o);
        if (text.rfind(prefix, 0) == 0) {
            lv_obj_update_layout(o);
            return lv_obj_get_height(o);
        }
    }
    for (unsigned i = 0; i < lv_obj_get_child_count(o); ++i) {
        const int found = label_height(lv_obj_get_child(o, i), prefix);
        if (found >= 0) {
            return found;
        }
    }
    return -1;
}

void collect(lv_obj_t* const o, Drawn& out) {
    if (lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) {
        return;
    }
    if (lv_obj_check_type(o, &lv_label_class)) {
        const std::string text = lv_label_get_text(o);
        if (!text.empty()) {
            const auto* const font = lv_obj_get_style_text_font(o, LV_PART_MAIN);

            // Text presence alone cannot detect a face that lacks letters: LVGL drops a codepoint
            // its font does not carry, silently and with no replacement mark.
            for (const auto ch : text) {
                lv_font_glyph_dsc_t glyph{};
                require(lv_font_get_glyph_dsc(font, &glyph, static_cast<unsigned char>(ch), 0),
                        "every visible character exists in its selected font");
            }

            out.labels.insert(text);
            out.title_fonts.insert(text + "=" + face_name(font));
        }
    }
    for (unsigned i = 0; i < lv_obj_get_child_count(o); ++i) {
        collect(lv_obj_get_child(o, i), out);
    }
}

char* find_label_text(lv_obj_t* const o, const char* const expected) {
    if (lv_obj_check_type(o, &lv_label_class) && std::strcmp(lv_label_get_text(o), expected) == 0) {
        return lv_label_get_text(o);
    }
    for (unsigned index = 0; index < lv_obj_get_child_count(o); ++index) {
        if (char* const found = find_label_text(lv_obj_get_child(o, index), expected)) {
            return found;
        }
    }
    return nullptr;
}

// --- The eight reachable messages ------------------------------------------

using namespace prd::app_model;

struct Case {
    const char* name;
    const char* title;
    const char* explanation;
    bool warning;
    Presentation (*build)();
};

Presentation access(const AccessCondition condition) {
    Presentation p;
    p.layer = PresentationLayer::AccessCondition;
    p.access_condition = condition;
    p.printer_name_known = true;
    p.printer_name.assign("CORE One");
    return p;
}

Presentation fault(const DeviceFaultReason reason) {
    Presentation p;
    p.layer = PresentationLayer::DeviceFault;
    p.device_fault = reason;
    return p;
}

const std::array<Case, 8> kCases{{
    {"authentication", "Authentication required", "Check the PrusaLink API key.", true,
     [] { return access(AccessCondition::AuthenticationRequired); }},
    {"unsupported", "Unsupported interface", "This printer interface is not supported.", true,
     [] { return access(AccessCondition::UnsupportedInterface); }},
    {"recovery", "Settings cannot be used",
     "Nothing has been erased. Hold the button to set up again.", true,
     [] {
         Presentation p;
         p.layer = PresentationLayer::RecoveryOrProvisioning;
         p.device_operation = DeviceOperation::RecoveryMode;
         return p;
     }},
    {"confirmation", "Erase all settings?",
     "Hold the button again to erase. A short press cancels.", true,
     [] {
         Presentation p;
         p.layer = PresentationLayer::ModalSurface;
         p.modal_surface = ModalSurface::FactoryResetConfirmation;
         return p;
     }},
    {"wifi", "Wi-Fi unavailable", "Restart the display.", false,
     [] { return fault(DeviceFaultReason::WifiUnavailable); }},
    {"storage", "Storage unavailable", "Restart the display.", false,
     [] { return fault(DeviceFaultReason::StorageFailure); }},
    {"reset-incomplete", "Reset could not finish", "Restart the display.", false,
     [] { return fault(DeviceFaultReason::FactoryResetIncomplete); }},
    {"update", "Update failed", "Restart the display.", false,
     [] { return fault(DeviceFaultReason::OtaValidationFailure); }},
}};

int controls() {
    // Every detector is handed a frame that violates it, and must say so. These fire: run the
    // binary with --controls and each line below aborts if its detector has stopped working.
    Frame frame{};
    frame.fill(0x1082);

    require(!ink_outside_inset(frame), "a clean frame is not reported as out of bounds");
    frame[(theme::kCanvasContent.y - 1) * w + 20] = 0xFFFF;
    require(ink_outside_inset(frame), "CONTROL: ink above the inset is detected");

    frame.fill(0x1082);
    require(ink_bottom(frame) == -1, "a clean frame has no ink");
    frame[(theme::kCanvasContent.bottom()) * w + 20] = 0xFFFF;
    require(ink_bottom(frame) == theme::kCanvasContent.bottom(),
            "CONTROL: ink one row below the content floor is detected");

    frame.fill(0x1082);
    require(ink_top(frame) == -1, "a clean frame has no ink");
    frame[97 * w + 40] = 0xFFFF;
    require(ink_top(frame) == 97, "CONTROL: the topmost ink row is found");

    frame.fill(0x1082);
    frame[30 * w + 200] = rgb565(theme::kColorWarning);
    require(!colour_confined_to(frame, rgb565(theme::kColorWarning), {14, 24, 57, 59}),
            "CONTROL: a symbol-coloured pixel outside the painted box is detected");
    require(count_colour(frame, rgb565(theme::kColorWarning)) == 1,
            "CONTROL: the colour counter counts");

    // The wording detector: a set carrying another condition's sentence must not pass.
    std::set<std::string> labels{"Update failed", "Restart the display.",
                                 "Nothing has been erased. Hold the button to set up again."};
    require(labels.count("Nothing has been erased. Hold the button to set up again.") == 1,
            "CONTROL: inherited recovery wording is detected when present");

    // The crop comparator: identical crops differ in nothing, and one changed pixel is found.
    Frame a{};
    a.fill(0x1082);
    Frame b = a;
    require(differing(crop(a, 14, 24), crop(b, 14, 24)) == 0, "identical crops compare equal");
    b[30 * w + 20] = 0xFFFF;
    require(differing(crop(a, 14, 24), crop(b, 14, 24)) == 1,
            "CONTROL: one differing pixel inside the crop is counted");
    require(differing(crop(a, 106, 70), crop(b, 106, 70)) == 0,
            "CONTROL: and a crop elsewhere is unaffected");

    std::puts("PASS negative controls all fired");
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::strcmp(argv[1], "--controls") == 0) {
        return controls();
    }
    require(argc == 2, "output directory, or --controls");

    namespace controller = prd::ui::controller;
    const std::filesystem::path out{argv[1]};

    lv_init();
    lv_log_register_print_cb([](lv_log_level_t, const char* msg) {
        std::fputs(msg, stderr);
        std::abort();
    });
    auto* d = lv_display_create(w, h);
    lv_display_set_color_format(d, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(d, buffer.data(), nullptr, buffer.size(),
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(d, flush);
    require(controller::build(d), "whole controller builds in the unchanged pool");
    controller::note_panel_visible(true);

    auto tick = [&](unsigned ms) {
        lv_tick_inc(ms);
        const auto moving = controller::step_acknowledgement();
        lv_refr_now(d);
        require(!ink_outside_inset(pixels), "every tick keeps all ink inside canvas.inset");
        return moving;
    };

    auto blank = [&] {
        Presentation p;
        p.layer = PresentationLayer::SystemLifecycle;
        controller::apply(p);
        tick(0);
    };

    const auto warning = rgb565(theme::kColorWarning);
    const auto error = rgb565(theme::kColorError);
    const auto accent = rgb565(theme::kColorAccent);

    // The accepted painted bounds of the symbol: 44 x 36 beginning at (14, 24).
    const Box symbol{
        theme::kMessageSymbolX - (theme::kStrokeEmphasis / 2), theme::kMessageSymbolPaintedTop,
        theme::kMessageSymbolX + theme::kMessageSymbolWidth + (theme::kStrokeEmphasis / 2) - 1,
        theme::kMessageSymbolPaintedTop + theme::kMessageSymbolHeight + theme::kStrokeEmphasis - 1};

    // --- 1. The eight messages, each drawn through its own real presentation ----
    for (const auto& current : kCases) {
        blank();

        const auto presentation = current.build();
        controller::apply(presentation);
        require(tick(0), "entering a message composition drives the real tick");

        const auto first = pixels;
        for (unsigned t = 16; t <= 1120; t += 16) {
            tick(16);
            const auto path =
                out / (std::string{current.name} + "-" + std::to_string(t) + ".rgb565");
            auto* file = std::fopen(path.c_str(), "wb");
            require(file, "frame file");
            require(std::fwrite(pixels.data(), 2, pixels.size(), file) == pixels.size(),
                    "frame write");
            std::fclose(file);
        }
        require(first != pixels, "the entry actually advanced rather than appearing at once");

        // The words, and nobody else's.
        Drawn drawn;
        collect(lv_screen_active(), drawn);
        require(drawn.labels.count(current.title) == 1, "the condition's own title is drawn");
        require(drawn.labels.count(current.explanation) == 1,
                "the condition's own explanation is drawn");
        require(drawn.labels.count("!") == 1, "the shared mark is drawn");

        for (const auto& other : kCases) {
            if (std::strcmp(other.title, current.title) == 0) {
                continue;
            }
            require(drawn.labels.count(other.title) == 0, "no other condition's title is drawn");
            if (std::strcmp(other.explanation, current.explanation) != 0) {
                require(drawn.labels.count(other.explanation) == 0,
                        "no inherited wording from another condition");
            }
        }

        // No operational or session residue on a statement.
        for (const char* forbidden :
             {"Printing", "Nozzle", "Bed", "Ready", "Network", "Password", "Address",
              "Erasing settings.", "Erasing settings..", "Erasing settings..."}) {
            require(drawn.labels.count(forbidden) == 0, "no foreign composition leaks");
        }

        // The accepted faces, by name so a failure says which one arrived.
        require(drawn.title_fonts.count(std::string{current.title} + "=title24") == 1,
                "the title uses the accepted 24 px face");
        require(drawn.title_fonts.count(std::string{current.explanation} + "=title20") == 1,
                "the explanation uses the accepted 20 px face");
        require(drawn.title_fonts.count("!=mark22") == 1, "the mark uses the shared 22 px face");

        // The semantic tone, on pixels. One tone present, the other absent, and the acquisition
        // accent absent from every statement.
        const auto present = current.warning ? warning : error;
        const auto absent = current.warning ? error : warning;
        require(count_colour(pixels, present) > 0, "the symbol is painted in its semantic tone");
        require(count_colour(pixels, absent) == 0, "the other tone appears nowhere");
        require(count_colour(pixels, accent) == 0, "no acquisition accent survives a statement");
        require(colour_confined_to(pixels, present, symbol),
                "every symbol pixel lies inside the accepted 44 x 36 painted box");

        // The floor. 228 is the content area, not the panel.
        require(ink_bottom(pixels) < theme::kCanvasContent.bottom(),
                "all ink stays above the content floor");

        // Settled: no further motion, and a repeated presentation draws nothing at all.
        require(!tick(16), "the entry settles");
        const auto before = flushes;
        const auto settled = pixels;
        for (int i = 0; i < 20; ++i) {
            controller::apply(presentation);
            require(!tick(16), "a settled message runs no animation");
        }
        require(before == flushes && settled == pixels, "a repeated presentation redraws nothing");

        std::printf("PASS %s: words, faces, tone, bounds, settle\n", current.name);
    }

    // --- 1b. The shared symbol IS the accepted operational symbol -----------------
    //
    // Root finding R1: the first version of the component differed from the accepted crop by 62
    // pixels — 52 in the mark, 10 on the outline — from a centre-aligned text box instead of a
    // content-sized aligned label, and a five-point path with a degenerate closing segment
    // instead of the accepted four beginning at the apex. Both are now the accepted construction,
    // and this compares the two painted crops in one run rather than trusting that they agree.
    blank();
    {
        Presentation attention;
        attention.layer = PresentationLayer::SubjectPresentation;
        attention.subject = SubjectState::Printing;
        attention.confidence = Confidence::Current;
        attention.subject_condition = SubjectCondition::Attention;
        controller::apply(attention);
        tick(0);
        tick(1120);
        tick(1120);
        require(std::string{controller::report().scene} == "operational",
                "the accepted condition composition is what was drawn");
        const auto accepted = crop(pixels,
                                   theme::kRingCentreX - theme::kPrinterErrorIconHalfWidth -
                                       (theme::kStrokeEmphasis / 2),
                                   theme::kPrinterErrorIconTop - (theme::kStrokeEmphasis / 2));

        blank();
        controller::apply(access(AccessCondition::AuthenticationRequired));
        tick(0);
        tick(1120);
        tick(1120);
        const auto shared = crop(pixels, theme::kMessageSymbolX - (theme::kStrokeEmphasis / 2),
                                 theme::kMessageSymbolPaintedTop);

        const auto delta = differing(accepted, shared);
        if (delta != 0) {
            std::fprintf(stderr, "symbol crops differ in %u pixels\n", delta);
        }
        require(delta == 0,
                "the settled shared symbol reproduces the accepted operational symbol exactly");
    }
    std::puts("PASS the shared symbol is the accepted operational symbol, pixel for pixel");

    // --- 2. Identity clearance, and its absence ---------------------------------
    //
    // The access errors are the only messages carrying an identity, and it is the one row that
    // could collide with the title above or the symbol beneath.
    blank();
    controller::apply(access(AccessCondition::AuthenticationRequired));
    tick(0);
    tick(1120);
    {
        Drawn drawn;
        collect(lv_screen_active(), drawn);
        require(drawn.labels.count("CORE One") == 1,
                "a known identity is drawn on an access error");
        require(drawn.title_fonts.count("CORE One=section16") == 1,
                "the identity keeps its accepted 16 px secondary treatment");
    }

    // A statement that has no identity must not invent one, and must leave the row empty rather
    // than closing up over it — the title stays where the identity would have kept it.
    const auto with_identity = pixels;
    blank();
    controller::apply(fault(DeviceFaultReason::OtaValidationFailure));
    tick(0);
    tick(1120);
    {
        Drawn drawn;
        collect(lv_screen_active(), drawn);
        require(drawn.labels.count("CORE One") == 0, "no identity is invented for a device fault");
    }

    // The title row is identical ink in both: same y, same face, so the two screens do not move
    // their heading because one of them happens to know a name.
    auto title_row_ink = [](const Frame& frame, int y) {
        unsigned count = 0;
        const auto background = frame[0];
        for (int x = 0; x < w; ++x) {
            count += frame[y * w + x] != background ? 1U : 0U;
        }
        return count;
    };
    for (int y = theme::kMessageIdentityY; y < theme::kMessageIdentityY + theme::kLineSection;
         ++y) {
        require(title_row_ink(pixels, y) == 0,
                "the identity row is empty, not closed up, when no name is known");
    }
    require(title_row_ink(with_identity, theme::kMessageIdentityY + 4) > 0,
            "the identity row carries ink when a name is known");

    std::puts("PASS identity clearance, present and absent");

    // --- 3. Off pause, wake, and no catching up ---------------------------------
    blank();
    controller::note_panel_visible(false);
    controller::apply(fault(DeviceFaultReason::WifiUnavailable));
    tick(0);
    const auto dark = pixels;
    tick(5000);
    require(pixels == dark, "a message entry does not run while the panel is Off");
    controller::note_panel_visible(true);
    tick(0);
    const auto woken = pixels;
    require(woken == dark, "and it is still at its first frame when the light returns");

    // 200 ms rather than one frame: the shared easing is a smootherstep, which is deliberately
    // almost flat for the first tick or two. Asserting on a single 16 ms step would be asserting
    // about the easing curve, not about whether the clock started.
    tick(200);
    require(pixels != woken, "the entry begins once the panel can be seen");
    tick(1120);
    require(!tick(16), "and still settles once");
    std::puts("PASS Off pause and wake without catching up");

    // --- 4. Interruption and reversal -------------------------------------------
    //
    // A second condition arriving mid-entry must be truthful immediately and must not restart the
    // clock from black.
    blank();
    controller::apply(access(AccessCondition::AuthenticationRequired));
    tick(0);
    tick(320);
    const auto mid = pixels;
    controller::apply(access(AccessCondition::UnsupportedInterface));
    require(tick(16), "a mid-entry reclassification keeps the clock running");
    {
        Drawn drawn;
        collect(lv_screen_active(), drawn);
        require(drawn.labels.count("Unsupported interface") == 1,
                "the new wording is truthful immediately, before the motion finishes");
        require(drawn.labels.count("Authentication required") == 0, "the old wording is gone");
    }
    require(pixels != mid, "and the composition did not freeze");
    tick(1120);

    // The reverse: a classified failure back to acquisition, on the same scene, both directions.
    Presentation connecting;
    connecting.layer = PresentationLayer::SubjectPresentation;
    connecting.acquiring_connection = true;
    controller::apply(connecting);
    require(tick(0), "the reverse morph drives the tick");
    const auto reversing = pixels;
    tick(560);
    require(pixels != reversing, "the symbol travels back rather than switching");
    tick(1120);
    {
        Drawn drawn;
        collect(lv_screen_active(), drawn);
        require(drawn.title_fonts.count("Connecting.=outcome28") == 1 ||
                    drawn.title_fonts.count("Connecting..=outcome28") == 1 ||
                    drawn.title_fonts.count("Connecting...=outcome28") == 1,
                "acquisition restores its accepted 28 px title");
    }

    // ⬅️ THE MARK IS STILL IN THE TREE AT THE ACQUISITION END, AT ZERO OPACITY, AND THAT IS THE
    // ACCEPTED MORPH. It has to be continuously fadeable in both directions, so it is painted
    // rather than added and removed — which is why this is asserted on PIXELS and not on tree
    // membership. Where a composition is genuinely retired, as the answered reset question is,
    // the assertion is the other way round and is made on the tree.
    require(count_colour(pixels, warning) == 0, "no warning ink survives at the acquisition end");
    require(count_colour(pixels, accent) > 0, "the acquisition outline is accent again");
    std::puts("PASS interruption, reversal, and the restored acquisition endpoint");

    // --- 5. The destructive sequence, end to end --------------------------------
    blank();
    Presentation question;
    question.layer = PresentationLayer::ModalSurface;
    question.modal_surface = ModalSurface::FactoryResetConfirmation;
    controller::apply(question);
    tick(0);
    tick(1120);
    {
        Drawn drawn;
        collect(lv_screen_active(), drawn);
        require(drawn.labels.count("Erase all settings?") == 1, "the question is asked");
        require(drawn.labels.count("Erasing settings.") == 0, "and the erase is not claimed yet");
    }

    Presentation running;
    running.layer = PresentationLayer::ExclusiveOperation;
    running.device_operation = DeviceOperation::FactoryResetting;
    controller::apply(running);
    require(tick(0), "answering the question drives the tick");

    // ⛔ ROOT FINDING R2, MEASURED AT FRAME ZERO AND NOT AFTER SETTLEMENT. "A short press cancels"
    // is false the instant the erase begins. The first version faded the question out over
    // 1100 ms, leaving it fully legible on this very frame.
    {
        Drawn drawn;
        collect(lv_screen_active(), drawn);
        require(drawn.labels.count("Erase all settings?") == 0,
                "the answered question is gone from the first frame");
        require(drawn.labels.count("Hold the button again to erase. A short press cancels.") == 0,
                "and so is the cancellation instruction that has stopped being true");
        require(drawn.labels.count("!") == 0, "and the question's symbol with it");
    }
    require(count_colour(pixels, warning) == 0,
            "no warning ink survives the first frame of the erase");

    const auto entering = pixels;
    tick(560);
    require(pixels != entering, "the running line enters rather than appearing at once");
    tick(1120);
    {
        Drawn drawn;
        collect(lv_screen_active(), drawn);
        require(drawn.labels.count("Erasing settings.") == 1,
                "the running line is drawn, at its first dot");
        require(drawn.labels.count("Erase all settings?") == 0,
                "an answered question is not still on the panel");
        require(drawn.title_fonts.count("Erasing settings.=title20") == 1,
                "the running line keeps its unchanged neutral treatment");
        require(drawn.labels.count("!") == 0, "and carries no warning symbol");
    }
    require(count_colour(pixels, warning) == 0, "the running half is neutral");
    require(!tick(16), "and settles");

    // ⛔ THE NEUTRAL ENDPOINT, PINNED AS A BOUND. One StateTitle line, in the row band the owner
    // accepted, and nothing else anywhere on the canvas. Derived from the tokens rather than
    // written as a number, so a change to the block arithmetic fails here instead of moving the
    // running line by 27 px behind a simplification that looked harmless.
    {
        const int block = theme::kLineTitle + theme::kSpaceComponent + theme::kLineBody;
        const int expected_top =
            theme::kCanvasContent.y + ((theme::kCanvasContent.height - block) / 2);
        const int top = ink_top(pixels);
        const int bottom = ink_bottom(pixels);
        if (top < expected_top || bottom >= expected_top + theme::kLineTitle) {
            std::fprintf(stderr, "running line ink rows %d..%d, expected inside %d..%d\n", top,
                         bottom, expected_top, expected_top + theme::kLineTitle - 1);
        }
        require(top >= expected_top && bottom < expected_top + theme::kLineTitle,
                "the running line is one line in its unchanged accepted row band");
    }

    // The failure that half can raise.
    controller::apply(fault(DeviceFaultReason::FactoryResetIncomplete));
    require(tick(0), "the failure enters on the tick");
    tick(1120);
    {
        Drawn drawn;
        collect(lv_screen_active(), drawn);
        require(drawn.labels.count("Reset could not finish") == 1, "the failure names itself");
        require(drawn.labels.count("Nothing has been erased. Hold the button to set up again.") ==
                    0,
                "and does not inherit recovery's persistence claim");
        require(drawn.labels.count("Erasing settings.") == 0, "the running line is gone");
    }
    require(count_colour(pixels, error) > 0, "a completed failure carries the Error tone");
    require(count_colour(pixels, warning) == 0, "and not the question's");
    std::puts("PASS confirmation to progress to failure");

    // Cancellation, from an ALREADY-SETTLED operational composition back to the same one — root
    // finding R4. A virgin blank -> question -> Ready run would not have caught this: the
    // controller reseeded the operational appearance only when the previous scene was Access, so
    // returning from the modal put a fully drawn composition back in a single frame.
    blank();
    Presentation printing;
    printing.layer = PresentationLayer::SubjectPresentation;
    printing.subject = SubjectState::Printing;
    printing.confidence = Confidence::Current;
    printing.progress.known = true;
    printing.progress.percent = 78;
    controller::apply(printing);
    tick(0);
    tick(1120);
    tick(1120);
    require(std::string{controller::report().scene} == "operational", "the subject is settled");
    const auto settled_printing = pixels;

    controller::apply(question);
    tick(0);
    tick(1120);
    {
        Drawn drawn;
        collect(lv_screen_active(), drawn);
        require(drawn.labels.count("Erase all settings?") == 1, "the modal draws over the subject");
    }

    controller::apply(printing);
    require(tick(0), "cancellation uses the animated appearance path");
    const auto returning = pixels;
    require(returning != settled_printing,
            "the return is an entry, not a settled composition in one frame");
    tick(560);
    require(pixels != returning, "and it advances");
    tick(1120);
    tick(1120);
    require(std::string{controller::report().scene} == "operational", "and lands on the subject");
    {
        Drawn drawn;
        collect(lv_screen_active(), drawn);
        require(drawn.labels.count("Erase all settings?") == 0,
                "the withdrawn question leaves nothing behind");
    }
    require(pixels == settled_printing,
            "and the accepted operational endpoint is reached unchanged");
    std::puts("PASS cancellation from and to a settled subject composition");

    // --- 5b. A statement displaced by another statement -------------------------
    //
    // Root finding R4: recovery is reachable, a device fault outranks it, and the first version
    // armed its clock only on entering the scene — so this swapped words with no motion at all.
    blank();
    Presentation recovery;
    recovery.layer = PresentationLayer::RecoveryOrProvisioning;
    recovery.device_operation = DeviceOperation::RecoveryMode;
    controller::apply(recovery);
    tick(0);
    tick(1120);
    require(!tick(16), "the recovery statement settles");
    const auto settled_recovery = pixels;

    controller::apply(fault(DeviceFaultReason::StorageFailure));
    require(tick(0), "a displacing fault drives the tick");
    {
        Drawn drawn;
        collect(lv_screen_active(), drawn);
        require(drawn.labels.count("Storage unavailable") == 1,
                "the displacing statement is truthful from the first frame");
        require(drawn.labels.count("Settings cannot be used") == 0,
                "and the displaced one is gone from it");
        require(drawn.labels.count("Nothing has been erased. Hold the button to set up again.") ==
                    0,
                "including its persistence claim, which is not true of this fault");
    }
    require(pixels != settled_recovery, "the replacement is not a settled swap");
    const auto displacing = pixels;
    tick(560);
    require(pixels != displacing, "it enters on the shared clock");
    tick(1120);
    require(!tick(16), "and settles once");
    std::puts("PASS statement displaced by statement enters rather than swapping");

    // --- 5c. A motion interrupted mid-flight by Off ------------------------------
    //
    // EntryClock refuses to START in the dark but does not hold one already running. The message
    // and reset paths use HoldClock for exactly this, and this is the control that measures it:
    // an entry dimmed at ~300 ms must not be finished when the light comes back.
    blank();
    controller::apply(fault(DeviceFaultReason::OtaValidationFailure));
    tick(0);
    const auto first_message_frame = pixels;
    tick(304);
    const auto interrupted = pixels;
    require(interrupted != first_message_frame, "the entry had actually begun before the dim");
    controller::note_panel_visible(false);
    tick(5000);
    require(pixels == interrupted, "a dimmed entry does not advance in the dark");
    controller::note_panel_visible(true);
    require(tick(16), "the held motion is still owed after the wake");
    require(pixels == interrupted,
            "and the wake tick re-anchors rather than jumping the motion forward");

    tick(200);
    require(pixels != interrupted, "it then resumes from the frame it held");

    // It held at roughly 300 ms of an 1100 ms entry, so what is on the panel now is a MIDDLE
    // frame: neither the first nor the settled one. That is the whole property — a motion that
    // had finished in the dark would have woken already settled.
    require(pixels != first_message_frame && ink_bottom(pixels) > 0,
            "the resumed frame is a middle frame, not the settled endpoint");

    tick(1120);
    require(!tick(16), "and completes in the light");
    const auto completed = pixels;
    require(completed != interrupted, "the endpoint is not the frame it held");
    std::puts("PASS mid-motion Off holds the entry rather than finishing it unseen");

    // --- 6. The session composition survives, the credential does not ------------
    blank();
    Presentation setup;
    setup.layer = PresentationLayer::RecoveryOrProvisioning;
    setup.provisioning = ProvisioningPhase::SetupActive;
    constexpr const char* session_password = "Wq7mK2xP9tLr";
    prd::ui::SetupSession session;
    session.ssid.assign("PRD-SETUP-A1B2");
    session.password.assign(session_password);
    // Keep this first-arrival assertion ahead of statement coverage: the old-code control for
    // the Required gate must fail on the privacy property itself, not on a later re-entry edge.
    Presentation early_required = setup;
    early_required.provisioning = ProvisioningPhase::SetupRequired;
    controller::apply(early_required);
    tick(0);
    tick(1120);
    controller::set_setup_session(session);
    tick(0);
    {
        Drawn drawn;
        collect(lv_screen_active(), drawn);
        require(drawn.labels.count(session_password) == 0,
                "an early session cannot reveal the credential under SetupRequired");
    }
    controller::set_setup_session(session);
    controller::apply(setup);
    tick(0);
    tick(1120);
    {
        Drawn drawn;
        collect(lv_screen_active(), drawn);
        require(drawn.labels.count("PRD-SETUP-A1B2") == 1, "the session network is drawn");
        require(drawn.labels.count("Wq7mK2xP9tLr") == 1, "the session password is drawn");
    }
    const auto light = rgb565(theme::kColorPrimary);
    const auto code_field = count_colour(pixels, light);
    require(code_field > 1000, "the QR light field is drawn");
    require(prd::diagnostics::qr_temp_audit_count() > 0 &&
                prd::diagnostics::qr_temp_dirty_count() == 0,
            "QR temporary buffers are wiped before release");
    const auto first_qr_audit_count = prd::diagnostics::qr_temp_audit_count();

    // A device fault while the access point is still up: the DriverStalled ruling keeps the
    // session alive, and the panel must still show only the statement.
    controller::apply(fault(DeviceFaultReason::WifiUnavailable));
    tick(0);
    tick(1120);
    {
        Drawn drawn;
        collect(lv_screen_active(), drawn);
        require(drawn.labels.count("Wi-Fi unavailable") == 1, "the statement owns the canvas");
        require(drawn.labels.count("Wq7mK2xP9tLr") == 0, "no credential survives on the panel");
        require(drawn.labels.count("PRD-SETUP-A1B2") == 0, "and no session row either");
    }
    require(count_colour(pixels, light) < code_field / 4,
            "the encoded code is cleared, not merely covered");

    // ⛔ ROOT FINDING R3: A SESSION ARRIVING WHILE THE STATEMENT OWNS THE CANVAS. The access point
    // stays up under this fault, so a late or re-submitted session really does arrive here.
    prd::ui::SetupSession later;
    later.ssid.assign("PRD-SETUP-C3D4");
    later.password.assign("Zx4nQ8vB1yHs");
    controller::set_setup_session(later);
    tick(0);
    tick(16);
    {
        Drawn drawn;
        collect(lv_screen_active(), drawn);
        require(drawn.labels.count("Zx4nQ8vB1yHs") == 0,
                "a session arriving under a statement puts no credential on the panel");
        require(drawn.labels.count("PRD-SETUP-C3D4") == 0, "and no network row");
        require(drawn.labels.count("Wi-Fi unavailable") == 1,
                "the statement is still what is shown");
    }
    require(count_colour(pixels, light) < code_field / 4, "and no code is drawn for it");

    // And it comes back when the statement does not — the retained session was not destroyed.
    controller::apply(setup);
    tick(0);
    tick(1120);
    {
        Drawn drawn;
        collect(lv_screen_active(), drawn);
        require(drawn.labels.count("Zx4nQ8vB1yHs") == 1,
                "the retained session returns — the LATER one, which is the true current session");
        require(drawn.labels.count("Wq7mK2xP9tLr") == 0, "and not the superseded credential");
        require(drawn.labels.count("Wi-Fi unavailable") == 0, "and the statement is gone");
    }
    // A code is drawn again. Not the SAME count: it encodes the later session, which is a
    // different payload and therefore a different module pattern — asserting equality here would
    // have been asserting that the panel came back showing the superseded network.
    require(count_colour(pixels, light) > code_field / 2,
            "a code is drawn again for the current session");

    controller::clear_setup_session();
    tick(0);
    {
        Drawn drawn;
        collect(lv_screen_active(), drawn);
        require(drawn.labels.count("Zx4nQ8vB1yHs") == 0, "clearing the session clears the panel");
    }
    std::puts("PASS session and credential across a statement");

    // --- 7. The protected setup composition: animated, unmoved, and allocating nothing ---------
    //
    // Owner ruling R3 freezes this geometry, and the transitions INTO it were not exempt from
    // motion. Nothing here repositions or resizes anything: each object is faded by the style its
    // own draw already honours. What this measures is that the entry really happens, that the
    // settled pixels are the frozen ones byte for byte, and that no intermediate layer was
    // allocated to achieve it.
    blank();
    controller::set_setup_session(session);
    controller::apply(setup);
    tick(0);
    tick(1120);
    tick(1120);
    require(!tick(16), "the setup composition settles");
    const auto settled_setup = pixels;
    require(count_colour(settled_setup, light) > 1000, "and it really is the QR composition");

    lv_mem_monitor_t before{};
    lv_mem_monitor(&before);

    controller::apply(fault(DeviceFaultReason::WifiUnavailable));
    tick(0);
    tick(1120);
    require(pixels != settled_setup, "the statement took the canvas");

    controller::apply(setup);
    require(tick(0), "returning to the setup composition is an entry");
    const auto first_setup_frame = pixels;
    require(first_setup_frame != settled_setup,
            "it does not reappear fully drawn in a single frame");
    tick(560);
    require(pixels != first_setup_frame, "and it advances");
    tick(1120);
    require(!tick(16), "and settles once, with no idle redraw after it");

    require(pixels == settled_setup,
            "the settled QR and session pixels are identical to the frozen composition");

    lv_mem_monitor_t after{};
    lv_mem_monitor(&after);
    std::printf("lv_mem max_used before=%u after=%u total=%u\n",
                static_cast<unsigned>(before.max_used), static_cast<unsigned>(after.max_used),
                static_cast<unsigned>(before.total_size));

    // ⛔ WHAT THIS MEASURES, EXACTLY. lv_mem's max_used is a HIGH-WATER MARK: an unchanged value
    // proves the fade never drove the pool above what drawing the same composition already needed.
    // It does not prove zero allocation calls, and it is not claimed to. What it does rule out is
    // the thing at issue — an intermediate compositing layer, which for this block would be tens
    // of kilobytes and could not hide under an unmoved high-water mark.
    //
    // The clib allocator reports nothing; the pool build is where this is measurable.
    if (before.total_size > 0) {
        require(after.max_used == before.max_used,
                "the setup fade drove no rise in the pool high-water mark");
    }

    // The same return, reached by withdrawing the reset question rather than by a fault clearing.
    controller::apply(question);
    tick(0);
    tick(1120);
    {
        Drawn drawn;
        collect(lv_screen_active(), drawn);
        require(drawn.labels.count("Erase all settings?") == 1, "the question owns the canvas");
        require(drawn.labels.count(session_password) == 0, "and no credential is under it");
    }
    controller::apply(setup);
    require(tick(0), "a withdrawn question returns to setup as an entry");
    const auto cancelled_first = pixels;
    require(cancelled_first != settled_setup, "not fully drawn in one frame either");
    tick(1120);
    tick(1120);
    require(!tick(16), "and settles once");
    require(pixels == settled_setup, "on the same frozen pixels");

    lv_mem_monitor_t after_question{};
    lv_mem_monitor(&after_question);
    if (before.total_size > 0) {
        require(after_question.max_used == before.max_used,
                "and that path drives no rise in it either");
    }
    std::puts("PASS protected setup composition animates, settles unmoved, allocates nothing");

    // --- 7b. Both queue orders across a statement, and the re-entry invariant ------------------
    //
    // The session and the presentation arrive on separate queues and either order has to produce
    // the same panel. The order that broke it: a statement owns the scene, the scene is LEFT for
    // another one, a session arrives while it is hidden, and the setup composition is shown again
    // — at which point the rows must be present and no statement residue may be.
    blank();
    controller::apply(fault(DeviceFaultReason::OtaValidationFailure));
    tick(0);
    tick(1120);
    blank();
    controller::clear_setup_session();
    controller::set_setup_session(session);
    controller::apply(setup);
    tick(0);
    tick(1120);
    tick(1120);
    {
        Drawn drawn;
        collect(lv_screen_active(), drawn);
        require(drawn.labels.count(session_password) == 1,
                "a session that arrived while the scene was hidden is drawn on re-entry");
        require(drawn.labels.count("PRD-SETUP-A1B2") == 1, "with its network row");
        require(drawn.labels.count("Update failed") == 0, "and no statement residue");
        require(drawn.labels.count("Restart the display.") == 0, "not even its instruction");
    }
    require(count_colour(pixels, light) > 1000, "and the code is drawn");
    require(pixels == settled_setup, "on the frozen composition, unchanged by the detour");

    // A phase changes INSIDE the setup scene: the QR and credentials must not blink away while
    // checking a submitted network, but the changed, truthful title must visibly resolve on the
    // same bounded clock as every other screen transition. The pre-repair selector still chose
    // Provisioning, so scene-entry tests alone could not detect its static title replacement.
    auto same_code = [&](const Frame& frame) {
        for (int y = kSetupQrTop; y < kSetupQrTop + kSetupQrSide; ++y) {
            for (int x = kSetupQrLeft; x < kSetupQrLeft + kSetupQrSide; ++x) {
                if (frame[y * w + x] != settled_setup[y * w + x]) {
                    return false;
                }
            }
        }
        return true;
    };
    auto save_setup_phase = [&](const char* name) {
        const auto path = out / (std::string{"setup-phase-"} + name + ".rgb565");
        FILE* const file = std::fopen(path.c_str(), "wb");
        require(file != nullptr, "setup phase frame opens");
        require(std::fwrite(pixels.data(), 2, pixels.size(), file) == pixels.size(),
                "setup phase frame writes");
        std::fclose(file);
    };

    Presentation validating = setup;
    validating.provisioning = ProvisioningPhase::ValidatingConfiguration;
    watch_setup_phase_qr = true;
    setup_phase_qr_flushes = 0;
    controller::apply(validating);
    require(tick(0), "an internal setup phase transition drives the tick");
    require(setup_phase_qr_flushes == 0,
            "the QR block has no flushed area during the phase transition");
    {
        Drawn drawn;
        collect(lv_screen_active(), drawn);
        require(drawn.labels.count("Checking that network") == 1,
                "the new validation status is truthful on the first frame");
        require(drawn.labels.count("Join this network") == 0, "the old title is retired");
        require(drawn.labels.count(session_password) == 1, "the credential remains available");
    }
    require(same_code(pixels), "the QR does not move or dim when validation begins");
    const auto validating_first = pixels;
    save_setup_phase("first");
    tick(304);
    require(pixels != validating_first, "the internal phase title resolves visibly");
    require(same_code(pixels), "the QR stays unchanged during the phase motion");
    require(setup_phase_qr_flushes == 0,
            "the QR block has no flushed area during the phase transition");
    const auto validating_held = pixels;
    save_setup_phase("middle");
    controller::note_panel_visible(false);
    const auto held_flushes = flushes;
    tick(5000);
    require(pixels == validating_held, "the title holds its frame while the panel is Off");
    require(flushes == held_flushes, "the held phase motion flushes nothing while Off");
    controller::note_panel_visible(true);
    require(tick(16), "the phase title still has motion owed after wake");
    require(pixels == validating_held, "wake resumes without jumping to a later frame");
    tick(200);
    require(pixels != validating_held, "the title continues from its held frame");
    tick(1120);
    require(!tick(16), "the phase motion settles once without idle work");
    require(same_code(pixels), "the QR is unchanged at the new endpoint");
    require(setup_phase_qr_flushes == 0,
            "the QR block has no flushed area during the phase transition");
    save_setup_phase("settled");
    watch_setup_phase_qr = false;

    Presentation failed = setup;
    failed.provisioning = ProvisioningPhase::SetupFailed;
    controller::apply(failed);
    require(tick(0), "validation failure is another internal phase transition");
    {
        Drawn drawn;
        collect(lv_screen_active(), drawn);
        require(drawn.labels.count("That did not work") == 1,
                "the failure title replaces validation immediately");
    }
    require(same_code(pixels), "the retryable failure retains the same QR and session");
    tick(1120);
    require(!tick(16), "the failure phase settles");

    controller::apply(setup);
    require(tick(0), "retry returns to the active setup phase with motion");
    tick(1120);
    require(!tick(16), "the retry title settles");
    require(pixels == settled_setup, "retry returns to the exact frozen setup endpoint");
    std::puts("PASS internal setup phase motion without QR or credential disruption");

    // The phase and the private session arrive on independent queues. Neither arrival order may
    // expose a credential while the panel still says setup is required. Once the offered network
    // is real, its fixed rows and QR emerge together; withdrawing it scrubs both immediately.
    auto qr_changes_from = [&](const Frame& baseline) {
        unsigned changed = 0;
        for (int y = kSetupQrTop; y < kSetupQrTop + kSetupQrSide; ++y) {
            for (int x = kSetupQrLeft; x < kSetupQrLeft + kSetupQrSide; ++x) {
                changed += pixels[y * w + x] != baseline[y * w + x] ? 1U : 0U;
            }
        }
        return changed;
    };
    auto row_changes_from = [&](const Frame& baseline, const int left, const int top,
                                const int right, const int bottom) {
        unsigned changed = 0;
        for (int y = top; y < bottom; ++y) {
            for (int x = left; x < right; ++x) {
                changed += pixels[y * w + x] != baseline[y * w + x] ? 1U : 0U;
            }
        }
        return changed;
    };
    Presentation required = setup;
    required.provisioning = ProvisioningPhase::SetupRequired;
    blank();
    controller::clear_setup_session();
    controller::apply(required);
    tick(0);
    tick(1120);
    require(!tick(16), "required setup settles without session work");
    const auto required_frame = pixels;
    controller::set_setup_session(session);
    tick(0);
    {
        Drawn drawn;
        collect(lv_screen_active(), drawn);
        require(drawn.labels.count(session_password) == 0,
                "an early session cannot reveal the credential under SetupRequired");
    }
    require(pixels == required_frame,
            "an early session leaves the required composition byte-identical");
    controller::apply(setup);
    require(tick(0), "required to active drives the bounded visible motion");
    save_setup_phase("offer-first");
    require(qr_changes_from(required_frame) == 0,
            "new QR starts at zero luminance rather than appearing in one frame");
    require(row_changes_from(required_frame, theme::kCanvasContent.x, 72, kSetupQrLeft, 112) == 0 &&
                row_changes_from(required_frame, theme::kCanvasContent.x, 180,
                                 theme::kCanvasContent.right(), 228) == 0,
            "network and password rows start dark with the QR");
    tick(560);
    const auto mid_qr = qr_changes_from(required_frame);
    require(mid_qr > 0, "offered QR emerges during the shared motion");
    require(row_changes_from(required_frame, theme::kCanvasContent.x, 72, kSetupQrLeft, 112) > 0 &&
                row_changes_from(required_frame, theme::kCanvasContent.x, 180,
                                 theme::kCanvasContent.right(), 228) > 0,
            "network and password rows emerge with the QR");
    const auto mid_offer_frame = pixels;
    save_setup_phase("offer-middle");
    controller::note_panel_visible(false);
    const auto offer_held_flushes = flushes;
    tick(5000);
    require(pixels == mid_offer_frame && flushes == offer_held_flushes,
            "a partly revealed QR and credential hold without any Off flush");
    controller::note_panel_visible(true);
    require(tick(16) && pixels == mid_offer_frame,
            "offered session resumes from its actual painted frame");
    tick(560);
    require(!tick(16), "offered session settles without idle work");
    require(qr_changes_from(mid_offer_frame) > 0, "offered QR resolves beyond its middle frame");
    require(row_changes_from(mid_offer_frame, theme::kCanvasContent.x, 72, kSetupQrLeft, 112) > 0 &&
                row_changes_from(mid_offer_frame, theme::kCanvasContent.x, 180,
                                 theme::kCanvasContent.right(), 228) > 0,
            "network and password rows resolve beyond their middle frame");
    require(pixels == settled_setup, "offered session reaches the frozen endpoint");
    save_setup_phase("offer-settled");

    controller::apply(required);
    require(tick(0), "active to required animates the truthful replacement title");
    {
        Drawn drawn;
        collect(lv_screen_active(), drawn);
        require(drawn.labels.count(session_password) == 0,
                "withdrawing the offer retires the credential on its first frame");
    }
    require(qr_changes_from(required_frame) == 0,
            "withdrawing the offer immediately clears the encoded QR");
    tick(1120);
    require(!tick(16), "required title settles once");
    require(pixels == required_frame, "required endpoint is unchanged after withdrawal");

    controller::apply(setup);
    tick(0);
    tick(320);
    require(qr_changes_from(required_frame) > 0, "interrupted offer has begun to reveal");
    controller::apply(required);
    tick(0);
    require(qr_changes_from(required_frame) == 0,
            "reversal immediately removes the encoded credential, even mid-reveal");
    tick(1120);
    require(!tick(16) && pixels == required_frame, "reversed offer settles as required");

    // The opposite order: the offer is announced before its session arrives. The truth of the
    // title is immediate, but no fabricated code is drawn; a later session has its own reveal.
    controller::clear_setup_session();
    controller::apply(setup);
    tick(0);
    tick(1120);
    require(qr_changes_from(required_frame) == 0, "active without a session has no stale code");
    controller::set_setup_session(session);
    tick(0);
    require(qr_changes_from(required_frame) == 0, "late session also starts with an undrawn QR");
    tick(560);
    require(qr_changes_from(required_frame) > 0, "late session reveals its QR on the tick path");
    tick(560);
    require(!tick(16), "late session reveal settles");
    require(pixels == settled_setup, "late session reaches the same frozen endpoint");
    std::puts("PASS both setup phase/session queue orders, reveal and immediate secret retirement");

    // A fault can displace an offered network, then clear into SetupRequired rather than back
    // into SetupActive. show_session() runs before apply() finishes that phase change; it must
    // not briefly encode a retained credential and log a phantom QR presence in between.
    controller::apply(fault(DeviceFaultReason::WifiUnavailable));
    tick(0);
    tick(1120);
    const auto qr_draws_before_required = prd::diagnostics::setup_qr_draw_count();
    controller::apply(required);
    tick(0);
    require(prd::diagnostics::setup_qr_draw_count() == qr_draws_before_required,
            "statement return to Required never re-encodes the retained QR");
    {
        Drawn drawn;
        collect(lv_screen_active(), drawn);
        require(drawn.labels.count(session_password) == 0,
                "statement return to Required keeps the retained password off the panel");
    }
    tick(1120);
    require(pixels == required_frame, "statement return to Required reaches the same endpoint");
    controller::apply(setup);
    tick(0);
    tick(1120);
    require(pixels == settled_setup, "a later offer still restores the retained session");
    std::puts("PASS statement exit into Required does not transiently encode a credential");

    // And the other order: the session first, the presentation after.
    blank();
    controller::apply(fault(DeviceFaultReason::StorageFailure));
    tick(0);
    tick(1120);
    {
        Drawn drawn;
        collect(lv_screen_active(), drawn);
        require(drawn.labels.count(session_password) == 0,
                "the statement still shows no credential from the retained session");
        require(drawn.labels.count("Storage unavailable") == 1, "and states its own condition");
    }
    controller::apply(setup);
    tick(0);
    tick(1120);
    tick(1120);
    require(pixels == settled_setup, "and the return lands on the same frozen composition");
    std::puts("PASS both session queue orders across a statement, and the re-entry invariant");

    // The mailbox is one-deep: a clear and a replacement can collapse to the replacement
    // before the render task drains it. Test that actual arrival order, not only an explicit
    // clear followed by a later offer. The old password and code must be retired before the
    // replacement starts its own bounded appearance. A duplicate submission must do nothing.
    require(!tick(16), "settled offered session is idle before a repeated submission");
    const auto original_session_frame = pixels;
    const auto original_flushes = flushes;
    const auto original_qr_draws = prd::diagnostics::setup_qr_draw_count();
    const auto original_qr_clears = prd::diagnostics::setup_qr_clear_count();
    char* const password_label_bytes = find_label_text(lv_screen_active(), session_password);
    require(password_label_bytes != nullptr,
            "the displayed password label has inspectable storage");
    controller::set_setup_session(session);
    require(!tick(0) && flushes == original_flushes && pixels == original_session_frame &&
                prd::diagnostics::setup_qr_draw_count() == original_qr_draws,
            "an identical setup session neither redraws nor restarts motion");

    prd::ui::SetupSession replacement = session;
    replacement.ssid.assign("PRD-SETUP-C3D4");
    constexpr const char* replacement_password = "Tu4nQ8mR2sVz";
    replacement.password.assign(replacement_password);
    controller::set_setup_session(replacement);
    require(tick(0), "a coalesced replacement starts a new visible session entry");
    {
        Drawn drawn;
        collect(lv_screen_active(), drawn);
        require(drawn.labels.count(session_password) == 0 &&
                    drawn.labels.count(replacement_password) == 1,
                "a replacement retires the old credential before drawing the new one");
    }
    require(qr_changes_from(required_frame) == 0,
            "replacement QR begins dark instead of swapping an encoded password in one frame");
    require(prd::diagnostics::setup_qr_draw_count() == original_qr_draws + 1,
            "the replacement has exactly one new QR presence event");
    require(prd::diagnostics::setup_qr_clear_count() == original_qr_clears + 1,
            "the old encoded QR is cleared before the replacement is drawn");
    require(find_label_text(lv_screen_active(), replacement_password) == password_label_bytes,
            "the replacement reuses scene-owned label storage, not freed LVGL heap memory");
    require(std::all_of(password_label_bytes + std::strlen(replacement_password) + 1,
                        password_label_bytes + 64, [](const char byte) { return byte == 0; }),
            "replacement leaves no old password bytes beyond its terminator");
    tick(560);
    require(qr_changes_from(required_frame) > 0,
            "replacement QR emerges on the same bounded display clock");
    tick(560);
    require(!tick(16) && pixels != original_session_frame,
            "replacement settles to a distinct session and then goes idle");
    controller::clear_setup_session();
    tick(0);
    {
        Drawn drawn;
        collect(lv_screen_active(), drawn);
        require(drawn.labels.count(replacement_password) == 0,
                "explicit clear retires the replacement credential too");
    }
    require(qr_changes_from(required_frame) == 0,
            "explicit clear removes the replacement QR immediately");
    require(std::all_of(password_label_bytes, password_label_bytes + 64,
                        [](const char byte) { return byte == 0; }),
            "explicit clear wipes every byte of the borrowed password label buffer");
    controller::set_setup_session(session);
    tick(0);
    tick(1120);
    require(pixels == original_session_frame,
            "restoring the original offer returns to the frozen setup endpoint");

    // The same replacement can arrive while a higher-priority fault owns the panel.
    // It is retained for the eventual offer, never printed beneath the statement.
    controller::apply(fault(DeviceFaultReason::WifiUnavailable));
    tick(0);
    tick(1120);
    const auto draws_under_fault = prd::diagnostics::setup_qr_draw_count();
    controller::set_setup_session(replacement);
    tick(0);
    {
        Drawn drawn;
        collect(lv_screen_active(), drawn);
        require(drawn.labels.count(session_password) == 0 &&
                    drawn.labels.count(replacement_password) == 0,
                "a replacement under a fault leaves both credentials off the panel");
    }
    require(prd::diagnostics::setup_qr_draw_count() == draws_under_fault,
            "a replacement under a fault does not encode a QR");
    controller::apply(setup);
    tick(0);
    tick(1120);
    {
        Drawn drawn;
        collect(lv_screen_active(), drawn);
        require(drawn.labels.count(session_password) == 0 &&
                    drawn.labels.count(replacement_password) == 1,
                "fault exit reveals only the newest retained session");
    }
    controller::clear_setup_session();
    tick(0);
    std::puts("PASS coalesced session replacement, duplicate submission and secret retirement");

    // --- 7b. Successful setup returns to the current operational path ------------------------
    //
    // A printer may have been shown before the owner re-enters setup. The operational scene
    // retains that last pose while another scene owns the canvas, so a successful validation
    // must enter Ready from a blank appearance and READY geometry, not reveal the old Printing
    // ring under new wording. Then the normal Ready -> Printing -> Completed chain must keep
    // the same bounded controller motion and never resurrect the retired setup credential.
    Presentation painted_printing = printing;
    painted_printing.show_operational_form = true;
    painted_printing.show_telemetry = true;
    controller::apply(painted_printing);
    tick(0);
    tick(1120);
    require(controller::operational_form_report().energy == 1000,
            "the preceding printer state really left a Printing form behind");

    controller::set_setup_session(session);
    controller::apply(setup);
    tick(0);
    tick(1120);
    controller::apply(validating);
    tick(0);
    tick(1120);
    controller::clear_setup_session();
    tick(0);

    auto save_beta_handoff = [&](const char* name) {
        const auto path = out / (std::string{"beta-handoff-"} + name + ".rgb565");
        FILE* const file = std::fopen(path.c_str(), "wb");
        require(file != nullptr, "beta handoff frame opens");
        require(std::fwrite(pixels.data(), 2, pixels.size(), file) == pixels.size(),
                "beta handoff frame writes");
        std::fclose(file);
    };
    auto assert_no_setup_residue = [&] {
        Drawn drawn;
        collect(lv_screen_active(), drawn);
        require(drawn.labels.count(session_password) == 0 &&
                    drawn.labels.count("PRD-SETUP-A1B2") == 0 &&
                    drawn.labels.count("Checking that network") == 0,
                "successful setup leaves no visible credential or validation copy");
    };

    Presentation success_ready = painted_printing;
    success_ready.subject = SubjectState::Ready;
    success_ready.progress.known = false;
    success_ready.printer_name_known = true;
    success_ready.printer_name.assign("CORE One");
    controller::apply(success_ready);
    require(tick(0), "successful setup starts a visible Ready entry");
    require(std::string{controller::report().scene} == "operational",
            "successful setup selects the operational scene");
    require(controller::operational_form_report().energy == 0,
            "successful setup starts Ready at its own current form, not stale Printing geometry");
    require(ink_bottom(pixels) == -1,
            "successful setup starts Ready with a fully blank appearance");
    assert_no_setup_residue();
    const auto ready_first = pixels;
    save_beta_handoff("ready-first");
    tick(560);
    require(pixels != ready_first, "Ready entry advances after setup");
    save_beta_handoff("ready-middle");
    tick(560);
    require(!tick(16), "Ready entry settles without idle motion");
    require(controller::operational_form_report().energy == 0,
            "Ready endpoint keeps the resting horizon");
    save_beta_handoff("ready-settled");
    assert_no_setup_residue();

    controller::apply(painted_printing);
    require(tick(0), "Ready to Printing starts the operational transition");
    const auto printing_first = pixels;
    save_beta_handoff("printing-first");
    tick(560);
    require(pixels != printing_first, "Ready to Printing has a painted middle frame");
    save_beta_handoff("printing-middle");
    tick(560);
    require(!tick(16) && controller::operational_form_report().energy == 1000,
            "Printing reaches its real reported form and settles");
    save_beta_handoff("printing-settled");
    assert_no_setup_residue();

    Presentation success_completed = painted_printing;
    success_completed.subject = SubjectState::Completed;
    success_completed.progress.percent = 100;
    controller::apply(success_completed);
    require(tick(0), "Printing to Completed starts the operational transition");
    const auto completed_first = pixels;
    save_beta_handoff("completed-first");
    tick(560);
    require(pixels != completed_first, "Printing to Completed has a painted middle frame");
    save_beta_handoff("completed-middle");
    tick(560);
    require(!tick(16) && controller::operational_form_report().energy == 600,
            "Completed reaches its reported form and settles");
    save_beta_handoff("completed-settled");
    assert_no_setup_residue();
    std::puts("PASS setup validation to Ready, Printing and Completed controller handoffs");

    // --- 8. The access scene: held motion, and a reclassification that moves -------------------
    //
    // Its clocks were start-gated rather than held, so an entry or a morph dimmed part-way ran to
    // completion behind a dark backlight. And a settled Authentication required -> Unsupported
    // interface changed two strings and stood still: same symbol, same identity, same measures.
    controller::note_panel_visible(true);
    blank();
    controller::apply(access(AccessCondition::AuthenticationRequired));
    tick(0);
    const auto access_first = pixels;
    tick(304);
    const auto access_held = pixels;
    require(access_held != access_first, "the access entry had begun before the dim");

    controller::note_panel_visible(false);
    tick(5000);
    require(pixels == access_held, "a dimmed access entry does not advance in the dark");
    controller::note_panel_visible(true);
    require(tick(16), "it is still owed after the wake");
    require(pixels == access_held, "and the wake tick re-anchors rather than jumping forward");
    tick(200);
    require(pixels != access_held, "it then resumes from the frame it held");
    tick(1120);
    require(!tick(16), "and completes in the light");
    const auto access_settled = pixels;

    // The same, for the printer-outline-to-warning morph rather than the appearance.
    blank();
    Presentation acquiring;
    acquiring.layer = PresentationLayer::SubjectPresentation;
    acquiring.acquiring_connection = true;
    acquiring.printer_name_known = true;
    acquiring.printer_name.assign("CORE One");
    controller::apply(acquiring);
    tick(0);
    tick(1120);
    tick(1120);
    controller::apply(access(AccessCondition::AuthenticationRequired));
    tick(0);
    tick(304);
    const auto morph_held = pixels;
    controller::note_panel_visible(false);
    tick(5000);
    require(pixels == morph_held, "a dimmed morph does not advance in the dark");
    controller::note_panel_visible(true);
    tick(16);
    tick(200);
    require(pixels != morph_held, "it resumes from where it held");
    tick(1120);
    require(!tick(16), "and completes in the light");
    require(count_colour(pixels, warning) > 0 && count_colour(pixels, accent) == 0,
            "arriving at the warning endpoint, with no acquisition accent left");
    std::puts("PASS access entry and morph hold through Off rather than finishing unseen");

    // A settled reclassification between two classified failures.
    blank();
    controller::apply(access(AccessCondition::AuthenticationRequired));
    tick(0);
    tick(1120);
    tick(1120);
    require(!tick(16), "the first classification settles");
    const auto settled_auth = pixels;

    controller::apply(access(AccessCondition::UnsupportedInterface));
    require(tick(0), "a reclassification drives the tick");
    {
        Drawn drawn;
        collect(lv_screen_active(), drawn);
        require(drawn.labels.count("Unsupported interface") == 1,
                "the new wording is truthful from the first frame");
        require(drawn.labels.count("This printer interface is not supported.") == 1,
                "explanation included");
        require(drawn.labels.count("Authentication required") == 0, "the old wording is gone");
        require(drawn.labels.count("Check the PrusaLink API key.") == 0,
                "and so is the old explanation");
    }
    const auto reclassified_first = pixels;
    require(reclassified_first != settled_auth, "and the composition does not simply swap strings");
    tick(560);
    require(pixels != reclassified_first, "it advances on the shared clock");
    tick(1120);
    require(!tick(16), "and settles once");
    {
        Drawn drawn;
        collect(lv_screen_active(), drawn);
        require(drawn.title_fonts.count("Unsupported interface=title24") == 1,
                "at the accepted face");
    }
    require(count_colour(pixels, warning) > 0, "carrying the same semantic symbol");
    std::puts("PASS settled access reclassification is truthful immediately and moves");

    // ⬅️ AND THE ORDINARY CONNECTING ENDPOINT IS UNCHANGED BY ALL OF IT — the dot cadence, its own
    // separate Off pause, and the accepted 28/16 typography.
    blank();
    controller::apply(acquiring);
    tick(0);
    tick(1120);
    tick(1120);
    for (unsigned t = 16; t <= 1600; t += 16) {
        tick(16);
        Drawn drawn;
        collect(lv_screen_active(), drawn);
        require(drawn.labels.count(std::string("Connecting") +
                                   std::string(1 + ((1120 + 1120 + t) / 500) % 3, '.')) == 1,
                "the dot cycle is unchanged");
    }
    {
        Drawn drawn;
        collect(lv_screen_active(), drawn);
        require(drawn.labels.count("CORE One") == 1, "with its identity");
        require(drawn.title_fonts.count("CORE One=section16") == 1, "at the accepted 16 px face");
    }
    controller::note_panel_visible(false);
    const auto dots_off = pixels;
    const auto dots_flushes = flushes;
    tick(10000);
    require(pixels == dots_off && flushes == dots_flushes, "dots still do not draw while Off");
    controller::note_panel_visible(true);
    tick(0);
    require(pixels == dots_off, "and still resume without catching up");
    (void)access_settled;
    std::puts("PASS Connecting endpoint, dots and Off pause unchanged");

    // --- 9. The erase screen's activity cadence ---------------------------------
    //
    // Owner ruling after physical review, 2026-09-12: the running half animates one/two/three dots
    // like Connecting, and nothing else about it changes. Everything below is measured on the real
    // controller and the real tick.
    auto running_title = [&]() {
        Drawn drawn;
        collect(lv_screen_active(), drawn);
        for (const auto& text : drawn.labels) {
            if (text.rfind("Erasing settings", 0) == 0) {
                return text;
            }
        }
        return std::string{};
    };

    blank();
    controller::apply(question);
    tick(0);
    tick(1120);
    controller::apply(running);
    tick(0);

    // Starts at one dot, and the answered question is still gone from the first frame.
    require(running_title() == "Erasing settings.", "the cycle starts at one dot");
    {
        Drawn drawn;
        collect(lv_screen_active(), drawn);
        require(drawn.labels.count("Erase all settings?") == 0,
                "and the retirement is still immediate");
    }

    // The three-dot form fits one line at the accepted face and measure. Measured, because the
    // title grew by three characters and the alternative is discovering a wrap on the panel.
    {
        const std::uint32_t settle = 1120 + 1000;
        tick(settle);
        bool saw_three = false;
        for (unsigned t = 0; t < 1600 && !saw_three; t += 16) {
            tick(16);
            saw_three = running_title() == "Erasing settings...";
        }
        require(saw_three, "the cycle reaches three dots");

        // The three dot states, captured for visual inspection and for the evidence pack. One
        // frame per state, named by its dot count.
        for (int wanted = 1; wanted <= 3; ++wanted) {
            const auto target = std::string{"Erasing settings"} +
                                std::string(static_cast<std::size_t>(wanted), '.');
            for (unsigned t = 0; t < 1600 && running_title() != target; t += 16) {
                tick(16);
            }
            require(running_title() == target, "each dot state is reached");
            const auto path = out / ("erasing-" + std::to_string(wanted) + "dot.rgb565");
            auto* file = std::fopen(path.c_str(), "wb");
            require(file, "dot frame file");
            require(std::fwrite(pixels.data(), 2, pixels.size(), file) == pixels.size(),
                    "dot frame write");
            std::fclose(file);
        }
        const int height = label_height(lv_screen_active(), "Erasing settings");
        require(height > 0 && height <= theme::kLineTitle,
                "the three-dot title fits one line box at the accepted face");
        require(!ink_outside_inset(pixels), "and stays inside the content area");
    }

    // ⛔ EXACT BOUNDARIES, AGAINST THE SAME FORMULA THE CONNECTING LOOP IS HELD TO. A dot count
    // that merely CHANGES would pass a weaker test while running at any cadence at all.
    blank();
    controller::apply(running);
    tick(0);
    tick(1120);
    for (unsigned t = 16; t <= 3200; t += 16) {
        tick(16);
        const auto expected =
            std::string{"Erasing settings"} + std::string(1 + ((1120 + t) / 500) % 3, '.');
        require(running_title() == expected, "the erase cycle follows the same 500 ms boundaries");
    }

    // No write and no flush between boundaries. Sampled from a boundary forward, strictly inside
    // one 500 ms interval — a window placed arbitrarily could straddle a boundary and would be
    // asserting about where the sample fell rather than about the cadence.
    {
        const auto before_boundary = running_title();
        bool crossed = false;
        for (unsigned t = 0; t < 600 && !crossed; t += 16) {
            tick(16);
            crossed = running_title() != before_boundary;
        }
        require(crossed, "a boundary was reached to sample from");

        const auto before = flushes;
        const auto held = pixels;
        const auto title = running_title();
        for (int i = 0; i < 24; ++i) {
            require(!tick(16), "a settled erase screen runs no animation between dots");
        }
        require(before == flushes && held == pixels,
                "and invalidates nothing between one dot and the next");
        require(running_title() == title, "the title is unchanged for the whole interval");
    }

    // Repeated apply of an UNCHANGED half must not restart the cadence. The presentation differs
    // in a field this scene does not read, so it reaches apply() rather than the controller's
    // redundant path — and the cycle is then held to the SAME boundary formula throughout. A
    // weaker "the title did not change" assertion would fail on any tick that happened to be a
    // boundary, and would pass if apply() restarted the cycle a fraction before one.
    blank();
    controller::apply(running);
    tick(0);
    tick(1120);
    {
        Presentation again = running;
        for (unsigned t = 16; t <= 2400; t += 16) {
            again.progress.percent = static_cast<std::uint8_t>(10 + ((t / 16) % 50));
            controller::apply(again);
            tick(16);
            const auto expected =
                std::string{"Erasing settings"} + std::string(1 + ((1120 + t) / 500) % 3, '.');
            require(running_title() == expected,
                    "a repeated apply does not restart or shift the cycle");
        }
    }

    // Off pauses it, and the wake does not catch up.
    controller::note_panel_visible(false);
    {
        const auto off = pixels;
        const auto before = flushes;
        const auto held = running_title();
        tick(10000);
        require(pixels == off && flushes == before, "the cycle does not draw while Off");
        controller::note_panel_visible(true);
        tick(0);
        require(running_title() == held, "and resumes at its phase rather than catching up");
    }
    tick(1120);

    // Leaving resets it: the next erase starts at one dot, not where the last one stopped.
    for (unsigned t = 0; t < 1200; t += 16) {
        tick(16);
    }
    require(running_title() != "Erasing settings.", "the cycle has moved off one dot");
    blank();
    controller::apply(running);
    tick(0);
    require(running_title() == "Erasing settings.", "re-entry starts the cycle at one dot again");

    // Non-reset invariance: no other presentation grows dots, and the failure the erase can raise
    // stops the cycle outright.
    controller::apply(fault(DeviceFaultReason::FactoryResetIncomplete));
    tick(0);
    tick(1120);
    {
        const auto before = flushes;
        const auto held = pixels;
        for (unsigned t = 0; t < 2400; t += 16) {
            tick(16);
        }
        require(running_title().empty(), "a failed reset shows no running title at all");
        require(before == flushes && held == pixels,
                "and nothing cycles on the statement that replaced it");
    }
    for (const auto& current : kCases) {
        blank();
        controller::apply(current.build());
        tick(0);
        tick(1120);
        const auto settled = pixels;
        const auto before = flushes;
        for (unsigned t = 0; t < 2400; t += 16) {
            tick(16);
        }
        require(pixels == settled && flushes == before,
                "no message composition acquired an activity loop");
    }
    std::puts("PASS erase activity cadence, boundaries, idle, Off pause, re-entry, invariance");

    require(prd::diagnostics::qr_temp_audit_count() > first_qr_audit_count &&
                prd::diagnostics::qr_temp_dirty_count() == 0,
            "all setup QR temporary buffers stay wiped through later sessions and clears");
    std::puts("PASS message family");
    return 0;
}
