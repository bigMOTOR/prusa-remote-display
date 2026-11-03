// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only
#include "scenes/operational_motion.hpp"
#include <gtest/gtest.h>
#include <limits>

namespace prd::ui::scene {
namespace {
OperationalPose pose(Energy energy) {
    OperationalContent content{};
    content.form = energy;
    return operational_pose(content, energy != kEnergyReady, false);
}
TEST(OperationalMotionTests, EveryDirectedPairHasExactEndpointsAndIntermediateFrames) {
    for (const Energy a : {0, 200, 400, 600, 800, 1000}) {
        for (const Energy b : {0, 200, 400, 600, 800, 1000}) {
            if (a == b)
                continue;
            OperationalMotion motion;
            motion.target(pose(a));
            motion.target(pose(b));
            EXPECT_TRUE(motion.step(true, 100));
            EXPECT_EQ(motion.current().values, pose(a).values);
            EXPECT_TRUE(motion.step(true, 650));
            EXPECT_DOUBLE_EQ(motion.current().values[OperationalPose::EnergyField], (a + b) / 2.0);
            EXPECT_FALSE(motion.step(true, 1200));
            EXPECT_EQ(motion.current().values, pose(b).values);
            EXPECT_FALSE(motion.step(true, 100000));
        }
    }
}
TEST(OperationalMotionTests, CompletionDoesNotDimTheBodyWhileTheFormOpens) {
    OperationalMotion motion;
    motion.target(pose(kEnergyPrinting));
    motion.target(pose(kEnergyCompleted));
    for (unsigned t = 0; t <= 1120; t += 16) {
        (void)motion.step(true, t);
        EXPECT_EQ(motion.current().values[OperationalPose::BodyEnergy], kEnergyPrinting);
    }
    EXPECT_EQ(motion.current().values[OperationalPose::EnergyField], kEnergyCompleted);
    motion.target(pose(kEnergyReady));
    (void)motion.step(true, 1200);
    (void)motion.step(true, 1750);
    const auto interrupted = motion.current();
    motion.target(pose(kEnergyCompleted));
    (void)motion.step(true, 1750);
    EXPECT_EQ(motion.current().values, interrupted.values);
    (void)motion.step(true, 2850);
    EXPECT_EQ(motion.current().values[OperationalPose::BodyEnergy], kEnergyPrinting);
}
TEST(OperationalMotionTests, RetargetStartsAtPaintedPoseAndSameTargetNeverRestarts) {
    OperationalMotion motion;
    motion.target(pose(1000));
    motion.target(pose(0));
    EXPECT_TRUE(motion.step(true, 100));
    EXPECT_TRUE(motion.step(true, 500));
    const auto painted = motion.current();
    motion.target(pose(800));
    EXPECT_TRUE(motion.step(true, 500));
    EXPECT_EQ(motion.current().values, painted.values);
    motion.target(pose(800));
    EXPECT_FALSE(motion.step(true, 1600));
    EXPECT_EQ(motion.current().values, pose(800).values);
}
TEST(OperationalMotionTests, DarkPanelHoldsFirstFrameAndUnsignedClockWrapIsSafe) {
    OperationalMotion motion;
    motion.target(pose(0));
    motion.target(pose(1000));
    EXPECT_FALSE(motion.step(false, 9000)); // No high-rate work while no pixels are visible.
    EXPECT_TRUE(motion.running());
    EXPECT_EQ(motion.current().values, pose(0).values);
    const auto start = std::numeric_limits<std::uint32_t>::max() - 500U;
    EXPECT_TRUE(motion.step(true, start));
    EXPECT_FALSE(motion.step(true, start + 1100U));
    EXPECT_EQ(motion.current().values, pose(1000).values);
}
TEST(OperationalMotionTests, CompletedAndPauseUseReviewedPoseAndOneProgress) {
    const auto completed = pose(kEnergyCompleted);
    EXPECT_EQ(completed.values[OperationalPose::TitleY], 54);
    EXPECT_EQ(completed.values[OperationalPose::TitleSize], 28);
    EXPECT_EQ(completed.values[OperationalPose::ValueSize], 44);
    EXPECT_EQ(pose(kEnergyPaused).values[OperationalPose::PauseAlpha], 255);
    EXPECT_EQ(pose(kEnergyPrinting).values[OperationalPose::PauseAlpha], 0);
    EXPECT_EQ(pose(kEnergyPaused).values[OperationalPose::CycleTreatmentAlpha], 0);
    EXPECT_EQ(pose(kEnergyPrinting).values[OperationalPose::CycleTreatmentAlpha], 255);
}

TEST(OperationalMotionTests, CycleTreatmentUsesTheSameClockAndPreservesBothEndpoints) {
    OperationalMotion motion;
    motion.target(pose(kEnergyPaused));
    motion.target(pose(kEnergyPrinting));
    EXPECT_TRUE(motion.step(true, 100));
    EXPECT_EQ(motion.current().values[OperationalPose::CycleTreatmentAlpha], 0);
    EXPECT_TRUE(motion.step(true, 650));
    EXPECT_DOUBLE_EQ(motion.current().values[OperationalPose::CycleTreatmentAlpha], 127.5);
    EXPECT_FALSE(motion.step(true, 1200));
    EXPECT_EQ(motion.current().values[OperationalPose::CycleTreatmentAlpha], 255);
}
TEST(OperationalMotionTests, ClosedPrintingRecoveryKeepsCycleTreatmentThroughout) {
    OperationalMotion motion;
    motion.resume_from_form({kEnergyPrinting, 0});
    motion.target(pose(kEnergyPrinting));
    for (unsigned elapsed = 0; elapsed <= 1120; elapsed += 16) {
        motion.step(true, elapsed);
        EXPECT_EQ(motion.current().values[OperationalPose::CycleTreatmentAlpha], 255);
    }
    for (const Energy energy : {0, 200, 400, 600, 800}) {
        motion.resume_from_form({energy, 0});
        EXPECT_EQ(motion.current().values[OperationalPose::CycleTreatmentAlpha], 0);
    }
}
TEST(OperationalMotionTests, RecoverySeedBeforeOrdinarySceneHasValidTypographyAndOffset) {
    OperationalMotion motion;
    motion.resume_from_form({kEnergyReady, 8});
    EXPECT_EQ(motion.current().values[OperationalPose::ValueSize], theme::kFontSizeHero);
    EXPECT_EQ(motion.current().values[OperationalPose::OffsetY], 8);
    motion.target(pose(kEnergyPrinting));
    EXPECT_TRUE(motion.step(true, 0));
    EXPECT_EQ(motion.current().values[OperationalPose::OffsetY], 8);
    EXPECT_FALSE(motion.step(true, 1100));
    EXPECT_EQ(motion.current().values, pose(kEnergyPrinting).values);
}
TEST(OperationalMotionTests, RecoveryTypographyDoesNotDependOnTheHiddenPreviousState) {
    for (const Energy hidden : {0, 400, 600, 800, 1000}) {
        OperationalMotion motion, clean;
        motion.target(pose(hidden));
        motion.resume_from_form({333, -7, 123});
        clean.resume_from_form({333, -7, 123});
        EXPECT_EQ(motion.current().values, clean.current().values);
        EXPECT_EQ(motion.current().values[OperationalPose::TitleY], theme::kMotionTitleY);
        EXPECT_EQ(motion.current().values[OperationalPose::TitleSize], theme::kMotionTitleSize);
        EXPECT_EQ(motion.current().values[OperationalPose::AppearanceAlpha], 123);
    }
}
TEST(OperationalMotionTests, BlankRecoveryRevealsOnlyTheCurrentDestinationAndWaitsForVisibility) {
    OperationalMotion motion;
    motion.target(pose(kEnergyPrinting));
    const auto destination = pose(kEnergyCompleted);
    motion.resume_from_blank(destination);
    EXPECT_EQ(motion.current().values[OperationalPose::EnergyField], kEnergyCompleted);
    EXPECT_EQ(motion.current().values[OperationalPose::TitleSize], theme::kFontSizeOutcome);
    EXPECT_EQ(motion.current().values[OperationalPose::AppearanceAlpha], 0);
    EXPECT_FALSE(motion.step(false, 9000));
    EXPECT_TRUE(motion.step(true, 10000));
    EXPECT_TRUE(motion.step(true, 10550));
    EXPECT_DOUBLE_EQ(motion.current().values[OperationalPose::AppearanceAlpha], 127.5);
    EXPECT_FALSE(motion.step(true, 11100));
    EXPECT_EQ(motion.current().values, destination.values);
}
TEST(OperationalMotionTests, InterruptedLossUsesPaintedStructureAndRetainsCanonicalEndpoints) {
    using O = app_model::LossOriginForm;
    for (const O origin :
         {O::Ready, O::Stopped, O::Preparing, O::Completed, O::Paused, O::Printing}) {
        for (const unsigned ms : {0U, 400U, 800U, 1600U, 2600U}) {
            const auto expected = phase_at(origin, ms);
            const auto actual = phase_from_position(origin, {origin_energy(origin), 0}, ms);
            EXPECT_EQ(actual.energy, expected.energy);
            EXPECT_EQ(actual.offset_y, expected.offset_y);
            EXPECT_EQ(actual.copy_alpha, expected.copy_alpha);
        }
        const auto first = phase_from_position(origin, {333, -7, 123}, 0);
        EXPECT_EQ(first.energy, 333);
        EXPECT_EQ(first.offset_y, -7);
        EXPECT_EQ(first.form_opacity, 123);
        EXPECT_EQ(first.copy_alpha, 0);
        const auto last = phase_from_position(origin, {333, -7, 123}, 2600);
        EXPECT_TRUE(last.settled);
        EXPECT_EQ(last.energy, kEnergyReady);
        EXPECT_EQ(last.offset_y, kUnavailableSettleY);
        EXPECT_EQ(last.copy_alpha, 255);
        EXPECT_EQ(last.form_opacity, 255);
    }
}
TEST(OperationalMotionTests, NewRecordWaitsForEnclosureAndUsesTheSameMotionClock) {
    OperationalContent content{};
    content.form = kEnergyPrinting;
    content.show_progress = true;
    const auto destination = operational_pose(content, true, false);
    OperationalMotion motion;
    motion.resume_from_form({kEnergyReady, 8});
    motion.target(destination);
    motion.step(true, 0);
    EXPECT_EQ(motion.reveal_record(record_enclosure(motion.current(), destination, 76, 76)), 0);
    motion.step(true, 400);
    EXPECT_EQ(motion.reveal_record(record_enclosure(motion.current(), destination, 76, 76)), 0);
    motion.step(true, 800);
    const auto visible =
        motion.reveal_record(record_enclosure(motion.current(), destination, 76, 76));
    EXPECT_GT(visible, 0);
    EXPECT_LT(visible, 255);
    EXPECT_DOUBLE_EQ(motion.reveal_record(record_enclosure(motion.current(), destination, 76, 76)),
                     visible); // Repainting the same tick cannot compound the gate.
    motion.step(true, 1100);
    EXPECT_EQ(motion.reveal_record(record_enclosure(motion.current(), destination, 76, 76)), 255);
}
TEST(OperationalMotionTests, InterruptedRevealStartsAtThePaintedOpacity) {
    OperationalContent content{};
    content.form = kEnergyPrinting;
    content.show_progress = true;
    OperationalMotion motion;
    motion.resume_from_form({kEnergyReady, 8});
    motion.target(operational_pose(content, true, false));
    motion.step(true, 0);
    motion.step(true, 800);
    const auto alpha = motion.reveal_record(0.25);
    content.form = kEnergyPaused;
    motion.target(operational_pose(content, true, false));
    motion.step(true, 800);
    EXPECT_DOUBLE_EQ(motion.reveal_record(1.0), alpha);
}
TEST(OperationalMotionTests, IncomingRecordWaitsForTheDestinationSideOfTheTitle) {
    using P = OperationalPose;
    auto destination = pose(kEnergyCompleted);
    destination.values[P::ValueY] = 100;
    destination.values[P::TitleY] = 54;
    auto current = destination;
    current.values[P::ValueY] = 60;
    current.values[P::TitleY] = 120;
    EXPECT_EQ(record_enclosure(current, destination, 160, 160), 0);
    current.values[P::ValueY] = 130;
    current.values[P::TitleY] = 54;
    EXPECT_EQ(record_enclosure(current, destination, 160, 160), 1);
}
TEST(OperationalMotionTests, ExistingRecordAndAllSettledLayoutsAreNotSuppressed) {
    for (const auto energy : {kEnergyPrinting, kEnergyPaused, kEnergyCompleted, kEnergyStopped}) {
        OperationalContent content{};
        content.form = energy;
        content.show_progress = true;
        const auto destination = operational_pose(content, true, false);
        OperationalMotion motion;
        motion.target(destination);
        EXPECT_EQ(record_enclosure(destination, destination, 76, 76), 1);
        EXPECT_EQ(motion.reveal_record(0), 255);
    }
}
} // namespace
} // namespace prd::ui::scene
