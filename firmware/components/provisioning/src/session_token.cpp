// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include "session_token.hpp"

namespace prd::provisioning {
namespace {

constexpr std::string_view kHexDigits = "0123456789abcdef";

} // namespace

void SessionToken::regenerate(const RandomByte source, void* const context) {
    for (std::size_t index = 0; index < kTokenBytes; ++index) {
        const std::uint8_t byte = source(context);
        characters_[index * 2] = kHexDigits[(byte >> 4U) & 0x0FU];
        characters_[(index * 2) + 1] = kHexDigits[byte & 0x0FU];
    }

    present_ = true;
}

void SessionToken::clear() {
    auto* const bytes = static_cast<volatile char*>(characters_.data());
    for (std::size_t index = 0; index < characters_.size(); ++index) {
        bytes[index] = '\0';
    }

    present_ = false;
}

std::string_view SessionToken::view() const {
    if (!present_) {
        return {};
    }

    return {characters_.data(), characters_.size()};
}

bool SessionToken::matches(const std::string_view candidate) const {
    if (!present_) {
        return false;
    }

    // The length is fixed and public, so answering it early leaks nothing.
    if (candidate.size() != characters_.size()) {
        return false;
    }

    // Every character is compared, every time. The accumulator is only inspected after the
    // loop, so the work done does not depend on where — or whether — the two differ.
    unsigned difference = 0;
    for (std::size_t index = 0; index < characters_.size(); ++index) {
        difference |=
            static_cast<unsigned>(characters_[index]) ^ static_cast<unsigned>(candidate[index]);
    }

    return difference == 0;
}

} // namespace prd::provisioning
