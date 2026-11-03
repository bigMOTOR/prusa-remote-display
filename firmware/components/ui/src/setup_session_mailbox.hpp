// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstddef>
#include <cstdint>
#include <utility>

// The UI's one-slot FreeRTOS queue copies a setup password by value. Receiving
// an item does not erase the queue's backing storage, so the consumer must
// overwrite that slot with zeroes before releasing the producer lock. Keeping
// this transaction free of FreeRTOS lets the host suite exercise its ordering.
namespace prd::ui::setup_session_mailbox {

inline void wipe_bytes(void* const data, const std::size_t size) {
    auto* const bytes = static_cast<volatile std::uint8_t*>(data);
    for (std::size_t index = 0; index < size; ++index) {
        bytes[index] = 0;
    }
}

template <typename Ops> void publish(Ops& ops, const typename Ops::Payload& session) {
    ops.lock();
    ops.overwrite(session);
    ops.unlock();
}

template <typename Ops, typename Handler> bool drain(Ops& ops, Handler&& handler) {
    using Payload = typename Ops::Payload;
    Payload session{};

    // The render task must not stall behind the producer. A missed try is
    // harmless: it will drain the latest value on its next iteration.
    if (!ops.try_lock()) {
        return false;
    }

    bool received = false;
    while (ops.try_receive(session)) {
        received = true;
    }
    if (received) {
        Payload blank{};
        wipe_bytes(&blank, sizeof(blank)); // Includes padding in the queue item's byte copy.
        ops.overwrite(blank);
        ops.reset(); // The slot is now zero; reset only makes the blank item unavailable.
    }
    ops.unlock();

    if (received) {
        std::forward<Handler>(handler)(session);
    }
    wipe_bytes(&session, sizeof(session));
    return received;
}

} // namespace prd::ui::setup_session_mailbox
