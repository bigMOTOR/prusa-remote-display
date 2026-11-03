// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include <gtest/gtest.h>

#include <string>

#include "app_model/bounded_string.hpp"

// BoundedString is the reason AppState has a fixed size, so its truncation
// behaviour is load-bearing rather than incidental: a printer that reports a very
// long filename must not be able to decide how much memory the state occupies, and
// must not be able to write past the end of it either. The overflow cases here are
// exercised under ASan in the sanitized suite, where a one-byte overrun would abort
// rather than pass quietly.

using prd::app_model::BoundedString;

TEST(BoundedStringTests, DefaultIsEmptyAndUntruncated) {
    const BoundedString<8> text;

    EXPECT_TRUE(text.empty());
    EXPECT_EQ(text.size(), 0U);
    EXPECT_FALSE(text.truncated());
    EXPECT_EQ(text.view(), "");
    EXPECT_STREQ(text.c_str(), "");
    EXPECT_EQ(BoundedString<8>::capacity(), 8U);
}

TEST(BoundedStringTests, StoresAValueThatFits) {
    BoundedString<16> text;

    EXPECT_TRUE(text.assign("CORE One"));

    EXPECT_EQ(text.view(), "CORE One");
    EXPECT_EQ(text.size(), 8U);
    EXPECT_FALSE(text.truncated());
    EXPECT_STREQ(text.c_str(), "CORE One");
}

TEST(BoundedStringTests, StoresAValueOfExactlyCapacity) {
    BoundedString<8> text;

    EXPECT_TRUE(text.assign("12345678"));

    EXPECT_EQ(text.view(), "12345678");
    EXPECT_EQ(text.size(), 8U);
    EXPECT_FALSE(text.truncated());
}

TEST(BoundedStringTests, TruncatesAndReportsIt) {
    BoundedString<8> text;

    EXPECT_FALSE(text.assign("123456789"));

    EXPECT_EQ(text.view(), "12345678");
    EXPECT_EQ(text.size(), 8U);
    EXPECT_TRUE(text.truncated());

    // §9.3: oversized input is classified, not silently accepted. The flag is the
    // classification, and it is what distinguishes this from a value that happened
    // to be exactly capacity long.
    EXPECT_STREQ(text.c_str(), "12345678");
}

TEST(BoundedStringTests, SurvivesGrosslyOversizedInput) {
    // The case that motivates the type: a remote party supplying far more than the
    // field can hold.
    const std::string huge(4096, 'x');

    BoundedString<24> text;
    EXPECT_FALSE(text.assign(huge));

    EXPECT_EQ(text.size(), 24U);
    EXPECT_TRUE(text.truncated());
    EXPECT_EQ(text.view(), std::string(24, 'x'));
}

TEST(BoundedStringTests, AssignReplacesRatherThanAppends) {
    BoundedString<16> text{"first value"};
    EXPECT_TRUE(text.assign("second"));

    EXPECT_EQ(text.view(), "second");
    EXPECT_EQ(text.size(), 6U);
}

TEST(BoundedStringTests, AssignClearsAPreviousTruncation) {
    BoundedString<4> text;

    EXPECT_FALSE(text.assign("too long"));
    EXPECT_TRUE(text.truncated());

    EXPECT_TRUE(text.assign("ok"));
    EXPECT_FALSE(text.truncated());
}

TEST(BoundedStringTests, ClearResetsEverything) {
    BoundedString<4> text;
    EXPECT_FALSE(text.assign("too long"));

    text.clear();

    EXPECT_TRUE(text.empty());
    EXPECT_FALSE(text.truncated());
    EXPECT_EQ(text.view(), "");
}

TEST(BoundedStringTests, AcceptsAnEmptyViewWithoutReadingItsPointer) {
    // An empty string_view may carry a null data pointer, which memcpy must not be
    // handed even with a zero length.
    BoundedString<8> text{"value"};

    EXPECT_TRUE(text.assign(std::string_view{}));
    EXPECT_TRUE(text.empty());
    EXPECT_FALSE(text.truncated());
}

TEST(BoundedStringTests, HandlesEmbeddedNulBytes) {
    // view() is length-based, so a value containing a NUL round-trips; c_str()
    // necessarily stops at it. Worth pinning: a payload from a printer is bytes,
    // not a C string.
    const std::string_view value{"ab\0cd", 5};

    BoundedString<8> text;
    EXPECT_TRUE(text.assign(value));

    EXPECT_EQ(text.size(), 5U);
    EXPECT_EQ(text.view(), value);
}

TEST(BoundedStringTests, EqualityComparesValueAndTruncation) {
    const BoundedString<8> fitted{"12345678"};

    BoundedString<8> truncated;
    EXPECT_FALSE(truncated.assign("123456789"));

    // Same eight characters, different provenance. They are not the same
    // observation, and treating them as equal would let a truncated value silently
    // satisfy a change comparison.
    EXPECT_EQ(fitted.view(), truncated.view());
    EXPECT_NE(fitted, truncated);

    EXPECT_EQ(fitted, BoundedString<8>{"12345678"});
    EXPECT_NE(fitted, BoundedString<8>{"1234567"});
}
