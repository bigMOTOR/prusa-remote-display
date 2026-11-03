// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "credential_scrub.hpp"

// §13.5's wipe, for the three provisioning buffers that were missing it — the 2026-08-23
// credential-boundary checkpoint, audit item 10.
//
// ⛔ WHAT THIS PROVES AND WHERE THE BOUNDARY IS. server.cpp is the component's ESP-IDF adapter TU
// and cannot be compiled by this suite, so what is proved here is the MECHANISM and the
// all-exits property of the guard. That the three buffers are actually guarded — the raw body, the
// cJSON values, the Submitted object — is a binding in server.cpp that must be READ. It is stated
// that way rather than claimed as covered.
//
// ⬅️ WHY THE ALL-EXITS TESTS MATTER MORE THAN THE ZEROING ONE. That a byte loop zeroes bytes is
// nearly self-evident. What actually went wrong in this component is that cleanup was absent on
// SOME paths, so the tests below deliberately exercise a normal return, an early return and a
// nested scope, which are the three shapes handle_submit() has.
//
// No real credential appears below. Every value is a generated pattern.

namespace prd::provisioning::scrub {
namespace {

constexpr unsigned char kFill = 0x5A;

[[nodiscard]] std::vector<unsigned char> filled(const std::size_t size) {
    return std::vector<unsigned char>(size, kFill);
}

[[nodiscard]] bool all_zero(const std::vector<unsigned char>& buffer) {
    for (const unsigned char byte : buffer) {
        if (byte != 0) {
            return false;
        }
    }
    return true;
}

// Stands in for `Submitted`: BoundedString-like fixed storage plus scalars, trivially copyable, so
// sizeof is the whole object.
struct CredentialObject {
    std::array<char, 16> secret{};
    std::uint16_t port{0};
};

[[nodiscard]] bool object_is_zero(const CredentialObject& object) {
    const auto* const bytes = reinterpret_cast<const unsigned char*>(&object);
    for (std::size_t index = 0; index < sizeof(object); ++index) {
        if (bytes[index] != 0) {
            return false;
        }
    }
    return true;
}

// ⬅️ THE OBJECT IS THE CALLER'S, SO ITS STATE AFTER THE GUARD FIRES IS OBSERVABLE. A guard over a
// function-local object could not be inspected once the function returned, and a test that could
// not inspect it would pass on a guard that did nothing.
//
// The success shape: fills, guards, returns normally.
void guarded_normal_return(CredentialObject& object, CredentialObject& observed) {
    const Scope guard{&object, sizeof(object)};

    std::memcpy(object.secret.data(), "pattern-abcdefg", object.secret.size());
    object.port = 8080;

    // What the sink would see.
    observed = object;
}

// The rejection shape: fills, guards, then leaves early — the parse-failure and stage-refusal paths.
void guarded_early_return(CredentialObject& object, CredentialObject& observed,
                          const bool leave_early) {
    const Scope guard{&object, sizeof(object)};

    std::memcpy(object.secret.data(), "pattern-abcdefg", object.secret.size());
    object.port = 8080;
    observed = object;

    if (leave_early) {
        return;
    }

    object.port = 1;
}

} // namespace

TEST(CredentialScrubTests, BytesZeroesTheWholeRegion) {
    auto buffer = filled(64);
    bytes(buffer.data(), buffer.size());
    EXPECT_TRUE(all_zero(buffer));
}

TEST(CredentialScrubTests, BytesLeavesNeighbouringMemoryAlone) {
    // A wipe that overran would be a memory-safety defect of its own, and the sanitizer build is
    // where that would surface — so the bound is asserted rather than assumed.
    auto buffer = filled(32);
    bytes(buffer.data(), 16);

    for (std::size_t index = 0; index < 16; ++index) {
        ASSERT_EQ(buffer[index], 0U) << "byte " << index;
    }
    for (std::size_t index = 16; index < 32; ++index) {
        ASSERT_EQ(buffer[index], kFill) << "byte " << index << " was written past the bound";
    }
}

TEST(CredentialScrubTests, BytesToleratesNullAndZeroBecauseCleanupMayNotFail) {
    bytes(nullptr, 0);
    bytes(nullptr, 64);

    auto buffer = filled(8);
    bytes(buffer.data(), 0);
    for (const unsigned char byte : buffer) {
        EXPECT_EQ(byte, kFill);
    }
}

TEST(CredentialScrubTests, TEXT_CLEARS_A_CJSON_STYLE_VALUE_IN_PLACE) {
    // ⛔ THE cJSON CASE. cJSON_Delete() frees `valuestring` without overwriting it, so this is what
    // must run first. The buffer is heap-allocated exactly as cJSON's would be.
    std::string owned = "pattern-passphrase-value";
    const std::size_t length = owned.size();

    text(owned.data());

    EXPECT_EQ(std::strlen(owned.data()), 0U);
    for (std::size_t index = 0; index < length; ++index) {
        ASSERT_EQ(owned[index], '\0') << "byte " << index << " survived the wipe";
    }
}

TEST(CredentialScrubTests, TextLeavesTheTerminatorAndNothingBeyondIt) {
    // The value is followed by a marker that must survive: cJSON's allocation holds one string, and
    // walking past its terminator would corrupt the allocator's neighbour.
    std::array<char, 16> storage{};
    std::memcpy(storage.data(), "abcd", 5);
    storage[5] = 'Z';

    text(storage.data());

    EXPECT_EQ(storage[0], '\0');
    EXPECT_EQ(storage[4], '\0');
    EXPECT_EQ(storage[5], 'Z') << "the wipe walked past the terminator";
}

TEST(CredentialScrubTests, TextToleratesNullAndAnAlreadyEmptyValue) {
    text(nullptr);

    std::array<char, 4> empty{};
    text(empty.data());
    EXPECT_EQ(empty[0], '\0');
}

TEST(CredentialScrubTests, THE_GUARD_WIPES_ON_A_NORMAL_RETURN) {
    CredentialObject object;
    CredentialObject observed;

    guarded_normal_return(object, observed);

    // The value genuinely was there — otherwise the assertion below would pass on a function that
    // never filled the object, which is the way this kind of test usually lies.
    EXPECT_EQ(observed.port, 8080);
    EXPECT_NE(observed.secret[0], '\0');

    // And it is gone now.
    EXPECT_TRUE(object_is_zero(object));
}

TEST(CredentialScrubTests, THE_GUARD_WIPES_ON_AN_EARLY_RETURN_TOO) {
    // ⛔ THE PATHS THAT WERE MISSING. handle_submit() leaves after the body is read by a short read,
    // a parse failure and a stage refusal, and hand-placed cleanup is what those paths did not have.
    CredentialObject early;
    CredentialObject observed_early;
    guarded_early_return(early, observed_early, true);

    EXPECT_NE(observed_early.secret[0], '\0');
    EXPECT_TRUE(object_is_zero(early)) << "an early return left the credential in place";

    CredentialObject full;
    CredentialObject observed_full;
    guarded_early_return(full, observed_full, false);

    EXPECT_NE(observed_full.secret[0], '\0');
    EXPECT_TRUE(object_is_zero(full));
}

TEST(CredentialScrubTests, THE_GUARD_WIPES_AT_THE_END_OF_A_NESTED_SCOPE) {
    // ⬅️ THE `Submitted` SHAPE. It is declared inside the accepted-and-sink-present block, so its
    // guard must fire at the end of that block rather than at the end of the function.
    auto buffer = filled(24);

    {
        const Scope guard{buffer.data(), buffer.size()};
        EXPECT_EQ(buffer[0], kFill) << "the guard wiped on construction rather than destruction";
    }

    EXPECT_TRUE(all_zero(buffer));
}

TEST(CredentialScrubTests, GuardsNestAndEachWipesItsOwnRegion) {
    // Two live guards, as handle_submit() has: the body for the whole function, `Submitted` for the
    // inner block. Neither may wipe the other's region early.
    auto outer = filled(16);
    auto inner = filled(16);

    {
        const Scope outer_guard{outer.data(), outer.size()};
        {
            const Scope inner_guard{inner.data(), inner.size()};
        }

        EXPECT_TRUE(all_zero(inner));
        EXPECT_EQ(outer[0], kFill) << "the inner guard wiped the outer region";
    }

    EXPECT_TRUE(all_zero(outer));
}

TEST(CredentialScrubTests, TheGuardHandlesAStringBufferWhichIsTheRawBodyCase) {
    // The raw JSON body is a std::string sized once at construction. Writing through data() and
    // wiping through the same pointer is exactly what server.cpp does.
    std::string body(48, 'x');

    {
        const Scope guard{body.data(), body.size()};
        EXPECT_EQ(body[0], 'x');
    }

    for (std::size_t index = 0; index < body.size(); ++index) {
        ASSERT_EQ(body[index], '\0') << "byte " << index << " survived the guard";
    }
}

} // namespace prd::provisioning::scrub
