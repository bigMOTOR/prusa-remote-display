// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include "panel_tuning_internal.hpp"

#include <cstdio>
#include <string>

#include "diagnostics/log.hpp"
#include "diagnostics/tags.hpp"

namespace prd::display {
namespace {

constexpr std::string_view kTag = diagnostics::tag::kDisplay;

// ---------------------------------------------------------------------------
// Waveshare ESP32-S3-LCD-2.8 vendor sequence, transcribed from the demo
// package's own driver:
//   ESP-IDF/ESP32-S3-LCD-2.8-Test/main/LCD_Driver/Vernon_ST7789T/
//     Vernon_ST7789T.c  ->  panel_st7789t_init()
//
// Every register below was checked against ST7789T3_SPEC_V1.0.pdf
// (SHA-256 7a91ce95b3f743111b975f9189c97d99a8c6bcaa0ed026dcaaf6c2979adf9d17).
// Parameter counts agree with the datasheet for every command except 0xD0 —
// see kVendorExact below.
// ---------------------------------------------------------------------------

// Positive / negative voltage gamma control. 14 parameters each, matching
// §9.2.24 (E0h) and §9.2.25 (E1h). Panel-specific values.
constexpr std::uint8_t kGammaPositive[] = {
    0xD0, 0x0D, 0x14, 0x0D, 0x0D, 0x09, 0x38, 0x44, 0x4E, 0x3A, 0x17, 0x18, 0x2F, 0x30,
};
constexpr std::uint8_t kGammaNegative[] = {
    0xD0, 0x09, 0x0F, 0x08, 0x07, 0x14, 0x37, 0x44, 0x4D, 0x38, 0x15, 0x16, 0x2C, 0x2E,
};

// --- Mode B: byte-for-byte reproduction of the vendor sequence -------------
//
// Reproduced exactly as shipped, INCLUDING the defect at 0xD0: the vendor
// passes length 1 for a two-byte array {0xA4, 0xA1}, so only 0xA4 reaches the
// panel and the AVDD/AVCL/VDS parameter is silently dropped. Datasheet §9.2.21
// defines 0xD0 as taking two parameters. This is deliberately NOT corrected
// here — mode D carries the corrected form.
constexpr std::uint8_t kVendorD0Truncated[] = {0xA4};

constexpr TuningCommand kVendorExact[] = {
    {0x36, {0x00}, 1},                         // MADCTL (vendor portrait)
    {0x3A, {0x55}, 1},                         // COLMOD 16bpp
    {0xB0, {0x00, 0xE8}, 2},                   // RAMCTRL: EPF=10, little-endian
    {0xB2, {0x0C, 0x0C, 0x00, 0x33, 0x33}, 5}, // PORCTRL
    {0xB7, {0x75}, 1},                         // GCTRL
    {0xBB, {0x1A}, 1},                         // VCOMS
    {0xC0, {0x80}, 1},                         // LCMCTRL
    {0xC2, {0x01, 0xFF}, 2},                   // VDVVRHEN
    {0xC3, {0x13}, 1},                         // VRHS
    {0xC4, {0x20}, 1},                         // VDVS
    {0xC6, {0x0F}, 1},                         // FRCTRL2 60 Hz
    {0xD0, {0xA4}, 1},                         // PWCTRL1 — TRUNCATED, as shipped
};

// --- Mode D: analogue + gamma, datasheet-corrected -------------------------
//
// Differences from mode B, each deliberate and justified:
//
//  * 0x36 MADCTL, 0x3A COLMOD and 0xB0 RAMCTRL are omitted. They are not
//    colour tuning: they carry our landscape transform, pixel format and the
//    big-endian data path that our byte-swapped pixels depend on. Sending the
//    vendor's values would rotate the image and flip endianness, invalidating
//    the comparison.
//  * 0xD0 sends BOTH parameters. Datasheet §9.2.21 defines 0xD0 as two
//    parameters: a fixed 0xA4 followed by AVDD[1:0] AVCL[1:0] 0 0 VDS[1:0].
//    The vendor's intended second byte 0xA1 decodes to AVDD=0b10 (6.8 V),
//    AVCL=0b10 (-4.8 V), reserved bits 0, VDS=0b01 (2.3 V) — every field is a
//    documented, in-range enumeration and the reserved bits are zero, so the
//    value is safe to transmit. It also matches the vendor's own source
//    comment, so this restores their stated intent rather than inventing one.
constexpr std::uint8_t kVendorD0Full[] = {0xA4, 0xA1};

constexpr TuningCommand kVendorAnalogueAndGamma[] = {
    {0xB2, {0x0C, 0x0C, 0x00, 0x33, 0x33}, 5},
    {0xB7, {0x75}, 1},
    {0xBB, {0x1A}, 1},
    {0xC0, {0x80}, 1},
    {0xC2, {0x01, 0xFF}, 2},
    {0xC3, {0x13}, 1},
    {0xC4, {0x20}, 1},
    {0xC6, {0x0F}, 1},
    {0xD0, {0xA4, 0xA1}, 2}, // corrected: 2 parameters
};

std::string hex_of(const std::uint8_t* data, const std::size_t length) {
    std::string out;
    char buffer[4] = {};
    for (std::size_t i = 0; i < length; ++i) {
        std::snprintf(buffer, sizeof(buffer), "%02X", data[i]);
        if (i != 0) {
            out += ' ';
        }
        out += buffer;
    }
    return out;
}

} // namespace

const char* tuning_mode_name(const TuningMode mode) {
    switch (mode) {
    case TuningMode::Generic:
        return "A-generic";
    case TuningMode::VendorExact:
        return "B-vendor-exact";
    case TuningMode::VendorGammaOnly:
        return "C-vendor-gamma-only";
    case TuningMode::VendorAnalogueAndGamma:
        return "D-vendor-analogue+gamma";
    }
    return "unknown";
}

TuningSequence tuning_sequence(const TuningMode mode) {
    switch (mode) {
    case TuningMode::Generic:
        return {nullptr, 0, false};

    case TuningMode::VendorExact:
        // Overwrites MADCTL and RAMCTRL; both are restored afterwards.
        return {kVendorExact, sizeof(kVendorExact) / sizeof(kVendorExact[0]), true};

    case TuningMode::VendorGammaOnly:
        // ACCEPTED BOARD DEFAULT. No register sequence at all: the caller
        // sends only PVGAMCTRL (0xE0) and NVGAMCTRL (0xE1). The rejected
        // analogue/power block (B2, B7, BB, C0, C2, C3, C4, C6, D0) is
        // deliberately not reachable from here.
        return {nullptr, 0, false};

    case TuningMode::VendorAnalogueAndGamma:
        return {kVendorAnalogueAndGamma,
                sizeof(kVendorAnalogueAndGamma) / sizeof(kVendorAnalogueAndGamma[0]), false};
    }

    return {nullptr, 0, false};
}

bool tuning_sends_gamma(const TuningMode mode) {
    return mode != TuningMode::Generic;
}

const std::uint8_t* gamma_positive(std::size_t& length) {
    length = sizeof(kGammaPositive);
    return kGammaPositive;
}

const std::uint8_t* gamma_negative(std::size_t& length) {
    length = sizeof(kGammaNegative);
    return kGammaNegative;
}

void log_tuning_command(const std::uint8_t command, const std::uint8_t* params,
                        const std::size_t length) {
    diagnostics::log(
        diagnostics::LogLevel::Info, kTag,
        "tx cmd=0x" + hex_of(&command, 1) +
            (length != 0 ? " params=" + hex_of(params, length) : std::string{" params=(none)"}));
}

// Silence unused-variable warnings for the documentation-only aliases above.
static_assert(sizeof(kVendorD0Truncated) == 1, "vendor 0xD0 ships one byte");
static_assert(sizeof(kVendorD0Full) == 2, "datasheet 0xD0 takes two parameters");

} // namespace prd::display
