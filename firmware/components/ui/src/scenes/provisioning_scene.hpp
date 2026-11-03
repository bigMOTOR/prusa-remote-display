// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <array>
#include <cstddef>
#include <string_view>

#include "app_model/presentation.hpp"
#include "components/message_form.hpp"
#include "scenes/unavailable_motion.hpp"
// text::TextId names which catalogue entry a composition draws — see draw_statement().
#include "text/text.hpp"
#include "theme/theme.hpp"
#include "ui/ui.hpp"

// The provisional provisioning composition — Milestone 3.6.
//
// ⚠️ PROVISIONAL, AND NOT A COMPOSITION CANDIDATE. Owner ruling R3: this scene is
// deliberately minimal and exists only to make Milestone 3.5's setup access point usable
// on hardware. It does NOT participate in the Operational Family freeze, it is NOT an
// accepted composition, and it must not cause corrective UI design work. It is built from
// existing Production UI §4 roles and §3 tokens and introduces no new one of either.
//
// WHAT IT DRAWS. A text column on the left, a Wi-Fi QR code on the right, and the session
// password full width beneath both:
//
//     SetupRequired    the title alone. The access point is not up yet — the phase is
//                      dispatched before setup_ap::start() — and it is also where M3.5
//                      leaves the phase if the access point fails to start.
//     SetupActive      the title, the network name, the address once M3.7's server is
//                      listening, the password, and the code that joins the same network.
//     RecoveryMode     the recovery title and ONE instruction, in Production UI §8's Status
//                      Message. No network, no password, no address, no code — Milestone 3.10
//                      raises no access point automatically, so there is nothing to show and
//                      the presence rule hides all of it.
//
// ⬅️ WHY RECOVERY IS HERE AT ALL RATHER THAN IN A SCREEN OF ITS OWN. UIAD 001 §12.3 puts Recovery
// Mode in the same presentation tier as the four provisioning phases, and UI Design Decision 003
// says a failure state does not establish a different visual identity. Production UI §9 specifies
// no Recovery screen, so a new composition would be invention; a scene-less state would be a blank
// panel for a condition the device enters on its own. Owner ruling B of Milestone 3.10 settles it:
// build §8's already-specified Status Message, and nothing else.
//
// ⬅️ THE QR CODE IS A CONVENIENCE PATH AND NEVER A REPLACEMENT. Owner ruling, 2026-08-16:
// the SSID, the password and the address stay on the panel as readable text, because a
// code that replaced them would strand anyone whose camera cannot resolve it — and the
// password's readability is itself a hardware-validated M3.6 finding. The code is drawn
// from the same session as the text, in the same call, so the two cannot describe
// different networks.
//
// ⛔ THE PASSWORD IS THE ONLY SECRET THE RENDERER EVER HOLDS, and it holds it because
// there is no other way to show a person a password. It arrives through
// ui::submit_setup_session(), not through Presentation, and it is overwritten — in this
// object and in LVGL's own label buffer — when the session ends.
//
// PRESENCE, NOT PLACEHOLDERS. A row is shown if and only if its value exists. UI Design
// Decision 003: "Information that does not yet exist shall remain absent rather than
// represented by placeholders." Row positions are fixed, so a hidden row leaves its space
// rather than moving the rows below it — Production UI §2: "Hidden components shall not
// cause unrelated layout movement."

namespace prd::ui::scene {

class ProvisioningScene {
  public:
    // Builds the composition under `parent`. Returns false if any LVGL allocation
    // failed. Render task only.
    [[nodiscard]] bool build(lv_obj_t* parent);

    // Applies a presentation. Only the provisioning phase is read; nothing else in the
    // presentation is relevant to this scene.
    void apply(const app_model::Presentation& presentation);

    // Applies a session. Idempotent, and independent of which scene is visible: the
    // session and the phase arrive on different queues and either order must work.
    void apply_session(const SetupSession& session);

    // Overwrites the retained credential and the characters LVGL holds.
    void clear_session();

    void set_visible(bool visible);

    // One render tick. Returns whether the composition is still moving, so the render loop keeps
    // its ramp cadence while a statement is appearing and settles to the idle wait afterwards.
    //
    // ⬅️ BOTH HALVES ANIMATE, AND NEITHER MOVES. Owner ruling R3 freezes the QR and session
    // GEOMETRY; nothing here repositions or resizes anything. What the clock carries is each
    // object's own opacity, so the settled composition is the pixels the ruling froze.
    [[nodiscard]] bool step(bool panel_visible, std::uint32_t now);

    [[nodiscard]] bool built() const {
        return root_ != nullptr;
    }

  private:
    // One caption-plus-value row. The caption is a catalogue entry; the value is a
    // runtime string and has none.
    struct Row {
        lv_obj_t* caption{nullptr};
        lv_obj_t* value{nullptr};
    };

    // LVGL's dynamic label setter frees before reallocating, leaving the old password in
    // its heap pool. Keep the displayed copy in scene-owned storage so every withdrawal
    // can wipe the exact bytes LVGL read, without depending on allocator reuse.
    std::array<char, 64> password_label_text_{};

    enum class RowIndex : std::size_t {
        Network = 0,
        Password = 1,
        Address = 2,
    };

    static constexpr std::size_t kRowCount = 3;

    // Which §4 role and luminance step each row's value is set in.
    //
    // ⬅️ THE PASSWORD IS LOUDER THAN THE ROWS AROUND IT, and that is a hardware-validation
    // finding rather than a design preference: on the physical 2.8-inch panel the session
    // password at Section was legible but too small to transcribe reliably by hand
    // (owner, 2026-08-15). It is the one value on this screen a person has to copy
    // character by character into another device, and it is read at arm's length.
    //
    // StateTitle is the CEILING, not a choice among many. tools/generate-fonts.py builds
    // hero_68 and display_36 from DISPLAY_SUBSET — space, '%', '.', ':' and the digits —
    // so either would drop every letter of an alphanumeric password silently. title_20 is
    // the largest face carrying TEXT_SUBSET, and it is already SemiBold.
    //
    // The other rows drop to Secondary rather than growing: Production UI §5 establishes
    // hierarchy "primarily through luminance", and lowering the neighbours is what keeps
    // the two rows distinct without inventing a size the token scale does not have.
    static constexpr std::array<theme::TextRole, kRowCount> kValueRoles{
        theme::TextRole::Body,       // Network
        theme::TextRole::StateTitle, // Password
        theme::TextRole::Body,       // Address
    };

    static constexpr std::array<theme::Colour, kRowCount> kValueColours{
        theme::Colour::Secondary, // Network
        theme::Colour::Primary,   // Password
        theme::Colour::Secondary, // Address
    };

    // The trap this scene is one edit away from at all times.
    static_assert(kValueRoles[static_cast<std::size_t>(RowIndex::Password)] !=
                          theme::TextRole::PrimaryValue &&
                      kValueRoles[static_cast<std::size_t>(RowIndex::Password)] !=
                          theme::TextRole::PrimaryUnit,
                  "the password is alphanumeric; the hero and display faces are generated "
                  "with a digits-only subset and would drop every letter without warning");

    [[nodiscard]] Row& row(RowIndex index) {
        return rows_[static_cast<std::size_t>(index)];
    }

    // The shared full-screen message composition — component::MessageForm. Drawn by the recovery
    // state and by the four device faults this scene has copy for. `subject` names the caller in
    // the overflow diagnostic.
    //
    // ⛔ IT NO LONGER BORROWS THE SESSION TITLE. Until 2026-09-12 a statement promoted `title_` to
    // StateTitle and widened it, then restored both on the way out — which is why the session
    // composition's geometry had to be re-stated on every other path. The statement now has its
    // own object, so `title_` is Body at the column measure for its whole life and owner ruling
    // R3's frozen geometry cannot be disturbed by a fault that happens to arrive.
    void draw_statement(text::TextId title, text::TextId message, text::MessageTone tone,
                        std::string_view subject);

    // Hides the session composition while a statement owns the canvas, and clears the credential
    // it was drawing. The retained session survives, so the rows and the code come back when the
    // statement does not.
    void hide_session();

    // Puts it back, from the session that was retained throughout.
    void show_session();

    // One appearance level for whichever half is drawn. paint_session() fades the session
    // composition object by object — see the note at its definition.
    void paint(std::int32_t level);
    void paint_session(std::int32_t level);
    void start_session_reveal();

    void set_row(RowIndex index, std::string_view value);

    // Rebuilds the code from the retained session, or hides it. Called on every path that
    // changes the session, so the drawn code and the drawn password can never disagree.
    void apply_qr();

    lv_obj_t* root_{nullptr};
    lv_obj_t* title_{nullptr};
    std::array<Row, kRowCount> rows_{};

    // The light field carrying the four-module quiet zone, and the code inside it.
    lv_obj_t* qr_panel_{nullptr};
    lv_obj_t* qr_{nullptr};

    // The shared message composition — 2026-09-12, superseding the Milestone 3.10 Status Message.
    //
    // ⛔ DRAWN ONLY WHEN THE SESSION ROWS AND THE CODE ARE NOT, and now enforced rather than
    // assumed. It covers the whole canvas, so the two really are mutually exclusive; before this
    // it merely occupied the space the session would have used, and a device fault raised while
    // the setup access point is still up — which the DriverStalled ruling explicitly allows —
    // would have drawn the code over the statement.
    component::MessageForm form_;

    // The statement's own entry clock. The session composition has none; see step(). It HOLDS
    // while the panel is Off rather than only refusing to start — see HoldClock.
    HoldClock clock_{};
    bool running_{false};
    bool statement_{false};

    // A setup phase may change without changing scenes (for example, checking the submitted
    // network). Keep the session rows and QR stable while the new, truthful title rises from the
    // existing Medium luminance step to Full on the shared motion clock.
    HoldClock phase_clock_{};
    app_model::ProvisioningPhase phase_{app_model::ProvisioningPhase::Inactive};
    bool phase_seen_{false};
    bool phase_running_{false};
    std::int32_t phase_from_level_{0};
    std::int32_t phase_level_{0};

    // Session data may arrive before or after its phase. It is never drawn while SetupRequired
    // owns the panel; when it becomes drawable, the fixed rows and QR fade in together without
    // relocating. Retiring a credential is immediate and never animated out.
    HoldClock session_clock_{};
    bool session_running_{false};
    std::int32_t session_level_{0};
    bool offering_network_{false};

    // Which statement is currently drawn, so a replacement is recognised as a change. A device
    // fault displacing a settled recovery statement is reachable and must enter, not swap.
    text::TextId statement_title_{text::TextId::StatePrinting};
    text::TextId statement_message_{text::TextId::StatePrinting};
    text::MessageTone statement_tone_{text::MessageTone::Warning};

    std::int32_t level_{-1};

    // Whether the retained session may currently be drawn. False while a statement owns the
    // canvas: the session still EXISTS — the access point may well still be up — but nothing of
    // it is on the panel, and the encoded code is cleared rather than merely covered.
    bool session_drawable_{true};

    // Whether a code is currently drawn, so the diagnostics report each transition once
    // rather than on every session that arrives.
    bool was_drawn_{false};

    SetupSession session_{};
    bool has_session_{false};
};

} // namespace prd::ui::scene
