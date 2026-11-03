// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only
// Native renderer regression: links the actual firmware sources and prepared pinned LVGL.
// Host-only instrumentation, never linked into target firmware.
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <vector>

#include "lvgl.h"

#include "app_model/events.hpp"
#include "app_model/presentation.hpp"
#include "app_model/reducer.hpp"
#include "app_store/store.hpp"
#include "fake_source/scenarios.hpp"
#include "presentation_diff.hpp"
#include "scenes/operational_form.hpp"
#include "scenes/operational_scene.hpp"
#include "scenes/unavailable_acknowledgement.hpp"
#include "scenes/unavailable_motion.hpp"
#include "status_timeline.hpp"
#include "theme/theme.hpp"
#include "ui/ui.hpp"

namespace {

constexpr std::int32_t kW = 320;
constexpr std::int32_t kH = 240;
constexpr std::int32_t kRows = prd::ui::kDefaultDrawBufferRows;
constexpr std::size_t kBufBytes =
    static_cast<std::size_t>(kW) * static_cast<std::size_t>(kRows) * sizeof(std::uint16_t);

std::vector<std::uint16_t> g_frame(static_cast<std::size_t>(kW) * kH, 0);
unsigned long g_flushes = 0;

void flush_cb(lv_display_t* disp, const lv_area_t* area, std::uint8_t* px_map) {
    ++g_flushes;
    const auto* src = reinterpret_cast<const std::uint16_t*>(px_map);
    const std::int32_t w = area->x2 - area->x1 + 1;
    for (std::int32_t y = area->y1; y <= area->y2 && y < kH; ++y) {
        for (std::int32_t x = area->x1; x <= area->x2 && x < kW; ++x) {
            g_frame[static_cast<std::size_t>(y) * kW + static_cast<std::size_t>(x)] =
                src[(y - area->y1) * w + (x - area->x1)];
        }
    }
    lv_display_flush_ready(disp);
}

std::uint64_t digest() {
    std::uint64_t h = 1469598103934665603ULL;
    for (const std::uint16_t px : g_frame) {
        h = (h ^ static_cast<std::uint8_t>(px & 0xFFU)) * 1099511628211ULL;
        h = (h ^ static_cast<std::uint8_t>(px >> 8)) * 1099511628211ULL;
    }
    return h;
}

void write_ppm(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (f == nullptr) {
        return;
    }
    std::fprintf(f, "P6\n%d %d\n255\n", kW, kH);
    for (const std::uint16_t c : g_frame) {
        const unsigned r = ((c >> 11) & 0x1FU) * 255U / 31U;
        const unsigned g = ((c >> 5) & 0x3FU) * 255U / 63U;
        const unsigned b = (c & 0x1FU) * 255U / 31U;
        const unsigned char px[3] = {static_cast<unsigned char>(r), static_cast<unsigned char>(g),
                                     static_cast<unsigned char>(b)};
        std::fwrite(px, 1, 3, f);
    }
    std::fclose(f);
}

prd::app_model::Timestamp at(std::int64_t ms) {
    return std::chrono::duration_cast<prd::app_model::Timestamp>(std::chrono::milliseconds{ms});
}

// Ink extent of the painted frame, so a disappearance or a jump is measured rather than eyeballed.
struct Ink {
    bool any{false};
    std::int32_t x1{kW}, y1{kH}, x2{-1}, y2{-1};
    std::uint32_t pixels{0};
    double cx{0}, cy{0};
};

// The FORM's own painted extent, measured only in the rows the production geometry says the form
// occupies. The copy is a separate component of the composition and would otherwise be counted in
// the width, which is the quantity under review.
struct FormInk {
    bool any{false};
    std::int32_t left{kW}, right{-1};
    std::int32_t top{kH}, bottom{-1};
    std::uint32_t pixels{0};
    double cx{0.0}, cy{0.0};
    std::int32_t width() const {
        return any ? right - left + 1 : 0;
    }
};

FormInk form_ink_of(std::uint16_t background, std::int32_t row_top, std::int32_t row_bottom) {
    FormInk f;
    const std::int32_t y0 = std::max<std::int32_t>(0, row_top);
    const std::int32_t y1 = std::min<std::int32_t>(kH - 1, row_bottom);
    for (std::int32_t y = y0; y <= y1; ++y) {
        for (std::int32_t x = 0; x < kW; ++x) {
            const auto px = g_frame[static_cast<std::size_t>(y) * kW + x];
            const unsigned r = ((px >> 11) & 31U) * 255U / 31U;
            const unsigned g = ((px >> 5) & 63U) * 255U / 63U;
            const unsigned b = (px & 31U) * 255U / 31U;
            if (px == background || r <= g || r <= b) {
                continue;
            }
            f.any = true;
            f.left = std::min(f.left, x);
            f.right = std::max(f.right, x);
            f.top = std::min(f.top, y);
            f.bottom = std::max(f.bottom, y);
            ++f.pixels;
            f.cx += x;
            f.cy += y;
        }
    }
    if (f.pixels > 0) {
        f.cx /= f.pixels;
        f.cy /= f.pixels;
    }
    return f;
}

Ink ink_of(std::uint16_t background) {
    Ink k;
    double sx = 0, sy = 0;
    for (std::int32_t y = 0; y < kH; ++y) {
        for (std::int32_t x = 0; x < kW; ++x) {
            if (g_frame[static_cast<std::size_t>(y) * kW + x] == background) {
                continue;
            }
            k.any = true;
            k.x1 = std::min(k.x1, x);
            k.x2 = std::max(k.x2, x);
            k.y1 = std::min(k.y1, y);
            k.y2 = std::max(k.y2, y);
            ++k.pixels;
            sx += x;
            sy += y;
        }
    }
    if (k.pixels != 0) {
        k.cx = sx / k.pixels;
        k.cy = sy / k.pixels;
    }
    return k;
}

} // namespace

int main(int argc, char** argv) {
    // argv: <origin 0..5 or -1 for all> <ppm-dir or "-"> [ppm-origin]
    const int origin_arg = (argc > 1) ? std::atoi(argv[1]) : -1;
    const std::string ppm_dir = (argc > 2) ? argv[2] : "-";
    const int ppm_origin = (argc > 3) ? std::atoi(argv[3]) : 0;

    lv_init();
    lv_log_register_print_cb([](lv_log_level_t, const char* message) {
        std::fputs(message, stderr);
        std::abort(); // Warn-level harness: never spin on a failed allocation.
    });
    auto* buf0 = static_cast<std::uint8_t*>(std::malloc(kBufBytes));
    auto* buf1 = static_cast<std::uint8_t*>(std::malloc(kBufBytes));
    lv_display_t* disp = lv_display_create(kW, kH);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(disp, buf0, buf1, static_cast<std::uint32_t>(kBufBytes),
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(disp, flush_cb);

    lv_obj_t* const screen = lv_display_get_screen_active(disp);
    lv_obj_remove_style_all(screen);
    lv_obj_set_style_bg_color(screen, prd::ui::theme::colour(prd::ui::theme::Colour::Background),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_opa(screen, prd::ui::theme::opacity(prd::ui::theme::Opacity::Full),
                            LV_PART_MAIN);

    prd::ui::scene::OperationalScene operational;
    if (!operational.build(screen)) {
        std::fprintf(stderr, "operational build failed\n");
        return 1;
    }
    prd::ui::scene::UnavailableAcknowledgement ack;
    if (!ack.build(screen)) {
        std::fprintf(stderr, "ack build failed\n");
        return 1;
    }

    // the fatal lifecycle, through the real product path, to step 16
    prd::app_store::Store store;
    const std::size_t prelude =
        prd::fake_source::step_count(prd::fake_source::Scenario::ColdBootToReady);
    for (std::size_t i = 0; i < prelude; ++i) {
        (void)store.dispatch(
            prd::fake_source::step_at(prd::fake_source::Scenario::ColdBootToReady, i, at(0), 0)
                .event);
    }
    operational.set_visible(true);
    prd::app_model::Presentation previous{};
    for (std::size_t i = 0; i <= 15; ++i) {
        const auto step = prd::validation::status_timeline_step_at(i);
        (void)prd::validation::apply_status_body(store, step.body, at(1000 + step.offset_ms),
                                                 {1, static_cast<std::uint32_t>(i)});
        const auto view = prd::app_model::project(store.state());
        operational.apply(view, prd::ui::diff(previous, view));
        static_cast<void>(operational.step(true, lv_tick_get()));
        static_cast<void>(
            operational.step(true, lv_tick_get() + prd::ui::theme::kOperationalMotionMs));
        previous = view;
        lv_tick_inc(16);
        lv_refr_now(disp);
        std::printf("OPERATIONAL %zu dg=%016llx\n", i, (unsigned long long)digest());
    }
    operational.set_visible(false);

    const std::uint16_t bg = g_frame[0];

    using LOF = prd::app_model::LossOriginForm;
    const LOF all[] = {LOF::Completed, LOF::Ready,     LOF::Printing,
                       LOF::Paused,    LOF::Preparing, LOF::Stopped};
    const char* names[] = {"Completed", "Ready", "Printing", "Paused", "Preparing", "Stopped"};

    const int first = (origin_arg >= 0) ? origin_arg : 0;
    const int last = (origin_arg >= 0) ? origin_arg : 5;
    const unsigned sample_ms = argc > 4 ? std::max(1, std::atoi(argv[4])) : 16;

    std::uint32_t peak_used = 0, min_free_big = 0xFFFFFFFFU, peak_frag = 0;
    std::uint32_t peak_used_at_ms = 0;
    const char* peak_origin = "";
    std::uint32_t peak_hiwater = 0, hiwater_at_ms = 0;
    const char* hiwater_origin = "";

    for (int oi = first; oi <= last; ++oi) {
        const LOF origin = all[oi];
        std::set<std::int32_t> radii;
        unsigned frames = 0, seg_frames = 0, arc_frames = 0, blank = 0;
        std::int32_t rmax = 0;
        Ink prev{};
        double max_centre_jump = 0.0;
        std::uint32_t max_jump_at = 0;

        for (std::uint32_t e = 0; e <= 2500; e += sample_ms) {
            const auto phase = prd::ui::scene::phase_at(origin, e);
            ack.apply(phase);
            lv_tick_inc(16);
            lv_refr_now(disp);
            ++frames;
            radii.insert(phase.form.radius);
            rmax = std::max(rmax, phase.form.radius);

#if defined(PRD_LEGACY_ACK)
            const bool seg = false;
#else
            const bool seg =
                ack.rasteriser() == prd::ui::scene::UnavailableAcknowledgement::Rasteriser::Segment;
#endif
            seg ? ++seg_frames : ++arc_frames;

            std::uint32_t used = 0;
            std::uint32_t big = 0;
            std::uint32_t frag = 0;
            std::uint32_t hiwater = 0;
#if LV_USE_STDLIB_MALLOC == LV_STDLIB_BUILTIN
            lv_mem_monitor_t m{};
            lv_mem_monitor(&m);
            used = static_cast<std::uint32_t>(m.total_size - m.free_size);
            hiwater = static_cast<std::uint32_t>(m.max_used);
            big = static_cast<std::uint32_t>(m.free_biggest_size);
            frag = static_cast<std::uint32_t>(m.frag_pct);
#endif
            if (used > peak_used) {
                peak_used = used;
                peak_used_at_ms = e;
                peak_origin = names[oi];
            }
            // ⛔ LVGL'S OWN HIGH-WATER MARK, NOT AN INTER-FRAME SAMPLE. lv_mem_monitor_t::max_used
            // is maintained inside lv_malloc_core/lv_realloc_core, so it captures the transient
            // radius masks that are allocated and freed WITHIN a frame and that a between-frames
            // reading of `used` cannot see.
            if (hiwater > peak_hiwater) {
                peak_hiwater = hiwater;
                hiwater_at_ms = e;
                hiwater_origin = names[oi];
            }
            min_free_big = std::min(min_free_big, big);
            peak_frag = std::max(peak_frag, frag);

            // Orange ink identifies the form independently of the achromatic status copy.
            const FormInk fi = form_ink_of(bg, 0, kH - 1);
            const Ink k = ink_of(bg);
            if (!k.any) {
                ++blank;
            }
            if (prev.any && k.any) {
                const double dx = k.cx - prev.cx, dy = k.cy - prev.cy;
                const double jump = std::sqrt(dx * dx + dy * dy);
                if (jump > max_centre_jump) {
                    max_centre_jump = jump;
                    max_jump_at = e;
                }
            }
            prev = k;

            std::printf("F %s %u r=%d ext=%d rot=%d sag=%d off=%d alpha=%u rast=%s pts=%u "
                        "used=%u bigfree=%u frag=%u ink=%d,%d..%d,%d px=%u cx=%.2f cy=%.2f "
                        "fl=%d fr=%d fw=%d hi=%u dg=%016llx energy=%d ft=%d fb=%d "
                        "fcx=%.4f fcy=%.4f fpx=%u\n",
                        names[oi], e, phase.form.radius, phase.form.extent, phase.form.rotation,
                        phase.form.sagitta, phase.offset_y, (unsigned)phase.copy_alpha,
                        seg ? "SEG" : "ARC",
#if defined(PRD_LEGACY_ACK)
                        0U,
#else
                        ack.segment_points(),
#endif
                        used, big, frag, k.x1, k.y1, k.x2, k.y2, k.pixels, k.cx, k.cy, fi.left,
                        fi.right, fi.width(), hiwater, (unsigned long long)digest(), phase.energy,
                        fi.top, fi.bottom, fi.cx, fi.cy, fi.pixels);

            if (ppm_dir != "-" && oi == ppm_origin) {
                char nm[1024];
                std::snprintf(nm, sizeof(nm), "%s/%s-%04u.ppm", ppm_dir.c_str(), names[oi], e);
                write_ppm(nm);
            }
        }

        std::printf("ORIGIN %s frames=%u arc=%u seg=%u blank=%u distinct_radii=%zu rmax=%d "
                    "max_centre_jump=%.3fpx@%ums\n",
                    names[oi], frames, arc_frames, seg_frames, blank, radii.size(), rmax,
                    max_centre_jump, max_jump_at);
    }

#if LV_USE_STDLIB_MALLOC == LV_STDLIB_BUILTIN
    std::printf("PEAK used=%u of %u (%.1f%%) at %s %ums; min_biggest_free=%u; max_frag=%u%%\n",
                peak_used, (unsigned)LV_MEM_SIZE, 100.0 * peak_used / LV_MEM_SIZE, peak_origin,
                peak_used_at_ms, min_free_big, peak_frag);
    std::printf("HIWATER lv_mem max_used=%u of %u (%.1f%%) first reached by %s at %ums; "
                "margin=%u B (%.1f%%)\n",
                peak_hiwater, (unsigned)LV_MEM_SIZE, 100.0 * peak_hiwater / LV_MEM_SIZE,
                hiwater_origin, hiwater_at_ms, (unsigned)LV_MEM_SIZE - peak_hiwater,
                100.0 * ((unsigned)LV_MEM_SIZE - peak_hiwater) / LV_MEM_SIZE);
#else
    std::printf("PEAK n/a (C-library allocator); peak_used_sample=%u at %s %ums\n", peak_used,
                peak_origin, peak_used_at_ms);
#endif
    std::printf("flushes=%lu\nclean exit\n", g_flushes);
    return 0;
}
