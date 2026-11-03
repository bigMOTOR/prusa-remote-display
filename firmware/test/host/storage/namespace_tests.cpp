// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include <cstddef>
#include <set>
#include <string_view>

#include <gtest/gtest.h>

#include "storage/storage.hpp"

// The §5.10 namespace registry.
//
// Most of what matters here is already a static_assert in namespaces.cpp and cannot
// reach a test at all — a violation fails the build. What is left for run time is the
// part a compile-time assertion cannot express: that name() is total over the enum, that
// it never returns a name for a value outside it, and that the names are stable.
//
// THE NAMES ARE PERSISTED IDENTIFIERS. Changing one orphans everything stored under the
// old name on every device already in the field, so they are pinned literally here. A
// future rename is then a test edit, which is a review conversation, rather than a silent
// data loss.

namespace {

using prd::storage::kNamespaceCount;
using prd::storage::Namespace;

TEST(StorageNamespaceTests, TheNamesAreTheOnesOnTheFlash) {
    EXPECT_EQ(prd::storage::name(Namespace::System), "system");
    EXPECT_EQ(prd::storage::name(Namespace::Config), "config");
    EXPECT_EQ(prd::storage::name(Namespace::Secrets), "secrets");
    EXPECT_EQ(prd::storage::name(Namespace::Runtime), "runtime");
    EXPECT_EQ(prd::storage::name(Namespace::Diagnostics), "diagnostics");
}

TEST(StorageNamespaceTests, EveryNamespaceIsNamedAndFitsTheNvsField) {
    for (std::size_t index = 0; index < kNamespaceCount; ++index) {
        const auto space = static_cast<Namespace>(index);
        const std::string_view stored = prd::storage::name(space);

        EXPECT_FALSE(stored.empty()) << "at index " << index;

        // NVS stores a namespace name in 16 bytes including its terminator.
        EXPECT_LE(stored.size(), 15U) << stored;
    }
}

TEST(StorageNamespaceTests, TheNamesAreDistinct) {
    std::set<std::string_view> seen;

    for (std::size_t index = 0; index < kNamespaceCount; ++index) {
        seen.insert(prd::storage::name(static_cast<Namespace>(index)));
    }

    EXPECT_EQ(seen.size(), kNamespaceCount);
}

// The value crosses a virtual call in bring_up(), so the index is checked rather than
// trusted. An out-of-range value reads past the array instead of producing a wrong
// string, which is the difference between a wrong log line and undefined behaviour.
TEST(StorageNamespaceTests, AValueOutsideTheEnumYieldsNoName) {
    const auto beyond = static_cast<Namespace>(kNamespaceCount);

    EXPECT_TRUE(prd::storage::name(beyond).empty());
    EXPECT_TRUE(prd::storage::log_label(beyond).empty());
}

TEST(StorageNamespaceTests, TheLogLabelAgreesWithTheStoredName) {
    for (std::size_t index = 0; index < kNamespaceCount; ++index) {
        const auto space = static_cast<Namespace>(index);

        EXPECT_EQ(prd::storage::log_label(space), prd::storage::name(space));
    }
}

} // namespace
