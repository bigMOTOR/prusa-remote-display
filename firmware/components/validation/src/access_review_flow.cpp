// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include "sdkconfig.h"

#if !defined(CONFIG_PRD_VALIDATION_ACCESS_REVIEW) ||                                               \
    !defined(CONFIG_PRD_VALIDATION_PRINTING_TIMELINE)
#error "Access review requires the isolated synthetic timeline selection"
#endif
#if defined(CONFIG_PRD_VALIDATION_PRINTING_LOSS_REVIEW) ||                                         \
    defined(CONFIG_PRD_VALIDATION_SCENARIOS) || defined(CONFIG_PRD_VALIDATION_OPERATIONAL_WALK) || \
    defined(CONFIG_PRD_UI_DIAGNOSTIC_SCENE)
#error "Access review must be the only diagnostic store and scene source"
#endif

#include "access_review.hpp"
#include "gesture_policy.hpp"
#include "validation/validation.hpp"

namespace prd::validation {
namespace {
AccessReview g_access_review;
} // namespace

// Deliberately retains the existing composition-root synthetic-source ownership API.
// CMake replaces printing_timeline_flow.cpp, so exactly one implementation can be linked.
bool printing_timeline_compiled() {
    return true;
}

app_model::display_policy::Output
access_review_output(const app_model::DisplayPower power,
                     const app_model::display_policy::Tier tier,
                     const app_model::display_policy::Levels& levels,
                     const app_model::display_policy::Timings& timings) {
    // Readable emitter, without extending any semantic On window.
    const auto light_power =
        g_access_review.step() + 1 < AccessReview::kStepCount ? app_model::DisplayPower::On : power;
    return app_model::display_policy::output_for(light_power, tier, levels, timings);
}

bool gesture_is_consumed(const app_model::ButtonGesture gesture) {
    return disposition_of(gesture, true) == GestureDisposition::DiagnosticOnly;
}

void printing_timeline_begin(app_store::Store& store, const app_model::Timestamp now) {
    g_access_review.begin(store, now);
}

void printing_timeline_tick(app_store::Store& store, const app_model::Timestamp now) {
    g_access_review.tick(store, now);
}

void printing_timeline_on_gesture(app_model::ButtonGesture /*gesture*/) {
    // This automatic review has no product button meaning or destructive diagnostic control.
}

} // namespace prd::validation
