// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include "prusalink/prusalink.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <new>
#include <string>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "diagnostics/log.hpp"
#include "diagnostics/tags.hpp"
#include "diagnostics/task_report.hpp"
#include "network/http.hpp"
#include "network/lan.hpp"

#include "decode.hpp"
#include "dto.hpp"
#include "normalize.hpp"
#include "outcome.hpp"
#include "request.hpp"
#include "worker_lifecycle.hpp"

// The single sequential request worker (Architecture Spec §10.4).
//
// One task, one request at a time — which is what §10.4 says a sequential worker buys: fewer
// sockets, less printer load, no response ordering problems and no heap spikes.
//
// ⛔ IT NEVER DISPATCHES. §10.2 makes the application task the store's only consumer, so this
// task's entire output is a Report handed to a sink that posts.
//
// ⛔ IT NEVER LOGS A CREDENTIAL, A HOST OR AN ADDRESS. The cycle line carries the purpose, the
// phase, the HTTP status, the latency and the body length. A serial log is the artifact most likely
// to be shared.
//
// ⬅️ OWNERSHIP, REBUILT AFTER THE 2026-08-23 AUDIT. A worker is HANDED an immutable LaunchContext
// through xTaskCreate's parameter: its generation, its target, its transaction and its sink. It
// never samples a shared "current generation" to discover what it holds — that was the defect, and
// removing the ability to ask is what fixes it. The only shared state left is two atomics, and the
// only thing the worker does with them is compare its own generation for equality.

namespace prd::prusalink {
namespace {

// Response bound (§11.4's "endpoint-specific hard limit", §16.4's "every network body must be
// bounded"). A CORE One status response is a few hundred bytes; this leaves room for optional
// objects a future firmware adds while refusing anything that could not plausibly be one of these
// two responses.
constexpr std::size_t kMaxBody = 2048;

// Deep enough for the body buffer plus esp_http_client's call chain, and reported through the same
// task-stack line every other task uses so the high-water mark is measured rather than assumed.
constexpr std::uint32_t kTaskStackBytes = 8192;

constexpr const char* kTaskName = "prd_prusalink";

// §10.8: "network/prusalink_task — medium", below the UI and application tasks.
constexpr UBaseType_t kTaskPriority = 4;

// One request's wall-clock budget, enforced inside network::get(). API Research Report §12.4
// proposes 4 s total; §11.4 approximately 5 s. Both are marked as needing physical validation.
constexpr std::uint32_t kRequestBudgetMs = 4000;

// How often the transport returns control so the budget and the generation can be checked.
constexpr std::uint32_t kRequestSliceMs = 400;

// Granularity of the inter-cycle wait, so a superseded generation is noticed promptly — a
// provisioning re-entry or a factory reset must not wait out a 60-second probe interval.
constexpr TickType_t kWaitSlice = pdMS_TO_TICKS(250);

// --- The only shared state, and it is atomic ------------------------------------------------
//
// ⬅️ std::atomic, NOT volatile. `volatile` prevents the compiler eliding an access; it provides no
// atomicity and no ordering, and the C++ memory model calls unsynchronised cross-thread access to a
// non-atomic object a data race whatever the target's aligned loads happen to do. The audit was
// right to reject the previous arrangement, and "it works on LX7" is not a warrant.
//
// Release on the writer, acquire on the reader: a worker that observes a new generation is
// guaranteed to see everything the writer did before publishing it.
std::atomic<Generation> g_generation{kNoGeneration};

// Whether a worker exists. Set by start() before the task can run, cleared by the departing worker
// as its last act — which is what makes the one-worker invariant hold across a non-blocking stop.
std::atomic<bool> g_worker_alive{false};

// Everything one worker needs, fixed at creation and owned by that worker alone.
//
// ⛔ HOLDS A CREDENTIAL, AND ITS LIFETIME IS THE WORKER'S. Allocated by start(), owned by the task,
// wiped and freed by the task on the way out. No other code can read it and nothing outlives the
// worker.
struct LaunchContext {
    Generation generation{kNoGeneration};
    Target target{};
    ReportSink sink{nullptr};
};

// Overwrites an object's bytes through a volatile view so the stores are not elided — the compiler
// can see nothing reads them afterwards, which is exactly the optimisation that defeats a wipe.
template <typename T> void wipe(T& object) {
    auto* bytes = reinterpret_cast<volatile unsigned char*>(&object);

    for (std::size_t index = 0; index < sizeof(T); ++index) {
        bytes[index] = 0;
    }
}

[[nodiscard]] bool superseded(const Generation mine) {
    return lifecycle::should_exit(mine, g_generation.load(std::memory_order_acquire));
}

// network::AbandonPredicate. The transport calls this between slices, on this same task, so a
// supersession is observed inside a request rather than only between them.
bool abandon_if_superseded(void* const context) {
    const auto mine = static_cast<Generation>(reinterpret_cast<std::uintptr_t>(context));
    return superseded(mine);
}

// One completed request: what the transport said, and what the body decoded to.
struct Attempt {
    network::HttpResponse response;
    AttemptOutcome outcome;
    DecodeError decoded{DecodeError::None};

    // Which required field ended a version decode, when one did. A literal from decode.cpp's own
    // §4.1 list, so it is safe to log; empty for every other outcome.
    std::string_view decoded_field;

    StatusDto status;
    VersionDto version;
    TargetVerdict target{TargetVerdict::Ok};
};

// Fills the parts of a connection observation that every Poll cycle reports the same way.
void describe_connection(app_model::ConnectionObservation& connection, const Target& target,
                         const AttemptOutcome& outcome, const network::HttpResponse& response) {
    connection.phase = outcome.phase;
    connection.endpoint = target.host;
    connection.auth_method = target.auth_method;
    connection.error.assign(outcome.error);

    // A status code exists only when the transport completed; reporting 0 as a code would make "no
    // answer" indistinguishable from a printer that answered oddly.
    connection.response_code_known = response.outcome == network::HttpOutcome::Ok;
    connection.response_code = response.status;

    // Measured for every attempt, including failures: how long a timeout took is exactly what
    // §7.5's lastLatencyMs is useful for.
    connection.latency_known = true;
    connection.latency_ms = response.latency_ms;

    // capability_tier stays empty. §18.4's tiers rest on a tested compatibility matrix that does
    // not exist yet, and naming one here would publish a claim nothing has verified.
}

// Resolves the target once and decides whether a credential may be sent to the result.
//
// ⛔ THE §13.5 BOUNDARY, IMMEDIATELY BEFORE CREDENTIALED I/O. The address returned here is the one
// the request is issued to — build_url() takes the address, not the host — so no second resolution
// can occur between the check and the connection. That is what makes DNS rebinding a non-event.
[[nodiscard]] TargetVerdict approve(const Target& target, network::lan_policy::Ipv4& address) {
    const network::lan_policy::Resolution resolution =
        network::lan_policy::resolve(target.host.view());

    const TargetVerdict verdict = verdict_for(resolution);

    if (verdict == TargetVerdict::Ok) {
        address = resolution.address;
    }

    // The class, never the address: a private address is still the owner's topology.
    diagnostics::log(diagnostics::LogLevel::Info, diagnostics::tag::kPrinter,
                     "target verdict=" + std::string{log_label(verdict)} + " resolution=" +
                         std::string{network::lan_policy::log_label(resolution.outcome)});

    return verdict;
}

// Performs one GET against `path` and classifies it.
[[nodiscard]] Attempt attempt(const LaunchContext& context, const std::string_view path,
                              const bool is_version) {
    Attempt result;

    network::lan_policy::Ipv4 address{};
    result.target = approve(context.target, address);

    if (result.target != TargetVerdict::Ok) {
        // ⛔ NOTHING IS SENT. Refused before any I/O, so the credential never leaves the device.
        result.response.outcome = network::HttpOutcome::Rejected;
        result.outcome = classify_target(result.target);
        return result;
    }

    app_model::BoundedString<kMaxUrl> url;

    if (!build_url(address, context.target.port, path, url)) {
        result.response.outcome = network::HttpOutcome::Rejected;
        result.outcome = classify(result.response.outcome, 0, DecodeError::None);
        return result;
    }

    std::array<char, kMaxBody> storage{};
    network::BodyBuffer buffer{storage.data(), storage.size()};

    const network::HttpHeader header{kApiKeyHeader, context.target.api_key.view()};

    network::HttpRequest request;
    request.url = url.view();
    request.headers = &header;
    request.header_count = 1;
    request.total_budget_ms = kRequestBudgetMs;
    request.slice_ms = kRequestSliceMs;
    request.abandon = &abandon_if_superseded;
    request.abandon_context =
        reinterpret_cast<void*>(static_cast<std::uintptr_t>(context.generation));

    result.response = network::get(request, buffer);

    // The decode is attempted only for a completed 2xx exchange: classify() ignores the decode
    // result otherwise, and running a decoder over an error page's body would be work nothing reads.
    const bool decodable = result.response.outcome == network::HttpOutcome::Ok &&
                           result.response.status >= 200 && result.response.status < 300;

    if (decodable) {
        result.decoded = is_version
                             ? decode_version(buffer.view(), result.version, &result.decoded_field)
                             : decode_status(buffer.view(), result.status);
    }

    result.outcome = classify(result.response.outcome, result.response.status, result.decoded);
    return result;
}

void log_attempt(const Purpose purpose, const bool is_version, const Attempt& made) {
    // ⛔ No endpoint, no URL, no body, no header.
    diagnostics::log(diagnostics::LogLevel::Info, diagnostics::tag::kPrinter,
                     "probe purpose=" + std::string{log_label(purpose)} +
                         " path=" + std::string{is_version ? "version" : "status"} +
                         " phase=" + std::string{app_model::log_label(made.outcome.phase)} +
                         " transport=" + std::string{network::log_label(made.response.outcome)} +
                         " status=" + std::to_string(made.response.status) +
                         " latency_ms=" + std::to_string(made.response.latency_ms) +
                         " bytes=" + std::to_string(made.response.body_length) +
                         " decode=" + std::string{log_label(made.decoded)} +
                         (made.decoded_field.empty()
                              ? std::string{}
                              : " field=" + std::string{made.decoded_field}));
}

// Stamps identity onto a report and hands it to the sink.
//
// ⛔ THE CHECK BELOW IS AN OPTIMISATION, NOT THE DECISION. It cannot be atomic with the callback that
// follows it, so it can only ever say "definitely superseded" — never "definitely current". What it
// buys is that a worker which already knows it has lost its lease does not enqueue a report nobody
// will accept. The authority is the stamp: the application task compares `generation` and
// `transaction` against the session IT armed, on its own task, before anything is acted on. See
// app_core/src/printer_session.hpp.
[[nodiscard]] bool publish(const LaunchContext& context, Report& report) {
    report.generation = context.generation;
    report.transaction = context.target.transaction;

    if (superseded(context.generation)) {
        diagnostics::log(diagnostics::LogLevel::Info, diagnostics::tag::kPrinter,
                         "result dropped: generation superseded during the request");
        return false;
    }

    if (context.sink != nullptr) {
        context.sink(report);
    }

    return true;
}

// Waits between Poll cycles, giving up early once superseded.
void wait(const Generation mine, const std::chrono::milliseconds delay) {
    TickType_t remaining = pdMS_TO_TICKS(delay.count());

    while (remaining > 0 && !superseded(mine)) {
        const TickType_t slice = remaining < kWaitSlice ? remaining : kWaitSlice;
        vTaskDelay(slice);
        remaining -= slice;
    }
}

// §14.3 steps 6–8 in one bounded pass, publishing a verdict and nothing else.
void run_validation(const LaunchContext& context) {
    Report report;
    report.purpose = Purpose::Validate;
    report.verdict = Verdict::Failed;

    const Attempt version = attempt(context, kVersionPath, true);
    log_attempt(Purpose::Validate, true, version);

    if (!version.outcome.usable) {
        report.failure_phase = version.outcome.phase;
        (void)publish(context, report);
        return;
    }

    if (superseded(context.generation)) {
        return;
    }

    // §14.3 step 8. The version probe proved the endpoint and the key; this proves a required
    // printer state decodes, which is what §13.3 step 5's "test connection" means for a product
    // whose entire job is showing that state.
    const Attempt status = attempt(context, kStatusPath, false);
    log_attempt(Purpose::Validate, false, status);

    if (!status.outcome.usable) {
        report.failure_phase = status.outcome.phase;
        (void)publish(context, report);
        return;
    }

    report.verdict = Verdict::Succeeded;
    (void)publish(context, report);
}

// Ongoing operational polling, against a configuration already marked active.
void run_polling(const LaunchContext& context) {
    // Report acquisition once per worker, before its first network request. Previously only
    // completed attempts published a connection phase, so Connecting could never be observed
    // while the initial probe was actually in flight. Normal retries do not replay this event;
    // the projection also excludes previously observed subjects and failed acquisition attempts.
    {
        Report starting;
        starting.purpose = Purpose::Poll;
        starting.connection_present = true;
        starting.connection.phase = app_model::ConnectionPhase::Probing;
        if (!publish(context, starting)) {
            return;
        }
    }

    // Sequence is per worker and starts at 1. Acceptance no longer depends on beating a previous
    // worker's watermark: the observation carries this worker's generation, and a generation change
    // resets the ordering (see app_model::snapshot_is_current). That is the audit's B2.
    std::uint32_t sequence = 1;

    // Whether `/api/version` has been answered on this run. §4.1 calls for the probe at startup and
    // after a long outage; a refusal or an incompatibility sends the worker back to it.
    bool preflighted = false;

    while (!superseded(context.generation)) {
        const bool probing = !preflighted;
        const Attempt made = attempt(context, probing ? kVersionPath : kStatusPath, probing);
        log_attempt(Purpose::Poll, probing, made);

        Report report;
        report.purpose = Purpose::Poll;
        report.connection_present = true;
        describe_connection(report.connection, context.target, made.outcome, made.response);

        if (made.outcome.usable) {
            if (probing) {
                preflighted = true;
                report.identity_present = true;
                apply_version(made.version, report.identity);
            } else {
                // ⛔ ONE ORDER FOR BOTH HALVES, TAKEN BEFORE THE COUNTER MOVES. The snapshot and
                // the job come from the same response, so they must carry the same
                // `{generation, sequence}`: incrementing between them would make the job look
                // newer than the printer state it arrived with, and the store would then be able
                // to accept one half and reject the other.
                // ⛔ ONE ORDER, ONE OBSERVATION, ONE SEAM. status_from() is the only thing that
                // assembles a reading, and this file — the one translation unit the host suite
                // cannot compile — does no assembling of its own.
                report.status_present = true;
                report.status = status_from(made.status, {context.generation, sequence});

                ++sequence;
            }
        } else if (made.outcome.phase == app_model::ConnectionPhase::Unauthorized ||
                   made.outcome.phase == app_model::ConnectionPhase::UnsupportedApi) {
            // Re-probe next cycle. A corrected key or an upgraded printer changes what the version
            // endpoint says, and this is what makes the recovery automatic rather than a reboot.
            preflighted = false;
        }

        if (!publish(context, report)) {
            return;
        }

        // ⛔ A SUCCESSFUL VERSION PROBE IS FOLLOWED BY STATUS IMMEDIATELY — audit item 7.
        // poll_pause() returns zero for exactly that case and next_delay()'s value for every other,
        // so the pacing decision is host-tested and this line is the binding.
        //
        // ⬅️ AND SUPERSESSION IS STILL RE-CHECKED BEFORE THE FOLLOW-UP GOES OUT. wait() is a no-op
        // for a zero pause, so control returns to the loop condition — `!superseded(...)` — exactly
        // as it does after a full interval. A stop() between the version response and the status
        // request therefore ends the run before the request is issued, and attempt()'s own
        // abandon_if_superseded callback and publish()'s generation check remain the two later
        // chances to drop an obsolete exchange. Nothing about cancellation is weakened by removing
        // the wait; what is removed is only the waiting.
        wait(context.generation,
             poll_pause(probing ? PollRequest::VersionProbe : PollRequest::Status, made.outcome));
    }
}

void worker(void* const argument) {
    diagnostics::log_task_stack(kTaskName, kTaskStackBytes);

    // ⬅️ HANDED, NOT SAMPLED. This is the whole correction: the context — generation, target,
    // transaction, sink — arrives as this task's parameter and is owned by this task alone. There is
    // no shared slot to race and no current-generation global to mistake for our own.
    auto* context = static_cast<LaunchContext*>(argument);

    diagnostics::log(diagnostics::LogLevel::Info, diagnostics::tag::kPrinter,
                     "worker running purpose=" + std::string{log_label(context->target.purpose)} +
                         " generation=" + std::to_string(context->generation) +
                         " profile=" + std::to_string(context->target.transaction.profile) +
                         " attempt=" + std::to_string(context->target.transaction.attempt));

    if (context->target.purpose == Purpose::Validate) {
        run_validation(*context);
    } else {
        run_polling(*context);
    }

    diagnostics::log(diagnostics::LogLevel::Info, diagnostics::tag::kPrinter,
                     "worker stopped generation=" + std::to_string(context->generation));

    // The credential dies here, explicitly, before the memory returns to the heap.
    wipe(*context);
    delete context;

    // ⬅️ CLEARED LAST, AFTER THE FINAL REQUEST HAS COMPLETED. start() refuses while this is true,
    // so clearing it here — and nowhere else — is what guarantees at most one worker is ever making
    // requests, however promptly a caller restarts. Release, so a start() that observes false also
    // observes everything this worker did.
    g_worker_alive.store(false, std::memory_order_release);
    vTaskDelete(nullptr);
}

} // namespace

const char* log_label(const Purpose purpose) {
    switch (purpose) {
    case Purpose::Validate:
        return "validate";
    case Purpose::Poll:
        return "poll";
    }

    return "unknown";
}

const char* log_label(const JobTelemetry telemetry) {
    switch (telemetry) {
    case JobTelemetry::None:
        return "none";
    case JobTelemetry::Absent:
        return "absent";
    case JobTelemetry::Present:
        return "present";
    }

    return "unknown";
}

const char* log_label(const Verdict verdict) {
    switch (verdict) {
    case Verdict::None:
        return "none";
    case Verdict::Succeeded:
        return "succeeded";
    case Verdict::Failed:
        return "failed";
    }

    return "unknown";
}

StartReport start(const Target& target, const ReportSink sink) {
    StartReport report;

    if (!lifecycle::may_start(g_worker_alive.load(std::memory_order_acquire))) {
        return report;
    }

    if (target.host.empty() || sink == nullptr || !target.transaction.valid()) {
        // Silent by design elsewhere, but not here: a refusal with no log is a supervisor that
        // retries forever with nothing to explain it. The three causes are all configuration facts,
        // and none of them can carry a credential.
        diagnostics::log(diagnostics::LogLevel::Warning, diagnostics::tag::kPrinter,
                         std::string{"worker not started: host="} +
                             (target.host.empty() ? "absent" : "present") +
                             " sink=" + (sink == nullptr ? "absent" : "present") +
                             " transaction=" + (target.transaction.valid() ? "valid" : "invalid"));
        return report;
    }

    if (!auth_supported(target.auth_method) || target.api_key.empty()) {
        // A refusal to start, not a connection failure: nothing was attempted, and a log line
        // saying the printer is unreachable would be untrue.
        diagnostics::log(
            diagnostics::LogLevel::Warning, diagnostics::tag::kPrinter,
            "worker not started: auth=" + std::string{app_model::log_label(target.auth_method)} +
                " key=" + (target.api_key.empty() ? "absent" : "present"));
        return report;
    }

    // One small allocation per worker, checked. §16.4 requires a typed report and no reboot loop on
    // allocation failure, and a refusal is exactly that: the supervisor retries on a later tick.
    auto* context = new (std::nothrow) LaunchContext{};

    if (context == nullptr) {
        diagnostics::log(diagnostics::LogLevel::Error, diagnostics::tag::kPrinter,
                         "worker not started: launch context allocation failed");
        return report;
    }

    const Generation generation =
        lifecycle::next_generation(g_generation.load(std::memory_order_acquire));

    context->generation = generation;
    context->target = target;
    context->sink = sink;

    // Ordered deliberately: publish the generation, then mark a worker alive, then create the task.
    // A stop() arriving at any point after the first store supersedes this worker before it can
    // publish — and the worker learns its own generation from the context, not from this atomic, so
    // a stop() between here and task entry can no longer be adopted as ownership.
    g_generation.store(generation, std::memory_order_release);
    g_worker_alive.store(true, std::memory_order_release);

    if (xTaskCreate(worker, kTaskName, kTaskStackBytes, context, kTaskPriority, nullptr) !=
        pdPASS) {
        // Nothing will consume the context, so this call owns it — including the credential in it.
        wipe(*context);
        delete context;

        g_worker_alive.store(false, std::memory_order_release);

        // The generation still advances. Leaving it where a worker could match it would let
        // something still alive believe it is current.
        g_generation.store(lifecycle::next_generation(g_generation.load(std::memory_order_acquire)),
                           std::memory_order_release);

        diagnostics::log(diagnostics::LogLevel::Error, diagnostics::tag::kPrinter,
                         "worker not started: task creation failed");
        return report;
    }

    // Reported only once the task exists, so a caller can never arm a session for a worker that
    // was never created.
    report.started = true;
    report.generation = generation;
    return report;
}

void stop() {
    // ⛔ DELIBERATELY DOES NOT WAIT. Called from the application task, which is the store's only
    // dispatcher and the thing that services button gestures. Superseding the generation is what
    // makes that safe: from this store onward the outgoing worker cannot publish, its in-flight
    // request is abandoned at its next slice, and it exits. Nothing is joined.
    //
    // Unconditional rather than guarded on liveness: advancing the generation when no worker exists
    // is harmless, and reading liveness first would be a race for no benefit.
    g_generation.store(lifecycle::next_generation(g_generation.load(std::memory_order_acquire)),
                       std::memory_order_release);
}

bool running() {
    return g_worker_alive.load(std::memory_order_acquire);
}

} // namespace prd::prusalink
