// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only
#pragma once
using esp_err_t = int;
inline constexpr esp_err_t ESP_OK = 0;
inline constexpr esp_err_t ESP_ERR_INVALID_ARG = 0x102;
inline constexpr esp_err_t ESP_ERR_HTTP_CONNECT = 0x7002;
inline constexpr esp_err_t ESP_ERR_HTTP_EAGAIN = 0x7007;
