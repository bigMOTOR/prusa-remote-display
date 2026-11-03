// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include "storage/key_value.hpp"

#include <string>

// ESP-IDF headers are not -Wundef clean. Suppress locally (per architecture
// spec §12.3) so the project -Wundef policy still applies to our own code.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wundef"
#include "esp_err.h"
#include "nvs.h"
#pragma GCC diagnostic pop

// The NVS key-value adapter.
//
// IT CONTAINS NO POLICY. Which keys exist, what they hold, when a record is written and
// which one is active are all `config`'s business; this file translates five typed
// operations onto NVS and nothing more.
//
// ⛔ THE SAME RULE AS storage.cpp: no security API, no key generation, no eFuse. The only
// NVS functions named here are nvs_open, nvs_close, nvs_commit, nvs_set_blob,
// nvs_get_blob, nvs_set_u32, nvs_get_u32 and nvs_erase_key —
// tools/check-efuse-safety.sh fails the build if that ever stops being true.
//
// nvs_erase_key ERASES ONE KEY. nvs_flash_erase — which erases a whole partition — does
// not appear in this project at all, and R10 is why.
//
// EVERY OPERATION OPENS AND CLOSES ITS OWN HANDLE. That is what keeps §5.10's "should not
// expose raw NVS handles outside the component" true without the component having to hold
// one, and it makes each write self-committing: nvs_close does NOT commit, so a retained
// handle would make "written" and "durable" two different states that only this file
// could tell apart. Configuration writes happen during provisioning, not in a loop, so
// the extra open costs nothing that matters.

namespace prd::storage {
namespace {

// Cross-check of the bound key_value.hpp asserts against, which cannot include this
// header.
static_assert(NVS_KEY_NAME_MAX_SIZE == 16,
              "key_value.hpp assumes NVS stores 15 characters plus a terminator");

// Opens one namespace, runs `operation` against the handle, and closes it.
//
// The handle never leaves this function, so no caller can retain one and none of the
// public operations can be given a handle-shaped signature by accident.
template <typename Operation>
[[nodiscard]] Result with_namespace(const Namespace space, const nvs_open_mode_t mode,
                                    Operation&& operation) {
    const std::string namespace_name{name(space)};

    nvs_handle_t handle = 0;
    const esp_err_t opened = nvs_open(namespace_name.c_str(), mode, &handle);

    if (opened != ESP_OK) {
        // A read-only open of a namespace that has never been written reports
        // NOT_FOUND. That is the first-boot case, not a fault.
        return opened == ESP_ERR_NVS_NOT_FOUND ? Result::NotFound : Result::Failed;
    }

    const Result result = operation(handle);

    nvs_close(handle);
    return result;
}

[[nodiscard]] Result classify_read(const esp_err_t error) {
    switch (error) {
    case ESP_OK:
        return Result::Ok;

    case ESP_ERR_NVS_NOT_FOUND:
        return Result::NotFound;

    case ESP_ERR_NVS_INVALID_LENGTH:
        return Result::TooLarge;

    default:
        return Result::Failed;
    }
}

// Commits after a successful set, so "the call returned Ok" and "the bytes survive a
// reset" are the same fact — see the file header.
[[nodiscard]] Result commit_after(const nvs_handle_t handle, const esp_err_t set_result) {
    if (set_result != ESP_OK) {
        return set_result == ESP_ERR_NVS_NOT_ENOUGH_SPACE ? Result::TooLarge : Result::Failed;
    }

    return nvs_commit(handle) == ESP_OK ? Result::Ok : Result::Failed;
}

class NvsKeyValue final : public KeyValue {
  public:
    [[nodiscard]] Result write_blob(const Namespace space, const std::string_view key,
                                    const std::uint8_t* const data,
                                    const std::size_t length) override {
        if (!key_is_storable(key)) {
            return Result::InvalidKey;
        }

        const std::string key_name{key};

        return with_namespace(space, NVS_READWRITE, [&](const nvs_handle_t handle) {
            return commit_after(handle, nvs_set_blob(handle, key_name.c_str(), data, length));
        });
    }

    [[nodiscard]] Result read_blob(const Namespace space, const std::string_view key,
                                   std::uint8_t* const buffer, const std::size_t capacity,
                                   std::size_t& length_out) override {
        length_out = 0;

        if (!key_is_storable(key)) {
            return Result::InvalidKey;
        }

        const std::string key_name{key};

        return with_namespace(space, NVS_READONLY, [&](const nvs_handle_t handle) {
            // Sized first, so a value too large for the caller reports its real length
            // rather than only that it did not fit.
            std::size_t stored = 0;
            const Result sized =
                classify_read(nvs_get_blob(handle, key_name.c_str(), nullptr, &stored));
            if (sized != Result::Ok) {
                return sized;
            }

            length_out = stored;

            if (stored > capacity) {
                return Result::TooLarge;
            }

            return classify_read(nvs_get_blob(handle, key_name.c_str(), buffer, &stored));
        });
    }

    [[nodiscard]] Result write_u32(const Namespace space, const std::string_view key,
                                   const std::uint32_t value) override {
        if (!key_is_storable(key)) {
            return Result::InvalidKey;
        }

        const std::string key_name{key};

        return with_namespace(space, NVS_READWRITE, [&](const nvs_handle_t handle) {
            return commit_after(handle, nvs_set_u32(handle, key_name.c_str(), value));
        });
    }

    [[nodiscard]] Result read_u32(const Namespace space, const std::string_view key,
                                  std::uint32_t& value_out) override {
        if (!key_is_storable(key)) {
            return Result::InvalidKey;
        }

        const std::string key_name{key};

        return with_namespace(space, NVS_READONLY, [&](const nvs_handle_t handle) {
            return classify_read(nvs_get_u32(handle, key_name.c_str(), &value_out));
        });
    }

    [[nodiscard]] Result erase_key(const Namespace space, const std::string_view key) override {
        if (!key_is_storable(key)) {
            return Result::InvalidKey;
        }

        const std::string key_name{key};

        return with_namespace(space, NVS_READWRITE, [&](const nvs_handle_t handle) {
            // ONE KEY. Removing a key this firmware wrote is not formatting the store —
            // see the warning in key_value.hpp.
            const esp_err_t erased = nvs_erase_key(handle, key_name.c_str());

            if (erased == ESP_ERR_NVS_NOT_FOUND) {
                return Result::NotFound;
            }

            return commit_after(handle, erased);
        });
    }
};

NvsKeyValue g_key_value;

} // namespace

KeyValue& key_value() {
    return g_key_value;
}

} // namespace prd::storage
