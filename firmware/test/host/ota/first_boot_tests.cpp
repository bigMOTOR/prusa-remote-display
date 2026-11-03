// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include "app_model/reducer.hpp"
#include "ota/first_boot.hpp"
#include "support/state_builders.hpp"
#include <gtest/gtest.h>
#include <limits>

namespace {
using namespace prd::ota;
class Port final : public BootPort {
  public:
    BootImage image{BootImage::Pending};
    bool success{true};
    int reads{0};
    int writes{0};
    BootImage running_image() override {
        ++reads;
        return image;
    }
    bool confirm_running_image() override {
        ++writes;
        return success;
    }
};
constexpr Readiness ready{true, true, true, false};

TEST(OtaFirstBoot, ValidationDoesNotTakeOverTheScreenButFailureRaisesDeviceFault) {
    using namespace prd::app_model;
    using prd::test::at_ms;
    auto state = prd::test::booted_device(at_ms(0));
    OtaObservation validating;
    validating.phase = OtaPhase::Validating;
    validating.pending_validation = true;
    state = reduce(state, Event::ota_observed(at_ms(1000), validating));
    EXPECT_EQ(state.device_operation, DeviceOperation::None);
    EXPECT_TRUE(state.ota.pending_validation);
    auto confirmed = reduce(state, Event::ota_observed(at_ms(31000), OtaObservation{}));
    EXPECT_FALSE(confirmed.ota.pending_validation);
    EXPECT_EQ(confirmed.device_fault, DeviceFaultReason::None);
    validating.phase = OtaPhase::Failed;
    state = reduce(state, Event::ota_observed(at_ms(31000), validating));
    EXPECT_EQ(state.device_fault, DeviceFaultReason::OtaValidationFailure);
    EXPECT_TRUE(state.ota.pending_validation);
}

TEST(OtaFirstBoot, DoesNothingBeforeBegin) {
    FirstBoot check;
    Port port;
    check.tick(port, 30000, ready);
    EXPECT_EQ(port.reads, 0);
    EXPECT_EQ(port.writes, 0);
}

TEST(OtaFirstBoot, OrdinaryAndUnknownImagesNeverWrite) {
    for (auto image : {BootImage::Ordinary, BootImage::Unknown}) {
        FirstBoot check;
        Port port;
        port.image = image;
        check.begin(port);
        check.tick(port, 0, ready);
        check.tick(port, 60000, ready);
        EXPECT_EQ(port.writes, 0);
        EXPECT_FALSE(check.pending());
    }
}

TEST(OtaFirstBoot, ConfirmsOnceAtThirtyHealthySecondsNotBootTime) {
    FirstBoot check;
    Port port;
    check.begin(port);
    check.tick(port, 60000, ready);
    check.tick(port, 89999, ready);
    EXPECT_EQ(port.writes, 0);
    check.tick(port, 90000, ready);
    EXPECT_EQ(port.writes, 1);
    EXPECT_EQ(check.health(), BootHealth::Confirmed);
    EXPECT_FALSE(check.pending());
    check.begin(port);
    check.tick(port, 150000, ready);
    EXPECT_EQ(port.reads, 1);
    EXPECT_EQ(port.writes, 1);
}

TEST(OtaFirstBoot, EveryMissingReadinessConditionRestartsTheWindow) {
    for (const Readiness missing :
         {Readiness{false, true, true, false}, Readiness{true, false, true, false},
          Readiness{true, true, false, false}}) {
        FirstBoot check;
        Port port;
        check.begin(port);
        check.tick(port, 0, ready);
        check.tick(port, 29999, missing);
        check.tick(port, 30000, ready);
        check.tick(port, 59999, ready);
        EXPECT_EQ(port.writes, 0);
        check.tick(port, 60000, ready);
        EXPECT_EQ(port.writes, 1);
    }
}

TEST(OtaFirstBoot, FatalFaultAtDeadlineNeverConfirmsEvenIfLaterCleared) {
    FirstBoot check;
    Port port;
    check.begin(port);
    check.tick(port, 0, ready);
    check.tick(port, 30000, {true, true, true, true});
    check.tick(port, 60000, ready);
    EXPECT_EQ(port.writes, 0);
    EXPECT_EQ(check.health(), BootHealth::Failed);
    EXPECT_TRUE(check.pending());
}

TEST(OtaFirstBoot, FailedConfirmationIsNeverRetriedOrReportedAsValid) {
    FirstBoot check;
    Port port;
    port.success = false;
    check.begin(port);
    check.tick(port, 0, ready);
    check.tick(port, 30000, ready);
    check.tick(port, 60000, ready);
    EXPECT_EQ(port.writes, 1);
    EXPECT_TRUE(check.pending());
    EXPECT_EQ(check.health(), BootHealth::Failed);
}

TEST(OtaFirstBoot, RebootStartsANewFullWindow) {
    Port port;
    FirstBoot old_boot;
    old_boot.begin(port);
    old_boot.tick(port, 0, ready);
    old_boot.tick(port, 29999, ready);
    FirstBoot new_boot;
    new_boot.begin(port);
    new_boot.tick(port, 0, ready);
    new_boot.tick(port, 29999, ready);
    EXPECT_EQ(port.writes, 0);
    // The fake deliberately still reports Pending; actual rollback selection
    // belongs to ESP-IDF on reboot, not to this timer or this host test.
}

TEST(OtaFirstBoot, UptimeWrapIsValidButBackwardSampleCannotConfirm) {
    Port port;
    FirstBoot check;
    check.begin(port);
    const auto start = std::numeric_limits<std::uint32_t>::max() - 10000U;
    check.tick(port, start, ready);
    check.tick(port, start + 29999U, ready);
    EXPECT_EQ(port.writes, 0);
    check.tick(port, start + 30000U, ready);
    EXPECT_EQ(port.writes, 1);
    Port other;
    FirstBoot backwards;
    backwards.begin(other);
    backwards.tick(other, 50000, ready);
    backwards.tick(other, 49999, ready);
    EXPECT_EQ(other.writes, 0);
    backwards.tick(other, 79999, ready);
    EXPECT_EQ(other.writes, 1);
}
} // namespace
