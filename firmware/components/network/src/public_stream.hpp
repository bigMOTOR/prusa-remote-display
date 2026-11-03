// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "network/public_https.hpp"

namespace prd::network {

enum class PublicStreamResult : std::uint8_t { Accepted, TooLarge, SinkRejected };

// Pure byte-accounting seam used by the SDK adapter and host tests. It checks
// before addition, so an oversized chunk cannot wrap the counter or reach the
// sink even when size_t is narrow.
class PublicStream {
  public:
    PublicStream(std::size_t maximum, PublicChunkSink sink, void* context)
        : maximum_(maximum), sink_(sink), context_(context) {}

    [[nodiscard]] PublicStreamResult append(const std::uint8_t* bytes, std::size_t size);
    [[nodiscard]] std::size_t length() const {
        return length_;
    }

  private:
    std::size_t maximum_{0};
    PublicChunkSink sink_{nullptr};
    void* context_{nullptr};
    std::size_t length_{0};
};

} // namespace prd::network
