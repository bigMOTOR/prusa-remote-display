// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

// The actual compile-time instrument -> Store -> production controller -> LVGL pixels.
// Virtual wall time runs unscaled product timers; this is not board cadence/backlight proof.
#include "../support/display_timer.hpp"
#include "app_model/presentation.hpp"
#include "controller.hpp"
#include "fonts/fonts.hpp"
#include "ui/ui.hpp"
#include "validation/validation.hpp"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace {
constexpr int kWidth = 320, kHeight = 240, kRows = prd::ui::kDefaultDrawBufferRows;
std::array<std::uint16_t, kWidth * kHeight> pixels{};
alignas(4) std::array<std::uint8_t, kWidth * kRows * 2> draw_a{}, draw_b{};
void require(bool ok, const char* message) {
    if (!ok) {
        std::fprintf(stderr, "FAIL %s\n", message);
        std::abort();
    }
}
void flush(lv_display_t* display, const lv_area_t* area, std::uint8_t* data) {
    require(area->x1 >= 0 && area->x2 < kWidth && area->y1 >= 0 && area->y2 < kHeight,
            "flush bounds");
    const auto* input = reinterpret_cast<std::uint16_t*>(data);
    const int width = area->x2 - area->x1 + 1;
    for (int y = area->y1; y <= area->y2; ++y)
        for (int x = area->x1; x <= area->x2; ++x)
            pixels[y * kWidth + x] = input[(y - area->y1) * width + x - area->x1];
    lv_display_flush_ready(display);
}
void save(const std::string& path) {
    FILE* file = std::fopen(path.c_str(), "wb");
    require(file != nullptr, "frame opens");
    require(std::fwrite(pixels.data(), 2, pixels.size(), file) == pixels.size(), "frame writes");
    std::fclose(file);
}
prd::app_model::Timestamp at(unsigned ms) {
    return std::chrono::milliseconds{ms};
}
} // namespace

int main(int argc, char** argv) {
    using namespace prd;
    using app_model::DisplayPower;
    if (argc == 2 && std::string{argv[1]} == "--rapid-controls") {
        app_store::Store control;
        validation::printing_timeline_begin(control, at(1000));
        for (int i = 0; i < 100; ++i)
            validation::printing_timeline_on_gesture(app_model::ButtonGesture::ShortPress);
        validation::printing_timeline_tick(control, at(1000));
        validation::printing_timeline_tick(control, at(6000));
        require(app_model::project(control.state()).subject == app_model::SubjectState::Ready,
                "rapid actual-flow controls finish in Ready after the clock floor");
        std::puts("PASS rapid actual-flow controls; completion must be labelled INCOMPLETE");
        return 0;
    }
    require(argc == 2, "output directory");
    const std::string out = argv[1];
    lv_init();
    require(ui::fonts::verify(), "font metrics");
    lv_log_register_print_cb([](lv_log_level_t, const char* message) {
        std::fputs(message, stderr);
        std::abort();
    });
    auto* display = lv_display_create(kWidth, kHeight);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(display, draw_a.data(), draw_b.data(), draw_a.size(),
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, flush);
    require(ui::controller::build(display), "real controller builds");
    app_store::Store store;
    test::DisplayTimer timer;
    validation::printing_timeline_begin(store, at(1000));
    require(validation::printing_timeline_compiled(), "instrument selected");
    unsigned losses = 0, recoveries = 0, degraded = 0, final_at = 0, off_at = 0;
    unsigned entry_at = 0, recovery_at = 0, frames = 0;
    bool prior_loss = false, prior_stale = false, true_off_seen = false;
    auto power = DisplayPower::On;
    auto p = app_model::project(store.state());
    for (unsigned now = 1000; now <= 1400000; now += 20) {
        lv_tick_inc(20);
        if (now % 100 == 0) {
            // app_core display_tick runs BEFORE the timeline; controller application follows.
            timer.tick(store, at(now));
            validation::printing_timeline_tick(store, at(now));
            p = app_model::project(store.state());
            if (now >= 2000)
                require(store.state().last_event_at <= at(now), "no invented future clock");
            if (p.observation_lost && !prior_loss) {
                ++losses;
                entry_at = now;
                std::printf("LOSS %u at=%u\n", losses, now);
            }
            if (!p.observation_lost && prior_loss) {
                ++recoveries;
                recovery_at = now;
                std::printf("RECOVERY %u at=%u delta=%u\n", recoveries, now, now - entry_at);
                if (recoveries == 4)
                    require(true_off_seen, "fourth recovery follows completed Off fade");
            }
            if (p.stale_indicator && !prior_stale) {
                ++degraded;
                std::printf("UNACCEPTED_DEGRADED %u at=%u\n", degraded, now);
            }
            prior_loss = p.observation_lost;
            prior_stale = p.stale_indicator;
            if (store.state().display.power != power) {
                power = store.state().display.power;
                if (power == DisplayPower::Off)
                    off_at = now;
                std::printf("POWER %.*s at=%u\n",
                            static_cast<int>(app_model::log_label(power).size()),
                            app_model::log_label(power).data(), now);
            }
            const bool visible = power != DisplayPower::Off || now - off_at < 2000;
            if (!visible && recoveries == 3)
                true_off_seen = true;
            ui::controller::note_panel_visible(visible);
            ui::controller::apply(p);
            if (recoveries == 4 && !p.observation_lost &&
                p.subject == app_model::SubjectState::Ready && final_at == 0)
                final_at = now;
            if (final_at != 0) {
                require(p.confidence == app_model::Confidence::Current && !p.stale_indicator &&
                            !p.observation_lost && p.subject == app_model::SubjectState::Ready,
                        "post-run Ready stays current, no unexpected screen");
            }
        }
        (void)ui::controller::step_acknowledgement();
        lv_refr_now(display);
        if (entry_at != 0 && now - entry_at <= 2400 && (now - entry_at) % 80 == 0) {
            save(out + "/loss-" + std::to_string(losses) + "-" + std::to_string(now - entry_at) +
                 ".rgb565");
            ++frames;
        }
        if (recovery_at != 0 && now - recovery_at <= 1200 && (now - recovery_at) % 80 == 0) {
            save(out + "/recovery-" + std::to_string(recoveries) + "-" +
                 std::to_string(now - recovery_at) + ".rgb565");
            ++frames;
        }
        // Preserve the disputed intermediate screen as evidence, not as a visual baseline.
        if (p.stale_indicator && now % 10000 == 0)
            save(out + "/unaccepted-degraded-" + std::to_string(degraded) + ".rgb565");
        if (final_at != 0 && now - final_at >= 600000) {
            require(losses == 4 && recoveries == 4 && degraded >= 4,
                    "all four chapters and ageing windows ran");
            require(power == DisplayPower::Off, "final Ready automatically receded to Off");
            save(out + "/final-ready.rgb565");
            lv_mem_monitor_t memory{};
            lv_mem_monitor(&memory);
            std::printf("PASS actual instrument 4 losses/4 recoveries, post-run=%u ms, "
                        "frames=%u pool_size=%zu max_used=%zu final_at=%u\n",
                        now - final_at, frames, memory.total_size, memory.max_used, final_at);
            return 0;
        }
    }
    require(false, "instrument did not finish in bounded wall time");
}
