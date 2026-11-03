// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include <gtest/gtest.h>

#include "diagnostics/redaction.hpp"

using prd::diagnostics::redact;

TEST(RedactionTests, ReplacesSingleOccurrence) {
    EXPECT_EQ(redact("X-Api-Key: hunter2", "hunter2"), "X-Api-Key: <redacted>");
}

TEST(RedactionTests, ReplacesEveryOccurrence) {
    EXPECT_EQ(redact("key=abc retry=abc last=abc", "abc"),
              "key=<redacted> retry=<redacted> last=<redacted>");
}

TEST(RedactionTests, RedactsAdjacentOccurrences) {
    EXPECT_EQ(redact("abcabc", "abc"), "<redacted><redacted>");
}

TEST(RedactionTests, LeavesMessageWithoutSecretUntouched) {
    EXPECT_EQ(redact("status=ok latency=42ms", "hunter2"), "status=ok latency=42ms");
}

TEST(RedactionTests, EmptySecretRedactsNothing) {
    EXPECT_EQ(redact("status=ok", ""), "status=ok");
}

TEST(RedactionTests, RedactsSecretEqualToWholeMessage) {
    EXPECT_EQ(redact("hunter2", "hunter2"), "<redacted>");
}
