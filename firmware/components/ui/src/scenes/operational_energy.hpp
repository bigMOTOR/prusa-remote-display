// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstdint>
#include <optional>

#include "app_model/state_layers.hpp"

// Operational energy — UI Design Decision 004 §2.
//
// INTERNAL to the ui component. LVGL-free, so the axis is host-testable.
//
// §1 states the law the whole family obeys: "The Operational Family is one composition
// observed at six energies. State shall never be drawn. Only energy shall be drawn." This file
// is the axis that makes that possible — the one place a subject state becomes a magnitude, so
// that everything downstream draws the magnitude and never the state.
//
// §2.1 defines it: operational energy is "how much of a job the machine is currently carrying".
// It is scalar. It has magnitude only, and direction of change is never expressed.
//
// WHY IT LIVES HERE AND NOT IN THE MODEL. UIDD 004's Scope is explicit: energy "belongs to this
// presentation layer alone. It is not a property of the state model". It determines shape (§3),
// luminance (§4) and accent (§5), all of which are visual — and `Presentation` is semantic by
// contract (Architecture §9.4: "only values required for presentation", and presentation.hpp's
// own rule that it "may say 'the progress ring is present', never 'the progress ring is
// 96 px'"). So the axis sits in the ui component beside the content table, and AppState never
// learns it exists.

namespace prd::ui::scene {

// A point on the axis. Integral rather than floating point: it is interpolated into pixel
// geometry, and an integer scale keeps that arithmetic exact and identical on the host and on
// the target.
using Energy = std::int32_t;

// The scale. Dimensionless — a proportion of the axis, not a measurement, so it is not a
// production token and does not belong in theme/.
inline constexpr Energy kEnergyMin = 0;
inline constexpr Energy kEnergyMax = 1000;

// --- The six energies ------------------------------------------------------
//
// §2.2 gives the ORDER and nothing else:
//
//     1 Printing    carrying a job and advancing it
//     2 Paused      carrying the whole job, suspended
//     3 Completed   carrying a fully realised job and its acknowledgement
//     4 Preparing   carrying a commitment with nothing yet realised
//     5 Stopped     carrying a remainder that will not be completed
//     6 Ready       carrying nothing
//
// ---------------------------------------------------------------------------
// WORKING BASELINE — NOT ACCEPTED VALUES
// ---------------------------------------------------------------------------
//
// §15 places "the magnitude of each of the six energies" among the design acts to be performed
// against the physical panel, and states that they are "not omissions from this decision". So
// the magnitudes below are provisional and are settled in Phase H, once, against the complete
// family — not here and not one state at a time.
//
// EVENLY SPACED DELIBERATELY. The ordering is all §2.2 gives, and even spacing asserts nothing
// beyond it. Any other spacing would be a claim about how much more energy Paused carries than
// Completed, which is exactly the judgement §15 reserves for the panel. Read these as "the
// ordering, made numeric", not as a curve anyone has chosen.
//
// The two endpoints are different in kind and are NOT provisional in the same way: Printing is
// the maximum and Ready the minimum by §2.2, and pinning them to the ends of the scale is what
// makes the two states with accepted artwork (UIDD 001 and UIDD 002) reproduce their current
// geometry exactly. They are the controls the rest of the axis is judged against.

inline constexpr Energy kEnergyPrinting = kEnergyMax;
inline constexpr Energy kEnergyPaused = 800;
inline constexpr Energy kEnergyCompleted = 600;
inline constexpr Energy kEnergyPreparing = 400;
inline constexpr Energy kEnergyStopped = 200;
inline constexpr Energy kEnergyReady = kEnergyMin;

// §2.2's order, asserted where it cannot drift. A magnitude edited in Phase H that broke the
// ordering would still compile and would silently contradict an accepted Project Source; this
// is the one property of the table that is not provisional.
static_assert(kEnergyPrinting > kEnergyPaused, "§2.2: Printing carries more than Paused");
static_assert(kEnergyPaused > kEnergyCompleted, "§2.2: Paused carries more than Completed");
static_assert(kEnergyCompleted > kEnergyPreparing, "§2.2: Completed carries more than Preparing");
static_assert(kEnergyPreparing > kEnergyStopped, "§2.2: Preparing carries more than Stopped");
static_assert(kEnergyStopped > kEnergyReady, "§2.2: Stopped carries more than Ready");

static_assert(kEnergyPrinting == kEnergyMax, "§2.2: Printing is the highest energy");
static_assert(kEnergyReady == kEnergyMin, "§2.2: Ready is the lowest energy");

// §2.2: "Minimum energy is not zero. Ready shall read as awake, connected and settled." The
// zero of this scale is therefore the bottom of the axis, not an absence — what it produces is
// the accepted resting form of UIDD 002, which is a drawn thing. Nothing downstream may treat
// kEnergyMin as "no form"; that is §13.1's constant inventory, and it is why the scale has no
// value below Ready to be confused with one.

// The operational energy of a subject state, or nothing if the state is not in the family.
//
// EMPTY IS NOT LOW. UIDD 004's Scope confines operational energy to the six operational states:
// it "shall not be used to express anything outside this family". A state outside the family
// does not sit at the bottom of the axis — it is not on the axis at all, and the two are
// different facts that must not be representable by the same value. Returning the minimum for
// a non-member would make it indistinguishable from Ready, which is a real point on the axis
// with an accepted resting form (UIDD 002).
//
// std::optional rather than a sentinel, for the reason app_model/values.hpp already records:
// the `known` flag pattern exists there because those values live inside AppState and a
// snapshot must stay a memcpy, and that note names std::optional as the right choice "where
// the value is not part of AppState". This is a return value in the ui component, so the
// engaged-ness check costs nothing that matters and the type states the rule.
//
// The caller therefore cannot reach a magnitude for a non-member by accident. That is
// deliberate: the operational composition is selected only for the family (scene_selection.hpp
// routes AcquiringState to Production UI §9's own Connecting screen), so an empty result at a
// drawing site means the selector and this axis disagree, which is a defect rather than a
// state to render.
//
// Total over SubjectState so that adding a state fails to compile until it has been placed on
// the axis, or deliberately kept off it — which is UIDD 004's ninth acceptance criterion:
// "adding a further operational state requires placing it on the axis rather than designing a
// screen."
[[nodiscard]] std::optional<Energy> energy_for(app_model::SubjectState state);

} // namespace prd::ui::scene
