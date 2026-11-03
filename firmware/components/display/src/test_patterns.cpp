// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include "display/test_patterns.hpp"

#include <cstdint>
#include <vector>

#include "display/display.hpp"

namespace prd::display::patterns {
namespace {

// Rows rendered per SPI transfer. 40 rows x 320 px x 2 B = 25.6 kB, matching
// the bus max_transfer_sz configured by the display component.
constexpr int kBandRows = 40;

// Per-band completion allowance. One band is about 5.1 ms at the validated
// 40 MHz clock; this is a wedged-bus detector, not a latency budget.
constexpr int kBlitTimeoutMs = 1000;

// RGB565 for a panel expecting big-endian colour data.
//
// The ESP32-S3 is little-endian, so a uint16_t 0xF800 sits in memory as
// 0x00,0xF8 and the panel would read it as 0x00F8. The bytes are therefore
// swapped here at construction. This is a validation hypothesis: if it is
// wrong, the colour-bar pattern shows obviously incorrect hues, which is
// exactly what that pattern is for.
constexpr std::uint16_t rgb565(const std::uint8_t r, const std::uint8_t g, const std::uint8_t b) {
    const auto native = static_cast<std::uint16_t>(((static_cast<std::uint16_t>(r) & 0xF8U) << 8) |
                                                   ((static_cast<std::uint16_t>(g) & 0xFCU) << 3) |
                                                   (static_cast<std::uint16_t>(b) >> 3));

    return static_cast<std::uint16_t>((native >> 8) | (native << 8));
}

constexpr std::uint16_t kBlack = rgb565(0x00, 0x00, 0x00);
constexpr std::uint16_t kWhite = rgb565(0xFF, 0xFF, 0xFF);
constexpr std::uint16_t kRed = rgb565(0xFF, 0x00, 0x00);
constexpr std::uint16_t kGreen = rgb565(0x00, 0xFF, 0x00);
constexpr std::uint16_t kBlue = rgb565(0x00, 0x00, 0xFF);

// Accepted Product UI tokens, used here purely as colour-reproduction probes.
// These are read-only references to the accepted palette; nothing here defines
// or alters a Project colour token.
constexpr std::uint16_t kNearBlack = rgb565(0x11, 0x13, 0x15); // color.background
constexpr std::uint16_t kOrange = rgb565(0xF7, 0x7F, 0x2F);    // color.accent
constexpr std::uint16_t kTertiary = rgb565(0x7C, 0x84, 0x8E);  // color.tertiary

constexpr std::uint16_t kGrey25 = rgb565(0x40, 0x40, 0x40);
constexpr std::uint16_t kGrey50 = rgb565(0x80, 0x80, 0x80);
constexpr std::uint16_t kGrey75 = rgb565(0xBF, 0xBF, 0xBF);

// Comparison-chart geometry, in transformed (landscape) pixels.
constexpr int kChartRowHeight = 56;   // four content rows
constexpr int kChartMarkerStrip = 16; // bottom strip carrying the mode markers
constexpr int kMarkerSize = 10;
constexpr int kMarkerGap = 6;

std::uint16_t comparison_pixel(const int x, const int y, const int width, const int height,
                               const int marker_count) {
    const int marker_top = height - kChartMarkerStrip;

    // Bottom strip: N white squares on black identify the tuning mode.
    if (y >= marker_top) {
        const int local_y = y - marker_top;
        if (local_y < 3 || local_y >= 3 + kMarkerSize) {
            return kBlack;
        }
        for (int i = 0; i < marker_count; ++i) {
            const int x0 = 6 + i * (kMarkerSize + kMarkerGap);
            if (x >= x0 && x < x0 + kMarkerSize) {
                return kWhite;
            }
        }
        return kBlack;
    }

    const int row = y / kChartRowHeight;

    switch (row) {
    case 0: { // accepted palette references
        const int cell = (x * 4) / width;
        switch (cell) {
        case 0:
            return kOrange; // color.accent      #F77F2F
        case 1:
            return kNearBlack; // color.background  #111315
        case 2:
            return kTertiary; // color.tertiary    #7C848E
        default:
            return kWhite; // neutral white     #FFFFFF
        }
    }
    case 1: { // neutral grey ramp
        const int cell = (x * 3) / width;
        switch (cell) {
        case 0:
            return kGrey25;
        case 1:
            return kGrey50;
        default:
            return kGrey75;
        }
    }
    case 2: { // primaries
        const int cell = (x * 3) / width;
        switch (cell) {
        case 0:
            return kRed;
        case 1:
            return kGreen;
        default:
            return kBlue;
        }
    }
    default: { // smooth RGB565 gradient
        const auto level = static_cast<std::uint8_t>((x * 255) / (width > 1 ? width - 1 : 1));
        return rgb565(level, level, level);
    }
    }
}

// White field with a bottom strip of black marker squares. Inverted relative to
// the comparison chart's strip so the field stays white: light output is what
// makes backlight flicker visible.
std::uint16_t marked_white_pixel(const int x, const int y, const int height,
                                 const int marker_count) {
    const int marker_top = height - kChartMarkerStrip;
    if (y < marker_top) {
        return kWhite;
    }

    const int local_y = y - marker_top;
    if (local_y < 3 || local_y >= 3 + kMarkerSize) {
        return kWhite;
    }

    for (int i = 0; i < marker_count; ++i) {
        const int x0 = 6 + i * (kMarkerSize + kMarkerGap);
        if (x >= x0 && x < x0 + kMarkerSize) {
            return kBlack;
        }
    }

    return kWhite;
}

std::uint16_t pixel_for(const Pattern pattern, const int x, const int y, const int width,
                        const int height) {
    switch (pattern) {
    case Pattern::CornerMarkers: {
        // 1 px border proves nothing is clipped and no offset shifts the
        // image; 12 px filled corners identify orientation unambiguously
        // because each corner uses a different colour.
        constexpr int kMarker = 12;
        const bool border = x == 0 || y == 0 || x == width - 1 || y == height - 1;
        if (border) {
            return kWhite;
        }
        if (x < kMarker && y < kMarker) {
            return kRed; // top-left
        }
        if (x >= width - kMarker && y < kMarker) {
            return kGreen; // top-right
        }
        if (x < kMarker && y >= height - kMarker) {
            return kBlue; // bottom-left
        }
        if (x >= width - kMarker && y >= height - kMarker) {
            return kWhite; // bottom-right
        }
        return kBlack;
    }

    case Pattern::ColourBars: {
        const int band = (x * 8) / width;
        switch (band) {
        case 0:
            return kWhite;
        case 1:
            return rgb565(0xFF, 0xFF, 0x00);
        case 2:
            return rgb565(0x00, 0xFF, 0xFF);
        case 3:
            return kGreen;
        case 4:
            return rgb565(0xFF, 0x00, 0xFF);
        case 5:
            return kRed;
        case 6:
            return kBlue;
        default:
            return kBlack;
        }
    }

    case Pattern::Checkerboard:
        return ((x + y) & 1) ? kWhite : kBlack;

    case Pattern::Gradient: {
        const auto level = static_cast<std::uint8_t>((x * 255) / (width > 1 ? width - 1 : 1));
        return rgb565(level, level, level);
    }

    case Pattern::Black:
        return kBlack;

    case Pattern::White:
        return kWhite;

    case Pattern::NearBlack:
        return kNearBlack;

    case Pattern::Orange:
        return kOrange;

    case Pattern::ComparisonChart:
        return comparison_pixel(x, y, width, height, 1);

    case Pattern::MarkedWhite:
        return marked_white_pixel(x, y, height, 1);
    }

    return kBlack;
}

// Shared band renderer used by both draw() and draw_comparison_chart().
bool render(const Pattern pattern, const int marker_count) {
    const auto size = geometry();
    if (size.width_px <= 0 || size.height_px <= 0) {
        return false;
    }

    std::vector<std::uint16_t> band(static_cast<std::size_t>(size.width_px) *
                                    static_cast<std::size_t>(kBandRows));

    for (int y0 = 0; y0 < size.height_px; y0 += kBandRows) {
        const int rows = (y0 + kBandRows <= size.height_px) ? kBandRows : size.height_px - y0;

        for (int row = 0; row < rows; ++row) {
            const int y = y0 + row;
            for (int x = 0; x < size.width_px; ++x) {
                std::uint16_t value = 0;
                switch (pattern) {
                case Pattern::ComparisonChart:
                    value = comparison_pixel(x, y, size.width_px, size.height_px, marker_count);
                    break;
                case Pattern::MarkedWhite:
                    value = marked_white_pixel(x, y, size.height_px, marker_count);
                    break;
                default:
                    value = pixel_for(pattern, x, y, size.width_px, size.height_px);
                    break;
                }

                band[static_cast<std::size_t>(row) * static_cast<std::size_t>(size.width_px) +
                     static_cast<std::size_t>(x)] = value;
            }
        }

        // blit_blocking, not blit: this loop reuses one band buffer for every
        // slice, so the next iteration overwrites the bytes DMA is reading unless
        // the transfer has completed first.
        //
        // Before the completion contract existed this loop relied on esp_lcd
        // happening to reclaim its transaction queue, which is incidental
        // blocking rather than a guarantee — with a 10-deep queue and 6 bands per
        // frame, nothing forced the earlier bands to finish before their memory
        // was rewritten.
        //
        // A validation pattern generator has no reason to pipeline: it draws a
        // static image and then waits seconds. Correctness is free here, so it is
        // taken rather than double-buffering.
        if (!blit_blocking(0, y0, size.width_px, y0 + rows, band.data(), kBlitTimeoutMs)) {
            return false;
        }
    }

    return true;
}

} // namespace

std::string_view name(const Pattern pattern) {
    switch (pattern) {
    case Pattern::CornerMarkers:
        return "corner-markers";
    case Pattern::ColourBars:
        return "colour-bars";
    case Pattern::Checkerboard:
        return "checkerboard-1px";
    case Pattern::Gradient:
        return "gradient";
    case Pattern::Black:
        return "black";
    case Pattern::White:
        return "white";
    case Pattern::NearBlack:
        return "near-black-111315";
    case Pattern::Orange:
        return "orange-f77f2f";
    case Pattern::ComparisonChart:
        return "comparison-chart";
    case Pattern::MarkedWhite:
        return "marked-white";
    }

    return "unknown";
}

Pattern next(const Pattern pattern) {
    switch (pattern) {
    case Pattern::CornerMarkers:
        return Pattern::ColourBars;
    case Pattern::ColourBars:
        return Pattern::Checkerboard;
    case Pattern::Checkerboard:
        return Pattern::Gradient;
    case Pattern::Gradient:
        return Pattern::Black;
    case Pattern::Black:
        return Pattern::White;
    case Pattern::White:
        return Pattern::NearBlack;
    case Pattern::NearBlack:
        return Pattern::Orange;
    case Pattern::Orange:
        return Pattern::ComparisonChart;
    case Pattern::ComparisonChart:
        return Pattern::CornerMarkers;

    // Sweep-only aid: nothing else returns it, so it stays out of the cycle.
    case Pattern::MarkedWhite:
        return Pattern::CornerMarkers;
    }

    return Pattern::CornerMarkers;
}

bool draw(const Pattern pattern) {
    return render(pattern, 1);
}

bool draw_comparison_chart(const int marker_count) {
    return render(Pattern::ComparisonChart, marker_count);
}

bool draw_marked_white(const int marker_count) {
    return render(Pattern::MarkedWhite, marker_count);
}

} // namespace prd::display::patterns
