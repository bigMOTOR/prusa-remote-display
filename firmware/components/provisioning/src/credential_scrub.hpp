// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstddef>

// Overwriting this component's own credential buffers — Architecture Spec §13.5.
//
// INTERNAL HEADER of the provisioning component. Pure — no ESP-IDF, no cJSON, no socket — so every
// property below is host-tested, which matters more here than usual: server.cpp is the component's
// ESP-IDF adapter TU and cannot be compiled by the host suite, so the MECHANISM is proved here and
// the BINDING is read.
//
// ⛔ WHAT §13.5 ASKS FOR AND WHAT WAS MISSING. "Embedded in crash dumps where avoidable" is the
// clause, and three project-owned buffers in handle_submit() were avoidable and unwiped: the raw
// JSON request body, cJSON's own copies of the credential values, and the Submitted object handed
// to app_core. The rest of this firmware already had the discipline — network's radio::wipe() on
// wifi_config_t, app_core's wipe_bytes() on every Submission — and this component had it only for
// the CSRF token. That inconsistency, not the absence of a mechanism, was the defect.
//
// ⬅️ WHY A THIRD COPY OF A THREE-LINE LOOP. §4.2 forbids reaching into another component's private
// headers, which is exactly why app_core.cpp carries its own wipe_bytes() beside network's
// radio::wipe() and says so in a comment. Promoting the primitive to a shared leaf would add a
// dependency edge for `provisioning`, converge three call-site families and touch two components
// outside this checkpoint's scope. The established pattern is a component-private volatile-pointer
// loop; this is that pattern, applied where it was missing.
//
// ⛔ AND NOTHING HERE COPIES A SECRET IN ORDER TO REMOVE IT. Every function takes the buffer that
// already holds the value and overwrites it in place. A scrubber that had to hold the credential
// would be the wrong shape, on the same argument wifi_log_policy.hpp makes about redaction.

namespace prd::provisioning::scrub {

// Overwrites `size` bytes at `data` through a volatile pointer so the compiler cannot elide the
// store. Byte-for-byte the mechanism network::radio::wipe() and app_core's wipe_bytes() use.
//
// A null pointer or a zero size is a no-op rather than an error: the callers below are cleanup
// paths, and cleanup that can fail is cleanup nobody can rely on.
void bytes(void* data, std::size_t size);

// Overwrites a NUL-terminated string in place, terminator kept, so the buffer is left as a valid
// empty string rather than as unreadable memory.
//
// ⬅️ THIS IS THE cJSON CASE. cJSON_Delete() frees each `valuestring` without overwriting it, so the
// passphrase and the API key survive in freed heap until something else happens to reuse it. The
// field is public, so clearing it before the delete needs no vendor internals — and length is taken
// from the buffer rather than passed in, because a caller that had to supply the length would be a
// second place that could be wrong about it.
void text(char* value);

// Overwrites a region when the enclosing scope ends, on EVERY exit from it.
//
// ⛔ RAII RATHER THAN A CALL BEFORE EACH `return`, AND THAT IS THE POINT. handle_submit() leaves by
// several paths after the body has been read — a parse failure, a stage refusal, the success path —
// and a cleanup that has to be repeated at each one is a cleanup that the next added return
// forgets. This makes "wiped on every exit" a property of the declaration instead of a property of
// the reviewer's attention.
//
// ⛔ THE REGION MUST NOT MOVE WHILE THIS IS ALIVE. It records the pointer it was given, so the
// buffer it guards must not be resized or reallocated inside the scope. Both current uses are a
// once-sized std::string and a stack object, neither of which changes address.
class Scope {
  public:
    Scope(void* data, std::size_t size) : data_{data}, size_{size} {}

    ~Scope() {
        bytes(data_, size_);
    }

    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
    Scope(Scope&&) = delete;
    Scope& operator=(Scope&&) = delete;

  private:
    void* data_;
    std::size_t size_;
};

} // namespace prd::provisioning::scrub
