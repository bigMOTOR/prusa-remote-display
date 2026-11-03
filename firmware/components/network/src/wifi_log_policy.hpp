// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstdint>

// The log level the ESP-IDF Wi-Fi stack is held at — Milestone 3.11 T19.
//
// INTERNAL HEADER of the network component. Pure — no ESP-IDF, no driver, no clock — so the rule
// below is host-tested, and `radio.cpp` static_asserts it against the real ESP-IDF enum.
//
// ⛔ WHY THIS EXISTS: THE VENDOR STACK LOGS THE SSID. At Info, libnet80211 emits
//
//     I (2261) wifi:connected with <ssid>, aid = 6, channel 8, BW20, bssid = <router mac>
//
// on every association. Architecture Spec §13.2 places `wifiSsid` INSIDE SecretConfigV1, and §13.5
// says credentials must never be written to logs — unqualified. §23.2 requires credential redaction
// in release, and Bootstrap §10.4 says the hardware-validation profile "shall never contain user
// credentials". T19 found the line in 8 of 19 bench captures. That the emitting code is Espressif's
// rather than this project's does not change what the shipped image does, which is what the
// contract governs.
//
// ⬅️ WHY A LEVEL AND NOT A FILTER. Owner ruling 2026-08-22 forbids value-based filtering: comparing
// or copying the SSID in order to redact it would put the credential on one more code path, and a
// redactor that has to hold the secret to hide it is the wrong shape. Refusing to emit the class of
// line is strictly stronger than editing it afterwards.
//
// ⛔ AND WHY IT IS NOT esp_log_level_set("wifi", ...). The blobs do not route through esp_log at
// all: libnet80211 imports `wifi_log` and `g_log_level`, not `esp_log_write`. The level that
// governs them is set only through esp_wifi_internal_set_log_level(), so the generic tag mechanism
// would have looked correct and changed nothing.
//
// ⛔ APPLIED AFTER esp_wifi_init(), NOT BEFORE. esp_wifi_init() itself calls
// esp_wifi_set_log_level(), which derives the level from CONFIG_LOG_MAXIMUM_LEVEL — Debug at 4 for
// debug and hardware-validation, Info at 3 for release. Every profile therefore lands at Info or
// above on its own, and anything set beforehand is overwritten. Immediately after init is both late
// enough to survive and early enough to be safe: the driver cannot associate before
// esp_wifi_start(), which is later in the same bring-up.

namespace prd::network::wifi_log_policy {

// Mirrors ESP-IDF's wifi_log_level_t. The numeric values are load-bearing — the adapter
// static_asserts each one against the real enum, so this cannot drift into meaning something else.
enum class Level : std::uint8_t {
    None = 0,
    Error = 1,
    Warning = 2,
    Info = 3,
    Debug = 4,
    Verbose = 5,
};

// Whether a level lets the Wi-Fi stack emit the association line, and with it the SSID and the
// router's BSSID. Info is where it starts; everything more verbose includes it.
[[nodiscard]] constexpr bool emits_credentials(const Level level) {
    return level >= Level::Info;
}

// Whether a level still reports faults. Owner requirement 2: the correction must not buy silence
// about a radio that is failing — a Wi-Fi stack that has stopped saying anything is its own defect.
[[nodiscard]] constexpr bool reports_faults(const Level level) {
    return level >= Level::Warning;
}

// The level this firmware applies, in EVERY profile.
//
// ⛔ NOT PROFILE-DEPENDENT, ON PURPOSE. Owner requirement 1: a profile boundary must not be what
// stands between a real SSID and the log. debug and hardware-validation are the profiles a real
// credential is most likely to be typed into, and Bootstrap §10.4 names hardware-validation
// explicitly. So the policy is one value with no build-type branch to get wrong.
//
// Warning is the highest level that satisfies both predicates below, which is what makes it the
// smallest correction rather than a general reduction in logging: Wi-Fi errors and warnings still
// print, and this policy governs the VENDOR stack only.
//
// ⛔ AND THE PROJECT'S OWN Wi-Fi LOGGING IS A SEPARATE OBLIGATION, WHICH THIS HEADER USED TO GET
// WRONG. It claimed the project's own network logging "already reports the SSID as a presence
// boolean rather than a value". That was FALSE when it was written: network::restart() ended its
// Info line in `ssid=` plus the configured network name, so a second same-boot credential
// submission put the user's SSID in the release log — through a path no bench capture of a
// first-time provisioning could reach, because restart() runs only when the station is already up.
// The claim is corrected here rather than deleted, so the next reader knows the gap was real.
//
// THE TWO PROJECT-OWNED PATHS THAT CAN NAME A NETWORK, STATED IN FULL:
//
//   wifi.cpp      — the STATION. Reports phase, address, reason code, attempt count and passphrase
//                   PRESENCE. It keeps no copy of the SSID and names it nowhere, in any profile.
//   setup_ap.cpp  — the device's OWN generated setup network. Logs that SSID by design: §14.2 has
//                   the panel show it, and `PRD-Setup-<device suffix>` is this device's identity,
//                   not the user's credential. Its PASSWORD is logged in no profile.
//
// ⬅️ AND THE LIST ABOVE IS GUARDED, NOT MERELY ASSERTED. tools/check-credential-logs.py fails the
// build when a project-owned log or timeline call takes a credential value in one of the shapes this
// repository is known to write. setup_ap.cpp's line is permitted by a marker BOUND TO THAT ONE CALL
// and consumed by it — not by the file, and not by proximity — and the gate still rejects that call
// if a credential is added to it.
//
// ⛔ AND THE GUARD IS NOT A PROOF, WHICH THIS HEADER OF ALL PLACES MUST SAY PLAINLY. It reads source
// text. It performs no alias analysis, no data-flow tracking and no interprocedural reasoning, so a
// credential copied into a differently named local — or formatted by a helper before it reaches a log
// — is outside its reach. It is a deterministic regression guard over known shapes, and the shapes it
// covers are enumerated in its own tests.
//
// ⬅️ IT HAS ALSO BEEN WRONG TWICE AND CORRECTED TWICE, which is the more useful thing to know about
// it than any claim of coverage: an ordered-regex mask let `"http://…" + password` through because
// the `//` inside the literal was read as a comment, and an earlier allowance was granted by line
// proximity rather than by binding. Both were found by review, not by the gate.
//
// This header went stale by claiming a property it did not have. Replacing that with a differently
// worded absolute claim would repeat the mistake, so the scope is stated instead of the certainty.
inline constexpr Level kApplied = Level::Warning;

static_assert(!emits_credentials(kApplied), "§13.5: the applied level must not log credentials");
static_assert(reports_faults(kApplied), "a level that hides radio faults is not acceptable");

} // namespace prd::network::wifi_log_policy
