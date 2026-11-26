// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include <cstdint>
#include <optional>
#include <vector>

#include <gtest/gtest.h>

#include "app_store/store.hpp"
#include "display_settings_flow.hpp"
#include "ota/first_boot.hpp"
#include "support/fake_key_value.hpp"
#include "support/state_builders.hpp"

namespace {
using namespace std::chrono_literals;
using namespace prd::app_model;
using namespace prd::app_core;
using namespace prd::provisioning;

class BootImagePort final : public prd::ota::BootPort {
  public:
    prd::ota::BootImage running_image() override {
        return image;
    }
    bool confirm_running_image() override {
        return true;
    }
    prd::ota::BootImage image{prd::ota::BootImage::Ordinary};
};

struct Frame {
    Presentation view;
    std::uint32_t revision{0};
};

struct FlowPort {
    FlowPort() {
        (void)model.add_observer(
            [](const AppState& state, void* context) {
                auto& port = *static_cast<FlowPort*>(context);
                if (!port.flow.hides_live(state)) {
                    port.present(project(state), state.revision);
                }
            },
            this);
    }

    void boot(const ConfigurationStatus config,
              const prd::ota::BootImage image = prd::ota::BootImage::Ordinary) {
        boot_port.image = image;
        health.begin(boot_port);
        ConfigurationObservation saved;
        saved.status = config;
        dispatch(Event::configuration_observed(1ms, saved));
        flow.begin(*this, 2ms);
        dispatch(Event::provisioning_phase(3ms, ProvisioningPhase::SetupActive));
        dispatch(Event::boot_phase(4ms, BootPhase::Ready));
        open = true;
        if (health.pending()) {
            OtaObservation ota;
            ota.phase = OtaPhase::Validating;
            ota.pending_validation = true;
            dispatch(Event::ota_observed(5ms, ota));
        }
        tick(6ms);
    }

    void tick(const std::chrono::milliseconds now) {
        flow.tick(*this, now, static_cast<std::uint32_t>(now.count()));
    }

    void start_demo() {
        queued = DisplayRequest{DisplayAction::Demo, {}};
        tick(10ms);
    }

    void gesture(const ButtonGesture kind) {
        const auto event = Event::button_gesture(20ms, kind);
        if (!flow.consume_gesture(*this, event)) {
            dispatch(event); // Same ordinary reducer entry as app_core.
            ++ordinary_gestures;
        }
    }

    [[nodiscard]] DisplaySettingsConditions conditions() const {
        auto facts = overrides;
        facts.setup_open = open;
        facts.exclusive.first_boot_pending = health.pending();
        facts.exclusive.factory_resetting = model.state().factory_resetting;
        facts.exclusive.factory_reset_confirmation_armed =
            model.state().modal_surface == ModalSurface::FactoryResetConfirmation;
        return facts;
    }

    [[nodiscard]] const AppState& state() const {
        return model.state();
    }
    [[nodiscard]] prd::storage::KeyValue& storage() {
        return bytes;
    }
    [[nodiscard]] bool setup_open() const {
        return open;
    }
    [[nodiscard]] bool receive(DisplayRequest& request) {
        if (!queued.has_value()) {
            return false;
        }
        request = *queued;
        queued.reset();
        return true;
    }
    void dispatch(const Event& event) {
        (void)model.dispatch(event);
    }
    void publish(const DisplaySettings& status) {
        publications.push_back(status);
    }
    void present(const Presentation& view, const std::uint32_t revision) {
        frames.push_back({view, revision});
    }
    void note_activity(std::uint32_t) {
        ++activity;
    }
    void age_observation(const Timestamp now) {
        model.age_observation_if_due(now);
    }
    void teardown_setup() {
        open = false;
        ++teardowns;
    }

    DisplaySettingsFlow flow;
    prd::app_store::Store model;
    prd::test::FakeKeyValue bytes;
    BootImagePort boot_port;
    prd::ota::FirstBoot health;
    DisplaySettingsConditions overrides;
    std::optional<DisplayRequest> queued;
    std::vector<Frame> frames;
    std::vector<DisplaySettings> publications;
    int activity{0};
    int teardowns{0};
    int ordinary_gestures{0};
    bool open{false};
};

TEST(DisplaySettingsFlowTests, OrdinaryFirstBootLoadsSettingsAndReturnsToSetup) {
    FlowPort port;
    ASSERT_TRUE(prd::config::save_display_preferences(
        port.bytes, {DisplayMode::PrinterStatus, Backlight::Low}));
    port.boot(ConfigurationStatus::Missing);
    ASSERT_FALSE(port.health.pending());
    const auto& first = port.publications.front();
    EXPECT_FALSE(first.adjustable);
    EXPECT_EQ(first.preferences.mode, DisplayMode::PrinterStatus);
    EXPECT_EQ(first.preferences.backlight, Backlight::Low);
    ASSERT_TRUE(port.publications.back().demo_available);

    port.start_demo();
    ASSERT_TRUE(port.publications.back().demo_running);
    EXPECT_EQ(port.frames.back().revision, 0U);
    EXPECT_EQ(port.frames.back().view.subject, SubjectState::Ready);
    port.gesture(ButtonGesture::ShortPress);
    EXPECT_EQ(port.ordinary_gestures, 0);
    EXPECT_EQ(port.frames.back().revision, 0U);
    port.tick(21ms);
    EXPECT_FALSE(port.publications.back().demo_running);
    EXPECT_EQ(port.frames.back().view.provisioning, ProvisioningPhase::SetupActive);
    EXPECT_EQ(port.frames.back().revision, port.model.revision());
    EXPECT_TRUE(port.open);
    EXPECT_EQ(port.teardowns, 0);
    EXPECT_EQ(prd::config::load_display_preferences(port.bytes).backlight, Backlight::Low);
}

TEST(DisplaySettingsFlowTests, ConfiguredExplicitAndTimedExitRestoreTheLiveStoreRevision) {
    for (const bool timed : {false, true}) {
        FlowPort port;
        port.boot(ConfigurationStatus::Valid);
        port.start_demo();
        ASSERT_TRUE(port.publications.back().demo_running);
        port.dispatch(
            Event::printer_snapshot(11ms, prd::test::printer_snapshot(PrinterState::Printing, 1)));
        EXPECT_EQ(port.frames.back().revision, 0U);
        const auto previous_live_revision = port.model.revision();

        if (!timed) {
            port.gesture(ButtonGesture::ShortPress);
            EXPECT_EQ(port.teardowns, 0);
        }
        port.tick(timed ? 31010ms : 21ms);
        EXPECT_EQ(port.teardowns, 1);
        EXPECT_FALSE(port.open);
        EXPECT_FALSE(port.publications.back().demo_running);
        EXPECT_EQ(port.model.state().provisioning, ProvisioningPhase::Inactive);
        EXPECT_EQ(port.frames.back().view.subject, SubjectState::Printing);
        EXPECT_EQ(port.frames.back().view.stale_indicator, timed);
        EXPECT_EQ(port.frames.back().revision, port.model.revision());
        EXPECT_GT(port.frames.back().revision, previous_live_revision);
    }
}

TEST(DisplaySettingsFlowTests, PendingSetupSubmissionPreservesSetupOnAnExitTap) {
    FlowPort port;
    port.boot(ConfigurationStatus::Valid);
    port.start_demo();
    port.gesture(ButtonGesture::ShortPress);
    port.overrides.submission_waiting = true;
    port.tick(21ms);
    EXPECT_TRUE(port.open);
    EXPECT_EQ(port.teardowns, 0);
    EXPECT_EQ(port.frames.back().view.provisioning, ProvisioningPhase::SetupActive);
    EXPECT_EQ(port.frames.back().revision, port.model.revision());
}

TEST(DisplaySettingsFlowTests, ValidationOtaAndOrdinaryHoldsDisplaceDemo) {
    FlowPort port;
    port.boot(ConfigurationStatus::Valid);
    port.start_demo();
    port.gesture(ButtonGesture::LongPress);
    EXPECT_EQ(port.ordinary_gestures, 1);
    EXPECT_EQ(port.teardowns, 0);
    EXPECT_EQ(port.frames.back().revision, port.model.revision());

    port.start_demo();
    port.gesture(ButtonGesture::VeryLongPress);
    EXPECT_EQ(port.ordinary_gestures, 2);
    EXPECT_EQ(port.model.state().modal_surface, ModalSurface::FactoryResetConfirmation);
    port.tick(21ms);
    EXPECT_FALSE(port.publications.back().demo_available);
    EXPECT_EQ(port.frames.back().revision, port.model.revision());

    FlowPort validating;
    validating.boot(ConfigurationStatus::Valid);
    validating.start_demo();
    validating.gesture(ButtonGesture::ShortPress);
    validating.overrides.exclusive.configuration_in_flight = true;
    validating.dispatch(
        Event::provisioning_phase(20ms, ProvisioningPhase::ValidatingConfiguration));
    validating.tick(21ms);
    EXPECT_EQ(validating.frames.back().view.provisioning,
              ProvisioningPhase::ValidatingConfiguration);
    EXPECT_EQ(validating.frames.back().revision, validating.model.revision());
    EXPECT_EQ(validating.teardowns, 0);

    FlowPort ota;
    ota.boot(ConfigurationStatus::Valid);
    ota.start_demo();
    ota.gesture(ButtonGesture::ShortPress);
    ota.overrides.exclusive.firmware_update_active = true;
    OtaObservation installing;
    installing.phase = OtaPhase::Installing;
    ota.dispatch(Event::ota_observed(20ms, installing));
    ota.gesture(ButtonGesture::ShortPress);
    EXPECT_EQ(ota.ordinary_gestures, 1);
    ota.tick(21ms);
    EXPECT_EQ(ota.teardowns, 0);
    EXPECT_EQ(ota.frames.back().view.layer, PresentationLayer::ExclusiveOperation);
    EXPECT_EQ(ota.frames.back().revision, ota.model.revision());
    EXPECT_FALSE(ota.publications.back().demo_running);
}

TEST(DisplaySettingsFlowTests, AnAlreadyQueuedSubmissionWinsOverDemoAdmission) {
    FlowPort port;
    port.boot(ConfigurationStatus::Valid);
    port.overrides.submission_waiting = true;
    port.start_demo();
    EXPECT_FALSE(port.publications.back().demo_running);
    EXPECT_EQ(port.activity, 0);
    EXPECT_EQ(port.teardowns, 0);
}

TEST(DisplaySettingsFlowTests, PendingOtaHealthIsDifferentFromOrdinaryFirstBoot) {
    FlowPort port;
    port.boot(ConfigurationStatus::Missing, prd::ota::BootImage::Pending);
    ASSERT_TRUE(port.health.pending());
    EXPECT_FALSE(port.publications.back().demo_available);
    port.start_demo();
    EXPECT_FALSE(port.publications.back().demo_running);
    EXPECT_EQ(port.activity, 0);
    EXPECT_EQ(port.teardowns, 0);
}

} // namespace
