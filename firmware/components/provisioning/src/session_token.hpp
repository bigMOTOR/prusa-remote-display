// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

// The provisioning session's CSRF token — Architecture Spec §14.4, owner ruling D9.
//
// PURE, AND HOST-TESTED. The entropy source is injected exactly as the setup access
// point's password generator injects it, so the properties that matter — an unbiased draw
// and a comparison that does not leak its answer through timing — are checkable without a
// board.
//
// ⛔ WHAT THIS TOKEN IS, AND IS NOT. It is ephemeral session state, not a credential and
// not a login. Owner ruling D9: never persisted, never logged, never in AppState,
// regenerated whenever the provisioning server or session is recreated. It defends against
// another origin in the reader's browser submitting on their behalf — which is what CSRF
// is — and not against whoever holds the setup access point's password, who is by
// definition the person standing in front of the panel.
//
// IT IS THEREFORE NOT COVERED BY §5.11's "never return stored secrets to the browser". The
// token is minted for the browser and is useless to anyone who cannot already read the
// same-origin response carrying it.

namespace prd::provisioning {

// 32 hex characters — 128 bits. Long enough that guessing is not a strategy, short enough
// to sit in one header line.
inline constexpr std::size_t kTokenBytes = 16;
inline constexpr std::size_t kTokenLength = kTokenBytes * 2;

// Where a byte of entropy comes from. The ObservationSink convention, not a second one.
using RandomByte = std::uint8_t (*)(void* context);

class SessionToken {
  public:
    // Draws a fresh token. Every call produces a new one — D9's "regenerated when the
    // provisioning server/session is recreated" is enforced by there being no other way to
    // obtain one.
    void regenerate(RandomByte source, void* context);

    // Overwrites the characters rather than marking them absent.
    void clear();

    [[nodiscard]] bool present() const {
        return present_;
    }

    [[nodiscard]] std::string_view view() const;

    // CONSTANT TIME OVER THE TOKEN'S OWN LENGTH. A comparison that returned early on the
    // first differing character would let a caller recover the token one character at a
    // time from response latency. Length mismatch is answered first and is not secret —
    // the length is fixed and public.
    [[nodiscard]] bool matches(std::string_view candidate) const;

  private:
    std::array<char, kTokenLength> characters_{};
    bool present_{false};
};

} // namespace prd::provisioning
