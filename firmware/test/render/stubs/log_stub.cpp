// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only
// Host logging preserves renderer complaints in the regression evidence.
#include "diagnostics/log.hpp"
#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace {
unsigned g_qr_temp_audits = 0;
unsigned g_qr_temp_dirty = 0;
} // namespace

extern "C" void prd_qr_temp_audit(const std::uint8_t* const qr, const std::uint8_t* const data,
                                  const std::size_t size) {
    ++g_qr_temp_audits;
    for (std::size_t index = 0; index < size; ++index) {
        if (qr[index] != 0 || data[index] != 0) {
            ++g_qr_temp_dirty;
            return;
        }
    }
}

namespace prd::diagnostics {
namespace {
unsigned g_setup_qr_draws = 0;
unsigned g_setup_qr_clears = 0;
} // namespace

unsigned setup_qr_draw_count() {
    return g_setup_qr_draws;
}

unsigned setup_qr_clear_count() {
    return g_setup_qr_clears;
}

unsigned qr_temp_audit_count() {
    return g_qr_temp_audits;
}

unsigned qr_temp_dirty_count() {
    return g_qr_temp_dirty;
}

void log(const LogLevel level, const std::string_view tag, const std::string_view message) {
    if (message == "setup qr drawn") {
        ++g_setup_qr_draws;
    } else if (message == "setup qr cleared") {
        ++g_setup_qr_clears;
    }
    static const char* kNames[] = {"D", "I", "W", "E"};
    const auto index = static_cast<unsigned>(level);
    std::fprintf(stderr, "[%s] %.*s: %.*s\n", index < 4 ? kNames[index] : "?",
                 static_cast<int>(tag.size()), tag.data(), static_cast<int>(message.size()),
                 message.data());
}
} // namespace prd::diagnostics
