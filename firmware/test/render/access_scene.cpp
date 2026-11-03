// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

// Actual production controller, not a duplicate scene implementation.
#include "controller.hpp"
#include "fonts/fonts.hpp"
#include "fonts/motion_fonts.hpp"
#include "scene_selection.hpp"
#include "theme/theme.hpp"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <set>
#include <string>

namespace {
constexpr int w = 320, h = 240;
std::array<std::uint16_t, w * h> pixels{};
std::array<std::uint8_t, w * 40 * 2> buffer{};
unsigned flushes = 0;
void require(bool ok, const char* why) {
    if (!ok) {
        std::fprintf(stderr, "FAIL %s\n", why);
        std::abort();
    }
}
void flush(lv_display_t* d, const lv_area_t* a, std::uint8_t* data) {
    ++flushes;
    const auto* input = reinterpret_cast<const std::uint16_t*>(data);
    require(a->x1 >= 0 && a->y1 >= 0 && a->x2 < w && a->y2 < h, "canvas bounds");
    for (int y = a->y1; y <= a->y2; ++y)
        for (int x = a->x1; x <= a->x2; ++x)
            pixels[y * w + x] = input[(y - a->y1) * (a->x2 - a->x1 + 1) + x - a->x1];
    lv_display_flush_ready(d);
}
void collect(lv_obj_t* o, std::set<std::string>& out) {
    if (lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN))
        return;
    if (lv_obj_check_type(o, &lv_label_class)) {
        // These fixtures use ASCII. Text presence alone cannot detect a face lacking letters.
        const auto* font = lv_obj_get_style_text_font(o, LV_PART_MAIN);
        for (const auto ch : std::string{lv_label_get_text(o)}) {
            lv_font_glyph_dsc_t glyph{};
            require(lv_font_get_glyph_dsc(font, &glyph, static_cast<unsigned char>(ch), 0),
                    "every visible fixture character exists in its selected font");
        }
        out.insert(lv_label_get_text(o));
        const std::string text = lv_label_get_text(o);
        if (text.rfind("Connecting", 0) == 0) {
            require(font == prd::ui::fonts::for_role(prd::ui::theme::TextRole::OutcomeTitle),
                    "Connecting restores accepted title font after an error");
        }
        if (text == "Authentication required" || text == "Unsupported interface") {
            require(font == prd::ui::fonts::motion_title(24), "accepted error title font");
        }
        if (text == "Check the PrusaLink API key." ||
            text == "This printer interface is not supported.") {
            require(font == prd::ui::fonts::motion_title(20), "accepted error explanation font");
        }
    }
    for (unsigned i = 0; i < lv_obj_get_child_count(o); ++i)
        collect(lv_obj_get_child(o, i), out);
}
} // namespace
int main(int argc, char** argv) {
    require(argc == 2, "output directory");
    using namespace prd::app_model;
    namespace controller = prd::ui::controller;
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
    require(controller::build(d), "whole controller builds in unchanged pool");
    controller::note_panel_visible(false);
    auto tick = [&](unsigned ms) {
        lv_tick_inc(ms);
        const auto moving = controller::step_acknowledgement();
        lv_refr_now(d);
        const auto background = pixels[0];
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                if (x < 12 || x >= 308 || y < 12 || y >= 228) {
                    require(pixels[y * w + x] == background, "every tick stays inside inset");
                }
            }
        }
        return moving;
    };
    Presentation p;
    p.layer = PresentationLayer::SubjectPresentation;
    p.acquiring_connection = true;
    p.printer_name_known = true;
    p.printer_name.assign("CORE One");
    controller::apply(p);
    tick(0);
    const auto dark = pixels;
    tick(5000);
    require(pixels == dark, "Off entry does not run invisibly");
    controller::note_panel_visible(true);
    tick(0);
    const char* titles[] = {"Connecting...", "Authentication required", "Unsupported interface",
                            "Connecting..."};
    for (int kind = 0; kind < 4; ++kind) {
        // Leave through actual blank selection so each independent entry has a fresh clock.
        Presentation blank;
        blank.layer = PresentationLayer::SystemLifecycle;
        controller::apply(blank);
        tick(0);
        p.layer = kind == 1 || kind == 2 ? PresentationLayer::AccessCondition
                                         : PresentationLayer::SubjectPresentation;
        p.access_condition = kind == 1   ? AccessCondition::AuthenticationRequired
                             : kind == 2 ? AccessCondition::UnsupportedInterface
                                         : AccessCondition::None;
        p.printer_name_known = kind != 3;
        controller::apply(p);
        tick(0);
        require(controller::report().scene == "access", "actual scene selected");
        auto first = pixels;
        for (unsigned t = 16; t <= 1120; t += 16) {
            tick(16);
            const auto path = std::filesystem::path(argv[1]) /
                              (std::to_string(kind) + "-" + std::to_string(t) + ".rgb565");
            auto* file = std::fopen(path.c_str(), "wb");
            require(file, "frame file");
            require(std::fwrite(pixels.data(), 2, pixels.size(), file) == pixels.size(),
                    "frame write");
            std::fclose(file);
            auto bg = pixels[0];
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                    if (x < 12 || x >= 308 || y < 12 || y >= 228)
                        require(pixels[y * w + x] == bg, "all ink inside inset");
        }
        require(first != pixels, "actual controller advances entry");
        std::set<std::string> labels;
        collect(lv_screen_active(), labels);
        require(labels.count(titles[kind]) == 1, "correct truthful title");
        require(labels.count("CORE One") == unsigned(kind != 3), "unknown identity absent");
        require(labels.count("Printing") == 0 && labels.count("Nozzle") == 0 &&
                    labels.count("Bed") == 0,
                "no operational data leaks");
        auto before = flushes;
        for (int i = 0; i < 20; ++i) {
            controller::apply(p);
            require(!tick(16), "settled no active animation");
        }
        require(before == flushes, "settled repeated presentation draws nothing");
        std::printf("PASS native entry %d, identity, bounds, no telemetry, idle\n", kind);
    }
    // A classified failure changing during entry cannot restart from zero or settle early.
    Presentation blank;
    blank.layer = PresentationLayer::SystemLifecycle;
    controller::apply(blank);
    p.layer = PresentationLayer::AccessCondition;
    p.access_condition = AccessCondition::AuthenticationRequired;
    controller::apply(p);
    tick(0);
    tick(320);
    p.access_condition = AccessCondition::UnsupportedInterface;
    controller::apply(p);
    require(tick(16), "mid-entry reclassification keeps clock running");
    tick(1100);
    // Service -> real Ready must use the actual operational appearance path.
    p = {};
    p.layer = PresentationLayer::SubjectPresentation;
    p.subject = SubjectState::Ready;
    p.confidence = Confidence::Current;
    controller::apply(p);
    require(tick(0), "service to Ready uses animated appearance");
    tick(1120);
    require(controller::report().scene == "operational", "Ready destination");
    std::puts("PASS interruption clock and operational return");

    // Actual settled Connecting -> access failure and the reverse, on the same scene.
    // The new title is truthful immediately; the semantic icon changes on the shared clock.
    p = {};
    p.layer = PresentationLayer::SubjectPresentation;
    p.acquiring_connection = true;
    controller::apply(p);
    tick(0);
    tick(1120);
    for (int edge = 4; edge < 6; ++edge) {
        p.layer =
            edge == 4 ? PresentationLayer::AccessCondition : PresentationLayer::SubjectPresentation;
        p.access_condition =
            edge == 4 ? AccessCondition::AuthenticationRequired : AccessCondition::None;
        controller::apply(p);
        require(tick(0), "semantic icon transition drives actual tick");
        const auto start = pixels;
        for (unsigned t = 16; t <= 1120; t += 16) {
            tick(16);
            const auto path = std::filesystem::path(argv[1]) /
                              (std::to_string(edge) + "-" + std::to_string(t) + ".rgb565");
            auto* file = std::fopen(path.c_str(), "wb");
            require(file, "transition frame file");
            require(std::fwrite(pixels.data(), 2, pixels.size(), file) == pixels.size(),
                    "transition frame write");
            std::fclose(file);
        }
        require(start != pixels, "icon moves rather than just changing the label");
        require(!tick(16), "icon transition stops at settlement");
        std::set<std::string> restored_labels;
        collect(lv_screen_active(), restored_labels);
    }
    std::puts("PASS both semantic icon transitions");
    // Owner-approved dots: one update per half-second, fixed text origin, Off pause, failure stop.
    controller::apply(blank);
    p = {};
    p.layer = PresentationLayer::SubjectPresentation;
    p.acquiring_connection = true;
    p.printer_name_known = true;
    p.printer_name.assign("CORE One");
    controller::apply(p);
    tick(0);
    for (unsigned t = 16; t <= 3200; t += 16) {
        tick(16);
        std::set<std::string> current;
        collect(lv_screen_active(), current);
        require(current.count(std::string("Connecting") + std::string(1 + (t / 500) % 3, '.')),
                "dots follow one-two-three cycle without resetting on apply");
        controller::apply(p);
        const auto path =
            std::filesystem::path(argv[1]) / ("dots-" + std::to_string(t) + ".rgb565");
        auto* file = std::fopen(path.c_str(), "wb");
        require(file, "dots frame file");
        require(std::fwrite(pixels.data(), 2, pixels.size(), file) == pixels.size(), "dots frame");
        std::fclose(file);
    }
    controller::note_panel_visible(false);
    const auto off = pixels;
    const auto before_off = flushes;
    tick(10000);
    require(pixels == off && flushes == before_off, "dots do not draw while Off");
    controller::note_panel_visible(true);
    tick(0);
    require(pixels == off, "dots resume without catching up through invisible time");
    p.layer = PresentationLayer::AccessCondition;
    p.access_condition = AccessCondition::AuthenticationRequired;
    controller::apply(p);
    tick(1120);
    tick(1120);
    const auto stopped = pixels;
    const auto before_stopped = flushes;
    tick(3000);
    require(pixels == stopped && flushes == before_stopped, "failure stops dots and stays idle");
    std::puts("PASS Connecting dots, unchanged presentation, Off pause, warning idle");
}
