// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <string_view>

#include "theme/theme.hpp"

// Production UI Specification §8 Status Message.
//
// INTERNAL HEADER of the ui component.
//
// §8 names this component and its typography outright — "Status Message. Used for: Setup,
// Validation, Unavailable, Attention, Completion, Failure. Uses Body Typography." — so it is a
// specified component being built rather than a composition being invented. Milestone 3.10 needs
// exactly one of those six uses.
//
// ⛔ MILESTONE 3.10 BUILDS IT FOR RECOVERY AND SETUP ONLY. Owner ruling B of this checkpoint:
// keep it general enough to match §8's role, and do NOT broaden this checkpoint into implementing
// §8's other uses. Validation, Unavailable, Attention and Completion get it when the screens that
// need them are built; each of those is a composition decision this checkpoint does not own.
//
// ⛔ NO NEW TOKEN OF ANY KIND. Body typography is §8's own instruction. The colour is an existing
// §5 semantic token chosen by the caller, so a failure message can be Error and an instruction can
// be Secondary without this component deciding which a message is. No new role, no new colour, no
// animation, no layout concept — the constraint owner ruling B states, expressed as an interface
// that has nowhere to put one.
//
// PRESENCE, NOT PLACEHOLDERS. An empty message draws nothing at all, and its space is held rather
// than collapsed — UI Design Decision 003's "information that does not yet exist shall remain
// absent rather than represented by placeholders", and Production UI §2's "hidden components shall
// not cause unrelated layout movement". The same rule every row in the provisioning scene follows.
//
// THIS COMPONENT OWNS NO USER-VISIBLE STRING. The caller passes resolved text from the catalogue
// at text/text.hpp; a literal here would have to be edited to add a language.

namespace prd::ui::component {

class StatusMessage {
  public:
    // Builds the message under `parent`, occupying `width` and wrapping within it. Returns false
    // if the LVGL allocation failed. Render task only.
    [[nodiscard]] bool build(lv_obj_t* parent, std::int32_t width);

    // Shows `message` in `token`. An empty message hides the component.
    void set(std::string_view message, theme::Colour token);

    // Hides it and clears the characters LVGL holds.
    void clear();

    [[nodiscard]] bool built() const {
        return label_ != nullptr;
    }

    // Whether a message is currently drawn. Diagnostic, and what a test asserts against.
    [[nodiscard]] bool visible() const {
        return visible_;
    }

    [[nodiscard]] lv_obj_t* object() const {
        return label_;
    }

  private:
    lv_obj_t* label_{nullptr};
    bool visible_{false};
};

} // namespace prd::ui::component
