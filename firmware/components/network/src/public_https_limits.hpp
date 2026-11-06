// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstddef>

namespace prd::network::public_https_limits {

inline constexpr std::size_t kMaximumUrl = 2048;
// ESP-IDF formats the complete GET request line in this buffer before sending.
// A 2048-byte URL plus method, protocol, CRLF and NUL fits with about 2 KiB spare;
// the SDK's default 512 bytes cannot hold GitHub's signed CDN redirect queries.
inline constexpr int kTransmitBufferSize = 4096;

} // namespace prd::network::public_https_limits
