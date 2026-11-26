// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#pragma once
#include "esp_err.h"
#include <cstddef>
#include <cstdint>
#include <sys/types.h>

using httpd_handle_t = void*;
struct httpd_req_t {
    std::size_t content_len{0};
    void* fixture{nullptr};
};
enum httpd_method_t { HTTP_GET, HTTP_POST };
struct httpd_uri_t {
    const char* uri;
    httpd_method_t method;
    esp_err_t (*handler)(httpd_req_t*);
    void* user_ctx;
};
struct httpd_config_t {
    std::uint16_t server_port{};
    unsigned task_priority{};
    std::size_t stack_size{};
    std::uint16_t max_open_sockets{};
    std::uint16_t max_uri_handlers{};
    std::uint16_t backlog_conn{};
    bool lru_purge_enable{};
    std::uint16_t recv_wait_timeout{};
    std::uint16_t send_wait_timeout{};
    std::size_t max_uri_len{};
};
#define HTTPD_DEFAULT_CONFIG()                                                                     \
    {                                                                                              \
    }
inline constexpr ssize_t HTTPD_RESP_USE_STRLEN = -1;
esp_err_t httpd_start(httpd_handle_t*, const httpd_config_t*);
esp_err_t httpd_stop(httpd_handle_t);
esp_err_t httpd_register_uri_handler(httpd_handle_t, const httpd_uri_t*);
int httpd_req_to_sockfd(httpd_req_t*);
std::size_t httpd_req_get_hdr_value_len(httpd_req_t*, const char*);
esp_err_t httpd_req_get_hdr_value_str(httpd_req_t*, const char*, char*, std::size_t);
int httpd_req_recv(httpd_req_t*, char*, std::size_t);
esp_err_t httpd_resp_set_type(httpd_req_t*, const char*);
esp_err_t httpd_resp_set_status(httpd_req_t*, const char*);
esp_err_t httpd_resp_set_hdr(httpd_req_t*, const char*, const char*);
esp_err_t httpd_resp_send(httpd_req_t*, const char*, ssize_t);
