// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

namespace prd::platform {

// Restarts the chip (Bootstrap Spec §6.6). Never returns.
[[noreturn]] void restart();

} // namespace prd::platform
