// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>

#include "app_model/display_policy.hpp"
#include "app_model/presentation.hpp"
#include "app_store/store.hpp"
#include "controller.hpp"
#include "status_timeline.hpp"
#include "ui/ui.hpp"

namespace {
using namespace prd;
using namespace app_model;
using namespace std::chrono_literals;

constexpr int kWidth = 320;
constexpr int kHeight = 240;
constexpr int kDrawBytes = kWidth * ui::kDefaultDrawBufferRows * 2;
using Pixels = std::array<std::uint16_t, kWidth * kHeight>;
Pixels pixels{};
alignas(4) std::array<std::uint8_t, kDrawBytes> buffer_a{};
alignas(4) std::array<std::uint8_t, kDrawBytes> buffer_b{};

void check(const bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL %s\n", message);
        std::abort();
    }
}

void flush(lv_display_t* display, const lv_area_t* area, std::uint8_t* data) {
    const auto* colours = reinterpret_cast<const std::uint16_t*>(data);
    const int width = area->x2 - area->x1 + 1;
    for (int y = area->y1; y <= area->y2; ++y) {
        for (int x = area->x1; x <= area->x2; ++x) {
            pixels.at(static_cast<std::size_t>(y * kWidth + x)) =
                colours[(y - area->y1) * width + x - area->x1];
        }
    }
    lv_display_flush_ready(display);
}

void paint(const std::uint32_t elapsed) {
    lv_tick_inc(elapsed);
    (void)ui::controller::step_acknowledgement();
    lv_refr_now(nullptr);
}

void record(const std::string& path) {
    FILE* file = std::fopen(path.c_str(), "wb");
    check(file != nullptr, "open RGB565 record");
    check(std::fwrite(pixels.data(), sizeof(std::uint16_t), pixels.size(), file) == pixels.size(),
          "write RGB565 record");
    std::fclose(file);
}

constexpr std::string_view kIdle = R"({"printer":{"state":"IDLE"}})";
constexpr std::string_view kPrint =
    R"({"printer":{"state":"PRINTING","temp_nozzle":211,"temp_bed":48},)"
    R"("job":{"id":71,"progress":62,"time_remaining":250,"time_printing":738}})";
#ifndef PRD_STOCK_111
constexpr std::string_view kFinish =
    R"({"printer":{"state":"FINISHED","temp_nozzle":181,"temp_bed":47},)"
    R"("job":{"id":71,"progress":100,"time_printing":1000}})";
#endif

struct Replay {
    Replay() : base{std::chrono::milliseconds{lv_tick_get()}} {
        // A fresh setup-to-operational entry exercises the real scene selector and clock.
        (void)store.dispatch(Event::boot_phase(base, BootPhase::Ready));
        (void)store.dispatch(Event::provisioning_phase(base, ProvisioningPhase::SetupActive));
        apply();
        paint(1200);
        base += 1200ms;
        (void)store.dispatch(Event::provisioning_phase(base, ProvisioningPhase::Inactive));
    }

    void at(const std::uint32_t ms) {
        check(ms >= elapsed, "replay clock goes forward");
        paint(ms - elapsed);
        elapsed = ms;
    }

    Timestamp now() const {
        return base + std::chrono::milliseconds{elapsed};
    }

    void dispatch(const Event& event) {
        (void)store.dispatch(event);
        apply();
    }

    void status(const std::string_view body, const std::uint32_t index) {
        const auto result = validation::apply_status_body(store, body, now(), {1, index});
        check(result.mutated, "status accepted by the production store");
        apply();
    }

    void age() {
        dispatch(Event::confidence_aged(now()));
    }

    Presentation view() const {
        return project(store.state());
    }

    void apply() {
        ui::controller::apply(view());
        paint(0);
    }

    app_store::Store store;
    Timestamp base;
    std::uint32_t elapsed{0};
};

void ordinary(const std::string& directory, const bool printer_status, const bool printing,
              const bool explicit_failure) {
    Replay replay;
#ifndef PRD_STOCK_111
    if (printer_status) {
        replay.dispatch(Event::display_preferences(
            replay.now(), {DisplayMode::PrinterStatus, Backlight::Standard}));
    }
#else
    check(!printer_status, "stock firmware supports only ordinary display policy");
#endif
    const auto body = printing ? kPrint : kIdle;
    const auto prefix = directory + "/" + (printing ? "printing-" : "ready-") +
                        (explicit_failure ? "explicit-" : "silence-");
    const auto sample = [&](const char* name) { record(prefix + name + ".rgb565"); };
    replay.status(body, 1);
    replay.at(2000);
    sample("settled");

    replay.at(3000);
    if (explicit_failure) {
        ConnectionObservation link;
        link.phase = ConnectionPhase::PrinterUnavailable;
        replay.dispatch(Event::connection_observed(replay.now(), link));
        check(!replay.view().observation_lost_acknowledgement,
              "failed poll cannot start the loss acknowledgement early");
#ifndef PRD_STOCK_111
        if (printer_status && !printing) {
            check(replay.view().at_visual_rest && !replay.view().stale_indicator,
                  "failed poll keeps the stock visible retry grace");
            check(display_policy::next_step_for(replay.store.state(), 20000, false, {}) ==
                      display_policy::Step::Dim,
                  "failed poll revokes opt-in hold immediately");
        }
#endif
    }
    replay.at(4200);
    sample("after-failed-poll");
    replay.at(15000);
    replay.age();
    sample("before-stale");
    replay.at(15001);
    replay.age();
    check(replay.view().stale_indicator && !replay.view().observation_lost_acknowledgement,
          "15 s boundary degrades on the ordinary clock");
    sample("stale-entry");
    replay.at(16201);
    sample("stale-settled");

    replay.at(60000);
    replay.age();
    check(!replay.view().observation_lost_acknowledgement, "60 s still has the last observation");
    sample("before-loss");
    replay.at(60001);
    replay.age();
    check(replay.view().observation_lost_acknowledgement, "ordinary loss starts after 60 s");
    sample("loss-t0");
    replay.at(60401);
    sample("loss-t400");
    replay.at(60801);
    sample("loss-t800");
    replay.at(61601);
    sample("loss-t1600");
    replay.at(62001);
    ConnectionObservation connection;
    connection.phase = ConnectionPhase::Connected;
    replay.dispatch(Event::connection_observed(replay.now(), connection));
    check(replay.view().observation_lost_acknowledgement,
          "transport recovery alone cannot qualify old state");
    sample("reconnect-without-body");

    replay.at(80001);
    replay.dispatch(Event::display_idle_timer_fired(replay.now()));
    sample("retirement-t0");
    replay.at(81001);
    sample("retirement-t1000");
    replay.at(82001);
    sample("retired");
    replay.at(83001);
    replay.status(body, 2);
    check(!replay.view().observation_lost, "new body restores the live subject");
    sample("recovery-t0");
    replay.at(83551);
    sample("recovery-t550");
    replay.at(84101);
    sample("recovery-settled");
}

#ifndef PRD_STOCK_111
void status_transitions(const std::string& directory) {
    Replay replay;
    replay.dispatch(Event::display_preferences(replay.now(),
                                               {DisplayMode::PrinterStatus, Backlight::Standard}));
    replay.status(kPrint, 1);
    replay.at(2000);
    replay.status(kFinish, 2);
    replay.at(19000);
    replay.status(kFinish, 3);
    replay.at(22000);
    check(app_model::display_policy::next_step_for(replay.store.state(), 20000, false, {}) ==
              app_model::display_policy::Step::Dim,
          "Completed can dim at the ordinary boundary");
    replay.dispatch(Event::display_idle_timer_fired(replay.now()));
    const auto completed = pixels;
    record(directory + "/completed-dimmed.rgb565");

    // Longer than the default 20-minute Dimmed hold, with a current FINISHED report.
    replay.at(1224010);
    replay.status(kFinish, 4);
    check(app_model::display_policy::next_step_for(replay.store.state(), 1222010, false, {}) ==
              app_model::display_policy::Step::Hold,
          "timer cannot remove an observed Completed");
    const auto current_completed = pixels;
    check(replay.view().subject == SubjectState::Completed && current_completed == completed,
          "dimmed Completed keeps its actual composition");

    PrinterSnapshotObservation snapshot;
    snapshot.normalized = PrinterState::Idle;
    snapshot.generation = 1;
    snapshot.sequence = 5;
    snapshot.is_busy = true;
    replay.at(1224011);
    replay.dispatch(Event::printer_snapshot(replay.now(), snapshot));
    check(replay.view().subject == SubjectState::Completed, "busy IDLE preserves Completed");
    replay.at(1225011);
    replay.status(kIdle, 6);
    check(replay.view().at_visual_rest, "new eligible IDLE qualifies for Ready");
    const auto entry = pixels;
    record(directory + "/completed-to-ready-t0.rgb565");
    replay.at(1225561);
    check(pixels != entry, "Completed to Ready animates through ordinary motion");
    record(directory + "/completed-to-ready-t550.rgb565");
    replay.at(1226111);
    const auto ready = pixels;
    check(ready != completed, "fresh Ready settles visibly");
    record(directory + "/completed-to-ready-settled.rgb565");

    snapshot.sequence = 7;
    replay.at(1226200);
    replay.dispatch(Event::printer_snapshot(replay.now(), snapshot));
    check(replay.view().ready_withheld && !replay.view().show_operational_form,
          "busy IDLE has no eligible resting form");
    check(pixels == ready, "withdrawal starts at the painted Ready frame");
    replay.at(1226750);
    const auto withdrawing = pixels;
    check(withdrawing != ready, "busy IDLE retires arc with visible motion");
    record(directory + "/busy-withdrawal-t550.rgb565");
    replay.at(1227300);
    const auto blank = pixels;
    check(blank != ready && blank != withdrawing,
          "busy IDLE settles to the background composition");
    const auto background = lv_color_to_u16(ui::theme::colour(ui::theme::Colour::Background));
    check(std::all_of(blank.begin(), blank.end(), [&](auto pixel) { return pixel == background; }),
          "busy IDLE settles to uniform background with no stale Ready arc");
    record(directory + "/busy-withdrawal-settled.rgb565");
    replay.at(1227320);
    check(pixels == blank, "busy IDLE remains settled after the withdrawal clock");

    replay.at(1227400);
    replay.status(kIdle, 8);
    replay.at(1227950);
    check(pixels != blank && pixels != ready, "fresh Ready returns from the painted blank");
    replay.at(1228500);
    check(pixels == ready, "fresh Ready returns to its exact endpoint");

    snapshot.sequence = 9;
    snapshot.is_busy = false;
    snapshot.requires_attention = true;
    replay.at(1228580);
    replay.dispatch(Event::printer_snapshot(replay.now(), snapshot));
    check(replay.view().subject_condition == SubjectCondition::Attention &&
              !replay.view().at_visual_rest && !replay.view().show_operational_form,
          "attention IDLE stays actionable and cannot claim Ready");
    const auto attention = pixels;
    replay.at(1229130);
    check(pixels != attention, "attention has visible motion");
    replay.at(1229680);
    record(directory + "/attention-idle-settled.rgb565");
    constexpr std::string_view raw_busy = R"({"printer":{"state":"BUSY"}})";
    replay.at(1229800);
    replay.status(raw_busy, 10);
    check(replay.view().subject == SubjectState::Preparing && !replay.view().at_visual_rest,
          "raw PrusaLink BUSY is a real Preparing observation");
    replay.at(1230900);
    check(pixels != ready && pixels != blank, "raw BUSY settles to operational Preparing");
    record(directory + "/raw-busy-preparing-settled.rgb565");

    std::puts("PASS opt-in reducer/controller held Completed, qualifying/ineligible IDLE");
    std::puts("PASS busy IDLE is uniform background; raw BUSY renders Preparing");
}
#endif
} // namespace

int main(int argc, char** argv) {
    check(argc == 3, "printer_status <output-dir> <timer|status|transitions>");
    const std::string mode = argv[2];
    lv_init();
    auto* display = lv_display_create(kWidth, kHeight);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(display, buffer_a.data(), buffer_b.data(), buffer_a.size(),
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, flush);
    check(ui::controller::build(display), "build native controller");
    ui::controller::note_panel_visible(true);

#ifndef PRD_STOCK_111
    if (mode == "transitions") {
        status_transitions(argv[1]);
    } else
#endif
    {
        check(mode == "timer" || mode == "status", "recognized replay mode");
        for (const bool printing : {false, true}) {
            for (const bool failed : {false, true}) {
                ordinary(argv[1], mode == "status", printing, failed);
            }
        }
        std::printf("PASS %s loss and recovery reducer/controller traces\n", argv[2]);
    }
    lv_display_delete(display);
    lv_deinit();
}
