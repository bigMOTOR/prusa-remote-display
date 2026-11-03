// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstdint>

namespace prd::ota {

enum class BootImage : std::uint8_t { Unknown, Ordinary, Pending };
enum class BootHealth : std::uint8_t { Inactive, Waiting, Confirmed, Failed };

struct Readiness {
    bool application{false};
    bool display{false};
    bool storage{false};
    bool fatal_fault{false};
};

// Narrow platform seam; tests never write OTA metadata or reboot hardware.
class BootPort {
  public:
    virtual ~BootPort() = default;
    [[nodiscard]] virtual BootImage running_image() = 0;
    [[nodiscard]] virtual bool confirm_running_image() = 0;
};

class FirstBoot {
  public:
    // Owner-approved September 12: thirty healthy seconds, not printer uptime.
    static constexpr std::uint32_t kHealthyWindowMs = 30000;
    void begin(BootPort& port);
    void tick(BootPort& port, std::uint32_t now_ms, Readiness readiness);
    [[nodiscard]] BootHealth health() const {
        return health_;
    }
    [[nodiscard]] bool pending() const {
        return pending_;
    }

  private:
    BootHealth health_{BootHealth::Inactive};
    bool begun_{false};
    bool pending_{false};
    bool timing_{false};
    std::uint32_t since_{0};
    std::uint32_t last_{0};
};

// ESP-IDF adapter. No restart, erase, download, key or boot-target selection.
[[nodiscard]] BootPort& device_boot_port();

} // namespace prd::ota
