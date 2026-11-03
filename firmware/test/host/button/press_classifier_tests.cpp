// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include "press_classifier.hpp"

#include <chrono>
#include <vector>

#include <gtest/gtest.h>

#include "support/fake_clock.hpp"

// Behaviour contract for debounce and gesture classification (Milestone 1
// Phase E, requirement 7). Every rule stated in press_classifier.hpp is pinned
// here, so a future change that breaks one fails a test rather than surprising
// somebody with a finger on a bench.

namespace {

using namespace std::chrono_literals;

using prd::button::Event;
using prd::button::PressClassifier;
using prd::button::Thresholds;
using prd::test::FakeClock;

constexpr Thresholds kThresholds{25ms, 1500ms, 8000ms};

// Drives the classifier the way the real task does: observations from edges, plus
// a poll every `poll` milliseconds. Collects every non-None event.
class Harness {
  public:
    Harness() : classifier_{kThresholds} {}

    // Holds `pressed` for `duration`, polling throughout, as the task would.
    void hold(const bool pressed, const std::chrono::milliseconds duration) {
        const auto deadline =
            clock_.now() + std::chrono::duration_cast<std::chrono::microseconds>(duration);
        while (clock_.now() < deadline) {
            clock_.advance(kPoll);
            observe(pressed);
        }
    }

    // A single edge observation at the current instant, with no time passing —
    // this is what the ISR path delivers.
    void edge(const bool pressed) {
        observe(pressed);
    }

    void advance(const std::chrono::milliseconds delta) {
        clock_.advance(delta);
    }

    void observe(const bool pressed) {
        const Event event = classifier_.observe(pressed, clock_.now());
        if (event != Event::None) {
            events_.push_back(event);
        }
    }

    void reset() {
        classifier_.reset();
    }

    [[nodiscard]] const std::vector<Event>& events() const {
        return events_;
    }
    [[nodiscard]] bool is_pressed() const {
        return classifier_.is_pressed();
    }
    [[nodiscard]] PressClassifier& classifier() {
        return classifier_;
    }

  private:
    static constexpr std::chrono::milliseconds kPoll{5};

    FakeClock clock_;
    PressClassifier classifier_;
    std::vector<Event> events_;
};

// --- clean gestures ---------------------------------------------------------

TEST(PressClassifier, CleanShortPress) {
    Harness h;
    h.hold(false, 100ms);
    h.hold(true, 200ms);
    h.hold(false, 100ms);

    EXPECT_EQ(h.events(), std::vector<Event>{Event::ShortPress});
}

TEST(PressClassifier, CleanLongPress) {
    Harness h;
    h.hold(false, 100ms);
    h.hold(true, 2000ms);
    h.hold(false, 100ms);

    EXPECT_EQ(h.events(), std::vector<Event>{Event::LongPress});
}

TEST(PressClassifier, VeryLongPressIsEmittedWhileStillHeld) {
    Harness h;
    h.hold(false, 100ms);
    h.hold(true, 8100ms);

    // Reported at the threshold, before the finger lifts.
    ASSERT_EQ(h.events(), std::vector<Event>{Event::VeryLongPress});

    h.hold(false, 100ms);

    // The release adds nothing: the gesture was already delivered.
    EXPECT_EQ(h.events(), std::vector<Event>{Event::VeryLongPress});
}

TEST(PressClassifier, VeryLongPressIsEmittedOnlyOnceHowLongEverHeld) {
    Harness h;
    h.hold(false, 100ms);
    h.hold(true, 30000ms);
    h.hold(false, 100ms);

    EXPECT_EQ(h.events(), std::vector<Event>{Event::VeryLongPress});
}

// --- threshold boundaries --------------------------------------------------

TEST(PressClassifier, JustBelowLongThresholdIsShort) {
    Harness h;
    h.hold(false, 100ms);
    h.edge(true);
    h.advance(1499ms);
    h.observe(true);
    h.edge(false);
    h.hold(false, 100ms);

    EXPECT_EQ(h.events(), std::vector<Event>{Event::ShortPress});
}

TEST(PressClassifier, ExactlyAtLongThresholdIsLong) {
    Harness h;
    h.hold(false, 100ms);
    h.edge(true);
    h.advance(1500ms);
    h.observe(true);
    h.edge(false);
    h.hold(false, 100ms);

    EXPECT_EQ(h.events(), std::vector<Event>{Event::LongPress});
}

TEST(PressClassifier, ExactlyAtVeryLongThresholdIsVeryLong) {
    Harness h;
    h.hold(false, 100ms);

    h.edge(true);
    h.hold(true, 30ms); // settle the press; it is dated at the edge above
    h.advance(7970ms);  // exactly 8000 ms after the edge
    h.observe(true);

    EXPECT_EQ(h.events(), std::vector<Event>{Event::VeryLongPress});
}

TEST(PressClassifier, StaleEdgeAlreadyPastTheThresholdReportsOnTheNextObservation) {
    // If the task were starved past the very-long threshold and only then
    // processed the queued press edge, the commit itself reports nothing — a
    // press beginning is not a gesture. The real task feeds the current pin level
    // immediately after draining edges in the same loop iteration, so the
    // threshold is reported without waiting for another poll. This pins that the
    // gesture is delayed, never lost.
    Harness h;
    h.hold(false, 100ms);

    h.edge(true);
    h.advance(9000ms);

    h.observe(true); // commits the press, dated 9000 ms ago
    EXPECT_TRUE(h.events().empty());

    h.observe(true); // the authoritative level read in the same iteration
    EXPECT_EQ(h.events(), std::vector<Event>{Event::VeryLongPress});
}

// --- bounce ----------------------------------------------------------------

TEST(PressClassifier, BounceOnPressYieldsOneGesture) {
    Harness h;
    h.hold(false, 100ms);

    // Contact chatter: five edges inside 8 ms, ending pressed.
    h.edge(true);
    h.advance(2ms);
    h.edge(false);
    h.advance(1ms);
    h.edge(true);
    h.advance(2ms);
    h.edge(false);
    h.advance(2ms);
    h.edge(true);

    h.hold(true, 200ms);
    h.hold(false, 100ms);

    EXPECT_EQ(h.events(), std::vector<Event>{Event::ShortPress});
}

TEST(PressClassifier, BounceOnReleaseYieldsOneGesture) {
    Harness h;
    h.hold(false, 100ms);
    h.hold(true, 200ms);

    h.edge(false);
    h.advance(2ms);
    h.edge(true);
    h.advance(1ms);
    h.edge(false);
    h.advance(3ms);
    h.edge(true);
    h.advance(2ms);
    h.edge(false);

    h.hold(false, 200ms);

    EXPECT_EQ(h.events(), std::vector<Event>{Event::ShortPress});
}

TEST(PressClassifier, DuplicateEdgesAtTheSameLevelChangeNothing) {
    Harness h;
    h.hold(false, 100ms);

    // A lost-then-refired interrupt, or simply repeated observations.
    h.edge(true);
    h.edge(true);
    h.edge(true);
    h.hold(true, 200ms);
    h.edge(false);
    h.edge(false);
    h.hold(false, 100ms);

    EXPECT_EQ(h.events(), std::vector<Event>{Event::ShortPress});
}

TEST(PressClassifier, DuplicateEdgesDoNotRestartTheDebounceInterval) {
    Harness h;
    h.hold(false, 100ms);

    h.edge(true);
    for (int i = 0; i < 5; ++i) {
        h.advance(4ms);
        h.observe(true); // 20 ms elapsed, still settling
    }
    EXPECT_FALSE(h.is_pressed());

    h.advance(5ms);
    h.observe(true); // 25 ms — commits
    EXPECT_TRUE(h.is_pressed());
}

// --- rejected transients ---------------------------------------------------

TEST(PressClassifier, ReleaseBeforeDebounceCompletesProducesNoEvent) {
    Harness h;
    h.hold(false, 100ms);

    h.edge(true);
    h.advance(10ms); // well short of 25 ms
    h.edge(false);
    h.hold(false, 200ms);

    EXPECT_TRUE(h.events().empty());
    EXPECT_FALSE(h.is_pressed());
}

TEST(PressClassifier, GlitchThatReturnsToIdleNeverBecomesAPress) {
    // The failure mode that ruled out first-edge debouncing. A naive
    // implementation would latch "pressed" here and invent a VeryLongPress
    // eight seconds later.
    Harness h;
    h.hold(false, 100ms);

    h.edge(true);
    h.advance(1ms);
    h.edge(false);

    h.hold(false, 10000ms);

    EXPECT_TRUE(h.events().empty());
    EXPECT_FALSE(h.is_pressed());
}

// --- duration accuracy -----------------------------------------------------

TEST(PressClassifier, DurationIsMeasuredFromTheFirstEdgeNotTheDebounceCommit) {
    // Press held 1490 ms physically. If the debounce delay were added to the
    // measured duration it would cross 1500 ms and misclassify as a long press.
    Harness h;
    h.hold(false, 100ms);

    h.edge(true);
    h.advance(30ms);
    h.observe(true); // commits, dated at the edge
    h.advance(1460ms);
    h.observe(true);
    h.edge(false);
    h.advance(30ms);
    h.observe(false);

    EXPECT_EQ(h.events(), std::vector<Event>{Event::ShortPress});
}

// --- reset -----------------------------------------------------------------

TEST(PressClassifier, ResetWhileHeldSuppressesTheGestureOnRelease) {
    Harness h;
    h.hold(false, 100ms);
    h.hold(true, 300ms);
    ASSERT_TRUE(h.is_pressed());

    h.reset();

    h.hold(true, 300ms);
    h.hold(false, 200ms);

    // No duration survived the reset, so no gesture is fabricated.
    EXPECT_TRUE(h.events().empty());
}

TEST(PressClassifier, ResetWhileHeldDoesNotStrandAVeryLongPress) {
    Harness h;
    h.hold(false, 100ms);
    h.hold(true, 300ms);

    h.reset();

    h.hold(true, 10000ms);

    EXPECT_TRUE(h.events().empty());
}

TEST(PressClassifier, WorksNormallyAfterAResetAndAFullReleaseCycle) {
    Harness h;
    h.hold(false, 100ms);
    h.hold(true, 300ms);
    h.reset();
    h.hold(false, 200ms);

    h.hold(true, 200ms);
    h.hold(false, 100ms);

    EXPECT_EQ(h.events(), std::vector<Event>{Event::ShortPress});
}

// --- sequences -------------------------------------------------------------

TEST(PressClassifier, RepeatedGesturesAreCountedExactly) {
    Harness h;
    h.hold(false, 100ms);

    for (int i = 0; i < 10; ++i) {
        h.hold(true, 200ms);
        h.hold(false, 200ms);
    }

    ASSERT_EQ(h.events().size(), 10U);
    for (const Event event : h.events()) {
        EXPECT_EQ(event, Event::ShortPress);
    }
}

TEST(PressClassifier, MixedGestureSequenceKeepsOrderAndIdentity) {
    Harness h;
    h.hold(false, 100ms);

    h.hold(true, 200ms); // short
    h.hold(false, 200ms);
    h.hold(true, 2000ms); // long
    h.hold(false, 200ms);
    h.hold(true, 8100ms); // very long, while held
    h.hold(false, 200ms);
    h.hold(true, 200ms); // short
    h.hold(false, 200ms);

    const std::vector<Event> expected{Event::ShortPress, Event::LongPress, Event::VeryLongPress,
                                      Event::ShortPress};
    EXPECT_EQ(h.events(), expected);
}

TEST(PressClassifier, UntouchedLineProducesNothing) {
    Harness h;
    h.hold(false, 60000ms);

    EXPECT_TRUE(h.events().empty());
    EXPECT_FALSE(h.is_pressed());
}

TEST(PressClassifier, PendingChangeIsVisibleWhileSettling) {
    // The real task uses this to decide whether to keep polling.
    Harness h;
    h.hold(false, 100ms);
    EXPECT_FALSE(h.classifier().has_pending_change());

    h.edge(true);
    EXPECT_TRUE(h.classifier().has_pending_change());

    h.hold(true, 100ms);
    EXPECT_FALSE(h.classifier().has_pending_change());
    EXPECT_TRUE(h.is_pressed());
}

} // namespace
