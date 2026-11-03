// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstddef>
#include <cstdint>

#include "app_store/store.hpp"

namespace prd::validation {

// Diagnostic event producer only: no renderer, network, storage or private state mutation.
class AccessReview {
  public:
    static constexpr std::size_t kStepCount = 10;
    void begin(app_store::Store& store, app_model::Timestamp now);
    void tick(app_store::Store& store, app_model::Timestamp now);
    [[nodiscard]] std::size_t step() const {
        return step_;
    }

  private:
    void publish(app_store::Store& store, app_model::Timestamp now);
    void ready(app_store::Store& store, app_model::Timestamp now);
    std::size_t step_{0};
    std::uint32_t sequence_{1};
    app_model::Timestamp next_{};
    bool started_{false};
};

} // namespace prd::validation
