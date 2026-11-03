// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include "app_model/presentation.hpp"
#include "controller.hpp"
#include "display_timer.hpp"
#include "validation/validation.hpp"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <string>
namespace {
std::array<std::uint16_t, 320 * 240> pixels{};
alignas(4) std::array<std::uint8_t, 320 * 40 * 2> draw{};
void require(bool v, const char* why) {
    if (!v) {
        std::fprintf(stderr, "FAIL %s\n", why);
        std::abort();
    }
}
void flush(lv_display_t* d, const lv_area_t* a, std::uint8_t* data) {
    require(a->x1 >= 0 && a->y1 >= 0 && a->x2 < 320 && a->y2 < 240, "flush bounds");
    auto* in = reinterpret_cast<std::uint16_t*>(data);
    for (int y = a->y1; y <= a->y2; ++y)
        for (int x = a->x1; x <= a->x2; ++x)
            pixels[y * 320 + x] = in[(y - a->y1) * (a->x2 - a->x1 + 1) + x - a->x1];
    lv_display_flush_ready(d);
}
void save(const std::string& path) {
    auto* f = std::fopen(path.c_str(), "wb");
    require(f, "file");
    require(std::fwrite(pixels.data(), 2, pixels.size(), f) == pixels.size(), "write");
    std::fclose(f);
}
} // namespace
int main(int argc, char** argv) {
    using namespace prd;
    using namespace app_model;
    require(argc == 2, "output directory");
    lv_init();
    lv_log_register_print_cb([](lv_log_level_t, const char* msg) {
        std::fputs(msg, stderr);
        std::abort();
    });
    auto* d = lv_display_create(320, 240);
    lv_display_set_color_format(d, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(d, draw.data(), nullptr, draw.size(), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(d, flush);
    require(ui::controller::build(d), "controller fits unchanged pool");
    app_store::Store store;
    test::DisplayTimer timer;
    auto at = [](unsigned ms) -> Timestamp { return std::chrono::milliseconds{ms}; };
    validation::printing_timeline_begin(store, at(1000));
    require(validation::printing_timeline_compiled(), "synthetic lease active");
    for (auto gesture :
         {ButtonGesture::ShortPress, ButtonGesture::LongPress, ButtonGesture::VeryLongPress}) {
        require(validation::gesture_is_consumed(gesture), "no product gestures");
        for (int i = 0; i < 100; ++i)
            validation::printing_timeline_on_gesture(gesture);
    }
    auto power = DisplayPower::On;
    unsigned off_at = 0, frames = 0;
    std::uint32_t first_loss = 0;
    for (unsigned now = 1000; now <= 763000; now += 20) {
        lv_tick_inc(20);
        if (now % 100 == 0) {
            timer.tick(store, at(now));
            namespace policy = app_model::display_policy;
            const auto& st = store.state();
            const auto tier = policy::output_tier_for(policy::holds_awake(st), st.subject,
                                                      st.confidence, st.display.power);
            const policy::Levels levels{65, 50, 38};
            const auto before = store.revision();
            const auto output =
                validation::access_review_output(st.display.power, tier, levels, policy::Timings{});
            require(before == store.revision(), "emitter does not dispatch");
            if (now <= 163000)
                require(output.target_percent == (tier == policy::Tier::Active ? 65 : 50) &&
                            output.fade_ms == 0,
                        "review output readable");
            else
                require(output ==
                            policy::output_for(st.display.power, tier, levels, policy::Timings{}),
                        "post-final ordinary output");
            if (now == 93100)
                require(st.display.power == DisplayPower::Dimmed, "real retirement remains");
            if (now >= 93100 && now < 127000)
                require(!project(st).observation_lost_acknowledgement,
                        "retired loss never replays during failed retry");
            require(st.display.last_interaction_at == Timestamp{}, "no fabricated interaction");
            validation::printing_timeline_tick(store, at(now));
            auto p = project(store.state());
            const unsigned chapter = (now - 1000) / 18000;
            if (chapter == 0 || chapter == 2 || chapter == 4 || chapter == 7)
                require(p.acquiring_connection, "scheduled Connecting");
            if (chapter == 1)
                require(p.access_condition == AccessCondition::AuthenticationRequired, "auth");
            if (chapter == 3)
                require(p.access_condition == AccessCondition::UnsupportedInterface, "unsupported");
            if (chapter == 5)
                first_loss = p.observation_lost_generation;
            if (chapter == 6)
                require(!p.acquiring_connection && p.observation_lost_generation == first_loss,
                        "failed retry no replay");
            if (chapter == 8)
                require(p.observation_lost && p.observation_lost_generation != first_loss,
                        "new failed status episode");
            if (chapter >= 9)
                require(p.subject == SubjectState::Ready && !p.observation_lost &&
                            !p.stale_indicator,
                        "final Ready stays healthy");
            require(store.state().last_event_at <= at(now), "no future time");
            if (store.state().display.power != power) {
                power = store.state().display.power;
                if (power == DisplayPower::Off)
                    off_at = now;
                std::printf("power=%d at=%u\n", int(power), now);
            }
            ui::controller::note_panel_visible(power != DisplayPower::Off || now - off_at < 2000);
            ui::controller::apply(p);
        }
        (void)ui::controller::step_acknowledgement();
        lv_refr_now(d);
        const auto chapter = (now - 1000) / 18000, offset = (now - 1000) % 18000;
        if (chapter < 10 && (offset <= 1200 || offset == 10000) && offset % 20 == 0) {
            save(std::string{argv[1]} + "/step-" + std::to_string(chapter) + "-" +
                 std::to_string(offset) + ".rgb565");
            ++frames;
        }
    }
    require(power == DisplayPower::Off, "post-final normal sleep not reset by refresh");
    lv_mem_monitor_t m{};
    lv_mem_monitor(&m);
    std::printf("PASS native access instrument frames=%u post-final600s pool=%zu peak=%zu\n",
                frames, m.total_size, m.max_used);
}
