// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include <array>
#include <cstddef>
#include <memory>
#include <set>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "submission_handoff.hpp"

// OWNERSHIP OF THE PROVISIONING SUBMISSION AS IT CROSSES TASKS — checkpoint 3d.
//
// ⛔ THE TEST THIS FILE EXISTS FOR IS THE REPLACEMENT TRANSACTION. On hardware, a second Wi-Fi
// submission arriving while the first is still pending is a two-tap sequence in a browser that takes
// deliberate effort to produce, the failure it can cause is a leaked heap object holding a
// passphrase, and nothing on the panel would ever show it. Here it is four lines.
//
// ⛔ AND THE FAKE MAILBOX NEVER ACTUALLY FREES. Every parcel stays alive in an arena, so a
// use-after-free the unit might commit is observable as a bookkeeping fault instead of undefined
// behaviour — which is the only way a test can assert "released exactly once" rather than crash on
// the second one.
//
// ⚠️ WHAT THIS FILE DOES NOT PROVE. `app_core.cpp` and `provisioning/src/server.cpp` are not
// host-compiled, so the FreeRTOS/heap binding and the HTTP handler are inspected, not tested. What
// is proven here is every ordering and disposal rule in submission_handoff.hpp, which is the same
// code the firmware runs.

namespace prd::app_core::submission_handoff {
namespace {

constexpr std::size_t kSecretBytes = 8;

// A stand-in for provisioning_flow::Submission. The canary is what makes a missed wipe visible: a
// released parcel whose bytes are still 0xA5 was freed with a credential in it.
constexpr unsigned char kCanary = 0xA5;

struct Parcel {
    std::array<unsigned char, kSecretBytes> secret{};
    int tag{0};
    bool filled{false};
};

// A depth-one mailbox with fault injection and full ownership bookkeeping.
class Mailbox {
  public:
    using Payload = Parcel;

    // --- Fault injection ---------------------------------------------------
    bool refuse_allocation{false};
    int refuse_next_puts{0};

    // ⚠️ NEGATIVE CONTROL ONLY. Set by the two tests whose job is to prove the wipe assertion can
    // fail; never by a test asserting correct behaviour.
    bool skip_wipe{false};

    // --- Observation -------------------------------------------------------
    int allocations{0};
    int releases{0};
    int double_releases{0};
    int invalid_releases{0};

    // Every mailbox operation, counted — so a test can assert that NOTHING ran after a failed
    // allocation, rather than only that the outcome was right.
    int takes{0};
    int puts{0};

    // --- The Ops contract --------------------------------------------------

    [[nodiscard]] Payload* allocate() {
        if (refuse_allocation) {
            return nullptr;
        }

        ++allocations;

        auto owned = std::make_unique<Parcel>();
        owned->tag = allocations;
        owned->secret.fill(kCanary);

        Parcel* const raw = owned.get();
        arena_.push_back(std::move(owned));
        known_.insert(raw);
        live_.insert(raw);
        return raw;
    }

    void wipe_and_release(Payload* const owned) {
        if (owned == nullptr || known_.count(owned) == 0) {
            // The unit handed back something it was never given.
            ++invalid_releases;
            return;
        }

        if (live_.count(owned) == 0) {
            // Released twice. Counted rather than crashing, so the test can assert on it.
            ++double_releases;
            return;
        }

        if (!skip_wipe) {
            owned->secret.fill(0);
        }

        live_.erase(owned);
        released_.insert(owned);
        ++releases;
    }

    [[nodiscard]] bool try_take(Payload*& out) {
        ++takes;

        if (slot_ == nullptr) {
            return false;
        }

        out = slot_;
        slot_ = nullptr;
        return true;
    }

    [[nodiscard]] bool try_put(Payload* const owned) {
        ++puts;

        if (refuse_next_puts > 0) {
            --refuse_next_puts;
            return false;
        }

        // Depth one, enforced rather than assumed.
        if (slot_ != nullptr) {
            return false;
        }

        slot_ = owned;
        return true;
    }

    // --- Invariants --------------------------------------------------------

    [[nodiscard]] bool pending() const {
        return slot_ != nullptr;
    }

    [[nodiscard]] const Parcel* peek() const {
        return slot_;
    }

    // Every allocated parcel is either released or the one the mailbox holds. Anything else is a
    // pointer the unit lost: live, unreachable, and still holding a credential.
    [[nodiscard]] bool nothing_leaked() const {
        const std::size_t expected = slot_ != nullptr ? 1U : 0U;
        if (live_.size() != expected) {
            return false;
        }

        return slot_ == nullptr || live_.count(slot_) == 1;
    }

    // Every released parcel was overwritten before it was let go.
    [[nodiscard]] bool released_were_wiped() const {
        for (const Parcel* const parcel : released_) {
            for (const unsigned char byte : parcel->secret) {
                if (byte != 0) {
                    return false;
                }
            }
        }

        return true;
    }

    // The whole invariant, asserted after every operation in every test below.
    [[nodiscard]] bool sound() const {
        return nothing_leaked() && released_were_wiped() && double_releases == 0 &&
               invalid_releases == 0;
    }

  private:
    std::vector<std::unique_ptr<Parcel>> arena_;
    std::set<const Parcel*> known_;
    std::set<const Parcel*> live_;
    std::set<const Parcel*> released_;
    Parcel* slot_{nullptr};
};

// The ordinary fill: succeeds and marks the parcel.
auto fills() {
    return [](Parcel& parcel) {
        parcel.filled = true;
        return true;
    };
}

// A fill that refuses, standing in for a bounded field that would not hold its value.
auto refuses() {
    return [](Parcel& parcel) {
        parcel.filled = true;
        return false;
    };
}

} // namespace

// --- 1. The ordinary handoff ----------------------------------------------

TEST(SubmissionHandoffTests, FirstPublishBecomesThePendingSubmission) {
    Mailbox mailbox;

    const Report report = publish(mailbox, fills());

    EXPECT_EQ(report.outcome, Outcome::Accepted);
    EXPECT_EQ(report.failure, Failure::None);
    EXPECT_FALSE(report.displaced);

    EXPECT_TRUE(mailbox.pending());
    EXPECT_EQ(mailbox.allocations, 1);
    EXPECT_EQ(mailbox.releases, 0);
    EXPECT_TRUE(mailbox.sound());
}

TEST(SubmissionHandoffTests, ConsumeAppliesOnceThenReleasesOnce) {
    Mailbox mailbox;
    ASSERT_EQ(publish(mailbox, fills()).outcome, Outcome::Accepted);

    const Parcel* const queued = mailbox.peek();
    ASSERT_NE(queued, nullptr);

    int applied = 0;
    const Parcel* seen = nullptr;

    EXPECT_TRUE(consume(mailbox, [&applied, &seen](Parcel& incoming) {
        ++applied;
        seen = &incoming;
    }));

    EXPECT_EQ(applied, 1);

    // ⛔ THE SAME OBJECT, NOT A COPY. Pointer identity is what proves the payload crossed by
    // reference: nothing was passed by value, so no second set of credential bytes ever existed.
    EXPECT_EQ(seen, queued);

    EXPECT_FALSE(mailbox.pending());
    EXPECT_EQ(mailbox.releases, 1);
    EXPECT_TRUE(mailbox.sound());
}

TEST(SubmissionHandoffTests, ConsumeOnAnEmptyMailboxDoesNothing) {
    Mailbox mailbox;

    int applied = 0;
    EXPECT_FALSE(consume(mailbox, [&applied](Parcel&) { ++applied; }));

    EXPECT_EQ(applied, 0);
    EXPECT_EQ(mailbox.allocations, 0);
    EXPECT_EQ(mailbox.releases, 0);
    EXPECT_TRUE(mailbox.sound());
}

// --- 2. Replacement -------------------------------------------------------

TEST(SubmissionHandoffTests, ReplacementWhileThePredecessorIsStillPending) {
    Mailbox mailbox;
    ASSERT_EQ(publish(mailbox, fills()).outcome, Outcome::Accepted);
    const Parcel* const first = mailbox.peek();

    const Report report = publish(mailbox, fills());

    EXPECT_EQ(report.outcome, Outcome::Accepted);
    EXPECT_TRUE(report.displaced);

    // The new one is pending; the old one is gone, wiped, and released exactly once.
    ASSERT_TRUE(mailbox.pending());
    EXPECT_NE(mailbox.peek(), first);
    EXPECT_EQ(mailbox.allocations, 2);
    EXPECT_EQ(mailbox.releases, 1);
    EXPECT_TRUE(mailbox.sound());
}

TEST(SubmissionHandoffTests, ReplacementAfterTheApplicationTaskAlreadyTookThePredecessor) {
    Mailbox mailbox;
    ASSERT_EQ(publish(mailbox, fills()).outcome, Outcome::Accepted);

    // The documented race: app_task consumes before the server task replaces.
    ASSERT_TRUE(consume(mailbox, [](Parcel&) {}));
    ASSERT_FALSE(mailbox.pending());

    const Report report = publish(mailbox, fills());

    // ⛔ NOT DOUBLE OWNERSHIP. The old transaction already belonged to the application task, so
    // there was nothing to displace and the new object simply becomes the pending one.
    EXPECT_EQ(report.outcome, Outcome::Accepted);
    EXPECT_FALSE(report.displaced);

    EXPECT_TRUE(mailbox.pending());
    EXPECT_EQ(mailbox.allocations, 2);
    EXPECT_EQ(mailbox.releases, 1);
    EXPECT_TRUE(mailbox.sound());
}

// --- 3. Failures before the mailbox is touched ----------------------------

TEST(SubmissionHandoffTests, AllocationFailurePreservesThePendingSubmission) {
    Mailbox mailbox;
    ASSERT_EQ(publish(mailbox, fills()).outcome, Outcome::Accepted);
    const Parcel* const first = mailbox.peek();

    mailbox.refuse_allocation = true;
    const Report report = publish(mailbox, fills());

    EXPECT_EQ(report.outcome, Outcome::Unavailable);
    EXPECT_EQ(report.failure, Failure::AllocationFailed);
    EXPECT_FALSE(report.displaced);

    // ⛔ THE PREDECESSOR IS UNTOUCHED AND STILL THE SAME OBJECT. A submission that could not even be
    // built must not be able to disturb one that already exists.
    EXPECT_EQ(mailbox.peek(), first);
    EXPECT_EQ(mailbox.allocations, 1);
    EXPECT_EQ(mailbox.releases, 0);
    EXPECT_TRUE(mailbox.sound());
}

TEST(SubmissionHandoffTests, ConversionFailurePreservesThePendingSubmission) {
    Mailbox mailbox;
    ASSERT_EQ(publish(mailbox, fills()).outcome, Outcome::Accepted);
    const Parcel* const first = mailbox.peek();

    const Report report = publish(mailbox, refuses());

    EXPECT_EQ(report.outcome, Outcome::Unavailable);
    EXPECT_EQ(report.failure, Failure::ConversionFailed);

    EXPECT_EQ(mailbox.peek(), first);
    EXPECT_EQ(mailbox.allocations, 2);

    // The half-built object was released; the pending one was not.
    EXPECT_EQ(mailbox.releases, 1);
    EXPECT_TRUE(mailbox.sound());
}

// --- 4. Failures after the mailbox is touched ----------------------------

TEST(SubmissionHandoffTests, QueueUnavailableWithNothingToDisplace) {
    Mailbox mailbox;
    mailbox.refuse_next_puts = 1;

    const Report report = publish(mailbox, fills());

    EXPECT_EQ(report.outcome, Outcome::Unavailable);
    EXPECT_EQ(report.failure, Failure::QueueUnavailable);

    EXPECT_FALSE(mailbox.pending());
    EXPECT_EQ(mailbox.allocations, 1);
    EXPECT_EQ(mailbox.releases, 1);
    EXPECT_TRUE(mailbox.sound());
}

TEST(SubmissionHandoffTests, ANewSendThatFailsRestoresThePredecessor) {
    Mailbox mailbox;
    ASSERT_EQ(publish(mailbox, fills()).outcome, Outcome::Accepted);
    const Parcel* const first = mailbox.peek();

    // Refuse only the first put of the next transaction, so the restore succeeds.
    mailbox.refuse_next_puts = 1;
    const Report report = publish(mailbox, fills());

    EXPECT_EQ(report.outcome, Outcome::Unavailable);
    EXPECT_EQ(report.failure, Failure::NewRejectedOldRestored);
    EXPECT_FALSE(report.displaced);

    // ⛔ THE OLD SUBMISSION IS BACK AND IS THE SAME OBJECT. The new one is what was lost, which is
    // the correct direction to fail in: a retry costs the user one tap, and the transaction that was
    // already under way was not destroyed to serve it.
    ASSERT_TRUE(mailbox.pending());
    EXPECT_EQ(mailbox.peek(), first);
    EXPECT_EQ(mailbox.allocations, 2);
    EXPECT_EQ(mailbox.releases, 1);
    EXPECT_TRUE(mailbox.sound());
}

TEST(SubmissionHandoffTests, WhenRestoringAlsoFailsBothAreReleasedExactlyOnce) {
    Mailbox mailbox;
    ASSERT_EQ(publish(mailbox, fills()).outcome, Outcome::Accepted);

    // Refuse the new put AND the restore. Unreachable through a conforming depth-one mailbox — see
    // Failure::NewAndOldReleased — so it is reached by injection rather than argued away.
    mailbox.refuse_next_puts = 2;
    const Report report = publish(mailbox, fills());

    EXPECT_EQ(report.outcome, Outcome::Unavailable);
    EXPECT_EQ(report.failure, Failure::NewAndOldReleased);

    EXPECT_FALSE(mailbox.pending());
    EXPECT_EQ(mailbox.allocations, 2);

    // Both objects released, each exactly once, both wiped, nothing dangling.
    EXPECT_EQ(mailbox.releases, 2);
    EXPECT_EQ(mailbox.double_releases, 0);
    EXPECT_TRUE(mailbox.sound());
}

// --- 5. Drain and refusal paths ------------------------------------------

TEST(SubmissionHandoffTests, DrainReleasesThePendingSubmissionExactlyOnce) {
    Mailbox mailbox;
    ASSERT_EQ(publish(mailbox, fills()).outcome, Outcome::Accepted);

    EXPECT_EQ(drain(mailbox), 1U);

    EXPECT_FALSE(mailbox.pending());
    EXPECT_EQ(mailbox.releases, 1);
    EXPECT_TRUE(mailbox.sound());

    // Idempotent: a second drain finds nothing and releases nothing.
    EXPECT_EQ(drain(mailbox), 0U);
    EXPECT_EQ(mailbox.releases, 1);
    EXPECT_TRUE(mailbox.sound());
}

TEST(SubmissionHandoffTests, DrainOnAnEmptyMailboxReleasesNothing) {
    Mailbox mailbox;

    EXPECT_EQ(drain(mailbox), 0U);
    EXPECT_EQ(mailbox.allocations, 0);
    EXPECT_EQ(mailbox.releases, 0);
    EXPECT_TRUE(mailbox.sound());
}

TEST(SubmissionHandoffTests, AnEarlyReturnInsideTheCallbackStillReleases) {
    Mailbox mailbox;
    ASSERT_EQ(publish(mailbox, fills()).outcome, Outcome::Accepted);

    // The stage refusal: app_core takes the submission, decides it cannot act on it, and returns
    // without copying anything. The release is on the unit's side, so there is no path for the
    // caller to forget it.
    bool refused = false;
    EXPECT_TRUE(consume(mailbox, [&refused](Parcel&) {
        refused = true;
        return; // NOLINT(readability-redundant-control-flow) — the early return IS the test
    }));

    EXPECT_TRUE(refused);
    EXPECT_FALSE(mailbox.pending());
    EXPECT_EQ(mailbox.releases, 1);
    EXPECT_TRUE(mailbox.sound());
}

// --- 6. A longer sequence -------------------------------------------------

TEST(SubmissionHandoffTests, RepeatedReplacementLeaksNothingAcrossManyRounds) {
    Mailbox mailbox;

    for (int round = 0; round < 32; ++round) {
        // Every fourth round the application task takes the submission first; every seventh the
        // mailbox refuses the new put once.
        if (round % 4 == 3) {
            (void)consume(mailbox, [](Parcel&) {});
        }

        if (round % 7 == 6) {
            mailbox.refuse_next_puts = 1;
        }

        (void)publish(mailbox, fills());
        ASSERT_TRUE(mailbox.sound()) << "round " << round;
    }

    // Read before draining: drain() empties the mailbox, so asking afterwards always answers "no".
    const unsigned expected = mailbox.pending() ? 1U : 0U;
    EXPECT_EQ(drain(mailbox), expected);
    EXPECT_TRUE(mailbox.sound());

    // Every object ever allocated has been accounted for.
    EXPECT_EQ(mailbox.releases, mailbox.allocations);
    EXPECT_EQ(mailbox.double_releases, 0);
    EXPECT_EQ(mailbox.invalid_releases, 0);
}

// --- 6b. The heap-exhaustion path does nothing else ----------------------

// ⛔ THE UNIT-LEVEL HALF OF THE EXHAUSTION CONTRACT. `note_submission()` returns the moment it sees
// Failure::AllocationFailed precisely because every line after it allocates — and that early return
// is only sound if publish() itself did nothing after the null either. This pins that: not one
// mailbox operation is invoked, so there is nothing between `new (std::nothrow)` answering null and
// the caller's return that could ask the allocator for anything.
//
// ⚠️ IT DOES NOT PROVE THE PRODUCTION PATH. `app_core.cpp` and `server.cpp` are not host-compiled;
// the rest of the chain — the early return, the counter, the 503 responder — was established by
// reading them.
TEST(SubmissionHandoffTests, AFailedAllocationInvokesNoOtherOperation) {
    Mailbox mailbox;
    mailbox.refuse_allocation = true;

    bool filled = false;
    const Report report = publish(mailbox, [&filled](Parcel&) {
        filled = true;
        return true;
    });

    EXPECT_EQ(report.outcome, Outcome::Unavailable);
    EXPECT_EQ(report.failure, Failure::AllocationFailed);

    // Nothing was built, nothing was taken, nothing was offered, nothing was released.
    EXPECT_FALSE(filled) << "fill must not run without an object to fill";
    EXPECT_EQ(mailbox.allocations, 0);
    EXPECT_EQ(mailbox.takes, 0) << "the mailbox must not be touched after a failed allocation";
    EXPECT_EQ(mailbox.puts, 0) << "the mailbox must not be touched after a failed allocation";
    EXPECT_EQ(mailbox.releases, 0);
    EXPECT_TRUE(mailbox.sound());
}

TEST(SubmissionHandoffTests, AFailedAllocationLeavesAPendingSubmissionCompletelyAlone) {
    Mailbox mailbox;
    ASSERT_EQ(publish(mailbox, fills()).outcome, Outcome::Accepted);
    const Parcel* const pending = mailbox.peek();
    const int takes_before = mailbox.takes;
    const int puts_before = mailbox.puts;

    mailbox.refuse_allocation = true;
    ASSERT_EQ(publish(mailbox, fills()).failure, Failure::AllocationFailed);

    // Not merely "still pending" — not even inspected.
    EXPECT_EQ(mailbox.peek(), pending);
    EXPECT_EQ(mailbox.takes, takes_before);
    EXPECT_EQ(mailbox.puts, puts_before);
    EXPECT_EQ(mailbox.releases, 0);
    EXPECT_TRUE(mailbox.sound());
}

// --- 7. NEGATIVE CONTROLS -------------------------------------------------
//
// ⛔ WITHOUT THESE, EVERY sound() ABOVE COULD BE VACUOUS. Each one proves a specific assertion is
// capable of failing, so a passing suite means the checks ran rather than that they were satisfiable
// by construction.

TEST(SubmissionHandoffNegativeControls, TheWipeCheckDetectsAMissedWipe) {
    Mailbox mailbox;
    mailbox.skip_wipe = true;

    ASSERT_EQ(publish(mailbox, fills()).outcome, Outcome::Accepted);
    ASSERT_TRUE(consume(mailbox, [](Parcel&) {}));

    ASSERT_EQ(mailbox.releases, 1);

    // The object was released with its canary intact. sound() must refuse it.
    EXPECT_FALSE(mailbox.released_were_wiped());
    EXPECT_FALSE(mailbox.sound());
}

TEST(SubmissionHandoffNegativeControls, TheDoubleReleaseCheckDetectsASecondRelease) {
    Mailbox mailbox;

    Parcel* const parcel = mailbox.allocate();
    ASSERT_NE(parcel, nullptr);

    mailbox.wipe_and_release(parcel);
    ASSERT_EQ(mailbox.double_releases, 0);
    ASSERT_TRUE(mailbox.sound());

    mailbox.wipe_and_release(parcel);

    EXPECT_EQ(mailbox.double_releases, 1);
    EXPECT_EQ(mailbox.releases, 1);
    EXPECT_FALSE(mailbox.sound());
}

TEST(SubmissionHandoffNegativeControls, TheLeakCheckDetectsALostPointer) {
    Mailbox mailbox;

    // Allocated and neither queued nor released — exactly what a dropped pointer looks like.
    ASSERT_NE(mailbox.allocate(), nullptr);

    EXPECT_FALSE(mailbox.nothing_leaked());
    EXPECT_FALSE(mailbox.sound());
}

TEST(SubmissionHandoffNegativeControls, TheOperationCountersDetectMailboxActivity) {
    // The mirror of AFailedAllocationInvokesNoOtherOperation: an ordinary publish DOES touch the
    // mailbox, so a zero count there is a real observation and not an artefact of never counting.
    Mailbox mailbox;
    ASSERT_EQ(publish(mailbox, fills()).outcome, Outcome::Accepted);

    EXPECT_GT(mailbox.takes, 0);
    EXPECT_GT(mailbox.puts, 0);
}

TEST(SubmissionHandoffNegativeControls, TheInvalidReleaseCheckDetectsAForeignPointer) {
    Mailbox mailbox;
    Parcel stranger;

    mailbox.wipe_and_release(&stranger);

    EXPECT_EQ(mailbox.invalid_releases, 1);
    EXPECT_FALSE(mailbox.sound());
}

TEST(SubmissionHandoffNegativeControls, ASoundMailboxIsNotVacuouslySound) {
    // The mirror of the four above: with no faults injected, the same predicate passes. So sound()
    // discriminates rather than always answering one way.
    Mailbox mailbox;

    ASSERT_EQ(publish(mailbox, fills()).outcome, Outcome::Accepted);
    ASSERT_TRUE(consume(mailbox, [](Parcel&) {}));

    EXPECT_TRUE(mailbox.released_were_wiped());
    EXPECT_TRUE(mailbox.nothing_leaked());
    EXPECT_TRUE(mailbox.sound());
}

// --- 8. The failure labels are non-secret and total ----------------------

TEST(SubmissionHandoffTests, EveryFailureHasANonSecretLabel) {
    for (const Failure failure :
         {Failure::None, Failure::AllocationFailed, Failure::ConversionFailed,
          Failure::QueueUnavailable, Failure::NewRejectedOldRestored, Failure::NewAndOldReleased}) {
        const std::string_view label = log_label(failure);

        EXPECT_FALSE(label.empty());
        EXPECT_NE(label, "unknown") << "a Failure value has no label of its own";
    }
}

} // namespace prd::app_core::submission_handoff
