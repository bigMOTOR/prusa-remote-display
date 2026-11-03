// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstdint>
#include <string_view>

#include "app_model/presentation.hpp"

// Product presentation (Architecture Spec §5.12).
//
// LVGL IS AN IMPLEMENTATION DETAIL OF THIS COMPONENT. No LVGL type, macro or
// header appears in this file, and none appears in any other component: ui is
// the only component that includes LVGL headers, and its single task is the only
// task that calls LVGL APIs (Architecture Spec §1 decision 6 and §10.3).
//
// The rest of the firmware therefore talks to the renderer only through the
// project-defined types below. Swapping the renderer would not change this file.
//
// The one project type that does cross this boundary is app_model::Presentation —
// the UI projection's output (Architecture Spec §9.4). §4.2 puts `ui` above
// `app_model` in the dependency direction, so consuming it here is the intended
// arrangement: the renderer is told what is presentable, in values, and decides
// everything about how it looks.

namespace prd::ui {

// --- Configuration ---------------------------------------------------------

// Draw-buffer band height in display rows.
//
// 16 rows at 320 px wide is 10,240 bytes of RGB565 per buffer. The prior
// provisional 40-row pair consumed 51,200 bytes of internal DMA RAM. A signed
// v0.0.17 physical probe reached TLS setup but could not allocate its internal
// record buffer; reducing both bands returns 30,720 bytes without moving TLS
// secrets to unencrypted PSRAM. The signed v0.0.18 bootstrap subsequently
// passed private HTTPS and rendering checks; the nine-case private OTA matrix
// also passed with this band height. Production performance remains a separate gate.
inline constexpr int kDefaultDrawBufferRows = 16;

// Two buffers, so LVGL can render the next band while the previous one is still
// being transferred. One buffer would serialise render and transfer; a full
// framebuffer is not justified by measurement (Architecture Spec §17.5).
inline constexpr int kDefaultDrawBufferCount = 2;

// PROVISIONAL, and deliberately generous: this is a starting point to be
// measured, not a budget. Phase F reports the high-water mark; see the hardware
// validation record for what it actually used.
inline constexpr std::uint32_t kDefaultTaskStackBytes = 8192;

// Medium-high per Architecture Spec §10.8: above the Milestone 1 validation
// tasks (4) so rendering is not held behind bench diagnostics, and below the
// ESP-IDF system and future Wi-Fi tasks so it cannot starve them.
inline constexpr unsigned kDefaultTaskPriority = 5;

struct Config {
    int draw_buffer_rows{kDefaultDrawBufferRows};
    int draw_buffer_count{kDefaultDrawBufferCount};
    std::uint32_t task_stack_bytes{kDefaultTaskStackBytes};
    unsigned task_priority{kDefaultTaskPriority};
};

// --- Lifecycle -------------------------------------------------------------

// Starts the renderer.
//
// PRECONDITION: display::init() has already succeeded. ui does not initialise
// the panel — the panel, its transform and its backlight belong to display
// (Architecture Spec §5.5), and ui must not reach past that boundary.
//
// Creates exactly one task. Every LVGL call in the firmware happens on it,
// including LVGL's own initialisation, so no LVGL state is ever touched by the
// caller's task. Returns once that task reports its setup outcome, so a false
// return means the renderer is not running rather than not yet running.
[[nodiscard]] bool init(const Config& config = {});

[[nodiscard]] bool is_running();

// Identity of the render task, for callers that report on it. Exposed so
// diagnostics do not restate a constant that belongs to this component.
[[nodiscard]] std::string_view task_name();
[[nodiscard]] std::uint32_t task_stack_bytes();

// --- Presentation intake ---------------------------------------------------

// Hands the renderer a resolved presentation. Callable from any task.
//
// LATEST VALUE WINS. Architecture Spec §10.7 asks for "one small UI state
// notification queue using overwrite/latest-value semantics", and that is exactly
// what this is: a one-deep queue whose contents are replaced. A presentation the
// renderer has not yet drawn is superseded rather than queued behind, because a
// stale frame has no value — §10 Interruptibility requires the interface to
// "immediately represent newly received printer state", and drawing an intermediate
// value first would delay the current one to show something already untrue.
//
// Nothing is dropped that matters. §10.7's "critical events must not be silently
// dropped" governs the application event queue, where a lost button press or OTA
// failure would lose a fact. This queue carries no facts: the store holds them all,
// and every presentation is derivable from it.
//
// A 412-byte copy per call, deliberately. The alternative is a pointer into the
// store, which would be shared mutable domain state across tasks — §10.7 forbids it,
// and it is the one thing the store's lock-free design depends on not happening.
void submit(const app_model::Presentation& presentation, std::uint32_t revision);

// Highest store revision whose resulting pixels completed a panel transfer. Product code uses
// this acknowledgement only to ensure Restarting was physically handed to the display before the
// selected image is rebooted; scene diagnostics are deliberately not consulted.
[[nodiscard]] std::uint32_t last_flushed_revision();

// --- Provisioning session intake -------------------------------------------
//
// ⛔ A SECOND INTAKE, BECAUSE THE FIRST ONE STRUCTURALLY CANNOT CARRY THIS.
//
// `Presentation` is produced by project(const AppState&). Architecture Spec §24.3 and
// §13.5 keep secrets out of AppState, so everything reachable through submit() is by
// construction not a secret — that is the property three checkpoints have been built on,
// and routing the setup password through it would end that.
//
// So the session travels its own path: app_core owns the credential and hands it here.
// The one-slot task mailbox temporarily holds a by-value copy; its backing bytes and
// the render task's local copy are wiped after handoff. The scene retains only the
// currently displayed session because LVGL must rasterise it, and scrubs it on
// retirement. The credential never reaches AppState, Presentation or a log in any
// profile (owner ruling D1).
//
// Push, not pull: `ui` must not call into `network`, which §4.2's dependency direction
// forbids. app_core is the composition root and already owns the session.

struct SetupSession {
    // Non-secret. §7.4 already documents the network name as displayable, and §14.2
    // requires the setup SSID to be shown.
    app_model::BoundedString<32> ssid;

    // SECRET. Shown on the panel and nowhere else — that is the whole reason Milestone
    // 3.6 exists, because owner ruling D1 left no other way to learn it.
    app_model::BoundedString<63> password;

    // The local setup address.
    //
    // ⚠️ NEVER POPULATED IN MILESTONE 3.6, and its row stays hidden as a result. Owner
    // ruling 2026-08-15: no URL while no server is listening, because a browser that
    // cannot reach it reads as a broken device. M3.7 sets this and the row appears under
    // the presence rule, with no change to the composition.
    app_model::BoundedString<32> url;
};

// Hands the renderer a provisioning session. Callable from any task.
//
// Latest value wins, the same one-deep overwrite semantics submit() uses.
void submit_setup_session(const SetupSession& session);

// Ends the session: the renderer overwrites its copy and the characters LVGL holds,
// rather than merely hiding the labels. A hidden label still owns its string.
void clear_setup_session();

// --- Display power ----------------------------------------------------------

// What the display-power policy wants the panel's backlight to be.
//
// ⛔ THE RENDER TASK OWNS BRIGHTNESS, AND THIS MAILBOX IS WHY. display.hpp states that the LEDC
// channel "must be owned by one task", and the render task is the one that owns the panel. The
// application task decides the POLICY and posts the result here; it never touches the driver, so
// two tasks can never drive one channel.
//
// ⛔ AND THE RAMP IS STEPPED, NOT BLOCKING. display::fade_brightness_to() blocks its caller for
// the whole duration, which would stall either the application task (halting polling, the station
// and the provisioning tick) or the render task (delaying the frame that an arriving fault needs
// to draw). Production UI §10 requires every animation to be interruptible; a blocked task cannot
// be interrupted, so the render loop interpolates instead and a new intent simply replaces the
// one in flight.
struct DisplayIntent {
    // 0-100. Zero is true off — display::set_brightness_percent(0) takes the same duty-0 path as
    // disabling the backlight, so the LED string is not driven at all.
    std::uint8_t target_percent{100};

    // Ramp duration. Zero applies the target immediately, which is what a wake wants: a fault
    // arriving on a dark panel must not fade up over two seconds before it can be read.
    std::uint32_t fade_ms{0};
};

// Posts an intent. Overwrites any intent not yet consumed — latest wins, like every other mailbox
// here. Safe from any task.
void submit_display(const DisplayIntent& intent);

// --- Bench instrumentation label -------------------------------------------

// What the owner-paced brightness harness wants written on the panel.
//
// ⛔ THE PANEL IS THE ORACLE, WHICH IS WHY THIS EXISTS AT ALL. The first brightness instrument
// advanced itself and named each step in chat; the announcements arrived batched, the owner could
// not tell which level was on screen, and the run was ruled invalid. The label has to be where
// the brightness is.
//
// Both pointers must have static storage duration — string literals. They are read by the render
// task, which never copies or frees them.
struct DiagnosticLabel {
    const char* id{""};
    const char* label{""};
};

// Posts a label. A NO-OP outside a diagnostic build: declared unconditionally so this public
// header stays free of sdkconfig, defined empty unless the diagnostic scene owns the screen. Same
// shape as validation::log_button_gesture(), which is a no-op outside its own profile.
//
// Overwrites any label not yet drawn — latest wins. Safe from any task.
void submit_diagnostic_label(const DiagnosticLabel& label);

// Presentations submitted, and how many were superseded before the render task could
// draw them. A rising `superseded` count means the renderer is slower than the
// dispatch rate, which is a measurement worth having rather than a fault: it is what
// says whether the draw-buffer size needs revisiting.
struct IntakeStatistics {
    std::uint32_t submitted{0};
    std::uint32_t superseded{0};
    std::uint32_t received{0};
    std::uint32_t rejected{0};
};

[[nodiscard]] IntakeStatistics intake_statistics();

// What the scene controller last resolved. Mirrors the internal controller report
// without exposing LVGL or the scene enumeration.
struct SceneReport {
    std::uint32_t applied{0};
    std::uint32_t redundant{0};
    std::uint32_t scene_changes{0};
    std::uint32_t operational{0};
    std::uint32_t unimplemented{0};
    std::string_view scene{};
};

[[nodiscard]] SceneReport scene_report();

// --- Diagnostic read-back --------------------------------------------------

// What the operational form was last drawn as — UI Design Decision 004 §3.
//
// DIAGNOSTIC AND READ-BACK ONLY. This is the renderer's account of what it put on the
// panel, published so that validation flows and diagnostics can state it. It is not an
// interface to the renderer and it is not a source of truth about anything.
//
// NOTHING MAY DEPEND ON THE REPORTED FIELDS. No application logic, no state
// projection, no scene selection and no product behaviour may read `energy`, `radius`,
// `centre_y`, `half_angle`, `extent`, `rotation`, `sagitta`, `midpoint_y`, `top_y`,
// `bottom_y`, `half_extent`, `accent`, `closed` or `draws_record` in order to decide
// anything. The rule covers the accent exactly as it covers the geometry: it is one
// more thing the renderer resolved and stated, not a colour anything may consult.
// The derivation runs the other way and only that way: a subject state has an energy
// (UIDD 004 §2), an energy has a shape (§3.1), and this is the far end of that chain
// reported back. A caller that branched on one of these numbers would be deriving
// state from a picture, and it would also breach UIDD 004's Scope, which confines
// operational energy to the presentation layer alone.
//
// WHY IT IS REPORTED RATHER THAN RECOMPUTED. The axis and the form derivation are
// private to this component. A caller that recomputed these figures would be printing
// a PREDICTION of what the renderer should have drawn, in a line a reviewer reads as a
// measurement. This project has twice paid for a gauge that measured something other
// than what was on the panel: a box-based telemetry gauge that reported a collision
// which did not exist, and a ring that drew nothing at all while a clean build, 350
// host tests, clang-tidy, the token guard and the flush cross-check all passed.
//
// A SNAPSHOT, taken by the caller's task from values the render task retained. Plain
// integers and no lock, exactly as SceneReport and FlushStatistics are read: a torn
// read costs one diagnostic line, and by the rule above nothing branches on it.
struct OperationalFormReport {
    // A form is on the panel. False for every state outside the Operational Family,
    // and before the first presentation has been applied.
    bool drawn{false};

    // The point on the axis the form was placed at (§2). Meaningless unless `drawn`.
    std::int32_t energy{0};

    // The circle the arc was drawn on, and the arguments LVGL was actually given.
    std::int32_t radius{0};
    std::int32_t centre_y{0};
    std::int32_t half_angle{0};
    std::int32_t extent{0};
    std::int32_t rotation{0};

    // The signed depth of the form's bowl — the axis's single parameter. Negative once
    // the form has become the resting crown.
    std::int32_t sagitta{0};

    // ⛔ THE FORM'S OWN HORIZONTAL CENTRE, AND THE WIDTH THE RELATION WAS SOLVED FOR.
    //
    // Reported because without them a physical run cannot tell whether the settled horizon was
    // drawn on the canvas centre or on the ring's — which is the exact defect the 2026-08-29
    // correction repairs, and which every other field in this struct is blind to. A photograph
    // of a wide arc and a photograph of a narrow one differ; a report that omits the centre
    // cannot say which the device believed it was drawing.
    std::int32_t centre_x{0};
    std::int32_t half_width{0};

    // The drawn arc's own extremes: where it crosses the composition's centre column,
    // its highest and lowest drawn rows, and how far it reaches either side of centre.
    std::int32_t midpoint_y{0};
    std::int32_t top_y{0};
    std::int32_t bottom_y{0};

    // The NOMINAL geometric reach: radius * sin(half_angle), with no stroke or cap allowance.
    std::int32_t half_extent{0};

    // ⛔ FOUR MEANINGS, KEPT APART BECAUSE THEY ARE NOT THE SAME NUMBER.
    //
    //   half_width               the sagitta relation's input, above; never drawn
    //   half_extent              the nominal path
    //   paint_left / paint_right the pinned renderer's own integer column bounds
    //   what the panel shows     UNVERIFIED until a photograph is taken
    //
    // The last distinction is the one worth stating: these are the columns LVGL's own arithmetic
    // resolves to, not an observation. Nothing here has been compared against a panel.
    //
    // ⬅️ AND THERE IS NO SINGLE PAINTED HALF-EXTENT. The renderer's half-pixel correction is
    // sign-dependent and an even stroke adds its correction to one side only, so the two ends
    // land on columns that are not mirror images. A symmetric figure was reported here once and
    // it was one pixel short of the true left edge at 319 of the 1001 reachable energies.
    std::int32_t nominal_left{0};
    std::int32_t nominal_right{0};
    std::int32_t paint_left{0};
    std::int32_t paint_right{0};

    // The accent token supplied to the form and any record sweep, as 0xRRGGBB (UI Design
    // Decision 004 §5.1 applies one `color.accent` to the operational form at every energy).
    //
    // Reported because a colour is the one thing a photograph cannot settle: white
    // balance, exposure and the panel's off-axis response all move it, and this project
    // has already twice mistaken an optical artefact for something the firmware drew.
    // This is the value handed to LVGL, so a frame can be checked against it rather than
    // against a recollection of what the rule ought to produce.
    std::uint32_t accent{0};

    // The shape is a complete cycle — §3.1's maximum-energy form.
    bool closed{false};

    // The sweep is carrying the job's record (§3.2). False does not mean the record is
    // absent: where the shape cannot host it, it survives as the numeric value.
    bool draws_record{false};

    // How the form was painted, named rather than inferred — see scene::FormTreatment and the
    // §5.1 ruling of 2026-08-14 recorded there.
    //
    // Reported because "the same rule produced all of these" is otherwise a claim rather than a
    // measurement: with the treatment on the read-back, three frames can be checked against one
    // rule instead of against three photographs. `0` is `None`, mirroring `drawn == false`.
    std::uint8_t treatment{0};

    // The form-accent → cycle-track/record blend actually handed to the renderer. Zero is the
    // stable form treatment. 255 is stable only once `closed` is also true: interpolation can
    // round to 255 one tick before geometry closes, and that frame still uses the cycle treatment
    // rather than flashing the stable FormAccent. Intermediate values qualify `treatment` too.
    std::uint8_t cycle_treatment_alpha{0};
};

[[nodiscard]] OperationalFormReport operational_form_report();

// Whether every generated face has uniform digit advance widths, measured at
// start-up (Production UI Specification §4's tabular-figure requirement). False means
// the renderer is running with proportional figures and telemetry will shift as it
// counts.
[[nodiscard]] bool tabular_figures_verified();

// --- Measurement -----------------------------------------------------------

// Pixel-transfer bookkeeping for the LVGL flush path, as seen by ui. It is
// deliberately separate from display::transfer_statistics(): that counts what
// the panel did, this counts what LVGL asked for, and comparing the two is what
// proves no flush completion was invented or lost.
//
// Every failure counter must stay at zero. `spurious_signals` non-zero means a
// completion arrived with no flush outstanding; `recovered_flushes` non-zero means
// a completion signal was lost and the deadline guard released the buffer instead;
// `abandoned_flushes` non-zero means a transfer never completed at all.
//
// `waits_blocked` is not a failure. It counts how often the renderer had to wait
// for a transfer before it could submit the next band, which is the measurement
// that says whether rendering or the SPI bus is the slower half.
struct FlushStatistics {
    std::uint32_t submitted{0};
    std::uint32_t completed{0};
    std::uint32_t waits_blocked{0};
    std::uint32_t submit_failures{0};
    std::uint32_t spurious_signals{0};
    std::uint32_t recovered_flushes{0};
    std::uint32_t deadline_misses{0};
    std::uint32_t abandoned_flushes{0};

    // Wall-clock from flush submission to completion, i.e. one band's DMA
    // transfer as observed by the renderer. Measured from after the byte swap, so
    // it is transfer time and nothing else.
    std::uint32_t duration_last_us{0};
    std::uint32_t duration_min_us{0};
    std::uint32_t duration_max_us{0};
    std::uint64_t duration_total_us{0};

    // CPU spent converting each band to the panel's byte order, accumulated. This
    // is the cost of the endianness conversion LVGL requires for RGB565, and it is
    // reported rather than assumed because the alternative was rejected on the
    // basis of it.
    std::uint64_t swap_total_us{0};
};

// What was actually allocated, not what was requested: `internal_dma` reports
// the placement achieved, so a silent fall back to PSRAM cannot be mistaken for
// the intended internal-RAM placement.
struct DrawBufferReport {
    std::uint32_t bytes_each{0};
    int rows{0};
    int count{0};
    bool internal_dma{false};
};

// LVGL's own heap, reported without LVGL types. LVGL is configured with its
// built-in allocator over a fixed pool, so this is a closed accounting system:
// a rising `used_bytes` with a static scene is a leak, not memory pressure.
struct MemoryReport {
    std::uint32_t lvgl_total_bytes{0};
    std::uint32_t lvgl_free_bytes{0};
    std::uint32_t lvgl_used_bytes{0};
    std::uint32_t lvgl_max_used_bytes{0};
    std::uint8_t lvgl_fragmentation_percent{0};
};

[[nodiscard]] FlushStatistics flush_statistics();
[[nodiscard]] DrawBufferReport draw_buffers();

// Last sample taken by the render task. LVGL's allocator may only be inspected
// from the task that owns LVGL, so this returns a snapshot rather than reading
// LVGL live — sampling it from the caller's task would break the single-owner
// rule this component exists to enforce.
[[nodiscard]] MemoryReport memory_report();

// Renderer version, for the boot report and the validation record.
[[nodiscard]] std::string_view renderer_version();

} // namespace prd::ui
