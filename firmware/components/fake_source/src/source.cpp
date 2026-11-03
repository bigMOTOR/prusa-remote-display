// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include "fake_source/source.hpp"

namespace prd::fake_source {
namespace {

// Observation sequences advance by this much per generation. Comfortably more than
// any scenario's step count, so two generations can never overlap and a replay can
// never be mistaken for a stale response.
constexpr std::uint32_t kSequenceStride = 100;

} // namespace

FakeSource::FakeSource(app_store::Store& store, const Scenario scenario,
                       const app_model::Timestamp base)
    : store_{store}, scenario_{scenario}, base_{base} {}

void FakeSource::load(const Scenario scenario, const app_model::Timestamp base) {
    scenario_ = scenario;
    base_ = base;
    next_index_ = 0;
    paused_ = false;
    dwelling_ = false;
    finished_at_ = app_model::Timestamp::zero();
    ++generation_;
}

void FakeSource::reset(const app_model::Timestamp base) {
    load(scenario_, base);
}

void FakeSource::pause() {
    paused_ = true;
}

void FakeSource::resume() {
    paused_ = false;
}

std::uint32_t FakeSource::sequence_base() const {
    return generation_ * kSequenceStride;
}

Step FakeSource::peek() const {
    return step_at(scenario_, next_index_, base_, sequence_base());
}

FakeSource::Advance FakeSource::advance() {
    Advance result;
    result.step_index = next_index_;
    result.revision = store_.revision();

    if (paused_ || finished()) {
        return result;
    }

    const Step step = peek();
    ++next_index_;

    // The only thing this class does to the store.
    const auto outcome = store_.dispatch(step.event);

    result.applied = outcome.accepted;
    result.mutated = outcome.mutated;
    result.revision = outcome.revision;
    result.label = step.label;

    if (outcome.accepted) {
        ++steps_applied_;
    }

    return result;
}

FakeSource::Sweep FakeSource::sweep(const app_model::Timestamp now,
                                    const std::chrono::milliseconds dwell) {
    Sweep result;

    result.steps_applied = advance_to(now);

    if (!finished()) {
        return result;
    }

    // Reached the end on this call. Reported once so a driver logs one completion
    // line, not one per tick for as long as the dwell lasts.
    if (!dwelling_) {
        dwelling_ = true;
        finished_at_ = now;
        result.just_finished = true;
        return result;
    }

    // The boundary rule. advance_to() already refuses while paused, but a finished
    // scenario has nothing left to advance — without this check the sweep would walk
    // straight past the pause and load the next scenario, and load() clears the pause
    // flag. See the note on the declaration.
    if (paused_) {
        return result;
    }

    if (now - finished_at_ >= std::chrono::duration_cast<app_model::Timestamp>(dwell)) {
        load(scenario_at((scenario_index(scenario_) + 1) % kScenarioCount), now);
        result.loaded_next = true;
    }

    return result;
}

std::size_t FakeSource::advance_to(const app_model::Timestamp now) {
    std::size_t applied = 0;

    if (paused_) {
        return applied;
    }

    // Bounded by the steps that remain, so a step whose timestamp somehow did not
    // advance cannot spin here.
    const std::size_t remaining = step_count() - next_index_;

    for (std::size_t taken = 0; taken < remaining; ++taken) {
        if (finished()) {
            break;
        }

        if (peek().event.observed_at > now) {
            break;
        }

        if (!advance().applied) {
            break;
        }

        ++applied;
    }

    return applied;
}

} // namespace prd::fake_source
