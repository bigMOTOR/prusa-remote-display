// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include "loss_review.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <string>

#include "app_model/display_policy.hpp"
#include "app_model/presentation.hpp"
#include "diagnostics/log.hpp"
#include "diagnostics/tags.hpp"
#include "status_timeline.hpp"

namespace prd::validation {
namespace {
using app_model::Timestamp;
using namespace std::chrono_literals;
struct Chapter {
    std::string_view name;
    std::size_t origin;
    std::size_t destination;
    Timestamp recover_after;
    bool via_off;
};
// Bench observation points, NOT product timing constants. The model still owns the
// 20s acknowledgement and 2s fade. Tail/Off preconditions below reject a missed window.
constexpr std::array<Chapter, LossReview::kChapterCount> kChapters{{
    {"Ready settled recovery", 0, 0, 3500ms, false},
    {"Printing entry recovery to Completed", 8, 14, 400ms, false},
    {"Printing tail recovery", 8, 10, 20600ms, false},
    {"Ready retired Off recovery to Printing", 0, 8, 23000ms, true},
}};
constexpr auto kAgeCadence = 100ms;
constexpr auto kRecoveryHold = 8s;
constexpr auto kRefresh = 5s;
} // namespace

void LossReview::report(const std::string_view event, const Timestamp now) const {
    diagnostics::log(
        diagnostics::LogLevel::Info, diagnostics::tag::kReview,
        "loss-review chapter=" + std::to_string(std::min(chapter_ + 1, kChapterCount)) + "/" +
            std::to_string(kChapterCount) + " event=" + std::string{event} + " at_ms=" +
            std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(now).count()) +
            " recovered=" + std::to_string(recoveries_) +
            " manual=" + std::to_string(manual_controls_));
}

void LossReview::fail(const std::string_view reason) {
    phase_ = Phase::Failed;
    diagnostics::log(diagnostics::LogLevel::Error, diagnostics::tag::kReview,
                     "loss-review INVALID: " + std::string{reason});
}

bool LossReview::publish(app_store::Store& store, const Timestamp now, const std::size_t fixture) {
    const auto applied = apply_status_body(store, status_timeline_step_at(fixture).body, now,
                                           {order_.generation, order_.sequence++});
    if (!applied.dispatched || store.state().printer.observed_at != now ||
        store.state().confidence != app_model::Confidence::Current) {
        fail("fixture did not become current at dispatch time");
        return false;
    }
    return true;
}

void LossReview::begin(app_store::Store& store, const Timestamp now,
                       const app_model::SnapshotOrder order,
                       const std::uint32_t prior_manual_controls) {
    *this = LossReview{};
    manual_controls_ = prior_manual_controls;
    order_ = order;
    phase_ = Phase::Seed;
    tick(store, now);
}

void LossReview::recover(app_store::Store& store, const Timestamp now) {
    if (!publish(store, now, kChapters.at(chapter_).destination)) {
        return;
    }
    if (app_model::project(store.state()).observation_lost) {
        fail("current recovery retained loss");
        return;
    }
    ++recoveries_;
    phase_ = Phase::Recovered;
    phase_at_ = now;
    report("recovery", now);
}

void LossReview::note_manual_control() {
    ++manual_controls_;
}

void LossReview::skip(app_store::Store& store, const Timestamp now) {
    note_manual_control();
    if (phase_ == Phase::Failed || phase_ == Phase::Inactive || phase_ == Phase::FinalReady ||
        chapter_ >= kChapterCount) {
        return;
    }
    report("SKIPPED", now);
    ++chapter_;
    phase_ = Phase::Seed;
    tick(store, now);
}

void LossReview::tick_seed(app_store::Store& store, const Timestamp now) {
    if (chapter_ >= kChapterCount) {
        if (!publish(store, now, status_timeline_step_count() - 1)) {
            return;
        }
        phase_ = Phase::FinalReady;
        next_refresh_ = now + kRefresh;
        report(unattended() && recoveries_ == kChapterCount ? "complete-final-ready"
                                                            : "INCOMPLETE-final-ready",
               now);
        return;
    }
    if (!publish(store, now, kChapters.at(chapter_).origin)) {
        return;
    }
    phase_ = Phase::AwaitLoss;
    next_age_ = now + kAgeCadence;
    report(kChapters.at(chapter_).name, now);
}

void LossReview::tick_loss(app_store::Store& store, const Timestamp now) {
    const auto& chapter = kChapters.at(chapter_);
    if (now - phase_at_ < chapter.recover_after) {
        return;
    }
    const auto view = app_model::project(store.state());
    const bool retired = chapter.recover_after > 20s;
    if (!view.observation_lost || view.observation_lost_acknowledgement == retired) {
        fail("recovery phase does not match the real retirement");
        return;
    }
    if (chapter.via_off) {
        if (store.state().display.power != app_model::DisplayPower::Dimmed) {
            fail("retired chapter is not Dimmed before manual sleep");
            return;
        }
        // Synthetic SHORT PRESS only. It takes the normal model path to Off; this
        // instrument never emits either reset gesture or any hardware operation.
        (void)store.dispatch(
            app_model::Event::button_gesture(now, app_model::ButtonGesture::ShortPress));
        if (store.state().display.power != app_model::DisplayPower::Off) {
            fail("short press did not reach Off");
            return;
        }
        phase_ = Phase::OffFade;
        phase_at_ = now;
        report("manual-off", now);
    } else {
        recover(store, now);
    }
}

void LossReview::tick(app_store::Store& store, const Timestamp now) {
    // The previous prelude can be ahead after very rapid manual steps. Wait for actual
    // time to catch up; never invent another future timestamp to clear that floor.
    if (now < store.state().last_event_at) {
        return;
    }
    if (phase_ == Phase::Seed) {
        tick_seed(store, now);
        return;
    }
    if (phase_ == Phase::AwaitLoss) {
        if (now < next_age_) {
            return;
        }
        next_age_ = now + kAgeCadence;
        (void)store.dispatch(app_model::Event::confidence_aged(now));
        if (store.state().confidence == app_model::Confidence::Unavailable) {
            if (!app_model::project(store.state()).observation_lost_acknowledgement) {
                fail("expiry produced no acknowledgement");
                return;
            }
            phase_ = Phase::Loss;
            phase_at_ = now;
            report("loss", now);
        }
        return;
    }
    if (phase_ == Phase::Loss) {
        tick_loss(store, now);
        return;
    }
    if (phase_ == Phase::OffFade) {
        const auto wait =
            std::chrono::milliseconds{app_model::display_policy::Timings{}.fade_ms} + 1s;
        if (now - phase_at_ < wait) {
            return;
        }
        if (store.state().display.power != app_model::DisplayPower::Off) {
            fail("Off was interrupted before recovery");
            return;
        }
        recover(store, now);
        return;
    }

    if (phase_ == Phase::Recovered && now - phase_at_ >= kRecoveryHold) {
        ++chapter_;
        phase_ = Phase::Seed;
        return;
    }

    if (phase_ == Phase::FinalReady && now >= next_refresh_) {
        if (publish(store, now, status_timeline_step_count() - 1)) {
            next_refresh_ = now + kRefresh;
        }
    }
}

} // namespace prd::validation
