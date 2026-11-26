// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstddef>
#include <memory>
#include <optional>

#include "app_model/presentation.hpp"
#include "app_store/store.hpp"

namespace prd::validation {

class UserDemo {
  public:
    [[nodiscard]] bool start(app_model::Timestamp now);
    [[nodiscard]] bool active() const {
        return store_ != nullptr;
    }
    [[nodiscard]] bool expired(app_model::Timestamp now) const;
    [[nodiscard]] std::optional<app_model::Presentation> advance(app_model::Timestamp now);
    void stop();

  private:
    // The demo starts from a new store. It never receives the live device's state.
    std::unique_ptr<app_store::Store> store_;
    app_model::Timestamp started_at_{};
    std::size_t next_{0};
};

// Setup is the surface that offered the action. Any higher-priority operation or a
// move into real provisioning work immediately gives the live presentation the panel.
[[nodiscard]] bool may_present_user_demo(const app_model::AppState& live);

} // namespace prd::validation
