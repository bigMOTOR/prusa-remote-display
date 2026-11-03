// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include "bring_up.hpp"

#include <cstddef>
#include <vector>

#include <gtest/gtest.h>

#include "app_model/device_data.hpp"
#include "storage/storage.hpp"

// The behaviour contract for storage bring-up (Milestone 3.1).
//
// THE RULE THIS FILE EXISTS FOR: storage is never erased automatically. Owner ruling,
// 2026-08-14 — after NO_FREE_PAGES, a version mismatch, corruption, migration failure or
// any other mount failure that could hold user configuration or credentials, the bytes
// stay where they are and the product reports that recovery is required. The erase
// belongs to Milestone 3.10, behind deliberate physical presence.
//
// The strongest guarantee is structural and cannot be tested here at all: `Backend` has
// no erase operation, so this component has nothing to call. What IS tested is that every
// failure path reports the right outcome, preserves the distinction between causes that a
// consented recovery could clear and causes it could not, and never claims a recovery
// that did not happen.

namespace {

using prd::app_model::StorageStatus;
using prd::storage::BackendResult;
using prd::storage::bring_up;
using prd::storage::kNamespaceCount;
using prd::storage::Namespace;
using prd::storage::Outcome;
using prd::storage::recovery_required;

// Records every call and replays scripted answers, so a test can assert what the sequence
// DID as well as what it concluded.
class FakeBackend final : public prd::storage::Backend {
  public:
    BackendResult init_result{BackendResult::Ok};
    BackendResult probe_result{BackendResult::Ok};

    // When set, only this namespace fails its probe and every other succeeds.
    bool probe_fails_for_one = false;
    Namespace failing_namespace{Namespace::System};

    std::size_t init_calls = 0;
    std::vector<Namespace> probed;

    [[nodiscard]] BackendResult initialise() override {
        ++init_calls;
        return init_result;
    }

    [[nodiscard]] BackendResult probe(const Namespace space) override {
        probed.push_back(space);

        if (probe_fails_for_one) {
            return space == failing_namespace ? BackendResult::Failed : BackendResult::Ok;
        }

        return probe_result;
    }
};

TEST(StorageBringUpTests, AHealthyPartitionComesUpReady) {
    FakeBackend backend;

    const auto report = bring_up(backend);

    EXPECT_EQ(report.status, StorageStatus::Ready);
    EXPECT_EQ(report.outcome, Outcome::Ready);
    EXPECT_EQ(backend.init_calls, 1U);
}

TEST(StorageBringUpTests, EveryNamespaceIsProbedInDeclarationOrder) {
    FakeBackend backend;

    const auto report = bring_up(backend);

    ASSERT_EQ(report.status, StorageStatus::Ready);
    ASSERT_EQ(backend.probed.size(), kNamespaceCount);

    for (std::size_t index = 0; index < kNamespaceCount; ++index) {
        EXPECT_EQ(backend.probed[index], static_cast<Namespace>(index)) << "at index " << index;
    }
}

// A partition with no free pages holds CONTENT — ESP-IDF's own description is "may happen
// if NVS partition was truncated", and a blank partition initialises normally. Erasing it
// would silently return a configured device to factory-blank.
TEST(StorageBringUpTests, AFullPartitionIsPreservedAndReportsRecoveryRequired) {
    FakeBackend backend;
    backend.init_result = BackendResult::NoFreePages;

    const auto report = bring_up(backend);

    EXPECT_EQ(report.status, StorageStatus::Failed);
    EXPECT_EQ(report.outcome, Outcome::NoFreePages);
    EXPECT_TRUE(recovery_required(report.outcome));
    EXPECT_EQ(backend.init_calls, 1U) << "no reformat-and-retry";
    EXPECT_TRUE(backend.probed.empty());
}

// §13.4: "migration failure must not erase secrets automatically". A partition written by
// a newer NVS format is a firmware DOWNGRADE, with the user's credentials intact behind
// it.
TEST(StorageBringUpTests, APartitionFromANewerFirmwareIsPreservedAndReportsRecoveryRequired) {
    FakeBackend backend;
    backend.init_result = BackendResult::VersionAhead;

    const auto report = bring_up(backend);

    EXPECT_EQ(report.status, StorageStatus::Failed);
    EXPECT_EQ(report.outcome, Outcome::VersionAhead);
    EXPECT_TRUE(recovery_required(report.outcome));
    EXPECT_EQ(backend.init_calls, 1U);
    EXPECT_TRUE(backend.probed.empty());
}

// The two preserved causes must stay distinguishable. Collapsing them would leave
// Milestone 3.10 unable to offer the right remedy — a truncated partition and a
// downgraded one need different conversations with the user.
TEST(StorageBringUpTests, TheTwoPreservedCausesAreNotConflated) {
    FakeBackend full;
    full.init_result = BackendResult::NoFreePages;

    FakeBackend newer;
    newer.init_result = BackendResult::VersionAhead;

    EXPECT_NE(bring_up(full).outcome, bring_up(newer).outcome);
}

TEST(StorageBringUpTests, AnOutrightInitialisationFailureIsReportedAsItsOwnOutcome) {
    FakeBackend backend;
    backend.init_result = BackendResult::Failed;

    const auto report = bring_up(backend);

    EXPECT_EQ(report.status, StorageStatus::Failed);
    EXPECT_EQ(report.outcome, Outcome::InitialisationFailed);
    EXPECT_TRUE(backend.probed.empty());
}

TEST(StorageBringUpTests, AnUnusableNamespaceFailsTheBringUpAndNamesItself) {
    FakeBackend backend;
    backend.probe_fails_for_one = true;
    backend.failing_namespace = Namespace::Secrets;

    const auto report = bring_up(backend);

    EXPECT_EQ(report.status, StorageStatus::Failed);
    EXPECT_EQ(report.outcome, Outcome::NamespaceUnusable);
    EXPECT_EQ(report.failed_namespace, Namespace::Secrets);
}

TEST(StorageBringUpTests, ProbingStopsAtTheFirstUnusableNamespace) {
    FakeBackend backend;
    backend.probe_fails_for_one = true;
    backend.failing_namespace = Namespace::Config;

    const auto report = bring_up(backend);

    ASSERT_EQ(report.status, StorageStatus::Failed);
    ASSERT_EQ(backend.probed.size(), 2U);
    EXPECT_EQ(backend.probed[0], Namespace::System);
    EXPECT_EQ(backend.probed[1], Namespace::Config);
}

// RecoveredWithDefaults means "defaults were applied", which means something was
// discarded. Nothing in this component discards anything, so no path may report it — if
// one ever does, it is claiming a recovery that did not happen.
TEST(StorageBringUpTests, NoPathEverReportsRecoveredWithDefaults) {
    const BackendResult init_results[] = {
        BackendResult::Ok,
        BackendResult::NoFreePages,
        BackendResult::VersionAhead,
        BackendResult::Failed,
    };

    for (const auto init_result : init_results) {
        for (const bool probes_fail : {false, true}) {
            FakeBackend backend;
            backend.init_result = init_result;
            backend.probe_result = probes_fail ? BackendResult::Failed : BackendResult::Ok;

            EXPECT_NE(bring_up(backend).status, StorageStatus::RecoveredWithDefaults);
        }
    }
}

// Only the two preserved causes are recoverable. A new Outcome added without deciding
// which side of this line it falls on fails to compile in namespaces.cpp; this pins the
// answers that were decided.
TEST(StorageBringUpTests, OnlyThePreservedCausesRequireRecovery) {
    EXPECT_TRUE(recovery_required(Outcome::NoFreePages));
    EXPECT_TRUE(recovery_required(Outcome::VersionAhead));

    EXPECT_FALSE(recovery_required(Outcome::NotAttempted));
    EXPECT_FALSE(recovery_required(Outcome::Ready));
    EXPECT_FALSE(recovery_required(Outcome::InitialisationFailed));
    EXPECT_FALSE(recovery_required(Outcome::NamespaceUnusable));
}

TEST(StorageBringUpTests, EveryOutcomeHasADistinctLabel) {
    const Outcome outcomes[] = {
        Outcome::NotAttempted,         Outcome::Ready,
        Outcome::NoFreePages,          Outcome::VersionAhead,
        Outcome::InitialisationFailed, Outcome::NamespaceUnusable,
    };

    for (const auto left : outcomes) {
        EXPECT_FALSE(prd::storage::log_label(left).empty());

        for (const auto right : outcomes) {
            if (left == right) {
                continue;
            }

            EXPECT_NE(prd::storage::log_label(left), prd::storage::log_label(right));
        }
    }
}

TEST(StorageBringUpTests, EveryBackendResultHasADistinctLabel) {
    const BackendResult results[] = {
        BackendResult::Ok,
        BackendResult::NoFreePages,
        BackendResult::VersionAhead,
        BackendResult::Failed,
    };

    for (const auto left : results) {
        EXPECT_FALSE(prd::storage::log_label(left).empty());

        for (const auto right : results) {
            if (left == right) {
                continue;
            }

            EXPECT_NE(prd::storage::log_label(left), prd::storage::log_label(right));
        }
    }
}

} // namespace
