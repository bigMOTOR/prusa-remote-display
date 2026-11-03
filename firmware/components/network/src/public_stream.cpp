// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include "public_stream.hpp"

namespace prd::network {

PublicStreamResult PublicStream::append(const std::uint8_t* bytes, const std::size_t size) {
    if (size > maximum_ - length_) {
        return PublicStreamResult::TooLarge;
    }
    if (size == 0) {
        return PublicStreamResult::Accepted;
    }
    if (bytes == nullptr || sink_ == nullptr || !sink_(context_, bytes, size)) {
        return PublicStreamResult::SinkRejected;
    }
    length_ += size;
    return PublicStreamResult::Accepted;
}

} // namespace prd::network
