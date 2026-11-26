// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstdint>
#include <utility>

#include "app_model/events.hpp"
#include "app_model/presentation.hpp"
#include "config/config.hpp"
#include "exclusive_operation.hpp"
#include "provisioning/server.hpp"
#include "validation/user_demo.hpp"

namespace prd::app_core {

struct DisplaySettingsConditions {
    exclusive::Conditions exclusive;
    bool setup_open{false};
    bool wifi_validated{false};
    bool rollback_pending{false};
    bool station_restore_due{false};
    bool submission_waiting{false};
    bool bench_owns_panel{false};
};

// The application task owns this flow. Its port binds the real queue, settings surface
// and renderer; a host can drive the same code with an explicit clock and those boundaries.
class DisplaySettingsFlow {
  public:
    template <typename Port> void begin(Port& port, const app_model::Timestamp now) {
        settings_.preferences = config::load_display_preferences(port.storage());
        port.dispatch(app_model::Event::display_preferences(now, settings_.preferences));
        port.publish(settings_);
    }

    [[nodiscard]] bool hides_live(const app_model::AppState& state) const {
        return demo_.active() && validation::may_present_user_demo(state);
    }

    template <typename Port> bool consume_gesture(Port& port, const app_model::Event& event) {
        if (event.type != app_model::EventType::ButtonGestureObserved ||
            !hides_live(port.state())) {
            return false;
        }
        if (event.button.gesture == app_model::ButtonGesture::ShortPress) {
            // The app task services existing setup and OTA work before this tap can close setup.
            exit_requested_ = true;
            return true;
        }

        // The caller applies the hold through the ordinary setup/reset paths.
        finish(port, false, event.observed_at);
        return false;
    }

    template <typename Port>
    void tick(Port& port, const app_model::Timestamp now, const std::uint32_t now_ms) {
        const auto conditions = port.conditions();
        const auto& state = port.state();
        const bool adjustable = conditions.setup_open &&
                                exclusive::provisioning_work_permitted(conditions.exclusive) &&
                                !conditions.exclusive.first_boot_pending &&
                                !conditions.exclusive.configuration_in_flight &&
                                !conditions.exclusive.factory_reset_confirmation_armed &&
                                state.device_fault == app_model::DeviceFaultReason::None;
        const bool demo_allowed = adjustable && validation::may_present_user_demo(state) &&
                                  !conditions.wifi_validated && !conditions.rollback_pending &&
                                  !conditions.station_restore_due && !conditions.bench_owns_panel;

        const bool exit_requested = std::exchange(exit_requested_, false);
        if (demo_.active()) {
            if (!demo_allowed) {
                finish(port, false, now);
            } else if (exit_requested || demo_.expired(now)) {
                finish(port, true, now);
            }
        }

        provisioning::DisplayRequest request;
        const bool received = port.receive(request);
        if (received) {
            if (request.action == provisioning::DisplayAction::Demo) {
                if (demo_allowed && !conditions.submission_waiting && port.setup_open() &&
                    demo_.start(now)) {
                    port.note_activity(now_ms);
                }
            } else {
                ++settings_.revision;
                settings_.saved =
                    adjustable && port.setup_open() && !demo_.active() &&
                    config::save_display_preferences(port.storage(), request.preferences);
                if (settings_.saved) {
                    settings_.preferences = request.preferences;
                    port.note_activity(now_ms);
                    port.dispatch(app_model::Event::display_preferences(now, request.preferences));
                }
            }
        }

        if (demo_.active()) {
            const auto frame = demo_.advance(now);
            if (frame.has_value()) {
                // The renderer cannot acknowledge a real store revision from a demo frame.
                port.present(*frame, 0U);
            } else if (!demo_.active()) {
                finish(port, true, now);
            }
        }

        const bool running = demo_.active();
        const bool adjustable_now = adjustable && port.setup_open() && !running;
        const bool available_now = demo_allowed && port.setup_open() && !running;
        if (received || settings_.adjustable != adjustable_now ||
            settings_.demo_available != available_now || settings_.demo_running != running) {
            settings_.adjustable = adjustable_now;
            settings_.demo_available = available_now;
            settings_.demo_running = running;
            port.publish(settings_);
        }
    }

  private:
    template <typename Port>
    void finish(Port& port, const bool resume_monitoring, const app_model::Timestamp now) {
        const auto& state = port.state();
        const auto conditions = port.conditions();
        const bool leave_setup =
            resume_monitoring && validation::may_present_user_demo(state) &&
            state.configuration.status == app_model::ConfigurationStatus::Valid &&
            !conditions.exclusive.configuration_in_flight && !conditions.wifi_validated &&
            !conditions.rollback_pending && !conditions.submission_waiting;
        if (leave_setup) {
            port.age_observation(now);
            demo_.stop();
            port.teardown_setup();
            port.dispatch(
                app_model::Event::provisioning_phase(now, app_model::ProvisioningPhase::Inactive));
        } else {
            demo_.stop();
            port.present(app_model::project(state), state.revision);
        }
    }

    validation::UserDemo demo_;
    provisioning::DisplaySettings settings_;
    bool exit_requested_{false};
};

} // namespace prd::app_core
