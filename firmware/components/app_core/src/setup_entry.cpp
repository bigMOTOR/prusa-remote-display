// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include "setup_entry.hpp"

namespace prd::app_core::setup_entry {

std::string_view log_label(const Outcome outcome) {
    switch (outcome) {
    case Outcome::Started:
        return "started";

    case Outcome::StartedWithoutServer:
        return "started-without-server";

    case Outcome::AccessPointUnavailable:
        return "access-point-unavailable";
    }

    return "access-point-unavailable";
}

Outcome begin(const Steps& steps, void* const context) {
    // 1. The access point, because everything after it needs an address that exists.
    if (!steps.start_access_point(context)) {
        // Nothing minted for this session may outlive it — the password most of all.
        steps.discard_session(context);
        return Outcome::AccessPointUnavailable;
    }

    // 2. ⛔ THE STEP MILESTONE 3.11 T4 WAS MISSING, AND IT IS SECOND FOR TWO REASONS. The panel's
    // address is derived from this set, and so is the access point's own Origin — so until this
    // runs, the session has no address to show AND the privileged surface refuses the only caller
    // it exists for. Both consequences have one cause and one fix, and this is it.
    steps.publish_addresses(context);

    // 3. The server. It may already be running from a previous entry, which is exactly the case
    // that hid the defect: on that path it starts nothing and publishes nothing, so step 2 above
    // is the only thing that makes the address known.
    const bool serving = steps.start_server(context);

    // 4. The panel last, so what it is handed is derived from a set that is already current.
    steps.submit_session(context);

    return serving ? Outcome::Started : Outcome::StartedWithoutServer;
}

} // namespace prd::app_core::setup_entry
