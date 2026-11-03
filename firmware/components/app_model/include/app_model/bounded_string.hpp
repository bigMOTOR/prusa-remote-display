// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

// Fixed-capacity string for application state (Architecture Spec §6: "bounded
// strings with documented maximum lengths", "fixed-capacity containers where
// practical").
//
// AppState is copied whenever a snapshot is taken and §21 budgets against
// "pervasive heap allocation in hot paths". A std::string in the state would put
// an allocation on every snapshot, and — worse — it would let an external party
// size the state: a printer reporting a 4 kB filename would decide how much
// memory the application state occupies. Both properties become static here.
// sizeof is known at compile time and a copy is a memcpy.
//
// Truncation is recorded rather than silent. §9.3 requires oversized and
// malformed input to be classified rather than quietly discarded, so a caller —
// and a test — can distinguish "the name is this" from "the name did not fit".

namespace prd::app_model {

template <std::size_t Capacity> class BoundedString {
    static_assert(Capacity > 0, "a bounded string needs capacity");
    static_assert(Capacity <= 0xFFFEU, "the stored length is a uint16_t");

  public:
    BoundedString() = default;

    explicit BoundedString(const std::string_view text) {
        assign(text);
    }

    // Copies as much as fits. Returns false when the input did not fit whole,
    // and the truncated() flag records the same fact for later inspection.
    bool assign(const std::string_view text) {
        const std::size_t copied = text.size() < Capacity ? text.size() : Capacity;

        // memcpy with a null source is undefined even for zero length, and an
        // empty string_view may legitimately carry one.
        if (copied != 0) {
            std::memcpy(storage_, text.data(), copied);
        }

        storage_[copied] = '\0';
        length_ = static_cast<std::uint16_t>(copied);
        truncated_ = copied != text.size();

        return !truncated_;
    }

    void clear() {
        storage_[0] = '\0';
        length_ = 0;
        truncated_ = false;
    }

    [[nodiscard]] std::string_view view() const {
        return {storage_, length_};
    }

    // NUL-terminated, so the value can reach a printf-style log without a copy.
    [[nodiscard]] const char* c_str() const {
        return storage_;
    }

    [[nodiscard]] bool empty() const {
        return length_ == 0;
    }

    [[nodiscard]] std::size_t size() const {
        return length_;
    }

    [[nodiscard]] bool truncated() const {
        return truncated_;
    }

    [[nodiscard]] static constexpr std::size_t capacity() {
        return Capacity;
    }

  private:
    char storage_[Capacity + 1]{};
    std::uint16_t length_{0};
    bool truncated_{false};
};

// Truncation is part of the value: two strings that render the same but reached
// that state differently are not the same observation.
template <std::size_t Capacity>
[[nodiscard]] bool operator==(const BoundedString<Capacity>& lhs,
                              const BoundedString<Capacity>& rhs) {
    return lhs.view() == rhs.view() && lhs.truncated() == rhs.truncated();
}

template <std::size_t Capacity>
[[nodiscard]] bool operator!=(const BoundedString<Capacity>& lhs,
                              const BoundedString<Capacity>& rhs) {
    return !(lhs == rhs);
}

} // namespace prd::app_model
