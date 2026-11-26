// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#pragma once
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

// Only address formatting uses the native library. Socket lookup never reaches the kernel.
#define getsockname fixture_getsockname
// Match the lwIP in6_addr member spelling in the production translation unit.
#if defined(__APPLE__)
#define un __u6_addr
#elif defined(__linux__)
#define un __in6_u
#endif
#define u32_addr __u6_addr32
int fixture_getsockname(int, sockaddr*, socklen_t*);
