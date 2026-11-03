// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include "network/wifi.hpp"

#include "sdkconfig.h"

#include <atomic>
#include <cstdio>
#include <string>

// ESP-IDF headers are not -Wundef clean. Suppress locally (per architecture
// spec §12.3) so the project -Wundef policy still applies to our own code.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wundef"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_random.h"
// esp_ptr_internal() — the runtime half of the event channel's internal-memory proof (R10).
#include "esp_memory_utils.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#pragma GCC diagnostic pop

#include "diagnostics/log.hpp"
#include "diagnostics/tags.hpp"
#include "driver_credential.hpp"
#include "network/setup_ap.hpp"
#include "radio.hpp"
#include "station_events.hpp"
#include "station_intent.hpp"

// The Wi-Fi driver adapter.
//
// IT CONTAINS NO POLICY. Which operation may be issued and when, which event discharges which
// completion, when to retry, how long to wait, when to stop trying and when to quarantine all live
// in station_intent.cpp, station_events.hpp, lifecycle.cpp and backoff.cpp, where the host suite
// can reach them. What is here is the translation of esp_wifi and esp_netif events into records,
// the four driver calls behind function pointers, and obedience to what comes back.
//
// ⛔ NO TASK. Until this checkpoint this file created `prd_wifi` — an undocumented sixth task
// against §10.1's five — and that task owned the retry deadline, the phase machine and every driver
// call. It is gone. station::service() below runs on the CALLER'S task, which §10.2 makes the
// application task, and it is the single consumer of the event ring. That is what makes the
// machine's state safe without a lock: there is one caller, so there is nothing to contend over.
//
// ⛔ THE EVENT HANDLER RECORDS AND NOTHING ELSE. It runs on ESP-IDF's `sys_evt` task, whose stack
// is 2,304 bytes (CONFIG_ESP_SYSTEM_EVENT_TASK_STACK_SIZE). Emitting a timeline record there built
// several std::string temporaries and ran ESP_LOGI's vprintf on that stack; on 2026-08-18 the board
// panicked with "A stack overflow in task sys_evt has been detected", rebooted, and left the
// browser polling a server that had forgotten the attempt. The handler now performs a bounds check,
// five scalar stores and one release store — no allocation, no formatting, no logging, no driver
// call, no dispatch, no block.
//
// ⛔ NO CREDENTIAL REACHES A LOG, AND ONLY ONE IS EVER HELD. Every line below is built from fixed
// text plus non-secret facts: a phase, an address, a reason code, a request identity, or the
// PRESENCE of a passphrase. The SSID and the password arrive as parameters and are copied into a
// wifi_config_t; that buffer is wiped the instant esp_wifi_set_config() consumes it, and again at
// every terminal boundary.
//
// ⬅️ THIS FILE HELD AND LOGGED THE SSID UNTIL 2026-08-23, WHICH IS WHY THE RULE IS SPELLED OUT.
// A file-scope `std::string g_ssid` mirrored the configured network name so that publish() could
// put it into NetworkState, and restart() appended it to a line at Info — which
// CONFIG_LOG_MAXIMUM_LEVEL_INFO compiles into the release image. The owner clarification of
// 2026-08-23 settled that the actual home SSID is credential material under §§7.3, 13.2 and 13.5,
// so both the copy and the line are gone. check-credential-logs.py guards against a line of that
// SHAPE returning — it reads source text and is not data-flow analysis, so it is a regression
// guard rather than a proof; the rule above is what actually governs.

namespace prd::network {
namespace {

using station::Delivery;
using station::Driver;
using station::Event;
using station::EventKind;
using station::EventRing;
using station::kNoTag;
using station::Manager;
using station::Moment;
using station::Outcome;
using station::Progress;
using station::Tag;
using station::Truth;

// --- Everything the application task owns ----------------------------------
//
// All five are touched only from station::service(), station::submit() and station::withdraw() —
// which §10.2 confines to the application task — except g_tag and the ring, which are the
// producer/consumer channel and carry their own memory ordering.

Manager g_manager;
EventRing g_ring;

// The identity the event producer stamps on every record. Written by the application task before a
// driver call and read by the event handler.
//
// ⛔ ATOMIC, NEVER VOLATILE. CONFIG_FREERTOS_UNICORE is not set, so the two really do run at the
// same instant on different cores, and `volatile` orders only volatile-to-volatile accesses.
//
// ⛔ AND IT IS **NOT** ASSERTED LOCK-FREE, WHICH AN EARLIER VERSION OF THIS COMMENT CLAIMED.
// std::atomic<std::uint32_t>::is_always_lock_free is FALSE on this target and the assertion that
// once said otherwise was removed from station_events.hpp. What actually holds is established at the
// ESP-IDF boundary below — the configuration guard, the recorded map evidence, and the runtime
// internal-memory check this file performs before the station is allowed to start.
std::atomic<Tag> g_tag{kNoTag};

// The identity counter. Application task only; next_tag() handles its wrap.
Tag g_minted = kNoTag;

// ⛔ THE ONE PENDING STATION CONFIGURATION, AND WHY IT EXISTS. A submission that arrives while a
// driver completion is still owed cannot be applied at once — esp_wifi_set_config() documents
// ESP_ERR_WIFI_STATE for a station that is still connecting — so its configuration waits here until
// the machine says it may go to the driver. It is wiped the instant set_config() consumes it and
// again whenever the intent it belonged to is superseded, so the window is one pending intent and
// not the life of the device.
wifi_config_t g_pending_config{};
bool g_pending_config_held = false;

// Progress produced outside service() — by submit() and withdraw() — merged into the next step so
// there is exactly ONE place an Update is assembled.
Progress g_deferred{};

bool g_running = false;

// The refused-record count last reported, so the §10.7 counter is logged when it moves rather than
// on every tick.
std::uint32_t g_reported_refusals = 0;

void timeline(const std::string_view event) {
    diagnostics::timeline(diagnostics::tag::kNetwork, event);
}

void timeline(const std::string_view event, const std::string_view name, const std::int64_t value) {
    diagnostics::timeline(diagnostics::tag::kNetwork, event, name, value);
}

void timeline(const std::string_view event, const std::string_view first_name,
              const std::int64_t first_value, const std::string_view second_name,
              const std::int64_t second_value) {
    diagnostics::timeline(diagnostics::tag::kNetwork, event, first_name, first_value, second_name,
                          second_value);
}

// --- The producer ----------------------------------------------------------

void wifi_event(void* /*arg*/, esp_event_base_t base, const std::int32_t id, void* data) {
    // Every record is charged to the identity that was current when the operation it completes was
    // issued. The driver thunks below publish it before they call, so this load cannot see a newer
    // identity than the operation that provoked the event.
    const Tag tag = g_tag.load(std::memory_order_acquire);

    // Every field is named as it is filled. Three of them are unsigned integers and only their
    // order would have told them apart if this were four positional arguments.
    Event record;
    record.tag = tag;

    if (base == WIFI_EVENT) {
        switch (id) {
        case WIFI_EVENT_STA_START:
            record.kind = EventKind::Started;
            break;

        case WIFI_EVENT_STA_CONNECTED:
            record.kind = EventKind::Associated;
            break;

        case WIFI_EVENT_STA_DISCONNECTED: {
            // ⬅️ THE REASON CODE IS A SMALL INTEGER describing the access point's answer, and it
            // contains nothing about the credential. Without it "wrong password" and "handshake
            // timed out" are the same event in the log.
            const auto* const event = static_cast<wifi_event_sta_disconnected_t*>(data);
            record.kind = EventKind::Disassociated;
            record.reason = event != nullptr ? event->reason : 0;
            break;
        }

        case WIFI_EVENT_STA_STOP:
            record.kind = EventKind::Stopped;
            break;

        default:
            return;
        }

        g_ring.publish(record);
        return;
    }

    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const auto* const event = static_cast<ip_event_got_ip_t*>(data);

        // A NUMBER, NOT A STRING. The lease crosses as the 32-bit value the netif already holds and
        // is formatted on the application task, which is the only task allowed to build a
        // NetworkObservation. Nothing here allocates.
        record.kind = EventKind::GotIp;
        record.ipv4 = event != nullptr ? event->ip_info.ip.addr : 0;
        g_ring.publish(record);
        return;
    }

    if (base == IP_EVENT && id == IP_EVENT_STA_LOST_IP) {
        record.kind = EventKind::LostIp;
        g_ring.publish(record);
    }
}

// ⚠️ NOT YET CHANGED, AND DELIBERATELY. Owner ruling, 2026-08-17: reasons 15 and 204 are
// documented by ESP-IDF as ambiguous — either can accompany a genuinely wrong pre-shared key,
// and HANDSHAKE_TIMEOUT can equally mean a handshake that timed out for reasons having nothing
// to do with the credential. A blanket reclassification would be a guess in the other
// direction. The reason code is now recorded on every disconnect; the classification decision
// waits on measurement from the real board and router.
//
// ⬅️ IT RUNS ON THE CONSUMER, NOT IN THE HANDLER. The WIFI_REASON_* macros live in esp_wifi.h and
// the intent machine may not see them, so the raw code crosses in the record and is classified
// here, on the application task, before the machine is asked to decide anything.
[[nodiscard]] DisconnectCause classify(const std::uint8_t reason) {
    switch (reason) {
    // §16.1's "credentials rejected". These are the reasons an access point gives when the
    // pre-shared key is wrong or the four-way handshake fails on it — the one class §16.2
    // forbids retrying at high frequency.
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_MIC_FAILURE:
        return DisconnectCause::CredentialsRejected;

    // Everything else — association timeouts, beacon loss, the access point going away,
    // DHCP problems — is §16.1's transient Wi-Fi loss, which §16.2 retries automatically.
    default:
        return DisconnectCause::Transient;
    }
}

// --- The hardware-latency measurement seam ---------------------------------
//
// ⚠️ THE ONE QUESTION THE PINNED SOURCE CANNOT ANSWER. esp_wifi_set_config(), esp_wifi_connect()
// and esp_wifi_disconnect() are thin wrappers over libnet80211.a blobs, so §10.2's "no long
// blocking I/O" cannot be proven by reading — and these three calls now run on the application
// task, which is the task that must never block. The measurement that settles it is a physical one.
//
// ⬅️ AND esp_wifi_sta_get_ap_info() IS IN THE SEAM TOO — R8. It used to run on the station's own
// task inside publish(); with that task gone it runs in observe(), on the application task, on every
// published observation while Connected. It is a fourth blob call that MOVED onto the task that must
// not block, so leaving it unmeasured would have measured three quarters of the change.
//
// This is the seam, and nothing more: a duration in microseconds and an esp_err_t, both non-secret
// integers, through the timeline channel whose signature admits no string value at all.
//
// ⛔ ENABLED PERMANENTLY IN THE HARDWARE-VALIDATION PROFILE, off in debug and release — PM decision,
// 2026-08-25. That is what lets the later physical run measure the EXACT reviewed candidate tree:
// flipping the option for the measurement would otherwise have been a source mutation, and the tree
// under review would not be the tree that was measured.
//
// ⛔ NOT RUN BY THIS CHECKPOINT. The board is untouched.
[[nodiscard]] esp_err_t measured(const std::string_view call, esp_err_t (*const invoke)()) {
#if defined(CONFIG_PRD_WIFI_DRIVER_TIMING)
    const std::int64_t began_us = esp_timer_get_time();
    const esp_err_t result = invoke();
    timeline(call, "us", esp_timer_get_time() - began_us, "err", static_cast<std::int64_t>(result));
    return result;
#else
    (void)call;
    return invoke();
#endif
}

// --- The four driver calls -------------------------------------------------
//
// ⛔ EACH PUBLISHES THE IDENTITY BEFORE IT CALLS. That ordering is the whole of §8.1's stale-answer
// rejection for this subsystem: a completion the driver posts from inside the call is stamped with
// the identity of the operation that provoked it, never with a later one.

esp_err_t invoke_set_config() {
    return esp_wifi_set_config(WIFI_IF_STA, &g_pending_config);
}

esp_err_t invoke_connect() {
    return esp_wifi_connect();
}

esp_err_t invoke_disconnect() {
    return esp_wifi_disconnect();
}

// The RSSI read, which is the fourth blob call now running on the application task. `g_ap_record` is
// application-task-only, like everything else observe() touches.
wifi_ap_record_t g_ap_record{};

esp_err_t invoke_get_ap_info() {
    return esp_wifi_sta_get_ap_info(&g_ap_record);
}

bool driver_configure(void* /*context*/, const Tag tag) {
    if (!g_pending_config_held) {
        // The machine asked for a configuration nobody is holding. It cannot happen while submit()
        // is the only way to create an intent; refused rather than guessed, because the alternative
        // is handing the driver whatever those bytes happen to be.
        diagnostics::log(diagnostics::LogLevel::Error, diagnostics::tag::kNetwork,
                         "wifi station has no pending configuration to apply");
        return false;
    }

    g_tag.store(tag, std::memory_order_release);

    const esp_err_t result = measured("set-config", &invoke_set_config);

    // Wiped whether it was accepted or refused: the credential leaves no copy behind either way
    // (§13.5's "embedded in crash dumps where avoidable").
    radio::wipe(&g_pending_config, sizeof(g_pending_config));
    g_pending_config_held = false;

    if (result != ESP_OK) {
        diagnostics::log(diagnostics::LogLevel::Error, diagnostics::tag::kNetwork,
                         "wifi station configuration refused err=" + std::to_string(result));
        return false;
    }

    return true;
}

bool driver_connect(void* /*context*/, const Tag tag) {
    g_tag.store(tag, std::memory_order_release);
    timeline("connect-requested", "tag", tag);

    const esp_err_t result = measured("connect", &invoke_connect);
    if (result != ESP_OK) {
        diagnostics::log(diagnostics::LogLevel::Error, diagnostics::tag::kNetwork,
                         "wifi station connect refused err=" + std::to_string(result));
        return false;
    }

    return true;
}

bool driver_disconnect(void* /*context*/, const Tag tag) {
    g_tag.store(tag, std::memory_order_release);
    timeline("disconnect-requested", "tag", tag);

    const esp_err_t result = measured("disconnect", &invoke_disconnect);
    if (result != ESP_OK) {
        diagnostics::log(diagnostics::LogLevel::Error, diagnostics::tag::kNetwork,
                         "wifi station disconnect refused err=" + std::to_string(result));
        return false;
    }

    return true;
}

constexpr Driver kDriver{&driver_configure, &driver_connect, &driver_disconnect, nullptr};

// --- What makes the event channel's atomics lock-free on this build --------
//
// ⛔ IT IS NOT THE TYPE TRAIT, AND THE TYPE TRAIT IS FALSE HERE. Established from the pinned SDK
// rather than assumed:
//
//   1. CONFIG_SPIRAM=y selects CONFIG_STDATOMIC_S32C1I_SPIRAM_WORKAROUND, which
//      components/newlib/project_include.cmake turns into -mdisable-hardware-atomics on every
//      translation unit. The reason is physical: S32C1I, Xtensa's compare-and-swap, does not work
//      against external PSRAM. So GCC stops emitting inline atomic instructions, emits calls to
//      __atomic_load_4 / __atomic_store_4 instead, and std::atomic<std::uint32_t> stops being
//      unconditionally lock-free as a TYPE.
//
//   2. ESP-IDF's own implementations (components/newlib/src/stdatomic.c, macros ATOMIC_LOAD and
//      ATOMIC_STORE in priv_include/esp_stdatomic.h) DISPATCH ON THE ADDRESS. _ATOMIC_HW_STUB_LOAD
//      and _ATOMIC_HW_STUB_STORE run first: `if (ptr is not in SOC_EXTRAM_DATA_LOW..HIGH)` they
//      tail-call the S32C1I hardware routine and return. Only an object that IS in external RAM
//      falls through to portENTER_CRITICAL_SAFE(&s_atomic_lock).
//
//   3. The ring, its counters and g_tag all have static storage duration, so they live in .bss —
//      which ESP-IDF places in INTERNAL DRAM. Moving a variable to external RAM requires both
//      EXT_RAM_BSS_ATTR on the variable (nothing here has it) and
//      CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY (asserted off below).
//
// So the producer's publish() executes the hardware path: no interrupt masking, no spinlock,
// nothing that can block inside an ESP-IDF event handler. The guard below is what stops step 3
// becoming untrue silently — if that option is ever enabled, this build fails rather than quietly
// taking a spinlock on the `sys_evt` task.
#if defined(CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY)
#error "CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY would allow the station event ring into "      \
    "external RAM, where ESP-IDF implements 32-bit atomics with a spinlock — inside an event "     \
    "handler that may not block. Re-verify station_events.hpp's channel before enabling it."
#endif

// ⛔ AND THE ADDRESSES ARE CHECKED AT RUN TIME, BEFORE THE STATION IS ALLOWED TO START — R10.
//
// The guard above and the recorded map evidence both describe the tree that was reviewed. Neither
// can see a future linker script, an IDF placement change or a section attribute somebody adds to
// this file: those would move the ring without touching the option. The consequence would be silent
// and exactly the thing the producer may not do — ESP-IDF would implement every one of the handler's
// atomics with `portENTER_CRITICAL_SAFE(&s_atomic_lock)`, a spinlock, inside the event handler.
//
// So the addresses are verified once, before bring-up, and a station that cannot pass refuses to
// start rather than running on an atomic implementation that can block. The WHOLE object is checked
// at both ends: a structure that straddled the boundary would pass a check on its first byte alone.
[[nodiscard]] bool spans_internal_memory(const void* const object, const std::size_t size) {
    const auto* const first = static_cast<const std::uint8_t*>(object);
    return esp_ptr_internal(first) && esp_ptr_internal(first + size - 1);
}

[[nodiscard]] bool event_channel_is_in_internal_memory() {
    if (spans_internal_memory(&g_ring, sizeof(g_ring)) &&
        spans_internal_memory(&g_tag, sizeof(g_tag))) {
        return true;
    }

    // Fixed text and no address: where the object landed is a build fact a map file records, and a
    // diagnostic is not the place to print a pointer.
    diagnostics::log(diagnostics::LogLevel::Error, diagnostics::tag::kNetwork,
                     "wifi station event channel is not in internal memory; the driver event "
                     "handler would take a lock, so no station will be started");
    return false;
}

// --- The pure unit's constants, checked against the pinned SDK -------------
//
// driver_credential names no ESP-IDF type so it can be host-tested; these two lines are what stop
// its bounds drifting away from the struct the driver actually reads.
static_assert(sizeof(wifi_sta_config_t::ssid) == driver_credential::kStationSsidBytes,
              "driver_credential's station SSID bound must be wifi_sta_config_t::ssid's size");
static_assert(
    sizeof(wifi_sta_config_t::password) == driver_credential::kStationPasswordBytes,
    "driver_credential's station password bound must be wifi_sta_config_t::password's size");

// Fills a station configuration from a credential — the ONE construction site.
//
// ⬅️ REFUSED BEFORE THE DRIVER SEES IT, rather than truncated into it. A name that does not fit is a
// different network, and driver_credential says so instead of quietly shortening it.
[[nodiscard]] bool configure_station(const Credentials& credentials, wifi_config_t& config) {
    return driver_credential::copy_ssid(credentials.ssid, config.sta.ssid,
                                        sizeof(config.sta.ssid)) &&
           driver_credential::copy_password(credentials.password, config.sta.password,
                                            sizeof(config.sta.password));
}

// One record per delivered event, emitted HERE rather than in the handler: this task has the stack
// for the formatting that killed the other one.
void record(const Event& event) {
    switch (event.kind) {
    case EventKind::Started:
        timeline("sta-start", "tag", event.tag);
        break;

    case EventKind::Associated:
        timeline("sta-connected", "tag", event.tag);
        break;

    case EventKind::Disassociated:
        timeline("sta-disconnected", "reason", event.reason, "tag", event.tag);
        break;

    case EventKind::GotIp:
        // No address in the line: the lease is not a secret but it is not needed to time DHCP, and
        // the timeline is meant to be quotable without review.
        timeline("got-ip", "tag", event.tag);
        break;

    case EventKind::LostIp:
        timeline("lost-ip", "tag", event.tag);
        break;

    case EventKind::Stopped:
        timeline("sta-stop", "tag", event.tag);
        break;
    }
}

// §10.7's saturating counter, reported when it moves. The controlled recovery policy that goes with
// it is the quarantine the machine has already entered by the time this runs.
void report_refusals() {
    const std::uint32_t refused = g_ring.refused();
    if (refused == g_reported_refusals) {
        return;
    }

    g_reported_refusals = refused;
    timeline("evidence-refused", "records", refused);
}

// Builds the observation from ONE instant of the machine's truth.
[[nodiscard]] app_model::NetworkObservation observe() {
    const Truth& truth = g_manager.truth();

    app_model::NetworkObservation observation;
    observation.phase = truth.phase;
    observation.error.assign(station::error_text(truth.fault));

    // ⛔ THE TYPED FACT TRAVELS BESIDE THE TEXT, AND IT IS THE ONE THAT DECIDES — R7. A quarantined
    // driver reaches UIAD 001 §12.1's highest presentation priority through this enumerator; the
    // string above stays a diagnostic, so no product decision anywhere depends on its wording.
    observation.fault = station::published_fault(truth.fault);

    // ⛔ NO STALE ADDRESS SURVIVES. The machine clears the lease on LostIp, on any disconnect and on
    // a quarantine, so an empty address here is a fact rather than an omission — and Connected is
    // only ever published with a lease in hand.
    if (truth.ipv4 != 0) {
        const esp_ip4_addr_t address{truth.ipv4};
        char text[16] = {};
        std::snprintf(text, sizeof(text), IPSTR, IP2STR(&address));
        observation.ip_address.assign(text);
    }

    // §7.4's signal strength: read only while associated, and absent otherwise rather than reported
    // as a stale number — the same presence rule the rest of the product follows.
    //
    // ⬅️ AND IT IS NEVER A REASON TO PUBLISH. RSSI moves constantly; UI Behaviour Decision 001 §4
    // forbids "small telemetry changes" becoming visible, so the change test that decides whether
    // to publish at all lives in the pure machine's Truth, which has no RSSI in it.
    if (truth.phase == app_model::NetworkPhase::Connected &&
        measured("get-ap-info", &invoke_get_ap_info) == ESP_OK) {
        observation.signal_known = true;
        observation.signal_strength_dbm = g_ap_record.rssi;
    }

    return observation;
}

[[nodiscard]] Moment moment_of(const std::uint32_t now_ms) {
    // The jitter byte comes from the hardware RNG, which is why backoff.cpp takes one rather than
    // generating it: that file stays pure and its ladder stays testable.
    return Moment{now_ms, Jitter{static_cast<std::uint8_t>(esp_random() & 0xFFU)}};
}

} // namespace

namespace station {

Tag submit(const Credentials& credentials, const std::uint32_t now_ms) {
    if (g_manager.quarantined()) {
        // The owner's ruling: no further station driver operation, and no transaction that cannot
        // be judged. The caller reports an unavailable station and asks for a restart.
        diagnostics::log(diagnostics::LogLevel::Warning, diagnostics::tag::kNetwork,
                         "wifi station is quarantined; no attempt will be started until the "
                         "display is restarted");
        return kNoTag;
    }

    if (credentials.ssid.empty()) {
        // Nothing to connect to. withdraw() is the entry point for that, and answering here would
        // hide a caller mistake behind a plausible-looking success.
        return kNoTag;
    }

    // ⬅️ THE CREDENTIAL IS CONVERTED BEFORE THE RADIO IS TOUCHED AT ALL. A name that does not fit is
    // a different network, and driver_credential refuses it instead of quietly shortening it — so a
    // submission that can never work does not first change the radio's mode.
    wifi_config_t config{};

    if (!configure_station(credentials, config)) {
        radio::wipe(&config, sizeof(config));
        diagnostics::log(diagnostics::LogLevel::Error, diagnostics::tag::kNetwork,
                         "wifi station credential does not fit the driver configuration");
        return kNoTag;
    }

    // ⛔ BRING-UP RUNS ONCE PER BOOT, AND A LATER SUBMISSION MUST NOT REPEAT IT. esp_wifi_set_mode()
    // is not free — on one radio, adding a station to a running access point can move the shared
    // channel — and the committed reconfiguration path deliberately never touched the mode, because
    // §14.3 has the browser sitting on the setup network while the device joins the user's. Doing
    // it again on every submission would put the browser session at risk for no gain.
    //
    // ⬅️ AND THE ACCESS POINT OWNS THE OTHER DIRECTION. If a long press raises the setup network
    // after the station is already up, setup_ap::start() reads network::running() and asks for
    // APSTA itself. Mode ownership stays in this component, which is what owner ruling D5 settled.
    if (!g_running) {
        // ⛔ BEFORE ANYTHING TOUCHES THE RADIO — R10. If the channel is not where the design says it
        // is, no station is started at all: an event handler that takes a spinlock is worse than a
        // device that reports it cannot reach the network.
        if (!event_channel_is_in_internal_memory()) {
            radio::wipe(&config, sizeof(config));
            return kNoTag;
        }

        const radio::Mode mode =
            setup_ap::running() ? radio::Mode::AccessPointStation : radio::Mode::Station;

        // ⛔ ONE CALL, BEFORE THE MODE CHANGE, AND BOTH HALVES MATTER. The shared stack must exist
        // before anything registers on its event loop (Milestone 3.11 T1), and the registration
        // must precede the mode change that can post STA_START from inside itself (Milestone 3.8
        // defect 2). radio::ensure_events() does both, in order, so neither can be satisfied while
        // the other is lost — and an event posted while the handler is absent would be a record the
        // ring never sees, which the consumer would correctly report as lost evidence.
        if (!radio::ensure_events(&wifi_event, nullptr)) {
            radio::wipe(&config, sizeof(config));
            return kNoTag;
        }

        if (!radio::prepare(mode)) {
            radio::wipe(&config, sizeof(config));
            return kNoTag;
        }

        if (!radio::start()) {
            radio::wipe(&config, sizeof(config));
            return kNoTag;
        }

        g_running = true;
        timeline("station-brought-up", "apsta", mode == radio::Mode::AccessPointStation ? 1 : 0);
    }

    // Held for the machine to consume. A superseded pending configuration is overwritten here —
    // wifi_config_t is trivially copyable, so every byte of the previous one goes — and the copy is
    // wiped again the instant set_config() takes it.
    g_pending_config = config;
    g_pending_config_held = true;
    radio::wipe(&config, sizeof(config));

    g_minted = next_tag(g_minted);
    timeline("station-submit", "tag", g_minted);

    merge(g_deferred, g_manager.submit(g_minted, moment_of(now_ms), kDriver));

    // ⛔ RE-CHECKED AFTER THE MACHINE RAN — R2. submit() issues set_config and connect synchronously,
    // so a driver refusal quarantines INSIDE this call. The entry check above cannot see that, and
    // returning the identity anyway would bind a browser transaction to a station that will never
    // answer for it: app_core would begin its validation on that tag and wait out the generic budget
    // before saying the network timed out. The truthful answer is the same one a quarantined station
    // gives any submission — kNoTag — which app_core maps to WifiDriverStalled.
    if (g_manager.quarantined()) {
        if (g_pending_config_held) {
            radio::wipe(&g_pending_config, sizeof(g_pending_config));
            g_pending_config_held = false;
        }

        // ⬅️ AND THE DEFERRED RESULT GOES WITH IT. It is charged to an identity no caller received,
        // so nothing can consume it — but leaving it would hand the NEXT service() an evidence
        // result belonging to a transaction that never began.
        g_deferred = Progress{};

        diagnostics::log(diagnostics::LogLevel::Error, diagnostics::tag::kNetwork,
                         "wifi station quarantined while starting the attempt; the display must be "
                         "restarted");
        return kNoTag;
    }

    // ⛔ THE NETWORK NAME IS NOT IN THIS LINE, AND THAT IS THE CORRECTION OF 2026-08-23. What a
    // reader actually needs from this path is whether a passphrase was supplied — an open network
    // and a protected one fail differently — and which identity the transaction is charged to.
    diagnostics::log(diagnostics::LogLevel::Info, diagnostics::tag::kNetwork,
                     std::string{"wifi station intent accepted password="} +
                         (credentials.password.empty() ? "absent" : "present") +
                         " state=" + std::string{log_label(g_manager.state())});

    return g_minted;
}

void station_absent(const std::uint32_t now_ms) {
    if (g_pending_config_held) {
        radio::wipe(&g_pending_config, sizeof(g_pending_config));
        g_pending_config_held = false;
    }

    merge(g_deferred, g_manager.station_absent(moment_of(now_ms)));
}

bool withdraw(const std::uint32_t now_ms) {
    if (g_manager.quarantined()) {
        // ⛔ THE DEVICE FAULT ALREADY OWNS THIS RADIO — PM ruling 3. A quarantined station issues no
        // further driver operation, so there is no honest way to take it down; the panel is already
        // saying Wi-Fi is unavailable and asking for a restart, and the caller must not dress that
        // up as a restore failure.
        diagnostics::log(diagnostics::LogLevel::Warning, diagnostics::tag::kNetwork,
                         "wifi station is quarantined; it cannot be withdrawn until the display is "
                         "restarted");
        return false;
    }

    if (g_manager.quiesced()) {
        diagnostics::log(diagnostics::LogLevel::Warning, diagnostics::tag::kNetwork,
                         "wifi station withdrawal refused: an exclusive operation owns the product "
                         "state");
        return false;
    }

    // ⛔ THE HELD CONFIGURATION GOES FIRST, ON quiesce()'s RULE. It carries the passphrase of the
    // candidate being abandoned, and the machine is about to forget the intent it belonged to.
    // service()'s per-step check would catch it a tick later; wiping here means it is gone the
    // moment the withdrawal is asked for.
    if (g_pending_config_held) {
        radio::wipe(&g_pending_config, sizeof(g_pending_config));
        g_pending_config_held = false;
    }

    timeline("station-withdraw-requested", "state", static_cast<std::int64_t>(g_manager.state()));

    merge(g_deferred, g_manager.withdraw(moment_of(now_ms), kDriver));

    // ⛔ RE-CHECKED AFTER THE MACHINE RAN, ON submit()'s RULE — R2. withdraw() issues the disconnect
    // synchronously, so a driver refusal quarantines INSIDE this call. Reporting success then would
    // tell the caller the station had been taken down when the truth is that the driver stopped
    // answering, and PM ruling 3 sends that condition down the Device Fault path instead.
    if (g_manager.quarantined()) {
        // ⬅️ THE EVIDENCE GOES AND THE PUBLISHED TRUTH STAYS, WHICH IS THE WHOLE DISTINCTION. The
        // outcome is charged to the abandoned candidate's identity and no transaction is waiting on
        // it, so carrying it forward would hand the next service() a verdict for a session that has
        // already ended. The truth change is the quarantine itself and MUST survive: it is what
        // becomes the typed Wi-Fi device fault on the panel.
        g_deferred.evidence = false;
        g_deferred.tag = kNoTag;

        diagnostics::log(diagnostics::LogLevel::Error, diagnostics::tag::kNetwork,
                         "wifi station quarantined while withdrawing; the display must be "
                         "restarted");
        return false;
    }

    diagnostics::log(diagnostics::LogLevel::Info, diagnostics::tag::kNetwork,
                     std::string{"wifi station withdrawal accepted state="} +
                         std::string{log_label(g_manager.state())} +
                         " retained=" + (g_manager.withdrawing() ? "yes" : "no"));

    return true;
}

void quiesce(const std::uint32_t now_ms) {
    // ⛔ THE HELD CONFIGURATION GOES FIRST — R4. It carries the passphrase from a submission the
    // user made into the window the exclusive operation opened, and the machine is about to forget
    // the intent it belonged to. Wiping it here rather than waiting for service()'s per-step check
    // means it is gone the moment the barrier is asserted, not a tick later.
    if (g_pending_config_held) {
        radio::wipe(&g_pending_config, sizeof(g_pending_config));
        g_pending_config_held = false;
    }

    merge(g_deferred, g_manager.quiesce(moment_of(now_ms)));

    // ⬅️ AND ANY RECORDED EVIDENCE IS DISPOSED OF. It belongs to a transaction the barrier has
    // already ended; app_core will not consume it, and carrying it forward would offer it to
    // whatever transaction comes next.
    g_deferred.evidence = false;
    g_deferred.tag = kNoTag;
}

Update service(const std::uint32_t now_ms) {
    const Moment moment = moment_of(now_ms);

    Progress progress = g_deferred;
    g_deferred = Progress{};

    Event event;

    for (;;) {
        const Delivery delivery = g_ring.take(event);

        if (delivery == Delivery::Empty) {
            break;
        }

        if (delivery == Delivery::EvidenceLost) {
            // ⛔ CHECKED BEFORE THE RECORD IS INTERPRETED. The ring reports the hole rather than
            // handing over a record that arrived after one, so nothing downstream can act on
            // evidence whose predecessors are missing.
            timeline("evidence-lost");
            merge(progress, g_manager.evidence_lost());
            continue;
        }

        record(event);

        // ⛔ THE ADAPTER'S OWN TRUTH, CORRECTED HERE — R5. `g_running` is what setup_ap consults to
        // decide whether it must share the radio, and what submit() consults to decide whether
        // bring-up is still needed. A station interface that has stopped invalidates both, and
        // leaving this true meant a later submission would skip ensure_events/prepare/start and then
        // issue connect() against a stopped driver — a refusal, and therefore a permanent quarantine
        // reported as DriverStalled when the real cause was our own stale flag.
        if (event.kind == EventKind::Stopped) {
            g_running = false;
        }

        merge(progress, g_manager.on(event, classify(event.reason), moment, kDriver));
    }

    merge(progress, g_manager.tick(moment, kDriver));

    // ⛔ THE HELD CONFIGURATION GOES THE MOMENT NOTHING CAN ASK FOR IT. set_config() wipes it when
    // it consumes it, but a quarantine, a station stop or a withdrawal ends the intent WITHOUT
    // consuming it — and the passphrase it was built from must not outlive the intent. Checked on
    // every step rather than at each of those boundaries, so a future boundary cannot forget.
    if (g_pending_config_held && !g_manager.credential_wanted()) {
        radio::wipe(&g_pending_config, sizeof(g_pending_config));
        g_pending_config_held = false;
    }

    report_refusals();

    Update update;

    if (progress.truth_changed) {
        update.published = true;
        update.observation = observe();

        diagnostics::log(
            diagnostics::LogLevel::Info, diagnostics::tag::kNetwork,
            "wifi phase=" + std::string{app_model::log_label(update.observation.phase)} + " ip=" +
                (update.observation.ip_address.empty()
                     ? std::string{"none"}
                     : std::string{update.observation.ip_address.view()}) +
                " state=" + std::string{log_label(g_manager.state())} +
                (update.observation.error.empty()
                     ? std::string{}
                     : " error=" + std::string{update.observation.error.view()}));
    }

    update.evidence = progress.evidence;
    update.tag = progress.tag;
    update.outcome = progress.outcome;

    return update;
}

bool quarantined() {
    return g_manager.quarantined();
}

std::uint32_t evidence_refused() {
    return g_ring.refused();
}

std::string_view log_label(const Outcome outcome) {
    switch (outcome) {
    case Outcome::Leased:
        return "leased";

    case Outcome::LeaseInterrupted:
        return "lease-interrupted";

    case Outcome::Unreachable:
        return "unreachable";

    case Outcome::DriverStalled:
        return "driver-stalled";

    case Outcome::EvidenceLost:
        return "evidence-lost";
    }

    return "leased";
}

} // namespace station

bool running() {
    return g_running;
}

} // namespace prd::network
