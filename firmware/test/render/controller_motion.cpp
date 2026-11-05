// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

// Exercises the actual controller, incremental presentation and visible tick seam.
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <set>
#include <string>

#include "../host/support/state_builders.hpp"
#include "app_model/presentation.hpp"
#include "app_store/store.hpp"
#include "controller.hpp"
#include "fake_source/scenarios.hpp"
#include "fonts/fonts.hpp"
#include "fonts/motion_fonts.hpp"
#include "fonts/motion_title_raster.hpp"
#include "formatters.hpp"
#include "scenes/unavailable_motion.hpp"
#include "status_timeline.hpp"
#include "theme/theme.hpp"
#include "ui/ui.hpp"

namespace {
constexpr int kWidth = 320, kHeight = 240, kRows = prd::ui::kDefaultDrawBufferRows;
std::array<std::uint16_t, kWidth * kHeight> pixels{};
std::size_t flush_count = 0;
alignas(4) std::array<std::uint8_t, kWidth * kRows * 2> draw_a{}, draw_b{};
void require(bool ok, const char* message) {
    if (!ok) {
        std::fprintf(stderr, "FAIL %s\n", message);
        std::abort();
    }
}
void flush(lv_display_t* display, const lv_area_t* area, std::uint8_t* data) {
    ++flush_count;
    const auto* input = reinterpret_cast<std::uint16_t*>(data);
    const int width = area->x2 - area->x1 + 1;
    require(area->x1 >= 0 && area->x2 < kWidth && area->y1 >= 0 && area->y2 < kHeight,
            "flush stays inside canvas");
    for (int y = area->y1; y <= area->y2; ++y) {
        for (int x = area->x1; x <= area->x2; ++x) {
            pixels[y * kWidth + x] = input[(y - area->y1) * width + x - area->x1];
        }
    }
    lv_display_flush_ready(display);
}
std::uint64_t digest() {
    std::uint64_t value = 1469598103934665603ULL;
    for (auto pixel : pixels) {
        value = (value ^ pixel) * 1099511628211ULL;
    }
    return value;
}

void verify_title_raster_continuity() {
    auto* root = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(root);
    lv_obj_set_size(root, kWidth, kHeight);
    lv_obj_set_style_bg_color(root, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, LV_PART_MAIN);
    auto* label = prd::ui::theme::bare_label(root);
    prd::ui::theme::apply_text_style(label, prd::ui::theme::TextRole::StateTitle,
                                     prd::ui::theme::Colour::Primary);
    prd::ui::fonts::MotionTitleRaster raster;
    raster.attach(label);
    auto render = [&](double size, bool smooth) {
        const auto* font = prd::ui::fonts::motion_title(static_cast<int>(std::lround(size)));
        lv_obj_set_style_text_font(label, font, LV_PART_MAIN);
        lv_obj_set_width(label, LV_SIZE_CONTENT);
        lv_obj_align(label, LV_ALIGN_TOP_MID, prd::ui::theme::kRingCentreX - kWidth / 2, 120);
        raster.update(label, {size, 120.0}, smooth);
        if (smooth && !raster.active()) {
            std::fprintf(stderr, "RASTER_FALLBACK %s size=%.12f\n", lv_label_get_text(label), size);
        }
        require(!smooth || raster.active(), "catalogue title fits bounded raster");
        lv_refr_now(nullptr);
        return pixels;
    };
    for (const auto* word : {"Printing", "Preparing", "Paused", "Completed", "Stopped"}) {
        lv_label_set_text(label, word);
        for (int size = 20; size <= 28; ++size) {
            const auto original = render(size, false);
            const auto smooth = render(size, true);
            require(original == smooth, "integer title raster is pixel-identical to LVGL label");
        }
    }
    lv_label_set_text(label, "Printing");
    auto exceeds_one_quantum = [](const auto& before, const auto& after) {
        for (std::size_t i = 0; i < before.size(); ++i) {
            for (const auto shift : {0, 5, 11}) {
                const int mask = shift == 5 ? 63 : 31;
                // Pinned LVGL lv_color_16_16_mix quantises alpha to 32 steps.
                // One blend quantum is ceil(channel_max / 32), not one green bit.
                const int quantum = (mask + 31) / 32;
                if (std::abs(((before[i] >> shift) & mask) - ((after[i] >> shift) & mask)) >
                    quantum) {
                    return true;
                }
            }
        }
        return false;
    };
    for (int size = 20; size < 28; ++size) {
        constexpr double epsilon = 1e-8;
        const auto below = render(size + 0.5 - epsilon, true);
        const auto above = render(size + 0.5 + epsilon, true);
        // A8 rounding may cross one native blend quantum. A glyph substitution may not.
        require(!exceeds_one_quantum(below, above), "no raster discontinuity at old face switch");
        const auto old_below = render(size + 0.5 - epsilon, false);
        const auto old_above = render(size + 0.5 + epsilon, false);
        require(exceeds_one_quantum(old_below, old_above),
                "whole-font-switch negative control fires");
    }
    lv_obj_delete(root);
    lv_refr_now(nullptr);
    std::puts(
        "TITLE_RASTER 45 exact integer endpoints; 8 continuous face boundaries; 8 firing controls");
}
void save(const std::string& path) {
    FILE* file = std::fopen(path.c_str(), "wb");
    require(file != nullptr, "frame opens");
    require(std::fwrite(pixels.data(), 2, pixels.size(), file) == pixels.size(), "frame writes");
    std::fclose(file);
}
using Labels = std::multiset<std::string>;
void labels(lv_obj_t* object, Labels& result) {
    if (lv_obj_has_flag(object, LV_OBJ_FLAG_HIDDEN))
        return;
    if (lv_obj_check_type(object, &lv_label_class)) {
        result.emplace(lv_label_get_text(object));
    }
    for (std::uint32_t i = 0; i < lv_obj_get_child_count(object); ++i)
        labels(lv_obj_get_child(object, i), result);
}
Labels labels() {
    Labels result;
    labels(lv_screen_active(), result);
    return result;
}
lv_obj_t* label_named(lv_obj_t* object, const char* text) {
    if (lv_obj_has_flag(object, LV_OBJ_FLAG_HIDDEN))
        return nullptr;
    if (lv_obj_check_type(object, &lv_label_class) &&
        std::string(lv_label_get_text(object)) == text)
        return object;
    for (std::uint32_t i = 0; i < lv_obj_get_child_count(object); ++i)
        if (auto* found = label_named(lv_obj_get_child(object, i), text))
            return found;
    return nullptr;
}
void require_live_title_raster(lv_obj_t* title, bool moving) {
    require(title != nullptr, "transition title exists");
    require(lv_obj_get_event_count(title) == 2, "title has the two raster callbacks");
    auto* first = lv_obj_get_event_dsc(title, 0);
    auto* second = lv_obj_get_event_dsc(title, 1);
    auto* data = lv_event_dsc_get_user_data(first);
    require(data != nullptr && data == lv_event_dsc_get_user_data(second) &&
                lv_event_dsc_get_cb(first) == lv_event_dsc_get_cb(second),
            "both title callbacks belong to the same raster");
    const auto* raster = static_cast<const prd::ui::fonts::MotionTitleRaster*>(data);
    require(raster->active() == moving,
            "real transition raster stays active until settlement without fallback");
}
auto accent_pixels() {
    auto result = pixels;
    for (auto& pixel : result) {
        const auto r = (pixel >> 11) & 31, g = (pixel >> 5) & 63, b = pixel & 31;
        if (!(r * 4 > g * 3 && g > b * 3))
            pixel = 0;
    }
    return result;
}
std::size_t accent_pixel_count() {
    const auto ink = accent_pixels();
    return static_cast<std::size_t>(
        std::count_if(ink.begin(), ink.end(), [](const auto pixel) { return pixel != 0; }));
}
prd::app_model::Timestamp at(std::int64_t ms) {
    return std::chrono::duration_cast<prd::app_model::Timestamp>(std::chrono::milliseconds{ms});
}
} // namespace

int main(int argc, char** argv) {
    using namespace prd;
    require(argc == 2 || (argc == 3 && (std::string(argv[2]) == "--remaining" ||
                                        std::string(argv[2]) == "--recovery")),
            "output directory [--remaining|--recovery]");
    const std::string out = argv[1];
    lv_init();
    require(ui::fonts::verify(), "every generated font fits its actual reserved line box");
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
    ui::controller::note_panel_visible(true);

    app_store::Store store;
    for (std::size_t i = 0; i < fake_source::step_count(fake_source::Scenario::ColdBootToReady);
         ++i) {
        (void)store.dispatch(
            fake_source::step_at(fake_source::Scenario::ColdBootToReady, i, at(0), 0).event);
    }
    std::array<app_model::Presentation, 6> states{};
    app_model::Presentation unknown{};
    app_model::Presentation printing_zero{}, printing_resumed{};
    for (std::size_t i = 0; i < validation::status_timeline_step_count(); ++i) {
        const auto step = validation::status_timeline_step_at(i);
        (void)validation::apply_status_body(store, step.body, at(1000 + step.offset_ms),
                                            {1, static_cast<std::uint32_t>(i)});
        const auto p = app_model::project(store.state());
        if (i == 0)
            states[0] = p;
        if (i == 2)
            states[1] = p;
        if (i == 4)
            printing_zero = p;
        if (i == 8)
            states[2] = p;
        if (i == 9)
            states[3] = p;
        if (i == 10)
            printing_resumed = p;
        if (i == 14)
            states[4] = p;
        if (i == 11)
            unknown = p;
    }
    states[5] = states[3];
    states[5].subject = app_model::SubjectState::Stopped;
    const char* names[] = {"Ready", "Preparing", "Printing", "Paused", "Completed", "Stopped"};
    const int energies[] = {0, 400, 1000, 800, 600, 200};
    auto tick = [&](unsigned ms) {
        lv_tick_inc(ms);
        const bool moving = ui::controller::step_acknowledgement();
        lv_refr_now(display);
        return moving;
    };
    auto settle = [&](const app_model::Presentation& p) {
        ui::controller::apply(p);
        tick(0);
        tick(ui::theme::kOperationalMotionMs);
    };
    if (argc == 3 && std::string(argv[2]) == "--recovery") {
        app_store::Store recovery(test::printing_device(at(0)));
        auto apply = [&](const app_model::Event& event) {
            require(recovery.dispatch(event).accepted, "recovery event accepted by store");
            ui::controller::apply(app_model::project(recovery.state()));
        };
        settle(app_model::project(recovery.state()));
        app_model::ConnectionObservation failed;
        failed.phase = app_model::ConnectionPhase::PrinterUnavailable;
        apply(app_model::Event::connection_observed(at(9000), failed));
        tick(9000 - lv_tick_get());
        require(labels().count("1h18m left") == 1, "one timeout retains current observation");
        apply(app_model::Event::confidence_aged(at(15000)));
        tick(15000 - lv_tick_get());
        require(labels().count("1h18m left") == 1, "exact 15-second boundary remains current");
        apply(app_model::Event::confidence_aged(at(15001)));
        tick(1);
        const auto entry = digest();
        std::set<std::uint64_t> transition{entry};
        for (unsigned ms = 16; ms <= 1104; ms += 16) {
            tick(16);
            transition.insert(digest());
        }
        require(transition.size() > 8, "reconnecting entry uses actual controller motion");
        require(labels().count("1h18m left") == 0, "stale remaining cannot appear live");
        const auto age_text = ui::format::observation_age(at(0), lv_tick_get());
        auto* age = label_named(lv_screen_active(), age_text.c_str());
        require(age != nullptr && age_text.view().find("Reconnecting:") == 0,
                "native controller shows reconnecting with explicit old-data age");
        require(lv_obj_get_x(age) >= ui::theme::kRegionOperational.x &&
                    lv_obj_get_x(age) + lv_obj_get_width(age) <=
                        ui::theme::kRegionOperational.right(),
                "reconnecting age fits operational region");
        save(out + "/Reconnecting.rgb565");
        for (const auto subject :
             {app_model::PrinterState::Paused, app_model::PrinterState::Finished,
              app_model::PrinterState::Ready}) {
            auto outcome =
                test::with_printer_state(test::printing_device(at(0)), subject, at(1000), 2);
            outcome =
                app_model::reduce(outcome, app_model::Event::confidence_aged(at(lv_tick_get())));
            const auto p = app_model::project(outcome);
            settle(p);
            require(labels().count("1h18m left") == 0, "stale outcome has no live estimate");
            require(label_named(
                        lv_screen_active(),
                        ui::format::observation_age(p.stale_observed_at, lv_tick_get()).c_str()) !=
                        nullptr,
                    "stale outcome retains reconnecting age");
        }
        auto attention = test::with_printer_state(test::printing_device(at(0)),
                                                  app_model::PrinterState::Attention, at(1000), 2);
        attention =
            app_model::reduce(attention, app_model::Event::confidence_aged(at(lv_tick_get())));
        settle(app_model::project(attention));
        require(labels().count("Attention") == 1, "real attention retains title priority");
        const auto degraded = app_model::project(recovery.state());
        settle(degraded);
        require(degraded.confidence == app_model::Confidence::Degraded &&
                    degraded.subject == app_model::SubjectState::Printing &&
                    labels().count("Printing") == 1 && labels().count("Attention") == 0 &&
                    labels().count("1h18m left") == 0,
                "recovery starts from degraded printing");
        const auto stale_labels = labels();
        require(std::any_of(stale_labels.begin(), stale_labels.end(),
                            [](const auto& label) { return label.find("Reconnecting:") == 0; }),
                "recovery starts with reconnecting age");
        app_model::ConnectionObservation success;
        success.phase = app_model::ConnectionPhase::Connected;
        const auto now = at(lv_tick_get());
        apply(app_model::Event::connection_observed(now, success));
        apply(app_model::Event::printer_snapshot(
            now, test::printer_snapshot(app_model::PrinterState::Printing, 2)));
        tick(0);
        const auto recovery_entry = digest();
        tick(ui::theme::kOperationalMotionMs);
        require(digest() != recovery_entry, "recovery uses actual visible motion");
        require(labels().count("1h18m left") == 1, "successful recovery restores estimate");
        const auto recovered_labels = labels();
        require(std::none_of(recovered_labels.begin(), recovered_labels.end(),
                             [](const auto& label) { return label.find("Reconnecting:") == 0; }),
                "recovery retires stale row");
        save(out + "/Recovered.rgb565");
        std::puts("recovery controller: actual store/reducer, 15s boundary, stale age, priority, "
                  "motion PASS");
        return 0;
    }
    // Focused mode reuses this suite's real controller/tick path without retaining
    // the unrelated whole-family frame corpus.
    if (argc == 3) {
        auto printing = states[2];
        printing.progress = {true, 29};
        printing.remaining = {true, 9720};
        printing.remaining_is_approximate = false;
        settle(printing);
        require(labels().count("2h42m left") == 1 && labels().count("2:42") == 0,
                "Printing uses explicit units and direction");
        save(out + "/Printing.rgb565");
        auto* estimate = label_named(lv_screen_active(), "2h42m left");
        require(estimate != nullptr &&
                    lv_obj_get_width(estimate) < ui::theme::kRegionOperational.width,
                "accepted estimate fits its operational region");
        for (const auto duration : {3600U, 3599U, 0U, 59U, 60U, 97500U, UINT32_MAX}) {
            printing.remaining.seconds = duration;
            settle(printing);
            const auto expected = ui::format::remaining_duration(printing.remaining, false);
            estimate = label_named(lv_screen_active(), expected.c_str());
            require(estimate != nullptr &&
                        lv_obj_get_width(estimate) < ui::theme::kRegionOperational.width,
                    "boundary estimate remains complete and within the operational region");
        }
        printing.remaining = {true, 9720};
        printing.remaining_is_approximate = true;
        settle(printing);
        require(labels().count("~2h42m left") == 1, "approximate marker reaches actual label");
        auto missing = printing;
        missing.remaining = {};
        settle(missing);
        require(labels().count("~2h42m left") == 0, "missing estimate disappears");
        auto unmeasured = printing;
        unmeasured.progress = {};
        settle(unmeasured);
        require(labels().count("~2h42m left") == 1 && labels().count("Progress unavailable") == 0,
                "known estimate outranks unknown-progress copy");
        unmeasured.remaining = {};
        settle(unmeasured);
        require(labels().count("Progress unavailable") == 1, "absence copy remains truthful");
        auto paused = printing;
        paused.subject = app_model::SubjectState::Paused;
        settle(printing);
        const auto start = digest();
        ui::controller::apply(paused);
        tick(0);
        std::set<std::uint64_t> frames{start};
        for (unsigned t = 16; t <= 1104; t += 16) {
            tick(16);
            frames.insert(digest());
            if (t == 560)
                save(out + "/Printing-Paused-0560.rgb565");
        }
        require(frames.size() > 8, "duration exit retains actual animated controller path");
        require(labels().count("~2h42m left") == 0 && labels().count("Paused") == 1,
                "Paused cannot show a countdown even with supplied remaining data");
        save(out + "/Paused.rgb565");
        settle(printing);
        require(labels().count("~2h42m left") == 1, "estimate reappears on resume");
        auto attention = printing;
        attention.subject_condition = app_model::SubjectCondition::Attention;
        settle(attention);
        require(labels().count("~2h42m left") == 0, "attention context outranks estimate");
        auto stale = printing;
        stale.confidence = app_model::Confidence::Degraded;
        stale.stale_indicator = true;
        stale.stale_observed_at = at(lv_tick_get() - 20000);
        stale.remaining = {}; // projection withholds time-sensitive values on degradation
        settle(stale);
        require(labels().count("~2h42m left") == 0, "degraded projection carries no countdown");
        require(label_named(
                    lv_screen_active(),
                    ui::format::observation_age(stale.stale_observed_at, lv_tick_get()).c_str()) !=
                    nullptr,
                "existing freshness indicator remains visible");
        std::puts("remaining controller: boundaries, presence, priority, stale, real motion PASS");
        return 0;
    }
    for (unsigned a = 0; a < states.size(); ++a) {
        for (unsigned b = 0; b < states.size(); ++b) {
            if (a == b)
                continue;
            settle(states[a]);
            require(ui::controller::operational_form_report().energy == energies[a],
                    "source endpoint");
            ui::controller::apply(states[b]);
            tick(0);
            require(ui::controller::operational_form_report().energy == energies[a],
                    "entry starts at painted origin");
            std::set<std::uint64_t> frames{digest()};
            const std::string sequence = std::string(names[a]) + "-" + names[b];
            const bool capture = (a == 0 && b == 2) || (a == 2 && (b == 3 || b == 4)) ||
                                 (a == 3 && b == 2) || (a == 4 && b == 0);
            if (capture)
                save(out + "/" + sequence + "-0000.rgb565");
            for (unsigned t = 16; t <= 1104; t += 16) {
                const bool moving = tick(16);
                if (b != 0) {
                    require_live_title_raster(label_named(lv_screen_active(), names[b]),
                                              t < ui::theme::kOperationalMotionMs);
                }
                frames.insert(digest());
                if (capture)
                    save(out + "/" + sequence + "-" + std::to_string(t) + ".rgb565");
                if (t < ui::theme::kOperationalMotionMs)
                    require(moving, "controller keeps active cadence");
            }
            require(ui::controller::operational_form_report().energy == energies[b],
                    "destination endpoint");
            require(frames.size() > 8, "real intermediate rendered frames");
            require(!tick(2000), "settled transition stops scheduling");
            const auto idle = digest();
            require(!tick(10000) && idle == digest(), "settled state does not animate");
            std::printf("EDGE %s frames=%zu end=%d\n", sequence.c_str(), frames.size(),
                        energies[b]);
        }
        settle(states[a]);
        save(out + "/" + names[a] + ".rgb565");
    }
    auto verify_record_entry = [&](const app_model::Presentation& from,
                                   const app_model::Presentation& to, const char* name) {
        settle(from);
        const auto source_accent = accent_pixel_count();
        std::size_t pre_endpoint_accent = 0;
        std::size_t late_transition_accent = 0;
        std::uint8_t previous_alpha = 0;
        ui::controller::apply(to);
        tick(0);
        save(out + "/" + name + "-0000.rgb565");
        for (unsigned t = 16; t <= 1104; t += 16) {
            tick(16);
            const auto report = ui::controller::operational_form_report();
            require(report.cycle_treatment_alpha >= previous_alpha,
                    "cycle treatment enters monotonically");
            if (report.cycle_treatment_alpha > 0)
                require(report.draws_record,
                        "entry paints the supplied sweep before an open form can read as 100%");
            if (report.cycle_treatment_alpha >= 128 && t < ui::theme::kOperationalMotionMs)
                late_transition_accent = std::max(late_transition_accent, accent_pixel_count());
            if (t == 1040)
                pre_endpoint_accent = accent_pixel_count();
            previous_alpha = report.cycle_treatment_alpha;
            save(out + "/" + name + "-" + std::to_string(t) + ".rgb565");
        }
        const auto destination_accent = accent_pixel_count();
        require(previous_alpha == 255, "cycle treatment reaches its exact endpoint");
        std::printf("RECORD_ENTRY %s source=%zu late-max=%zu pre-endpoint=%zu destination=%zu\n",
                    name, source_accent, late_transition_accent, pre_endpoint_accent,
                    destination_accent);
        std::fflush(stdout);
        require(late_transition_accent <= std::max(source_accent, destination_accent),
                "late entry never becomes a false full-accent cycle");
        std::printf("RECORD_ENTRY %s PASS\n", name);
    };
    verify_record_entry(states[1], printing_zero, "Preparing-Printing0");
    verify_record_entry(states[3], printing_resumed, "Paused-Printing79");
    settle(states[2]);
    ui::controller::apply(states[0]);
    tick(0);
    tick(400);
    const auto interrupted = ui::controller::operational_form_report().energy;
    ui::controller::apply(states[3]);
    tick(0);
    require(ui::controller::operational_form_report().energy == interrupted,
            "interruption retains painted origin");
    tick(1100);
    require(ui::controller::operational_form_report().energy == 800,
            "interrupted motion reaches new target");
    settle(states[0]);
    ui::controller::note_panel_visible(false);
    ui::controller::apply(states[2]);
    require(!tick(2000), "dark panel does not request high-rate animation ticks");
    require(ui::controller::operational_form_report().energy == 0,
            "dark entry does not consume motion");
    ui::controller::note_panel_visible(true);
    tick(0);
    require(ui::controller::operational_form_report().energy == 0, "first visible frame is origin");
    tick(1100);
    for (unsigned direction = 0; direction < 2; ++direction) {
        settle(direction == 0 ? printing_resumed : unknown);
        ui::controller::apply(direction == 0 ? unknown : printing_resumed);
        int previous_baseline = 0;
        for (unsigned t = 0; t <= 1120; t += 16) {
            tick(t == 0 ? 0 : 16);
            auto* title = label_named(lv_screen_active(), "Printing");
            require_live_title_raster(title, t < ui::theme::kOperationalMotionMs);
            require(title != nullptr, "Printing title remains present during validity change");
            const auto* font = lv_obj_get_style_text_font(title, LV_PART_MAIN);
            const int baseline = lv_obj_get_y(title) + font->line_height - font->base_line;
            if (t != 0) {
                require(direction == 0 ? baseline <= previous_baseline
                                       : baseline >= previous_baseline,
                        "font switches never reverse the travelling title baseline");
            }
            previous_baseline = baseline;
        }
    }
    settle(states[4]);
    auto* completed_title = label_named(lv_screen_active(), "Completed");
    require(completed_title != nullptr &&
                lv_obj_get_style_text_opa(completed_title, LV_PART_MAIN) == LV_OPA_COVER,
            "Completed title has no extra body attenuation");
    settle(unknown);
    save(out + "/Unknown.rgb565");
    require(!ui::controller::operational_form_report().draws_record,
            "invalid progress has no sweep");
    require(unknown.remaining.known, "wrong-type progress fixture retains known estimate");
    const auto remaining =
        ui::format::remaining_duration(unknown.remaining, unknown.remaining_is_approximate);
    require(labels().count(remaining.c_str()) == 1 && labels().count("Progress unavailable") == 0,
            "unknown progress must not displace a supplied estimate");
    auto no_estimate = unknown;
    no_estimate.remaining.known = false;
    settle(no_estimate);
    save(out + "/UnknownNoEstimate.rgb565");
    require(labels().count("Progress unavailable") == 1,
            "absence copy fills unknown progress only without an estimate");
    settle(unknown);
    require(labels().count(remaining.c_str()) == 1 && labels().count("Progress unavailable") == 0,
            "estimate returning replaces absence copy without a subject change");

    const app_model::LossOriginForm origins[] = {
        app_model::LossOriginForm::Ready,     app_model::LossOriginForm::Preparing,
        app_model::LossOriginForm::Printing,  app_model::LossOriginForm::Paused,
        app_model::LossOriginForm::Completed, app_model::LossOriginForm::Stopped};
    unsigned generation = 0;
    unsigned recovery_cases = 0;
    for (unsigned a = 0; a < states.size(); ++a) {
        for (unsigned b = 0; b < states.size(); ++b) {
            for (const unsigned phase_ms : {0U, 400U, 1000U, 2600U}) {
                settle(states[a]);
                auto loss = states[a];
                loss.confidence = app_model::Confidence::Unavailable;
                loss.observation_lost = true;
                loss.observation_lost_eligible = true;
                loss.observation_lost_acknowledgement = true;
                loss.observation_lost_generation = ++generation;
                loss.observation_lost_origin = origins[a];
                ui::controller::apply(loss);
                tick(0);
                tick(phase_ms);
                const auto expected = ui::scene::phase_at(origins[a], phase_ms);
                ui::controller::apply(states[b]);
                tick(0);
                const auto first = ui::controller::operational_form_report();
                if (b != 0) {
                    auto* title = label_named(lv_screen_active(), names[b]);
                    require(title && lv_obj_get_y(title) == ui::theme::kMotionTitleY &&
                                lv_obj_get_style_text_font(title, LV_PART_MAIN) ==
                                    ui::fonts::motion_title(ui::theme::kMotionTitleSize),
                            "recovery typography uses neutral current stack, never hidden history");
                }
                require(first.energy == expected.energy,
                        "recovery starts from painted loss energy");
                require(first.centre_y == expected.form.centre_y + expected.offset_y,
                        "recovery starts from painted loss offset");
                if (a == 2 && b == 2 && phase_ms == 0) {
                    for (unsigned elapsed = 0; elapsed <= 1120; elapsed += 16) {
                        if (elapsed != 0)
                            tick(16);
                        const auto recovered = ui::controller::operational_form_report();
                        require(recovered.cycle_treatment_alpha == 255 && recovered.draws_record,
                                "closed Printing recovery keeps truthful cycle and sweep");
                        save(out + "/Printing-Recovery0-" + std::to_string(elapsed) + ".rgb565");
                    }
                } else {
                    tick(1100);
                }
                require(ui::controller::operational_form_report().energy == energies[b],
                        "recovery destination");
                require(!ui::controller::report().observation_lost_acknowledged,
                        "old loss copy withdrawn immediately");
                ++recovery_cases;
            }
        }
    }
    std::printf("RECOVERY %u origin/destination/phase cases\n", recovery_cases);
    auto lose = [&](unsigned origin) {
        auto loss = states[origin];
        loss.confidence = app_model::Confidence::Unavailable;
        loss.observation_lost = loss.observation_lost_eligible = true;
        loss.observation_lost_acknowledgement = true;
        loss.observation_lost_generation = ++generation;
        loss.observation_lost_origin = origins[origin];
        return loss;
    };
    // All directed pairs are interrupted while moving, including movement towards Ready.
    unsigned loss_cases = 0;
    for (unsigned a = 0; a < states.size(); ++a) {
        for (unsigned b = 0; b < states.size(); ++b) {
            if (a == b)
                continue;
            settle(states[a]);
            ui::controller::apply(states[b]);
            tick(0);
            tick(400);
            const auto before = ui::controller::operational_form_report();
            ui::controller::apply(lose(b));
            tick(0);
            const auto first = ui::controller::observation_lost_form_report();
            require(first.energy == before.energy && first.centre_y == before.centre_y &&
                        first.radius == before.radius,
                    "mid-motion loss starts at actual painted structure");
            require(labels().empty(), "loss withdraws all prior state copy and measurements");
            const bool capture = a == 2 && b == 0;
            if (capture)
                save(out + "/Moving-Loss-0000.rgb565");
            for (unsigned t = 16; t <= 2512; t += 16) {
                tick(16);
                if (capture)
                    save(out + "/Moving-Loss-" + std::to_string(t) + ".rgb565");
            }
            require(ui::controller::observation_lost_form_report().energy == 0,
                    "interrupted loss reaches accepted horizon");
            require(labels().count("Printer unavailable") == 1, "loss copy resolved");
            ++loss_cases;
        }
    }
    // Visible entry/settled/tail and post-tail empty recovery. New truthful labels are
    // applied immediately; geometry and appearance are the only animated quantities.
    unsigned late_cases = 0;
    for (unsigned b = 0; b < states.size(); ++b) {
        for (unsigned mode = 0; mode < 4; ++mode) {
            settle(states[2]);
            auto loss = lose(2);
            ui::controller::apply(loss);
            tick(0);
            tick(mode == 0 ? 400 : 2600);
            if (mode >= 2) {
                loss.observation_lost_acknowledgement = false;
                ui::controller::apply(loss);
                tick(mode == 2 ? 600 : 2000);
            }
            const bool blank = mode == 3;
            const auto before = ui::controller::observation_lost_form_report();
            const auto empty_digest = digest();
            ui::controller::apply(states[b]);
            tick(0);
            require(labels().count("Printer unavailable") == 0, "recovery removes old copy now");
            const auto first = ui::controller::operational_form_report();
            if (b == 2 && (mode == 1 || mode == 2)) {
                const auto value = ui::format::percent(states[b].progress);
                auto* record = label_named(lv_screen_active(), value.c_str());
                require(record != nullptr && labels().count("Printing") == 1,
                        "recovery applies current state and record immediately");
                require(lv_obj_get_style_text_opa(record, LV_PART_MAIN) == 0,
                        "incoming record cannot float above the recovering horizon");
            }
            if (blank) {
                require(first.energy == energies[b], "blank recovery has current geometry only");
                require(digest() == empty_digest, "blank recovery first frame stays empty");
            } else {
                require(first.energy == before.energy && first.centre_y == before.centre_y,
                        "visible/tail recovery inherits painted form");
            }
            const bool capture = b == 4 || (mode == 3 && b == 2);
            const std::string sequence = std::string(mode == 0   ? "Early"
                                                     : mode == 1 ? "Settled"
                                                     : mode == 2 ? "Tail"
                                                                 : "Blank") +
                                         "-" + names[b];
            if (capture)
                save(out + "/" + sequence + "-0000.rgb565");
            std::set<std::uint64_t> frames{digest()};
            for (unsigned t = 16; t <= 1104; t += 16) {
                tick(16);
                frames.insert(digest());
                if (capture)
                    save(out + "/" + sequence + "-" + std::to_string(t) + ".rgb565");
            }
            require(frames.size() > 8, "recovery has real intermediate frames");
            require(ui::controller::operational_form_report().energy == energies[b],
                    "late recovery current endpoint");
            if (b == 2) {
                const auto value = ui::format::percent(states[b].progress);
                auto* record = label_named(lv_screen_active(), value.c_str());
                require(record && lv_obj_get_style_text_opa(record, LV_PART_MAIN) == LV_OPA_COVER,
                        "Printing record is fully present at settlement");
            }
            require(!tick(2000), "late recovery stops scheduling");
            ++late_cases;
        }
    }
    std::printf("HANDOFF %u interrupted losses; %u visible/tail/blank recoveries\n", loss_cases,
                late_cases);
    // Defensive controller edge cases, not claims that the 60s freshness policy can cause
    // each interruption at this exact instant. Keep the fixed law and inspect actual pixels.
    for (unsigned mode = 0; mode < 3; ++mode) {
        const char* name = mode == 0   ? "Near-Ready-Loss"
                           : mode == 1 ? "Offset-Ready-Loss"
                                       : "Zero-Appearance-Loss";
        settle(states[2]);
        if (mode == 0) {
            ui::controller::apply(states[0]);
            tick(0);
            tick(1000);
        } else {
            auto loss = lose(2);
            ui::controller::apply(loss);
            tick(0);
            tick(2600);
            if (mode == 2) {
                loss.observation_lost_acknowledgement = false;
                ui::controller::apply(loss);
                tick(2000);
            }
            ui::controller::apply(states[mode == 1 ? 0 : 4]);
            tick(0);
            if (mode == 1)
                tick(400);
        }
        const auto before = ui::controller::operational_form_report();
        const auto ink = accent_pixels();
        ui::controller::apply(lose(mode == 2 ? 4 : 0));
        tick(0);
        const auto first = ui::controller::observation_lost_form_report();
        require(before.energy == first.energy && before.centre_y == first.centre_y,
                "edge loss retains the actual near-Ready/offset/zero-appearance origin");
        if (mode == 2) {
            require(std::none_of(ink.begin(), ink.end(), [](auto p) { return p != 0; }),
                    "zero-appearance case is genuinely unpainted");
            require(accent_pixels() == ink, "zero-appearance reloss cannot flash an opaque form");
        }
        save(out + "/" + name + "-0.rgb565");
        for (unsigned t = 16; t <= 2608; t += 16) {
            tick(16);
            if (t % 32 == 0)
                save(out + "/" + name + "-" + std::to_string(t) + ".rgb565");
        }
        require(ui::controller::observation_lost_form_report().energy == 0,
                "edge loss settles to the existing endpoint");
        require(labels().count("Printer unavailable") == 1, "edge loss resolves its copy");
        std::printf("EDGECASE %s initial_energy=%d centre_y=%d PASS\n", name, before.energy,
                    before.centre_y);
    }
    // Loss interrupting an appearance fade must retain its actual painted opacity too.
    settle(states[2]);
    auto fading_loss = lose(2);
    ui::controller::apply(fading_loss);
    tick(0);
    tick(2600);
    fading_loss.observation_lost_acknowledgement = false;
    ui::controller::apply(fading_loss);
    tick(2000);
    ui::controller::note_panel_visible(false);
    ui::controller::apply(states[4]);
    require(!tick(9000), "blank recovery does not consume entry while Off");
    const auto dark = digest();
    ui::controller::note_panel_visible(true);
    tick(0);
    require(digest() == dark, "blank recovery starts at first visible tick");
    tick(400);
    const auto appearing_form = accent_pixels();
    require(std::any_of(appearing_form.begin(), appearing_form.end(),
                        [](auto pixel) { return pixel != 0; }),
            "appearance control measures actual nonempty accent pixels");
    ui::controller::apply(lose(4));
    tick(0);
    require(accent_pixels() == appearing_form, "loss preserves painted appearance opacity");
    tick(2600);

    // Reachable Store→projection→controller trace, not hand-authored Presentation flags.
    // Protocol bodies are existing fixtures; no printer or network worker is involved.
    for (unsigned mode = 0; mode < 3; ++mode) {
        app_store::Store recovery_store;
        constexpr std::int64_t base = 1000000;
        for (std::size_t i = 0; i < fake_source::step_count(fake_source::Scenario::ColdBootToReady);
             ++i)
            (void)recovery_store.dispatch(
                fake_source::step_at(fake_source::Scenario::ColdBootToReady, i, at(base), 0).event);
        (void)validation::apply_status_body(
            recovery_store, validation::status_timeline_step_at(8).body, at(base + 1000), {1, 1});
        settle(app_model::project(recovery_store.state()));
        (void)recovery_store.dispatch(app_model::Event::confidence_aged(at(base + 61001)));
        auto lost = app_model::project(recovery_store.state());
        require(lost.observation_lost_acknowledgement, "reducer creates real loss episode");
        ui::controller::apply(lost);
        tick(0);
        tick(2600);
        if (mode != 0) {
            (void)recovery_store.dispatch(
                app_model::Event::display_idle_timer_fired(at(base + 81001)));
            lost = app_model::project(recovery_store.state());
            require(!lost.observation_lost_acknowledgement, "reducer retires acknowledgement");
            ui::controller::apply(lost);
            tick(2000);
        }
        if (mode == 2) {
            (void)recovery_store.dispatch(
                app_model::Event::display_sleep_timer_fired(at(base + 500000)));
            require(recovery_store.state().display.power == app_model::DisplayPower::Off,
                    "real model reaches true Off");
            ui::controller::apply(app_model::project(recovery_store.state()));
            ui::controller::note_panel_visible(false);
        }
        const auto actual_before = ui::controller::observation_lost_form_report();
        (void)validation::apply_status_body(recovery_store,
                                            validation::status_timeline_step_at(14).body,
                                            at(base + 501000), {1, 2});
        const auto recovered = app_model::project(recovery_store.state());
        require(recovered.subject == app_model::SubjectState::Completed &&
                    !recovered.observation_lost,
                "real observation recovers into current Completed");
        ui::controller::apply(recovered);
        if (mode == 2) {
            require(!tick(9000), "model-driven Off recovery holds motion");
            ui::controller::note_panel_visible(true);
        }
        tick(0);
        const auto recovered_form = ui::controller::operational_form_report();
        require(recovered_form.energy == (mode == 0 ? actual_before.energy : energies[4]),
                "reachable recovery chooses painted form or current blank destination");
        require(labels().count("Completed") == 1 && labels().count("Printer unavailable") == 0,
                "reachable recovery changes semantic copy immediately");
        tick(1100);
    }
    std::puts("REACHABLE 3 Store/projection recoveries; Off clock; interrupted appearance pixels");
    const std::array<app_model::Presentation, 7> age_states{
        states[0], states[1], states[2], states[3], states[4], states[5], unknown};
    const char* age_names[] = {"Ready",     "Preparing", "Printing", "Paused",
                               "Completed", "Stopped",   "Unknown"};
    // Count real LVGL flush callbacks, not just the controller's motion-request bool.
    // Explicit refresh still does no work when there are no invalidated areas.
    for (const auto& p : age_states) {
        settle(p);
        tick(100);
        const auto before = flush_count;
        for (unsigned ms = 0; ms < 2000; ms += 20) {
            require(!tick(20), "resting current scene requests no animation");
        }
        require(flush_count == before, "resting current scene performs zero display flushes");
    }
    std::puts("IDLE current: zero flush callbacks in seven settled layouts over 2s each");
    auto check_entry_overlap = [&](lv_obj_t* age = nullptr) {
        if (age == nullptr) {
            age = label_named(lv_screen_active(), "Reconnecting: 20 s old");
        }
        if (age == nullptr) {
            age = label_named(lv_screen_active(), "Reconnecting: 21 s old");
        }
        if (age == nullptr) {
            return;
        }
        const auto with_age = pixels;
        lv_obj_add_flag(age, LV_OBJ_FLAG_HIDDEN);
        lv_refr_now(display);
        for (std::size_t pixel = 0; pixel < pixels.size(); ++pixel) {
            require(with_age[pixel] == pixels[pixel] || pixels[pixel] == pixels[0],
                    "intermediate age ink never overprints existing foreground");
            if (with_age[pixel] != pixels[pixel]) {
                for (int dy = 1 - ui::theme::kSpace1; dy < ui::theme::kSpace1; ++dy) {
                    for (int dx = 1 - ui::theme::kSpace1; dx < ui::theme::kSpace1; ++dx) {
                        const int x = static_cast<int>(pixel % 320) + dx;
                        const int y = static_cast<int>(pixel / 320) + dy;
                        if (x >= 0 && x < 320 && y >= 0 && y < 240) {
                            require(pixels[y * 320 + x] == pixels[0],
                                    "intermediate age ink keeps four-pixel breathing room");
                        }
                    }
                }
            }
        }
        lv_obj_remove_flag(age, LV_OBJ_FLAG_HIDDEN);
        lv_refr_now(display);
    };
    for (unsigned i = 0; i < age_states.size(); ++i) {
        settle(age_states[i]);
        const auto paused_record_opacity =
            i == 3 ? lv_obj_get_style_text_opa(label_named(lv_screen_active(), "78"), LV_PART_MAIN)
                   : LV_OPA_TRANSP;
        const auto energy = ui::controller::operational_form_report().energy;
        auto stale = age_states[i];
        stale.confidence = app_model::Confidence::Degraded;
        stale.stale_indicator = true;
        stale.at_visual_rest = false;
        stale.stale_observed_at = at(static_cast<std::int64_t>(lv_tick_get()) - 20000);
        stale.nozzle = {};
        stale.bed = {};
        stale.remaining = {};
        if (i != 4 && i != 5) {
            stale.layers = {};
        }
        // Owner ruling 2026-09-08: render the retained last observation with its age,
        // not the previous empty-record policy. The projection tests bind this to real expiry.
        ui::controller::apply(stale);
        tick(0);
        save(out + "/Age-" + age_names[i] + "-entry-0.rgb565");
        for (unsigned ms = 16; ms <= 1104; ms += 16) {
            tick(16);
            save(out + "/Age-" + age_names[i] + "-entry-" + std::to_string(ms) + ".rgb565");
            check_entry_overlap();
            if (i == 3) {
                auto* record = label_named(lv_screen_active(), "78");
                require(record != nullptr, "Paused retains its actual percentage");
                require(lv_obj_get_y(record) >=
                            ui::theme::kPauseTop + ui::theme::kPauseBarHeight + ui::theme::kSpace1,
                        "retained Paused record stays below the pause mark throughout entry");
                require(lv_obj_get_style_text_opa(record, LV_PART_MAIN) == paused_record_opacity,
                        "retained Paused record never disappears to mask a collision");
            }
            require(ui::controller::operational_form_report().energy == energy,
                    "confidence cannot change operational energy");
        }
        require(labels().count("Reconnecting: 21 s old") == 1,
                "age counts from successful status while controller ticks");
        require(labels().count("Not current") == 0, "rejected indicator gone");
        auto* age = label_named(lv_screen_active(), "Reconnecting: 21 s old");
        require(age != nullptr && lv_obj_get_style_text_opa(age, LV_PART_MAIN) == LV_OPA_COVER,
                "age reaches readable full opacity");
        // Actual glyph pixels, not just label boxes: removing only age must expose background,
        // never arc, retained terminal record, title or pause bars under its ink.
        const auto with_age = pixels;
        lv_obj_add_flag(age, LV_OBJ_FLAG_HIDDEN);
        lv_refr_now(display);
        for (std::size_t pixel = 0; pixel < pixels.size(); ++pixel) {
            if (with_age[pixel] != pixels[pixel]) {
                // Owner rejected touching-looking rows even though no pixels overlapped.
                // Reserve one space.2 around actual age ink against ALL other foreground.
                for (int dy = 1 - ui::theme::kSpace2; dy < ui::theme::kSpace2; ++dy) {
                    for (int dx = 1 - ui::theme::kSpace2; dx < ui::theme::kSpace2; ++dx) {
                        const int x = static_cast<int>(pixel % 320) + dx;
                        const int y = static_cast<int>(pixel / 320) + dy;
                        if (x >= 0 && x < 320 && y >= 0 && y < 240) {
                            if (pixels[y * 320 + x] != pixels[0]) {
                                std::fprintf(stderr, "CROWDED %s age=%zu,%zu other=%d,%d\n",
                                             age_names[i], pixel % 320, pixel / 320, x, y);
                            }
                            require(pixels[y * 320 + x] == pixels[0],
                                    "age has eight-pixel clearance from other foreground");
                        }
                    }
                }
            }
            if (with_age[pixel] != pixels[pixel] && pixels[pixel] != pixels[0]) {
                std::fprintf(stderr, "COLLISION %s x=%zu y=%zu\n", age_names[i], pixel % 320,
                             pixel / 320);
            }
            require(with_age[pixel] == pixels[pixel] || pixels[pixel] == pixels[0],
                    "age ink does not overprint any existing foreground");
        }
        lv_obj_remove_flag(age, LV_OBJ_FLAG_HIDDEN);
        lv_refr_now(display);
        // 1104ms since the anchor: no second boundary occurs in the next 500ms.
        const auto before = flush_count;
        for (unsigned ms = 0; ms < 500; ms += 20) {
            require(!tick(20), "settled degraded scene requests no animation");
        }
        require(flush_count == before, "age performs zero flushes within an unchanged second");
        tick(500);
        require(flush_count > before, "age second change causes a real display flush");
        // Restore the same clock offset used by the unchanged recovery frame inventory.
        stale.stale_observed_at = at(static_cast<std::int64_t>(lv_tick_get()) - 21104);
        ui::controller::apply(stale);
        tick(0);
        ui::controller::apply(age_states[i]);
        tick(0);
        save(out + "/Age-" + age_names[i] + "-recovery-0.rgb565");
        for (unsigned ms = 16; ms <= 1104; ms += 16) {
            tick(16);
            save(out + "/Age-" + age_names[i] + "-recovery-" + std::to_string(ms) + ".rgb565");
            check_entry_overlap();
        }
        require(label_named(lv_screen_active(), "Reconnecting: 22 s old") == nullptr,
                "recovery retires age copy");
    }
    // Check the real painted opacity when either fade reverses before settlement.
    // Ready alone cannot expose geometry-gated title/record collisions.
    for (const auto& current : age_states) {
        for (const unsigned elapsed : {320U, 550U, 800U}) {
            settle(current);
            auto aged = current;
            aged.confidence = app_model::Confidence::Degraded;
            aged.stale_indicator = true;
            aged.stale_observed_at = at(static_cast<std::int64_t>(lv_tick_get()) - 20000);
            aged.remaining = {};
            ui::controller::apply(aged);
            tick(0);
            tick(elapsed);
            auto* label = label_named(lv_screen_active(), "Reconnecting: 20 s old");
            const auto before = label ? lv_obj_get_style_text_opa(label, LV_PART_MAIN) : 0;
            ui::controller::apply(current);
            tick(0);
            label = label_named(lv_screen_active(), "Reconnecting: 20 s old");
            const auto after = label ? lv_obj_get_style_text_opa(label, LV_PART_MAIN) : 0;
            require(before == after, "geometry-gated recovery reversal keeps painted opacity");
            tick(180);
            label = label_named(lv_screen_active(), "Reconnecting: 20 s old");
            const auto recovering = label ? lv_obj_get_style_text_opa(label, LV_PART_MAIN) : 0;
            ui::controller::apply(aged);
            tick(0);
            label = label_named(lv_screen_active(), "Reconnecting: 20 s old");
            const auto reentered = label ? lv_obj_get_style_text_opa(label, LV_PART_MAIN) : 0;
            require(recovering == reentered,
                    "geometry-gated repeated loss reversal keeps painted opacity");
        }
    }
    // Reversal and visible-clock control on Ready: no competing row masks this witness.
    settle(states[0]);
    auto aged_ready = states[0];
    aged_ready.confidence = app_model::Confidence::Degraded;
    aged_ready.stale_indicator = true;
    aged_ready.at_visual_rest = false;
    aged_ready.stale_observed_at = at(static_cast<std::int64_t>(lv_tick_get()) - 20000);
    ui::controller::apply(aged_ready);
    tick(0);
    tick(550);
    auto* fading_age = label_named(lv_screen_active(), "Reconnecting: 20 s old");
    require(fading_age != nullptr, "age is already visible during appearance");
    const auto half_opacity = lv_obj_get_style_text_opa(fading_age, LV_PART_MAIN);
    require(half_opacity > 0 && half_opacity < 255, "age appearance is genuinely intermediate");
    ui::controller::apply(states[0]);
    tick(0);
    require(lv_obj_get_style_text_opa(fading_age, LV_PART_MAIN) == half_opacity,
            "recovery reversal preserves painted age opacity");
    tick(180);
    const auto reversing_opacity = lv_obj_get_style_text_opa(fading_age, LV_PART_MAIN);
    ui::controller::apply(aged_ready);
    tick(0);
    require(lv_obj_get_style_text_opa(fading_age, LV_PART_MAIN) == reversing_opacity,
            "repeated loss reversal preserves painted age opacity");
    ui::controller::note_panel_visible(false);
    require(!tick(5000), "Off age motion requests no fast ticks");
    require(lv_obj_get_style_text_opa(fading_age, LV_PART_MAIN) == reversing_opacity,
            "Off retains last painted opacity");
    ui::controller::note_panel_visible(true);
    tick(0);
    require(labels().count("Reconnecting: 25 s old") == 1,
            "wake uses actual current age, not the frozen display clock");
    tick(1100);
    require(!tick(1000), "settled age seconds require no continuous animation cadence");
    require(labels().count("Reconnecting: 27 s old") == 1,
            "age updates without a new model presentation");
    auto modal = aged_ready;
    modal.modal_surface = app_model::ModalSurface::FactoryResetConfirmation;
    ui::controller::apply(
        modal); // Rendering only: no button/reducer reset operation is dispatched.
    tick(0);
    require(labels().count("Reconnecting: 27 s old") == 0,
            "higher-priority surface cannot retain the confidence label");
    ui::controller::apply(states[0]);
    tick(1200);
    require(labels().count("Reconnecting: 27 s old") == 0,
            "return to fresh Ready cannot resurrect a hidden confidence label");
    std::puts("AGE 7 layouts: timed appearance/recovery; last-status age; invariant energy");
    // Printer-condition rendering uses the same controller and actual LVGL pool.
    // Its content must not retarget the subject's form or repaint its sweep.
    for (unsigned i = 0; i < states.size(); ++i) {
        settle(states[i]);
        const auto form_pixels = accent_pixels();
        auto error = states[i];
        error.subject_condition = app_model::SubjectCondition::Error;
        ui::controller::apply(error);
        tick(0);
        require(labels().count("Printer error") == 1, "error wording is immediate");
        const auto context = ui::format::error_context(error);
        require(labels().count(context.c_str()) == 1, "retained subject is explicitly qualified");
        for (unsigned t = 0; t <= 1120; t += 16) {
            if (t != 0) {
                tick(16);
            }
            // Isolate the new warning ink: at low RGB565 alpha it can satisfy the
            // old orange heuristic. Hide-and-rerender tests the actual form instead.
            auto* mark = label_named(lv_screen_active(), "!");
            require(mark != nullptr, "warning mark exists");
            auto* parent = lv_obj_get_parent(mark);
            lv_obj_t* outline = nullptr;
            for (std::uint32_t child = 0; child < lv_obj_get_child_count(parent); ++child) {
                auto* object = lv_obj_get_child(parent, child);
                if (lv_obj_check_type(object, &lv_line_class)) {
                    outline = object;
                }
            }
            require(outline != nullptr, "warning outline exists");
            lv_obj_add_flag(mark, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(outline, LV_OBJ_FLAG_HIDDEN);
            lv_refr_now(display);
            if (accent_pixels() != form_pixels) {
                std::fprintf(stderr, "state=%s t=%u form differs\n", names[i], t);
                const auto current = accent_pixels();
                unsigned shown = 0;
                for (std::size_t pixel = 0; pixel < current.size() && shown < 12; ++pixel) {
                    if (current[pixel] != form_pixels[pixel]) {
                        std::fprintf(stderr, "pixel %zu,%zu %04x -> %04x\n", pixel % 320,
                                     pixel / 320, form_pixels[pixel], current[pixel]);
                        ++shown;
                    }
                }
            }
            require(accent_pixels() == form_pixels, "error does not change form or sweep");
            lv_obj_remove_flag(mark, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(outline, LV_OBJ_FLAG_HIDDEN);
            lv_refr_now(display);
            check_entry_overlap(label_named(lv_screen_active(), context.c_str()));
            save(out + "/Error-" + names[i] + "-entry-" + std::to_string(t) + ".rgb565");
        }
        require(!tick(100), "settled error requests no animation tick");
        const auto idle_flushes = flush_count;
        tick(100);
        require(flush_count == idle_flushes, "settled error does not invalidate the panel");
        ui::controller::apply(states[i]);
        for (unsigned t = 0; t <= 1120; t += 16) {
            tick(t == 0 ? 0 : 16);
            require(labels().count("Printer error") == 0, "recovered wording is immediate");
            require(accent_pixels() == form_pixels, "recovery preserves form and sweep");
            save(out + "/Error-" + names[i] + "-recovery-" + std::to_string(t) + ".rgb565");
        }
    }
    for (unsigned i = 0; i < states.size(); ++i) {
        auto error = states[i];
        error.subject_condition = app_model::SubjectCondition::Error;
        settle(error);
        error.confidence = app_model::Confidence::Degraded;
        error.stale_indicator = true;
        error.stale_observed_at = at(lv_tick_get() - 20000);
        ui::controller::apply(error);
        for (unsigned t = 0; t <= 1120; t += 16) {
            tick(t == 0 ? 0 : 16);
            check_entry_overlap();
            save(out + "/Error-" + names[i] + "-aged-" + std::to_string(t) + ".rgb565");
        }
        auto* age = label_named(lv_screen_active(), "Reconnecting: 21 s old");
        require(age != nullptr && lv_obj_get_style_text_opa(age, LV_PART_MAIN) == LV_OPA_COVER,
                "settled error age is fully readable, not hidden to avoid collision");
        auto* mark = label_named(lv_screen_active(), "!");
        require(mark != nullptr && lv_obj_get_style_text_opa(mark, LV_PART_MAIN) == LV_OPA_COVER,
                "settled error symbol is fully visible");
        const auto context = ui::format::error_context(error);
        auto* context_label = label_named(lv_screen_active(), context.c_str());
        require(context_label != nullptr &&
                    lv_obj_get_style_text_opa(context_label, LV_PART_MAIN) ==
                        ui::theme::opacity(ui::theme::Opacity::High),
                "settled error context reaches its full role opacity");
        error.confidence = app_model::Confidence::Current;
        error.stale_indicator = false;
        ui::controller::apply(error);
        for (unsigned t = 0; t <= 1120; t += 16) {
            tick(t == 0 ? 0 : 16);
            const auto age_text =
                ui::format::observation_age(error.stale_observed_at, lv_tick_get());
            check_entry_overlap(label_named(lv_screen_active(), age_text.c_str()));
            save(out + "/Error-" + names[i] + "-age-recovery-" + std::to_string(t) + ".rgb565");
        }
        require(labels().count("Printer error") == 1,
                "fresh observation does not clear a reported error");
        tick(100);
        const auto idle_flushes = flush_count;
        tick(1000);
        require(flush_count == idle_flushes, "recovered error stops updating retired age text");
    }
    for (unsigned phase : {0U, 160U, 560U, 1040U}) {
        settle(states[2]);
        auto error = states[2];
        error.subject_condition = app_model::SubjectCondition::Error;
        ui::controller::apply(error);
        tick(0);
        tick(phase);
        ui::controller::note_panel_visible(false);
        const auto frozen = pixels;
        require(!tick(5000) && pixels == frozen, "Off freezes condition motion and paint");
        ui::controller::note_panel_visible(true);
        tick(0);
        ui::controller::apply(states[2]);
        tick(0);
        tick(160);
        ui::controller::apply(error);
        tick(0);
        tick(1120);
        require(labels().count("Printer error") == 1,
                "interrupted recovery restores current condition");
        settle(states[2]);
        require(labels().count("Printer error") == 0, "repeated reversal clears retired condition");
    }
    std::puts(
        "ERROR 12 directed edges; 6 degraded entries; 4 Off/reversal cases; first observation");
    auto initial_error = states[0];
    initial_error.subject = app_model::SubjectState::AcquiringState;
    initial_error.subject_condition = app_model::SubjectCondition::Error;
    initial_error.show_operational_form = false;
    initial_error.show_telemetry = false;
    initial_error.progress = {};
    settle(initial_error);
    require(labels().count("Printer error") == 1, "first error is not a blank scene");
    require(labels().count("Check printer display") == 1, "unknown prior has no invented context");
    require(accent_pixel_count() == 0, "unknown prior has no invented form");
    save(out + "/Error-Acquiring-settled.rgb565");
    {
        auto attention_states = states;
        for (auto& state : attention_states) {
            state.layers = {};
        }
        // Printer-condition rendering uses the same controller and actual LVGL pool.
        // Its content must not retarget the subject's form or repaint its sweep.
        for (unsigned i = 0; i < attention_states.size(); ++i) {
            settle(attention_states[i]);
            const auto form_pixels = accent_pixels();
            auto error = attention_states[i];
            error.subject_condition = app_model::SubjectCondition::Attention;
            ui::controller::apply(error);
            tick(0);
            require(labels().count("Attention") == 1, "error wording is immediate");
            const auto context = ui::format::error_context(error);
            require(labels().count(context.c_str()) == 1,
                    "retained subject is explicitly qualified");
            for (unsigned t = 0; t <= 1120; t += 16) {
                if (t != 0) {
                    tick(16);
                }
                // Isolate the new warning ink: at low RGB565 alpha it can satisfy the
                // old orange heuristic. Hide-and-rerender tests the actual form instead.
                auto* mark = label_named(lv_screen_active(), "!");
                require(mark != nullptr, "warning mark exists");
                auto* parent = lv_obj_get_parent(mark);
                lv_obj_t* outline = nullptr;
                for (std::uint32_t child = 0; child < lv_obj_get_child_count(parent); ++child) {
                    auto* object = lv_obj_get_child(parent, child);
                    if (lv_obj_check_type(object, &lv_line_class)) {
                        outline = object;
                    }
                }
                require(outline != nullptr, "warning outline exists");
                lv_obj_add_flag(mark, LV_OBJ_FLAG_HIDDEN);
                lv_obj_add_flag(outline, LV_OBJ_FLAG_HIDDEN);
                lv_refr_now(display);
                if (accent_pixels() != form_pixels) {
                    std::fprintf(stderr, "state=%s t=%u form differs\n", names[i], t);
                    const auto current = accent_pixels();
                    unsigned shown = 0;
                    for (std::size_t pixel = 0; pixel < current.size() && shown < 12; ++pixel) {
                        if (current[pixel] != form_pixels[pixel]) {
                            std::fprintf(stderr, "pixel %zu,%zu %04x -> %04x\n", pixel % 320,
                                         pixel / 320, form_pixels[pixel], current[pixel]);
                            ++shown;
                        }
                    }
                }
                require(accent_pixels() == form_pixels, "error does not change form or sweep");
                lv_obj_remove_flag(mark, LV_OBJ_FLAG_HIDDEN);
                lv_obj_remove_flag(outline, LV_OBJ_FLAG_HIDDEN);
                lv_refr_now(display);
                check_entry_overlap(label_named(lv_screen_active(), context.c_str()));
                save(out + "/Attention-" + names[i] + "-entry-" + std::to_string(t) + ".rgb565");
            }
            require(!tick(100), "settled error requests no animation tick");
            const auto idle_flushes = flush_count;
            tick(100);
            require(flush_count == idle_flushes, "settled error does not invalidate the panel");
            ui::controller::apply(attention_states[i]);
            for (unsigned t = 0; t <= 1120; t += 16) {
                tick(t == 0 ? 0 : 16);
                require(labels().count("Attention") == 0, "recovered wording is immediate");
                require(accent_pixels() == form_pixels, "recovery preserves form and sweep");
                save(out + "/Attention-" + names[i] + "-recovery-" + std::to_string(t) + ".rgb565");
            }
        }
        for (unsigned i = 0; i < attention_states.size(); ++i) {
            auto error = attention_states[i];
            error.subject_condition = app_model::SubjectCondition::Attention;
            settle(error);
            error.confidence = app_model::Confidence::Degraded;
            error.stale_indicator = true;
            error.stale_observed_at = at(lv_tick_get() - 20000);
            ui::controller::apply(error);
            for (unsigned t = 0; t <= 1120; t += 16) {
                tick(t == 0 ? 0 : 16);
                check_entry_overlap();
                save(out + "/Attention-" + names[i] + "-aged-" + std::to_string(t) + ".rgb565");
            }
            auto* age = label_named(lv_screen_active(), "Reconnecting: 21 s old");
            require(age != nullptr && lv_obj_get_style_text_opa(age, LV_PART_MAIN) == LV_OPA_COVER,
                    "settled error age is fully readable, not hidden to avoid collision");
            auto* mark = label_named(lv_screen_active(), "!");
            require(mark != nullptr &&
                        lv_obj_get_style_text_opa(mark, LV_PART_MAIN) == LV_OPA_COVER,
                    "settled error symbol is fully visible");
            const auto context = ui::format::error_context(error);
            auto* context_label = label_named(lv_screen_active(), context.c_str());
            require(context_label != nullptr &&
                        lv_obj_get_style_text_opa(context_label, LV_PART_MAIN) ==
                            ui::theme::opacity(ui::theme::Opacity::High),
                    "settled error context reaches its full role opacity");
            error.confidence = app_model::Confidence::Current;
            error.stale_indicator = false;
            ui::controller::apply(error);
            for (unsigned t = 0; t <= 1120; t += 16) {
                tick(t == 0 ? 0 : 16);
                const auto age_text =
                    ui::format::observation_age(error.stale_observed_at, lv_tick_get());
                check_entry_overlap(label_named(lv_screen_active(), age_text.c_str()));
                save(out + "/Attention-" + names[i] + "-age-recovery-" + std::to_string(t) +
                     ".rgb565");
            }
            require(labels().count("Attention") == 1,
                    "fresh observation does not clear a reported error");
            tick(100);
            const auto idle_flushes = flush_count;
            tick(1000);
            require(flush_count == idle_flushes, "recovered error stops updating retired age text");
        }
        for (unsigned phase : {0U, 160U, 560U, 1040U}) {
            settle(attention_states[2]);
            auto error = attention_states[2];
            error.subject_condition = app_model::SubjectCondition::Attention;
            ui::controller::apply(error);
            tick(0);
            tick(phase);
            ui::controller::note_panel_visible(false);
            const auto frozen = pixels;
            require(!tick(5000) && pixels == frozen, "Off freezes condition motion and paint");
            ui::controller::note_panel_visible(true);
            tick(0);
            ui::controller::apply(attention_states[2]);
            tick(0);
            tick(160);
            ui::controller::apply(error);
            tick(0);
            tick(1120);
            require(labels().count("Attention") == 1,
                    "interrupted recovery restores current condition");
            settle(attention_states[2]);
            require(labels().count("Attention") == 0, "repeated reversal clears retired condition");
        }
        std::puts("ATTENTION 12 directed edges; 6 degraded entries; 4 Off/reversal cases; first "
                  "observation");
        auto initial_error = attention_states[0];
        initial_error.subject = app_model::SubjectState::AcquiringState;
        initial_error.subject_condition = app_model::SubjectCondition::Attention;
        initial_error.show_operational_form = false;
        initial_error.show_telemetry = false;
        initial_error.progress = {};
        settle(initial_error);
        require(labels().count("Attention") == 1, "first error is not a blank scene");
        require(labels().count("Check printer display") == 1,
                "unknown prior has no invented context");
        save(out + "/Attention-Acquiring-settled.rgb565");
        // Warning amber satisfies the orange-form heuristic. The outline is created
        // immediately before its mark; pin that relationship rather than hiding a class.
        auto* mark = label_named(lv_screen_active(), "!");
        require(mark != nullptr, "unknown Attention has a warning mark");
        auto* parent = lv_obj_get_parent(mark);
        const auto mark_index = lv_obj_get_index(mark);
        require(mark_index > 0, "warning outline precedes its mark");
        auto* outline = lv_obj_get_child(parent, mark_index - 1);
        require(lv_obj_check_type(outline, &lv_line_class) && lv_line_get_point_count(outline) == 4,
                "identified warning outline has the triangle geometry");
        lv_obj_t* segment = nullptr;
        for (std::uint32_t child = 0; child < lv_obj_get_child_count(parent); ++child) {
            auto* object = lv_obj_get_child(parent, child);
            if (object != outline && lv_obj_check_type(object, &lv_line_class)) {
                require(segment == nullptr, "exactly one other line is the progress segment");
                segment = object;
            }
        }
        require(segment != nullptr, "real progress segment is present for the control");
        const auto form_ink_without_warning = [&]() {
            lv_obj_add_flag(mark, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(outline, LV_OBJ_FLAG_HIDDEN);
            lv_refr_now(display);
            const auto count = accent_pixel_count();
            lv_obj_remove_flag(mark, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(outline, LV_OBJ_FLAG_HIDDEN);
            lv_refr_now(display);
            return count;
        };
        require(form_ink_without_warning() == 0, "unknown prior has no invented form");

        // A permanent firing control: draw an invented form through the ACTUAL segment.
        // The old class-wide exclusion would hide it and fail this assertion.
        const auto* saved_points = lv_line_get_points(segment);
        const auto saved_count = lv_line_get_point_count(segment);
        const auto saved_x = lv_obj_get_x(segment);
        const auto saved_y = lv_obj_get_y(segment);
        const auto saved_opa = lv_obj_get_style_line_opa(segment, LV_PART_MAIN);
        const bool was_hidden = lv_obj_has_flag(segment, LV_OBJ_FLAG_HIDDEN);
        const std::array<lv_point_precise_t, 2> invented{{{0, 0}, {100, 0}}};
        lv_line_set_points(segment, invented.data(), invented.size());
        lv_obj_set_pos(segment, 60, 200);
        lv_obj_set_style_line_opa(segment, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_remove_flag(segment, LV_OBJ_FLAG_HIDDEN);
        require(form_ink_without_warning() > 0, "control detects invented progress segment");
        lv_line_set_points(segment, saved_points, saved_count);
        lv_obj_set_pos(segment, saved_x, saved_y);
        lv_obj_set_style_line_opa(segment, saved_opa, LV_PART_MAIN);
        if (was_hidden) {
            lv_obj_add_flag(segment, LV_OBJ_FLAG_HIDDEN);
        }
        lv_refr_now(display);
        require(form_ink_without_warning() == 0, "control restores absent form");
        std::puts("ATTENTION unknown form: exact warning exclusion; real segment control fires");
    }
    verify_title_raster_continuity();
    lv_mem_monitor_t memory{};
    lv_mem_monitor(&memory);
    std::printf("PASS 30 directed controller edges; interruption; wake; unknown; pool_size=%zu "
                "max_used=%zu\n",
                memory.total_size, memory.max_used);
    lv_display_delete(display);
    lv_deinit();
}
