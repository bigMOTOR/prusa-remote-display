// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include <cstddef>
#include <cstring>
#include <string>
#include <type_traits>

#include <gtest/gtest.h>

#include "setup_session_mailbox.hpp"
#include "ui/ui.hpp"

namespace prd::ui::setup_session_mailbox {
namespace {

// The production queue memcpy's the entire fixed-size item. A successful
// receive marks the item absent but does not clear these backing bytes.
struct FakeMailbox {
    using Payload = SetupSession;
    static_assert(std::is_trivially_copyable_v<Payload>);

    Payload slot{};
    bool pending{false};
    bool busy{false};
    bool locked{false};
    bool skip_zero_overwrite{false}; // Causal negative control.
    int resets{0};

    bool try_lock() {
        if (busy) {
            return false;
        }
        locked = true;
        return true;
    }
    void lock() {
        locked = true;
    }
    void unlock() {
        locked = false;
    }
    bool try_receive(Payload& output) {
        if (!pending) {
            return false;
        }
        std::memcpy(&output, &slot, sizeof(output));
        pending = false;
        return true;
    }
    void overwrite(const Payload& input) {
        if (skip_zero_overwrite && input.password.empty() && input.ssid.empty()) {
            pending = true;
            return;
        }
        std::memcpy(&slot, &input, sizeof(slot));
        pending = true;
    }
    void reset() {
        ++resets;
        pending = false;
    }
};

bool all_zero(const SetupSession& session) {
    const auto* const bytes = reinterpret_cast<const unsigned char*>(&session);
    for (std::size_t index = 0; index < sizeof(session); ++index) {
        if (bytes[index] != 0) {
            return false;
        }
    }
    return true;
}

SetupSession make_session(const char* const password) {
    SetupSession session{};
    session.ssid.assign("PrusaDisplay-test");
    session.password.assign(password);
    return session;
}

TEST(SetupSessionMailbox, ReceiveRetiresQueueBytesBeforeHandlerRuns) {
    FakeMailbox mailbox;
    const SetupSession session = make_session("SECRET-ALPHA");
    publish(mailbox, session);
    ASSERT_TRUE(mailbox.pending);
    ASSERT_FALSE(all_zero(mailbox.slot));

    bool called = false;
    EXPECT_TRUE(drain(mailbox, [&](const SetupSession& received) {
        called = true;
        EXPECT_EQ(received.password.view(), "SECRET-ALPHA");
        EXPECT_TRUE(all_zero(mailbox.slot));
        EXPECT_FALSE(mailbox.pending);
        EXPECT_FALSE(mailbox.locked);
    }));
    EXPECT_TRUE(called);
    EXPECT_EQ(mailbox.resets, 1);
}

TEST(SetupSessionMailbox, ReplacementDeliversOnlyNewestThenErasesSlot) {
    FakeMailbox mailbox;
    const SetupSession old_session = make_session("SECRET-ALPHA");
    const SetupSession replacement = make_session("SECRET-BETA");
    publish(mailbox, old_session);
    publish(mailbox, replacement);

    std::string seen;
    EXPECT_TRUE(
        drain(mailbox, [&](const SetupSession& received) { seen = received.password.view(); }));
    EXPECT_EQ(seen, "SECRET-BETA");
    EXPECT_TRUE(all_zero(mailbox.slot));
    EXPECT_FALSE(mailbox.pending);
    EXPECT_FALSE(drain(mailbox, [&](const SetupSession&) { FAIL() << "duplicate delivery"; }));
}

TEST(SetupSessionMailbox, ClearOverwritesAPendingPasswordBeforeAnyDrain) {
    FakeMailbox mailbox;
    const SetupSession session = make_session("SECRET-ALPHA");
    const SetupSession clear{};
    publish(mailbox, session);
    publish(mailbox, clear);

    EXPECT_TRUE(drain(mailbox, [&](const SetupSession& received) {
        EXPECT_TRUE(received.password.empty());
        EXPECT_TRUE(received.ssid.empty());
    }));
    EXPECT_TRUE(all_zero(mailbox.slot));
}

TEST(SetupSessionMailbox, PublicationDuringHandlerSurvivesPriorScrub) {
    FakeMailbox mailbox;
    const SetupSession first = make_session("SECRET-ALPHA");
    const SetupSession later = make_session("SECRET-BETA");
    publish(mailbox, first);

    EXPECT_TRUE(drain(mailbox, [&](const SetupSession& received) {
        EXPECT_EQ(received.password.view(), "SECRET-ALPHA");
        publish(mailbox, later);
    }));
    EXPECT_TRUE(mailbox.pending);
    EXPECT_EQ(mailbox.slot.password.view(), "SECRET-BETA");
    EXPECT_TRUE(drain(mailbox, [&](const SetupSession& received) {
        EXPECT_EQ(received.password.view(), "SECRET-BETA");
    }));
    EXPECT_TRUE(all_zero(mailbox.slot));
}

TEST(SetupSessionMailbox, ContentionDefersWithoutLosingTheSession) {
    FakeMailbox mailbox;
    const SetupSession session = make_session("SECRET-ALPHA");
    publish(mailbox, session);
    mailbox.busy = true;
    EXPECT_FALSE(drain(mailbox, [&](const SetupSession&) { FAIL() << "delivered while busy"; }));
    EXPECT_TRUE(mailbox.pending);

    mailbox.busy = false;
    EXPECT_TRUE(drain(mailbox, [&](const SetupSession& received) {
        EXPECT_EQ(received.password.view(), "SECRET-ALPHA");
    }));
    EXPECT_TRUE(all_zero(mailbox.slot));
}

TEST(SetupSessionMailbox, WipeCoversTheEntireLocalObject) {
    SetupSession session = make_session("SECRET-ALPHA");
    wipe_bytes(&session, sizeof(session));
    EXPECT_TRUE(all_zero(session));
}

TEST(SetupSessionMailbox, SkippingTheScrubLeavesTheOldSecretInQueueStorage) {
    FakeMailbox mailbox;
    mailbox.skip_zero_overwrite = true;
    const SetupSession session = make_session("SECRET-ALPHA");
    publish(mailbox, session);
    EXPECT_TRUE(drain(mailbox, [&](const SetupSession&) {}));
    EXPECT_FALSE(all_zero(mailbox.slot));
}

} // namespace
} // namespace prd::ui::setup_session_mailbox
