// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

// Native OTA UI evidence through the production controller and pinned LVGL renderer.
// This is deliberately separate from the browser proposal: every frame below is painted by the
// firmware scene that ships on the panel.

#include "controller.hpp"
#include "fake_source/fixtures.hpp"
#include "fonts/fonts.hpp"
#include "scene_selection.hpp"
#include "theme/theme.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <set>
#include <string>

namespace {

namespace theme = prd::ui::theme;
using Frame = std::array<std::uint16_t, 320U * 240U>;

constexpr int kWidth = 320;
constexpr int kHeight = 240;
Frame pixels{};
std::array<std::uint8_t, kWidth * 40U * 2U> buffer{};
unsigned flushes = 0;

void require(const bool condition, const char* const reason) {
    if (!condition) {
        std::fprintf(stderr, "FAIL %s\n", reason);
        std::abort();
    }
}

void flush(lv_display_t* const display, const lv_area_t* const area, std::uint8_t* const data) {
    ++flushes;
    require(area->x1 >= 0 && area->y1 >= 0 && area->x2 < kWidth && area->y2 < kHeight,
            "LVGL flush stays on the physical canvas");
    const auto* const input = reinterpret_cast<const std::uint16_t*>(data);
    for (int y = area->y1; y <= area->y2; ++y) {
        for (int x = area->x1; x <= area->x2; ++x) {
            pixels[static_cast<std::size_t>(y * kWidth + x)] =
                input[(y - area->y1) * (area->x2 - area->x1 + 1) + x - area->x1];
        }
    }
    lv_display_flush_ready(display);
}

[[nodiscard]] std::uint16_t rgb565(const std::uint32_t rgb) {
    const auto red = static_cast<std::uint16_t>(((rgb >> 16) & 0xFFU) >> 3);
    const auto green = static_cast<std::uint16_t>(((rgb >> 8) & 0xFFU) >> 2);
    const auto blue = static_cast<std::uint16_t>((rgb & 0xFFU) >> 3);
    return static_cast<std::uint16_t>((red << 11) | (green << 5) | blue);
}

[[nodiscard]] bool inside(const theme::Rect& box, const int x, const int y) {
    return x >= box.x && x < box.right() && y >= box.y && y < box.bottom();
}

[[nodiscard]] bool ink_outside_content(const Frame& frame) {
    const auto background = frame.front();
    for (int y = 0; y < kHeight; ++y) {
        for (int x = 0; x < kWidth; ++x) {
            if (!inside(theme::kCanvasContent, x, y) &&
                frame[static_cast<std::size_t>(y * kWidth + x)] != background) {
                return true;
            }
        }
    }
    return false;
}

[[nodiscard]] bool changed_only_inside(const Frame& before, const Frame& after,
                                       const theme::Rect& box) {
    for (int y = 0; y < kHeight; ++y) {
        for (int x = 0; x < kWidth; ++x) {
            const auto index = static_cast<std::size_t>(y * kWidth + x);
            if (before[index] != after[index] && !inside(box, x, y)) {
                return false;
            }
        }
    }
    return true;
}

[[nodiscard]] std::size_t colour_in(const Frame& frame, const std::uint16_t colour,
                                    const theme::Rect& box) {
    std::size_t count = 0;
    for (int y = box.y; y < box.bottom(); ++y) {
        for (int x = box.x; x < box.right(); ++x) {
            count += frame[static_cast<std::size_t>(y * kWidth + x)] == colour ? 1U : 0U;
        }
    }
    return count;
}

[[nodiscard]] bool colour_confined_above(const Frame& frame, const std::uint16_t colour,
                                         const theme::Rect& box, const int exclusive_bottom) {
    for (int y = box.y; y < box.bottom(); ++y) {
        for (int x = box.x; x < box.right(); ++x) {
            if (frame[static_cast<std::size_t>(y * kWidth + x)] == colour &&
                y >= exclusive_bottom) {
                return false;
            }
        }
    }
    return true;
}

void collect_labels(lv_obj_t* const object, std::set<std::string>& labels) {
    if (lv_obj_has_flag(object, LV_OBJ_FLAG_HIDDEN)) {
        return;
    }
    if (lv_obj_check_type(object, &lv_label_class)) {
        const std::string value{lv_label_get_text(object)};
        const auto* const font = lv_obj_get_style_text_font(object, LV_PART_MAIN);
        for (const char character : value) {
            lv_font_glyph_dsc_t glyph{};
            require(lv_font_get_glyph_dsc(font, &glyph, static_cast<unsigned char>(character), 0),
                    "every visible OTA character exists in its selected font");
        }
        labels.insert(value);
    }
    for (std::uint32_t index = 0; index < lv_obj_get_child_count(object); ++index) {
        collect_labels(lv_obj_get_child(object, index), labels);
    }
}

[[nodiscard]] std::set<std::string> visible_labels() {
    std::set<std::string> labels;
    collect_labels(lv_screen_active(), labels);
    return labels;
}

[[nodiscard]] bool has_installing_percent(const std::set<std::string>& labels) {
    return labels.count("Installing.") != 0 && (labels.count("100") != 0 || labels.count("%") != 0);
}

void save(const std::filesystem::path& path) {
    auto* const file = std::fopen(path.c_str(), "wb");
    require(file != nullptr, "OTA frame file opens");
    require(std::fwrite(pixels.data(), sizeof(pixels.front()), pixels.size(), file) ==
                pixels.size(),
            "OTA frame writes completely");
    std::fclose(file);
}

prd::app_model::Presentation operational(const prd::app_model::SubjectState subject) {
    const auto fixture = subject == prd::app_model::SubjectState::Ready
                             ? prd::fake_source::Fixture::Ready
                             : prd::fake_source::Fixture::Printing;
    const auto presentation = prd::app_model::project(prd::fake_source::state_for(fixture));
    require(presentation.subject == subject, "native fixture projects the requested endpoint");
    return presentation;
}

prd::app_model::Presentation update(const prd::app_model::OtaPhase phase,
                                    const std::uint8_t progress = 0) {
    prd::app_model::Presentation presentation;
    presentation.layer = phase == prd::app_model::OtaPhase::Failed
                             ? prd::app_model::PresentationLayer::SubjectPresentation
                             : prd::app_model::PresentationLayer::ExclusiveOperation;
    presentation.device_operation = phase == prd::app_model::OtaPhase::Failed
                                        ? prd::app_model::DeviceOperation::None
                                        : prd::app_model::DeviceOperation::FirmwareUpdating;
    presentation.subject = prd::app_model::SubjectState::Ready;
    presentation.confidence = prd::app_model::Confidence::Current;
    presentation.ota_phase = phase;
    presentation.update_version.assign("0.2.0");
    presentation.update_progress = {true, progress};
    return presentation;
}

int controls() {
    Frame frame{};
    frame.fill(1);
    frame.front() = 2;
    require(ink_outside_content(frame), "outside-content detector fires");

    Frame before{};
    Frame after{};
    after.front() = 1;
    require(!changed_only_inside(before, after, theme::kUpdateReadyMark),
            "Ready-mark confinement detector fires");

    Frame fill{};
    const auto accent = rgb565(theme::kColorAccent);
    fill[static_cast<std::size_t>((theme::kUpdateDownloadMark.bottom() - 1) * kWidth +
                                  theme::kUpdateDownloadMark.x)] = accent;
    require(!colour_confined_above(fill, accent, theme::kUpdateDownloadMark,
                                   theme::kUpdateDownloadMark.y + 1),
            "top-to-bottom detector fires");

    require(has_installing_percent({"Installing.", "100", "%"}),
            "Installing-percent detector fires");
    std::puts("PASS OTA pixel negative controls");
    return 0;
}

} // namespace

int main(const int argc, char** const argv) {
    if (argc == 2 && std::string{argv[1]} == "--controls") {
        return controls();
    }
    require(argc == 2, "output directory");
    const std::filesystem::path output{argv[1]};
    std::filesystem::create_directories(output);

    lv_init();
    require(prd::ui::fonts::verify(), "generated font boxes verify");
    lv_log_register_print_cb([](lv_log_level_t, const char* const message) {
        std::fputs(message, stderr);
        std::abort();
    });
    auto* const display = lv_display_create(kWidth, kHeight);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(display, buffer.data(), nullptr, buffer.size(),
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, flush);
    require(prd::ui::controller::build(display), "production controller builds");
    prd::ui::controller::note_panel_visible(true);

    auto tick = [&](const std::uint32_t milliseconds) {
        lv_tick_inc(milliseconds);
        const bool moving = prd::ui::controller::step_acknowledgement();
        lv_refr_now(display);
        return moving;
    };
    auto settle = [&](const prd::app_model::Presentation& presentation) {
        prd::ui::controller::apply(presentation);
        tick(0);
        tick(theme::kOperationalMotionMs);
        require(!ink_outside_content(pixels), "settled OTA frame stays inside canvas.inset");
    };

    // Availability never changes Printing pixels.
    auto printing = operational(prd::app_model::SubjectState::Printing);
    settle(printing);
    const auto printing_plain = pixels;
    save(output / "printing-no-update.rgb565");
    printing.ota_phase = prd::app_model::OtaPhase::Available;
    prd::ui::controller::apply(printing);
    tick(0);
    require(pixels == printing_plain, "Printing has no OTA pixels");
    save(output / "printing-update-available.rgb565");

    // Ready gains only the compact mark.
    auto ready = operational(prd::app_model::SubjectState::Ready);
    settle(ready);
    const auto ready_plain = pixels;
    save(output / "ready-no-update.rgb565");
    ready.ota_phase = prd::app_model::OtaPhase::Available;
    prd::ui::controller::apply(ready);
    tick(0);
    const auto ready_available = pixels;
    require(ready_available != ready_plain, "Ready availability mark is visible");
    require(changed_only_inside(ready_plain, ready_available, theme::kUpdateReadyMark),
            "Ready changes only inside the compact download-mark box");
    require(colour_in(ready_available, rgb565(theme::kColorAccent), theme::kUpdateReadyMark) > 0,
            "Ready mark uses the accent token");
    save(output / "ready-update-available.rgb565");

    // First safe Ready gets one shared-hierarchy statement. Its retirement returns to the compact
    // mark; Printing above remains byte-identical and never sees this surface.
    auto ready_notice = ready;
    ready_notice.update_notice = true;
    settle(ready_notice);
    const auto notice_labels = visible_labels();
    require(notice_labels.count("Update available") == 1 &&
                notice_labels.count("Hold for settings") == 1,
            "availability notice uses the accepted exact hierarchy copy");
    save(output / "ready-update-notice.rgb565");
    settle(ready);

    // The entry is real controller motion, not a static scene sample.
    const auto downloading = update(prd::app_model::OtaPhase::Downloading, 42);
    prd::ui::controller::apply(downloading);
    tick(0);
    const auto download_start = pixels;
    save(output / "downloading-0000.rgb565");
    require(tick(theme::kOperationalMotionMs / 2), "download entry is active at midpoint");
    const auto download_mid = pixels;
    save(output / "downloading-0550.rgb565");
    tick(theme::kOperationalMotionMs - (theme::kOperationalMotionMs / 2));
    const auto download_end = pixels;
    require(download_start != download_mid && download_mid != download_end,
            "download entry paints intermediate frames");

    // Exact transfer percentages grow the accent clip from the top down.
    std::array<std::size_t, 4> fill_counts{};
    constexpr std::array<std::uint8_t, 4> percentages{0, 25, 50, 100};
    const auto accent = rgb565(theme::kColorAccent);
    for (std::size_t index = 0; index < percentages.size(); ++index) {
        settle(update(prd::app_model::OtaPhase::Downloading, percentages[index]));
        fill_counts[index] = colour_in(pixels, accent, theme::kUpdateDownloadMark);
        if (percentages[index] != 100) {
            const auto bottom = theme::kUpdateDownloadMark.y +
                                (theme::kUpdateDownloadMark.height * percentages[index] / 100);
            require(colour_confined_above(pixels, accent, theme::kUpdateDownloadMark, bottom),
                    "download accent is clipped top-to-bottom");
        }
        const auto labels = visible_labels();
        require(labels.count(std::to_string(percentages[index])) == 1 && labels.count("%") == 1,
                "Downloading shows its truthful numeric percentage");
        require(labels.count("Downloading") == 1 &&
                    labels.count("Keep the display powered.") == 1 &&
                    labels.count("Firmware 0.2.0") == 1,
                "Downloading owns the accepted text and version");
        save(output / ("downloading-" + std::to_string(percentages[index]) + ".rgb565"));
    }
    require(fill_counts[0] == 0 && fill_counts[0] < fill_counts[1] &&
                fill_counts[1] < fill_counts[2] && fill_counts[2] < fill_counts[3],
            "download glyph accent grows monotonically with measured progress");

    // Verifying, Installing and Restarting use the same 500 ms 1/2/3-dot cadence.
    struct ProcessCase {
        prd::app_model::OtaPhase phase;
        const char* stem;
        const char* file;
    };
    constexpr std::array<ProcessCase, 3> processes{{
        {prd::app_model::OtaPhase::Verifying, "Verifying", "verifying"},
        {prd::app_model::OtaPhase::Installing, "Installing", "installing"},
        {prd::app_model::OtaPhase::RebootRequired, "Restarting", "restarting"},
    }};
    for (const auto& process : processes) {
        prd::ui::controller::apply(update(process.phase, 100));
        tick(0);
        // Entry settles at 1100 ms. The dot clock has then advanced from one to three, so capture
        // three first and continue at the exact 500 ms cadence to one and two; every saved frame is
        // fully visible rather than using the zero-opacity first entry frame as evidence.
        tick(theme::kOperationalMotionMs);
        constexpr std::array<unsigned, 3> visible_order{3, 1, 2};
        for (const unsigned dots : visible_order) {
            const auto labels = visible_labels();
            require(labels.count(std::string{process.stem} + std::string(dots, '.')) == 1,
                    "process phase advances through the exact dot title");
            if (process.phase == prd::app_model::OtaPhase::Installing) {
                require(!has_installing_percent(labels), "Installing never exposes transfer 100%");
                require(labels.count("100") == 0 && labels.count("%") == 0,
                        "Installing owns no numeric progress fields");
            }
            save(output /
                 (std::string{process.file} + "-dots-" + std::to_string(dots) + ".rgb565"));
            tick(500);
        }
        require(visible_labels().count(std::string{process.stem} + "...") == 1,
                "dot cycle returns to three after 1500 ms");
        require(!ink_outside_content(pixels), "process frame stays inside canvas.inset");
    }

    // Follow the actual update sequence without a scene change. A settled download turning into
    // verification, then installation and restart must change truthful copy immediately while
    // each new composition enters on the same visible clock. A dark interval must not consume
    // the installation entry or run the dot loop behind the backlight.
    settle(update(prd::app_model::OtaPhase::Downloading, 100));
    for (const auto& process : processes) {
        prd::ui::controller::apply(update(process.phase, 100));
        require(tick(0), "an internal update phase starts visible motion");
        require(visible_labels().count(std::string{process.stem} + ".") == 1,
                "the new update phase is truthful on its first frame");
        const auto first = pixels;
        save(output / (std::string{process.file} + "-handoff-first.rgb565"));
        tick(550);
        require(pixels != first, "internal update phase has an intermediate frame");
        const auto middle = pixels;
        save(output / (std::string{process.file} + "-handoff-middle.rgb565"));
        if (process.phase == prd::app_model::OtaPhase::Installing) {
            prd::ui::controller::note_panel_visible(false);
            const auto held_flushes = flushes;
            tick(5000);
            require(pixels == middle && flushes == held_flushes,
                    "installation phase holds its painted frame without Off flushes");
            prd::ui::controller::note_panel_visible(true);
            require(tick(0) && pixels == middle,
                    "installation phase resumes without a hidden time jump");
        }
        tick(550);
        require(pixels != middle, "internal update phase reaches a distinct endpoint");
        require(visible_labels().count(std::string{process.stem} + "...") == 1,
                "the update phase settles on its current truthful title");
        if (process.phase == prd::app_model::OtaPhase::Installing) {
            require(!has_installing_percent(visible_labels()),
                    "installation never carries the old download percentage");
        }
        save(output / (std::string{process.file} + "-handoff-settled.rgb565"));
    }

    settle(update(prd::app_model::OtaPhase::Failed));
    const auto failure_labels = visible_labels();
    require(failure_labels.count("Update failed") == 1 &&
                failure_labels.count("Restart the display.") == 1,
            "failure uses the accepted exact copy");
    require(failure_labels.count("100") == 0 && failure_labels.count("%") == 0,
            "failure leaks no transfer progress");
    require(colour_in(pixels, rgb565(theme::kColorError), theme::kCanvasContent) > 0,
            "failure uses the shared Error-tone symbol");
    save(output / "update-failed.rgb565");

    // An update failure may be dismissed without installing anything. The previous
    // statement must retire immediately, while the ordinary Ready scene returns on
    // the shared visible entry clock rather than appearing fully formed in one frame.
    prd::ui::controller::apply(operational(prd::app_model::SubjectState::Ready));
    require(tick(0), "Ready return after update failure drives visible motion");
    require(visible_labels().count("Update failed") == 0 &&
                prd::ui::controller::report().scene == "operational",
            "failure copy retires on the first truthful operational frame");
    const auto recovery_first = pixels;
    save(output / "update-recovery-first.rgb565");
    require(tick(theme::kOperationalMotionMs / 2), "Ready return remains active at the midpoint");
    const auto recovery_middle = pixels;
    save(output / "update-recovery-middle.rgb565");
    tick(theme::kOperationalMotionMs - theme::kOperationalMotionMs / 2);
    require(recovery_first != recovery_middle && recovery_middle != pixels && pixels == ready_plain,
            "Ready return has intermediate frames and its unchanged settled endpoint");
    require(!tick(16), "recovered Ready stops scheduling after settlement");
    save(output / "update-recovery-settled.rgb565");

    std::printf("PASS native OTA controller pixels frames=%zu progress=%zu/%zu/%zu/%zu\n",
                percentages.size() + (processes.size() * 3U) + 11U, fill_counts[0], fill_counts[1],
                fill_counts[2], fill_counts[3]);
    return 0;
}
