<!-- SPDX-FileCopyrightText: 2026 Mykola Fiantsev -->
<!-- SPDX-License-Identifier: CC-BY-4.0 -->

# ADR-0005: Integrate LVGL 9.5.0 directly, owned by a single ui task

## Status

Accepted

## Date

2026-08-04

## Context

Milestone 1 Phase F adds the rendering foundation the accepted Production UI
Specification will be built on in Milestone 2. Before Phase F the firmware drove
the panel only through the `display` component's own pixel-transfer API, which is
enough for validation patterns and nothing more.

The accepted sources constrain this heavily:

- Architecture Spec §1 decision 6 and §10.3: **only the UI task may call LVGL**.
- Architecture Spec §5.5: `display` owns esp_lcd; nothing else may touch it.
- Architecture Spec §5.12: `ui` owns product presentation.
- Architecture Spec §17.5: partial draw buffers sized after profiling; a full
  framebuffer only if measurement justifies it.
- Bootstrap Spec §7.7: pin exact versions, no floating Git branches, commit
  `dependencies.lock`.
- Owner instruction, 2026-08-04: LVGL is an implementation detail; the rest of the
  firmware communicates only through project-defined abstractions.

The pre-Phase F hardening batch had already established the mechanism a renderer
needs: `display::blit()` with an explicit buffer-ownership contract, and
`display::set_transfer_complete_callback()` firing in interrupt context when a
transfer completes.

The ADR numbering starts at 0005 because ADR-0001 to ADR-0004 are reserved by
Bootstrap Spec §18 for the Milestone 0 quality-tooling batch, which is a separate
approved task.

## Decision

### 2026-09-13 addendum: exact mask backport and local callback compatibility

The owner approved extending the generated, shared LVGL dependency with a narrow
callback-signature repair. LVGL remains 9.5.0; managed components are not edited.
`firmware/tools/prepare-lvgl.py` applies two distinctly attributed transformations:

1. Existing upstream `f9c059715db7cc7c3aeedd77f6c88437f6a22c49` circular-mask fix.
2. Local compatibility repair for the five software-mask callbacks. Their final
   argument now matches the callback typedef's `void *`; each function immediately
   converts it to its original typed pointer. Incompatible function-pointer casts
   are removed. No rendering arithmetic, layout, allocation or timing is changed.

The archive, original mask, intermediate upstream-backport result and final result
are independently hash-pinned. Each local signature/assignment replacement must
occur exactly once. Every prepared file is still verified against its expected
contents, and an unexpected or stale generated tree is refused, not overwritten.
Firmware and native tests consume the same generated dependency.

For an expected stale generated tree after this approved upgrade, move the exact
`firmware/build/dependencies/lvgl__lvgl` directory aside, preserving it for inspection,
then rerun `python3 firmware/tools/prepare-lvgl.py`. Do not delete or alter
`managed_components`, weaken verification, or reuse an unexpected modified tree.

Linux Clang 18.1.3 with ASan/UBSan exposed the incompatible indirect calls. Separate
tests of line, angle, radius, fade and map each fail on that exact diagnostic before
the repair and pass afterward, without sanitizer suppression. This is evidence of
C undefined behavior, not evidence of an exploitable vulnerability or an observed
ESP32 crash caused by this particular defect. Full delivery verification is tracked
in the review evidence, not implied by this architectural record.

An upstream PR is being prepared separately; this local repair must not be labelled
an accepted upstream fix. A future upstream replacement requires normal upgrade
review. This addendum does not change task ownership, pool size or draw buffers.

### 2026-09-25 addendum: wipe temporary QR credential copies

The private beta security repair uses the same prepared LVGL 9.5.0 tree.
`prepare-lvgl.py` now applies a third, separately hash-pinned transformation
to `src/libs/qrcode/lv_qrcode.c`: both temporary QR buffers are wiped through
volatile byte stores before `lv_free`, on successful and failed encoding. This
changes neither the QR algorithm nor the displayed pixels. The pinned archive,
original QR file and patched QR file are checked independently. Native tests
inspect the buffers immediately before release, and an old-code control proves
the assertion catches the missing wipe.

The full prepared tree is still verified file by file. One narrow in-place
upgrade is allowed: if an existing generated QR file matches the pinned,
unmodified upstream file exactly, the preparation step replaces that file with
the pinned security patch. Any other modification or stale content is refused.
The earlier mask/callback transformation and the rule against silently
overwriting unexpected generated files remain unchanged. No `managed_components`
source is edited. Firmware and native tests continue to consume the same tree.

### Version: LVGL 9.5.0, pinned exactly

Declared as `lvgl/lvgl: "==9.5.0"` in `firmware/components/ui/idf_component.yml`,
resolved into `firmware/dependencies.lock` with component hash
`184e532558c1c45f…`.

9.x is the current major line; 8.x is legacy and its display API
(`lv_disp_drv_t`, `LV_COLOR_16_SWAP`) is gone in 9. Within 9.x, 9.5.0 was the
newest release at the time of this decision (published 2026-02-18, roughly five
and a half months of field time) and carries every accumulated fix in the line.
LVGL does not publish patch releases for 9.x minors, so waiting for a "9.5.1"
would mean waiting indefinitely.

The pin is exact rather than a range. A range would let a future clean build
resolve to a renderer nobody validated on this panel, which is precisely the
failure Bootstrap Spec §7.7 exists to prevent.

### Direct integration, not `esp_lvgl_port`

Espressif's `esp_lvgl_port` (2.8.0) was rejected. It is incompatible with the
accepted architecture in three concrete ways, each verified against its source
rather than assumed:

1. **It requires esp_lcd handles.** `lvgl_port_display_cfg_t` takes
   `esp_lcd_panel_io_handle_t` and `esp_lcd_panel_handle_t` directly. Using it
   would force `display` to publish its esp_lcd handles, breaking the §5.5 rule
   that no esp_lcd type leaves the component — the boundary the whole display
   component exists to draw.
2. **It registers its own completion callback.** `esp_lvgl_port_disp.c` calls
   `esp_lcd_panel_io_register_event_callbacks()`. esp_lcd holds one callback set
   per panel IO, so this would silently overwrite the callback the hardening batch
   registered, destroying the buffer-ownership accounting that proves no buffer is
   reused under DMA.
3. **It owns the task and offers a lock.** It creates its own LVGL task and
   exposes `lvgl_port_lock()`, i.e. a multi-task-with-mutex model. The accepted
   architecture chose the opposite: one owning task and no LVGL locking at all,
   specifically to avoid cross-task object lifetime bugs (§10.3).

Direct integration costs roughly 150 lines in `ui/src/lvgl_panel.cpp`. That is
cheaper than working around a port layer that disagrees with the architecture on
every point that matters.

### Task ownership: exactly one, created by ui

One task, `prd_ui`, priority 5, created by `ui::init()`. It is the only task in
the firmware that calls any LVGL function — **including `lv_init()` itself**,
which is why `ui::init()` creates the task and then waits for it to report its
setup outcome rather than initialising LVGL on the caller's task. Initialising
LVGL from app_core's task and rendering from another would already be two tasks
touching LVGL.

The rule is enforced by the build as well as by convention: `lvgl` is a *private*
requirement of `ui` alone, so no other component can reach an LVGL header even
through a transitive dependency, and `ui`'s public header names no LVGL type.

Priority 5 follows §10.8's relative policy — above the Milestone 1 validation
tasks (4) so rendering is not held behind bench diagnostics, below ESP-IDF system
and future Wi-Fi tasks so it cannot starve them.

### Draw buffers: two partial bands, 320×40, internal DMA RAM

`LV_DISPLAY_RENDER_MODE_PARTIAL` with two buffers of 320×40 px RGB565 — 25,600
bytes each, 51,200 total.

Two buffers rather than one so LVGL can render the next band while the previous
one is still being transferred. Partial rather than a full framebuffer because
§17.5 requires measurement before spending 150 kB on one, and the product is
mostly static (§17.6).

Placement is internal DMA-capable RAM via
`heap_caps_malloc(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL)`. `MALLOC_CAP_INTERNAL`
is explicit, not decorative: the board enables `CONFIG_SPIRAM_USE_MALLOC` with a
16 kB always-internal threshold, so a plain allocation of this size would be
served from PSRAM. The achieved placement is verified with `esp_ptr_internal()`
and `esp_ptr_dma_capable()` and reported, so a silent fallback cannot be mistaken
for success.

40 rows is **provisional**. It matches the band height the display component's
own transfers already use, so the SPI transaction size is one the bus is known to
carry. Measured flush duration and cadence are in the hardware validation record.
Resizing belongs to Milestone 2, when real scenes exist to profile.

### Transfer completion: ISR notifies, task calls `lv_display_flush_ready()`

The flush callback runs on `prd_ui` (LVGL calls it from `lv_timer_handler()`) and
submits the band with the asynchronous `display::blit()`. It does **not** wait.

`display`'s completion callback fires in interrupt context and does exactly one
thing: `vTaskNotifyGiveFromISR(prd_ui)`. The render task then calls
`lv_display_flush_ready()` from task context.

In LVGL 9.5 `lv_display_flush_ready()` is a single flag store and would very
likely be ISR-safe. It is still not called from the ISR, for two reasons: LVGL
documents no ISR-safety guarantee for any of its API, and an earlier 9.x release
did more work in that function. The handoff costs one context switch per band —
measured against a ~5 ms transfer, that is noise — and buys a rule that does not
depend on reading LVGL's implementation and re-reading it at every upgrade.

The path cannot race. `flush_outstanding` is written only by the render task; the
ISR touches no LVGL state and no flush state. A completion arriving with no flush
outstanding is counted as spurious and deliberately ignored, because calling
`flush_ready()` then would release a buffer LVGL is currently drawing into. A
flush outstanding past a 500 ms deadline is treated as a lost notification: the
guard asks `display::transfers_in_flight()` first and only releases the buffer if
the transfer really has finished, so a wedged bus produces a reported fault rather
than a corrupted transfer. Both counters must read zero for a run to pass.

### Colour format: `LV_COLOR_FORMAT_RGB565`, swapped in the flush callback

This panel expects big-endian colour data — established in Phase C, which is why
the display component's validated pattern generator byte-swaps at construction.

The renderer is therefore set to plain `LV_COLOR_FORMAT_RGB565`, and
`lv_draw_sw_rgb565_swap()` converts each band in place in the flush callback,
before the buffer is lent to DMA. That window is the only safe one: LVGL has
finished rendering the band and DMA does not own it yet.

`LV_COLOR_FORMAT_RGB565_SWAPPED` was tried first, on the assumption that letting
LVGL render directly in the panel's byte order would avoid the extra pass. **It
does not work as a display colour format and was corrected after seeing it on
hardware.** `lv_display_set_color_format()` documents exactly four valid values —
RGB565, RGB888, XRGB8888, ARGB8888 — and its note directs callers to
`lv_draw_sw_rgb565_swap()` in the flush callback for endianness.
`RGB565_SWAPPED` exists as an image and blend format, not a display format.
Setting it produced a screen where every grey and white rendered blue and the
near-black background rendered navy, while the accent orange looked roughly
correct — a failure mode invisible in every counter and log line, and caught only
by looking at the panel.

The measured cost of doing it the documented way is **62 µs mean per flush** over
mixed band sizes, and about 390 µs for a full 40-row band against a 5.4 ms
transfer — roughly 7%. The original cost objection was not worth the correctness.

This keeps `display::blit()` receiving exactly the pixel format it was validated
with, and required no change to the display component.

### Memory placement: LVGL's own pool, fixed at 64 kB

`LV_USE_BUILTIN_MALLOC` with `LV_MEM_SIZE_KILOBYTES=64`, statically reserved in
`.bss` (internal RAM).

Chosen over `LV_USE_CLIB_MALLOC` because a fixed pool is a closed accounting
system: `lv_mem_monitor()` then reports used, peak and fragmentation exactly, so a
rising figure under a static scene is a leak rather than ambient system pressure.
It also cannot fail because something else exhausted the system heap. 64 kB is
LVGL's default and is **provisional** — Milestone 1 has one scene to size it
against.

LVGL objects are CPU-accessed only, never DMA, so PSRAM would be viable for the
pool if Milestone 2 needs the internal RAM back. Only the draw buffers have a hard
internal-RAM requirement.

#### 2026-09-24 private OTA memory diagnostic (not a production decision)

A signed hardware-validation bootstrap connected to Wi-Fi but left too little
internal RAM for the OTA and button task stacks. For a local diagnostic, the
PSRAM board now reserves the same 64 kB builtin TLSF pool once, before
`lv_init()`, using an explicit PSRAM-only allocation. A failed or non-PSRAM
reservation stops UI setup; LVGL's pinned one-pool callback rejects an absent
or changed-size reservation. This retains the fixed-size allocator and
`lv_mem_monitor()` accounting, but gives up the original compile-time `.bss`
reservation guarantee. The two DMA draw buffers remained internal (their
320×40 bands were reduced to 320×16 in the follow-up below). No global
ESP-IDF external-BSS option is enabled: that would cross
the Wi-Fi event channel's atomic-placement guard. The target link map must show
that LVGL no longer reserves its 64 kB `work_mem_int` in internal `.bss`.
At this point physical boot, rendering, network, button and OTA headroom were
still required before proposing it for `main`. The signed v0.0.18 bootstrap
and subsequent nine-case private OTA matrix supplied that evidence; official
production-channel validation remains separate.

#### 2026-09-24 follow-up: internal TLS heap gate (subsequently validated privately)

Signed v0.0.17 started all services, but the first private HTTPS release check failed
inside `mbedtls_ssl_setup()` with `-0x7F00` (allocation failure). Just before the
attempt, internal heap free/largest was about 21.8/10.2 kB while the TLS input
record limit remained 16 kB. The OTA worker still had 4,268 bytes of stack
low-water headroom. This is not evidence of a bad certificate or server route.

Keep mbedTLS on internal-only allocation and retain the full TLS input record
limit. For a new diagnostic, reduce each of the two internal DMA draw buffers
from 320×40 to 320×16 pixels, releasing 30,720 bytes of internal RAM while
preserving double buffering and the LVGL pool. Host render tests use the same
default row count rather than a copied constant. Signed v0.0.18 subsequently
passed physical rendering, DMA and private HTTPS discovery; the nine-case
private OTA matrix then passed on the same memory configuration. Production
GitHub delivery and broader release readiness remain separate gates.

### Tick source: the project monotonic clock, via `lv_tick_set_cb()`

`lv_tick_set_cb()` with a callback returning milliseconds from
`platform::monotonic_time()`, rather than a periodic timer calling
`lv_tick_inc()`.

There is then no second timer to keep in step with, no tick drift if the render
task is ever delayed, and no ESP-IDF timer API call inside `ui` — the clock
already has a project abstraction and this uses it.

### Configuration through Kconfig, not `lv_conf.h`

Under ESP-IDF, LVGL maps `CONFIG_LV_*` onto its own options. Renderer settings
therefore live in `sdkconfig.defaults` and are visible in the generated
`sdkconfig` and in any config diff — the same reason the project's own validation
flags are Kconfig rather than environment variables. Only settings that must not
drift are pinned; anything left unset deliberately keeps LVGL's default.

Two of LVGL's defaults had to be overridden: `LV_BUILD_EXAMPLES` and
`LV_BUILD_DEMOS` both default to `y` and would otherwise compile LVGL's entire
example and demo set into every image.

## Consequences

**Good**

- No LVGL type appears outside `ui`. Nothing else in the firmware knows a
  renderer exists; swapping it would touch one component.
- `display` needed no change at all. The seam the hardening batch built was the
  right seam, and its buffer-ownership contract now has a real consumer.
- Two independent views of the same transfers exist — `ui::flush_statistics()`
  counts what LVGL asked for, `display::transfer_statistics()` counts what the
  panel did — so agreement between them is evidence rather than a tautology.
- Debug and release images contain no LVGL at all. Nothing references `ui::init()`
  in those profiles, so the linker drops the renderer: both image sizes are
  byte-identical to their pre-Phase F values.

**Costs and risks accepted**

- The LVGL foundation adds roughly 419 kB to the hardware-validation image
  (0x85a90 → 0xec3b0). The OTA partition is 6.8 MB, so this is 14% of a slot, but
  it is a large jump. Trimming unused widgets is a Milestone 2 exercise once the
  scenes say which are unused; guessing now would be optimisation before
  requirements.
- Direct integration means the project owns the flush path. When LVGL changes its
  display API, this code is ours to update. That is the intended trade for not
  inheriting a port layer's architecture.
- LVGL's assert handler behaviour in release is unexamined. Its asserts default to
  enabled, and the default handler loops rather than panicking, which on the bench
  is caught by the task watchdog but is not a considered release policy. This is
  the same class of open question as the existing `ESP_TASK_WDT_PANIC` item and is
  recorded with it, not resolved here.
- Product typography is not addressed. The validation scene uses LVGL's built-in
  Montserrat. Production UI Specification §4 requires Inter with tabular figures,
  which means generated LVGL fonts with `tnum` applied at conversion time —
  Milestone 2 work, and the one part of the accepted specification the foundation
  cannot yet satisfy.

## Alternatives Considered

**`esp_lvgl_port`.** Rejected — see above. Three concrete incompatibilities with
the accepted architecture, not a matter of taste.

**LVGL 8.4.0.** Rejected. It is the legacy line, its display API is gone in 9, and
adopting it would mean a forced migration during Milestone 2 UI work.

**`lv_conf.h` instead of Kconfig.** Rejected. A checked-in `lv_conf.h` is
invisible to the sdkconfig diff, and profile-specific renderer settings (the 48 px
validation font, for instance) would need `#ifdef` gymnastics inside it.

**Calling `lv_display_flush_ready()` from the ISR.** Rejected. Correct today by
inspection of LVGL's implementation, unguaranteed by its documentation, and it
would have to be re-verified at every version bump. The task handoff costs a
context switch and removes the question.

**`LV_COLOR_FORMAT_RGB565_SWAPPED` as the display colour format.** Tried, and
rejected on hardware evidence rather than on principle — it renders wrong colours,
as described above. It is not a supported display format.

**One draw buffer.** Rejected. It serialises render and transfer for a 25 kB
saving.

**A full 320×240 framebuffer (150 kB).** Rejected. §17.5 requires measurement
first, and the product is mostly static.

**LVGL software rotation.** Not needed. The panel transform is already done in
hardware (`swap_xy`, `mirror_x`), so LVGL sees a native 320×240 landscape display
at `LV_DISPLAY_ROTATION_0`. Software rotation with partial buffers is expensive
and would duplicate a transform that is already correct and validated.

## Validation / Follow-up

Validated on the Waveshare ESP32-S3-LCD-2.8 (SKU 30223) — orientation, clipping,
anti-aliasing, colour reproduction, flush accounting, stack and heap
measurements, and a soak. Results, including every measured value that this
decision calls provisional, are recorded in
`hardware/validation/waveshare-esp32-s3-lcd-2.8.md`.

Open for Milestone 2, in the order they will be forced:

1. Inter fonts with tabular figures, replacing Montserrat.
2. Draw-buffer size and LVGL pool size re-profiled against real scenes.
3. Image size: trim widgets the accepted scenes do not use.
4. LVGL assert-handler policy for release, alongside the existing task-watchdog
   policy question.
