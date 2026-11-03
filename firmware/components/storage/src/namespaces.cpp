// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include <array>
#include <cstddef>
#include <string_view>

#include "storage/storage.hpp"

// The §5.10 namespace registry.
//
// PURE. No ESP-IDF header, so the host suite compiles this file directly and the
// properties below are checked by the same compiler that builds the firmware.

namespace prd::storage {
namespace {

// NVS stores a namespace name in a 16-byte field including its terminator, so 15
// characters is the limit.
//
// DECLARED HERE RATHER THAN INCLUDED, because this translation unit must stay free of
// ESP-IDF headers, and CROSS-CHECKED IN THE ADAPTER: storage.cpp static_asserts this
// value against ESP-IDF's own NVS_NS_NAME_MAX_SIZE. A constant copied from a header and
// never compared against it is the same trap as a diagnostic threshold taken from the
// wrong table — the check passes while measuring the wrong thing.
constexpr std::size_t kMaxNameLength = 15;

// Indexed by the enumerator, which is the property asserted below.
constexpr std::array<std::string_view, kNamespaceCount> kNames{
    "system", "config", "secrets", "runtime", "diagnostics",
};

// Every entry is non-empty and fits NVS's field.
constexpr bool names_are_storable() {
    // clang-tidy suggests std::all_of. Declined, and it is a language limit rather than a
    // preference: std::all_of does not become constexpr until C++20, and this function is
    // evaluated by a static_assert below. Rewriting it would move the check from compile
    // time to run time, which is the whole property being asserted.
    // NOLINTNEXTLINE(readability-use-anyofallof)
    for (const auto& entry : kNames) {
        if (entry.empty() || entry.size() > kMaxNameLength) {
            return false;
        }
    }

    return true;
}

// No two namespaces share a name. Two that did would silently become one partition of
// data, which is the failure this separation exists to prevent and the one a reviewer
// is least likely to see.
constexpr bool names_are_distinct() {
    for (std::size_t left = 0; left < kNames.size(); ++left) {
        for (std::size_t right = left + 1; right < kNames.size(); ++right) {
            if (kNames[left] == kNames[right]) {
                return false;
            }
        }
    }

    return true;
}

static_assert(kNames.size() == kNamespaceCount, "the registry must hold every namespace");
static_assert(names_are_storable(), "every namespace name must be non-empty and fit NVS's field");
static_assert(names_are_distinct(), "namespace names must be distinct");

// The catalogue is indexed by its own enumerator — entry *i* carries namespace *i*.
//
// One property, three defects ruled out at once: a missing entry, a duplicated
// enumerator and a reordered table. It is also what makes name() an array index rather
// than a switch that somebody has to remember to extend. The same argument as the text
// catalogue's, and the same mechanism.
static_assert(kNames[static_cast<std::size_t>(Namespace::System)] == "system");
static_assert(kNames[static_cast<std::size_t>(Namespace::Config)] == "config");
static_assert(kNames[static_cast<std::size_t>(Namespace::Secrets)] == "secrets");
static_assert(kNames[static_cast<std::size_t>(Namespace::Runtime)] == "runtime");
static_assert(kNames[static_cast<std::size_t>(Namespace::Diagnostics)] == "diagnostics");

} // namespace

std::string_view name(const Namespace space) {
    const auto index = static_cast<std::size_t>(space);

    // Unreachable through the enum, and checked anyway: the value crosses a virtual call
    // in bring_up() and an out-of-range index would read past the array rather than
    // producing a wrong string.
    if (index >= kNames.size()) {
        return {};
    }

    return kNames[index];
}

std::string_view log_label(const Namespace space) {
    return name(space);
}

std::string_view log_label(const Outcome outcome) {
    switch (outcome) {
    case Outcome::NotAttempted:
        return "not-attempted";

    case Outcome::Ready:
        return "ready";

    case Outcome::NoFreePages:
        return "no-free-pages";

    case Outcome::VersionAhead:
        return "version-ahead";

    case Outcome::InitialisationFailed:
        return "init-failed";

    case Outcome::NamespaceUnusable:
        return "namespace-unusable";
    }

    return "init-failed";
}

bool recovery_required(const Outcome outcome) {
    switch (outcome) {
    // The partition was left intact and a user-consented recovery could plausibly clear
    // it. Listed rather than defaulted, so a new outcome fails to compile until somebody
    // has decided which side of this line it falls on — the question is whether the
    // user's stored credentials are sitting behind it.
    case Outcome::NoFreePages:
    case Outcome::VersionAhead:
        return true;

    case Outcome::NotAttempted:
    case Outcome::Ready:
    case Outcome::InitialisationFailed:
    case Outcome::NamespaceUnusable:
        return false;
    }

    return false;
}

} // namespace prd::storage
