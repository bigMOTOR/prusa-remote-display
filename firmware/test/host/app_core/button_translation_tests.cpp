// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include <gtest/gtest.h>

#include "button_translation.hpp"

// The gesture boundary (Architecture Spec §5.6). Three lines of code, and worth
// pinning anyway: it is the seam between a driver that knows nothing about the
// product and a reducer that decides everything, and a wrong mapping here would make
// a long press factory-reset the device.

using prd::app_core::to_gesture;
using prd::app_model::ButtonGesture;

TEST(ButtonTranslationTests, MapsEveryPhysicalGesture) {
    ASSERT_TRUE(to_gesture(prd::button::Event::ShortPress).has_value());
    EXPECT_EQ(*to_gesture(prd::button::Event::ShortPress), ButtonGesture::ShortPress);

    ASSERT_TRUE(to_gesture(prd::button::Event::LongPress).has_value());
    EXPECT_EQ(*to_gesture(prd::button::Event::LongPress), ButtonGesture::LongPress);

    ASSERT_TRUE(to_gesture(prd::button::Event::VeryLongPress).has_value());
    EXPECT_EQ(*to_gesture(prd::button::Event::VeryLongPress), ButtonGesture::VeryLongPress);
}

TEST(ButtonTranslationTests, NoneIsNotAGesture) {
    // button::Event::None means "this observation produced no event". Mapping it to
    // any gesture would invent an input nobody made — and since a very long press
    // opens the factory-reset confirmation, inventing input is not a small mistake.
    EXPECT_FALSE(to_gesture(prd::button::Event::None).has_value());
}

TEST(ButtonTranslationTests, IsATotalMappingWithNoCollisions) {
    // Every gesture must map to a distinct value: two physical gestures collapsing
    // onto one application fact would silently merge wake with Settings.
    const auto short_press = to_gesture(prd::button::Event::ShortPress);
    const auto long_press = to_gesture(prd::button::Event::LongPress);
    const auto very_long = to_gesture(prd::button::Event::VeryLongPress);

    EXPECT_NE(*short_press, *long_press);
    EXPECT_NE(*long_press, *very_long);
    EXPECT_NE(*short_press, *very_long);
}
