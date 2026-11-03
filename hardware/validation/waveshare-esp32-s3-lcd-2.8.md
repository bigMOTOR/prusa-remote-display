<!-- SPDX-FileCopyrightText: 2026 Mykola Fiantsev -->
<!-- SPDX-License-Identifier: CC-BY-4.0 -->

# Hardware Validation Record — Waveshare ESP32-S3-LCD-2.8 (non-touch)

Reference target: `waveshare_esp32_s3_lcd_2_8`

This record tracks every provisional board-specific value from Firmware
Repository Bootstrap Specification v0.3 §17 until it is confirmed or
disproved on the physical unit. A value copied from vendor documentation but
not yet exercised on the purchased unit remains **Unvalidated** (Bootstrap
Spec §2). Disproved values are corrected in
`firmware/sdkconfig.defaults.waveshare_esp32_s3_lcd_2_8` or
`firmware/components/board/targets/waveshare_esp32_s3_lcd_2_8/` in the same
change that records the finding here.

## Status

**Milestone 1 Phase B (Bootstrap Spec §19.8 first hardware bring-up)
completed 2026-08-02 — all criteria passed.**

**Phase C0 (schematic pin mapping) completed 2026-08-02** — pin allocation is
confirmed by the official schematic; see "Pin allocation" below.

**Phase C (display bring-up) completed 2026-08-02** — the ST7789 panel
initialises and renders over SPI2 at 40 MHz in landscape 320×240; orientation,
offsets, clipping, colour order, byte order, inversion and pixel alignment are
all physically validated, and repeated initialisation passed 23/23. Backlight
on/off and its active-high polarity are validated.

**Phase C1 (vendor panel tuning A/B) completed 2026-08-02** — gamma-only
tuning adopted as this board's default initialisation; the vendor analogue
block rejected. See "Accepted panel initialisation for this board".

**Phase D (backlight PWM and brightness) completed 2026-08-03** — LEDC PWM on
IO5 at 5 kHz with 12-bit duty resolution; true full-off at duty 0, monotonic
perceptual curve, 1 % lowest visible level, no flicker, banding, audible noise
or thermal rise at any swept frequency.

**Phase E (button bring-up) completed 2026-08-03** — BOOT / IO0 validated as an
application input by owner decision: 31 gestures classified with no duplicate,
missed or false event, no queue drops, and no measurable contact bounce.

**Pre-Phase F hardening completed 2026-08-04** — explicit draw-buffer ownership
across DMA, a registered esp_lcd completion callback, and stack high-water
measurement for every project-owned task.

**Phase F (LVGL foundation) completed 2026-08-04** — LVGL 9.5.0 renders to the
panel through the asynchronous transfer contract, with two 320×40 RGB565 draw
buffers in verified internal DMA RAM and a 31.9-minute soak at 20 redraws/s.

**Milestone 1 closure (Phase G) verified 2026-08-04** — the committed tree
(`c6b574e`, `dirty=false`) was re-flashed and re-run; every figure below
reproduced.

**Milestone 2 Phase B (application core on hardware) completed 2026-08-05** —
the application store, reducer, projection, bounded event queue and application
task run on the target; the 21-fixture catalogue resolves identically on the
device and on the host (21/21, checked mechanically); all nine scenarios apply
exactly their host-asserted step counts and the sweep repeats with monotonically
rising observation generations; the button drives the three bench controls
through the real gesture pipeline. Evidence:
[phase-b-application-core](evidence/2026-08-05-phase-b-application-core.log).

Six defects were found by running it, five of which no host test or static check
could have caught — all fixed and re-verified:

1. **`main` task stack overflow.** `reduce()` is a pure function over a 2 kB
   `AppState`, so one dispatch puts two copies plus an `Event` on the caller's
   stack — most of the ESP-IDF main task's 3584 bytes. The boot sequence now runs
   on the application task, which also makes the single-dispatcher rule of
   Architecture Spec §10.2 unconditional.
2. **`button::init()` called from the main task.** Milestone 1 ran it on a
   dedicated 6144-byte task for exactly this reason; it now initialises itself on
   its own task again.
3. **`ipc0` / `ipc1` task stack overflow.** Installing the GPIO ISR service with
   `ESP_INTR_FLAG_IRAM` runs work on the other core's IPC task, whose 1280-byte
   stack cannot take it with the interrupt allocator's logging on top.
   `CONFIG_ESP_IPC_TASK_STACK_SIZE` raised to 2560 in the base defaults — not in
   the verbose profile, because a stack that only survives when logging is quiet
   is not sized. **Milestone 1 installed the same service from an unpinned task
   and passed 25 consecutive boots: it had this latent throughout.**
4. **Button task core affinity.** Whether the IPC hop happens at all depended on
   which core the scheduler placed the task on, so the same image booted or died
   on luck. Pinned to core 0 — the documented core-affinity decision §10.9 allows
   when measurement shows a need.
5. **Application task stack.** Sized at 8192 by argument; measured peak is 7996
   bytes, which is 98 % of it. Raised to 12288, now at 35 % headroom.
6. **Pause not honoured at a scenario boundary.** The validation flow checked
   `paused()` before advancing but not before auto-loading the next scenario, and
   loading clears the pause flag — so from the bench the pause/resume toggle
   silently did not toggle. Found only by pressing the button at the moment a
   scenario ended. The pre-commit review then moved the whole sweep policy out of the
   flow into `FakeSource::sweep()`, where a host test covers it and has been verified
   to fail without the fix.

**Re-validated after that refactor**, and the figures are the point: the cumulative
`steps_applied` and `rev` values are identical to the pre-refactor run
(`steps_applied=118 rev=98` at the end of the first pass), so moving the policy
changed its owner and not its behaviour. Twelve gestures across the session with
`raw_dropped=0 events_dropped=0`; the boundary pause now reports `paused=yes` then
`paused=no`, with the next scenario loading only after the resume. No fault, no
panic, and no `dispatch from a task other than prd_app` in roughly 21 minutes of
running across two captures.

Measured task stacks after the fixes: `main` 1668/3584 (53 % headroom),
`prd_app` 7868/12288 (35 %), `prd_button_in` 3268/6144 (46 %). Zero faults,
zero dropped edges and zero dropped events across the validation runs.

## Unit identity

| Field | Value |
|---|---|
| Product | Waveshare ESP32-S3-LCD-2.8 (non-touch variant) |
| SKU | 30223 (box label) |
| PCB silkscreen | `ESP32-S3-T…-LCD-2.8` / `240×320 Pixels` (shared-platform PCB — see note 1) |
| Visible markings | QC sticker "16"; white label "2" on reverse side of board area |
| MCU | ESP32-S3 (QFN56), revision v0.2, WiFi + BLE, 40 MHz crystal |
| Module type | Bare chip on board (no shielded module can), embedded 8 MB PSRAM → ESP32-S3R8 |
| USB VID/PID | `0x303A` / `0x1001`, product string "USB JTAG_serial debug unit" |
| Chip MAC | redacted (device identifier — see evidence policy) |
| Host port (macOS) | `/dev/cu.usbmodem2101` |

### Note 1 — variant identity (resolves former open flag)

The purchased unit is the **non-touch** variant, as required by Prototype
Hardware Selection v0.1. The earlier apparent conflict — vendor documentation
for SKU 30223 describing capacitive touch, speaker, IMU, RTC, TF card and
battery — is explained by the product being **one PCB platform sold as a
single listing with two "Version Options"**:

- `ESP32-S3-LCD-2.8` — "without touch control" (SKU 30223, this unit)
- `ESP32-S3-Touch-LCD-2.8` — "with touch control"

Consequences, all confirmed on this unit:

- the touch FPC connector next to the LCD ribbon is **present but empty**;
- the panel has a bare polarizer with no bonded touch lens and no second ribbon;
- SKU 30223's official resources list **no touch-controller datasheet**
  (only ST7789T3 and QMI8658);
- the PCB platform and schematic are shared with the touch variant, and the
  schematic is filed under the touch sibling's name — **touch-related
  circuitry is not populated on SKU 30223**.

Vendor prose for this SKU must not be treated as a feature list for the
non-touch variant. The schematic and the physical board are authoritative.

### Peripherals present on the shared platform

Not required, not initialized, and all outside MVP scope (Architecture Spec
§2.2), but they occupy GPIOs and must be accounted for during Phase C–E pin
assignment: PCM5101A audio DAC + APA2068 amplifier + volume potentiometer +
speaker header; QMI8658 6-axis IMU; PCF85063 RTC + RTC battery header;
microSD (TF) slot; battery header, charger, charge LED and a **BAT_PWR
button**; ceramic antenna with IPEX alternative; 12-pin header
(IO15, IO18, RXD, TXD, SDA, SCL, 3V3, GND, D+, D−, VBus, GND); separate I2C
and UART headers; BOOT and RESET buttons; power LED.

## Reference documents

| Document | Location | Integrity |
|---|---|---|
| Schematic (shared platform) | `files.waveshare.com/wiki/ESP32-S3-LCD-2.8/ESP32-S3-Touch-LCD-2.8.pdf` | SHA-256 `837c1bd1abeab2bcc12575177ffb9c41ed58e82dfe3fc96d3fed8a21cdd73ace` (233,721 bytes) |
| Display controller datasheet | ST7789T3 v1.0, linked from SKU 30223 resources page | SHA-256 `7a91ce95b3f743111b975f9189c97d99a8c6bcaa0ed026dcaaf6c2979adf9d17` (6,123,258 bytes). Exercised in Phase C1: §9.2.10, §9.2.21, §9.2.24 and §9.2.25 were used to decode the vendor initialisation sequence |

The schematic is vendor-copyrighted and is **not redistributed in this
repository**; it is cited by URL and checksum. A local copy is kept outside
the repository for development use.

## Provisional values (Bootstrap Spec §17)

| Value | Provisional assumption | Status | Evidence |
|---|---|---|---|
| MCU/module | ESP32-S3R8 | **Confirmed** | ESP32-S3 (QFN56) rev v0.2, "Embedded PSRAM 8MB (AP_3v3)" — [chip-identity](evidence/2026-08-02-chip-identity.log), [first-boot](evidence/2026-08-02-first-boot.log) |
| Flash capacity | 16 MB | **Confirmed** | JEDEC mfr `0x20` dev `0x4018`; esptool "Detected flash size: 16MB"; bootloader "SPI Flash Size : 16MB"; firmware `flash_detected=16777216 expected=16777216 result=pass`; full 16 MB read showed no address aliasing (see note 2) |
| Flash mode | QIO | **Confirmed** | eFuse "quad (4 data lines)"; bootloader `qio_mode: Enabling default flash chip QIO`, `SPI Mode : QIO`; app `spi_flash: flash io: qio` (see note 3) |
| Flash frequency | 80 MHz | **Confirmed** | bootloader "Boot SPI Speed : 80MHz"; stable through 31.8-min soak and 25 boots |
| PSRAM capacity | 8 MB | **Confirmed** | `octal_psram: density : 0x03 (64 Mbit)`; `esp_psram: Found 8MB PSRAM device`; `Adding pool of 8192K`; firmware `psram_detected=8388608 expected=8388608 result=pass` |
| PSRAM type | Octal | **Confirmed** | `octal_psram` driver path, vendor id `0x0d (AP)`, dev id `0x02`, VCC `3V`; `CONFIG_SPIRAM_MODE_OCT=y` initializes successfully |
| PSRAM frequency | 80 MHz | **Confirmed** | `esp_psram: Speed: 80MHz`; MSPI timing tuning succeeded (best index 5); `SPI SRAM memory test OK` |
| USB path | native USB Serial/JTAG over USB-C | **Confirmed** | esptool "USB mode: USB-Serial/JTAG"; no bridge IC present in USB tree; flashing and logging both work over the exposed USB-C port |
| USB VID/PID | unknown | **Confirmed** | `0x303A` / `0x1001`, "USB JTAG_serial debug unit" (Espressif) |
| Required BOOT sequence | unknown | **Confirmed — none required** | esptool auto-reset connects, flashes and resets with no manual BOOT/RESET press across all operations |
| USB driver requirement | unknown | **Confirmed — none** | native CDC enumerates on macOS with no vendor driver installed |
| USB power stability | expected | **Confirmed** | 31.8 min continuous soak on USB-C bus power, 0 serial link drops, 0 resets — [stability-soak](evidence/2026-08-02-stability-soak.log) |
| Display controller | ST7789-family | **Confirmed by physical validation** as ST7789-compatible | esp_lcd's ST7789 init sequence succeeds and the panel renders correctly. The exact **T3** sub-variant remains documentation-only (vendor datasheet link); the controller is on the panel FPC, not in the board schematic |
| Display interface | SPI | **Confirmed by schematic + physical validation** | connector L4 pinout is RST / SCK / RS(D/C) / CS / SDA / SDO; 4-wire SPI drives the panel successfully |
| Native display resolution | 240×320 | **Confirmed by physical validation** | full 240×320 addressable area exercised; the 1 px test border is intact on all four edges |
| Landscape transform | rotation/offset unknown | **Confirmed by physical validation** | `swap_xy=on, mirror_x=on, mirror_y=off` yields 320×240 landscape with the panel ribbon exiting **right**; corner markers land in the correct logical corners |
| Display offsets / clipping | unknown | **Confirmed by physical validation** | `gap 0,0` correct — no offset and nothing clipped; border visible on every edge |
| LCD SPI host | unknown | **Not a board fact — firmware choice: SPI2 (FSPI)** | SPI0/SPI1 are reserved for flash and octal PSRAM; no LCD pin is a dedicated FSPI pad, so signals route via the GPIO matrix (`spi: SPI2 use gpio matrix`) and SPI3 stays free |
| LCD SCLK | unknown | **Confirmed by schematic + physical validation** | `LCD_SCK` = **IO40** → L4.3 |
| LCD MOSI | unknown | **Confirmed by schematic + physical validation** | `LCD_MOSI` = **IO45** → L4.6 (strapping pin) |
| LCD MISO | unknown | **Confirmed by schematic; deliberately unused** | `LCD_MISO` = **IO46** → L4.7. The panel is driven write-only, so MISO is left unconfigured and this strapping pin is never driven |
| LCD CS | unknown | **Confirmed by schematic + physical validation** | `LCD_CS` = **IO42** → L4.5 |
| LCD DC | unknown | **Confirmed by schematic + physical validation** | `LCD_D/C` = **IO41** → L4.4 |
| LCD RESET | unknown | **Confirmed by schematic + physical validation** | `LCD_RST` = **IO39** → L4.2 |
| LCD SPI frequency | unknown | **Confirmed by physical validation — 40 MHz selected** | 10 / 20 / 40 / 80 MHz all produced a clean image; 40 MHz chosen to keep margin (see Phase C log) |
| RGB / BGR order | unknown | **Confirmed by physical validation** | `LCD_RGB_ELEMENT_ORDER_RGB` correct — red renders red, blue renders blue |
| RGB565 byte order | unknown | **Confirmed by physical validation — big-endian** | Colour bars and corner markers render correct hues. Two swap sites exist by design: `display`'s pattern generator swaps at construction, and `ui` swaps each band in the LVGL flush callback via `lv_draw_sw_rgb565_swap()`. Phase F **disproved** the tempting alternative — `LV_COLOR_FORMAT_RGB565_SWAPPED` is an LVGL image/blend format, not a display format, and setting it renders greys and whites blue |
| Colour inversion | unknown | **Confirmed by physical validation** | `invert_colour = on` required; dark fields render dark and the white border renders white |
| Pixel alignment / tearing | unknown | **Confirmed by physical validation** | 1 px checkerboard renders as crisp uniform grain; no tearing observed on static patterns |
| Backlight GPIO | unknown | **Confirmed by schematic + physical validation** | `LCD_BL` = **IO5** switches the panel illumination |
| Backlight driver path | unknown | **Confirmed by schematic** | IO5 → R19 (1 kΩ) → base of T3 (S8050 NPN); emitter GND, collector VLED−; VLED+ from 3V3 via R18 (0 Ω); 4× LEDK through 10 Ω ballasts |
| Backlight polarity | unknown | **Confirmed by physical validation — active HIGH** | driving IO5 high illuminates the panel, as the low-side NPN topology predicted. Re-confirmed under PWM in Phase D: brightness rises monotonically with duty |
| Backlight PWM frequency | unknown | **Confirmed by physical validation — 5 kHz selected** | 200 Hz, 1, 2, 5, 10, 20, 40 kHz all swept; none showed flicker, banding, noise or corruption, so selection rests on duty resolution (see Phase D log) |
| Backlight duty resolution | unknown | **Confirmed — 12 bits (4096 steps) at 5 kHz** | LEDC on a 40 MHz XTAL source; `ledc_find_suitable_duty_resolution` yields 14 bits ≤2441 Hz, 12 at 5 kHz, 11 at 10 kHz, 10 at 20 kHz, 9 at 40 kHz |
| Lowest usable brightness | unknown | **Confirmed by physical validation — 1 %** | 1 % (duty 1/4096) reads as lit rather than off in a dark room; 2–3 % is comfortable in the dark, 50 % with room lights on |
| Full backlight off | expected | **Confirmed by physical validation — true off** | at 0 % (duty 0) the panel is completely black with no residual glow, verified in a dark room |
| Backlight thermal behaviour | unknown | **Confirmed by physical validation — no perceptible warmth** | board and the T3 switch stayed cool through the full Phase D session, including repeated 100 % holds |
| Backlight audible noise | unknown | **Confirmed by physical validation — none** | no whine or buzz at any swept frequency with an ear at a few centimetres; consistent with a resistively ballasted LED path containing no magnetics |
| Available button GPIO | unknown (BOOT/GPIO0 expected) | **Confirmed by physical validation — BOOT `Key2` = IO0 selected** | owner decision 2026-08-03. Validated as an application input: 31 gestures classified with no duplicate, missed or false event. BAT_PWR `Key1` = IO6 deliberately NOT used as a product input |
| Button pull-up/down | unknown | **Confirmed by schematic + physical validation** | both active low with 10 kΩ pull-ups to 3V3 (R20 on IO0, R1 on IO6); debounce cap C28 **not fitted** → firmware debounce required. Firmware leaves the internal pull-up disabled and relies on R20 |
| Button contact bounce | unknown | **Confirmed by physical validation — none measurable** | 62 interrupt edges for 31 press/release cycles, i.e. exactly 2 per press and not one bounce edge. The 25 ms firmware debounce is therefore margin, not a working necessity, on this switch |
| BOOT button physical position | unknown | **Confirmed by physical validation** | the button on the board edge **opposite** the USB-C connector. Identified electrically, by pressing it and observing IO0 edges. Enclosure-relevant: see the orientation note in the Phase C log |
| BOOT flashing recovery | expected | **Confirmed by physical validation** | esptool auto-reset flashes with no button press at all, with IO0 configured as an application input. Manual entry also works: holding BOOT across a USB power cycle reaches `boot:0x22 DOWNLOAD(USB/UART0)`, proven by esptool connecting with `--before no_reset` |
| Exit from USB download mode | unknown | **Confirmed by physical validation — power cycle required** | the ESP32-S3 USB-Serial/JTAG download-mode latch survives an RTS reset and `esptool run`; only unplugging and replugging USB-C (without BOOT held) returns to the application. Affects the documented recovery procedure |
| Deep/light sleep suitability | unknown | Unvalidated — deferred | Architecture Spec §25.8 defers this; MVP display-off strategy decided later |

No row may be marked confirmed solely because a similarly named touch-board
variant uses that value (Bootstrap Spec §17).

## Pin allocation (Phase C0, 2026-08-02)

Source: the official schematic (see Reference documents above). It contains an
explicit **GPIO allocation table** with columns GPIO / LCD / SD Card / IMU /
RTC / UART / I2C / Speak / Other. Every row below was read from that table and
independently cross-checked by pairing schematic net labels with MCU pin labels
via PDF text-coordinate analysis; the two methods agree completely. Connector
pinouts and the discrete circuits (backlight driver, buttons, power latch) were
read from the rendered schematic sheet.

Classification: **S** = confirmed by schematic · **V** = confirmed by physical
validation · **U** = unvalidated · **NP** = not populated on SKU 30223.

| GPIO | Net | Function | Class | Notes |
|---|---|---|---|---|
| IO0 | — | **BOOT button** (`Key2`) | S | 10 kΩ pull-up (R20), active low, C28 not fitted. ESP32-S3 strapping pin; input-safe after boot |
| IO1 | `TP_SDA` | touch I²C data → L5.4 | NP | connector fitted, no touch panel |
| IO2 | `TP_RST` | touch reset → L5.2 | NP | " |
| IO3 | `TP_SCL` | touch I²C clock → L5.5 | NP | " — also a strapping pin |
| IO4 | `TP_INT` | touch interrupt → L5.3 | NP | " |
| IO5 | `LCD_BL` | **backlight control** | S | → R19 1 kΩ → base of T3 (S8050 NPN), low-side, active high |
| IO6 | `Key_BAT` | **BAT_PWR button** (`Key1`) | S | 10 kΩ pull-up (R1), active low via D2; also gates the battery latch |
| IO7 | `BAT_Control` | battery power latch | S | → R9 1 kΩ → base of T2 (S8050); holds Q1 on. **Do not drive** |
| IO8 | `BAT_ADC` | battery voltage sense | S | |
| IO9 | `RTC_INT` | PCF85063 interrupt | S | RTC is populated |
| IO10 | `SCL` | shared I²C clock | S | IMU + RTC + J8 header; vendor docs state it cannot be remapped |
| IO11 | `SDA` | shared I²C data | S | " |
| IO12 | `IMU_INT2` | QMI8658 interrupt 2 | S | IMU is populated |
| IO13 | `IMU_INT1` | QMI8658 interrupt 1 | S | " |
| IO14 | `SD_SCLK` | microSD clock | S | slot populated, unused by MVP |
| IO15 | `SD_D2` | microSD data 2 | S | also exposed on J8 as `GPIO15` |
| IO16 | `SD_MISO` | microSD / SDIO_D0 | S | |
| IO17 | `SD_MOSI` | microSD / SDIO_CMD | S | |
| IO18 | `SD_D1` | microSD data 1 | S | also exposed on J8 as `GPIO18` |
| IO19 | `D_N` | **USB D−** | S + V | native USB Serial/JTAG, exercised throughout Phase B |
| IO20 | `D_P` | **USB D+** | S + V | " |
| IO21 | `SD_CS` | microSD chip select / SDIO_D3 | S | |
| IO26–IO32 | — | SPI flash | S | absent from the allocation table; reserved by the module |
| IO33–IO37 | — | "Internal occupancy" | S + V | schematic wording; consistent with octal PSRAM, which Phase B proved active |
| IO38 | `I2S_LRCK` | audio word clock | S | PCM5101A populated |
| IO39 | `LCD_RST` | **LCD reset** → L4.2 | S | |
| IO40 | `LCD_SCK` | **LCD SPI clock** → L4.3 | S | |
| IO41 | `LCD_D/C` | **LCD data/command** → L4.4 | S | |
| IO42 | `LCD_CS` | **LCD chip select** → L4.5 | S | |
| IO43 | `UART_TXD` | UART TX | S | on J8 and the UART header |
| IO44 | `UART_RXD` | UART RX | S | " |
| IO45 | `LCD_MOSI` | **LCD SPI MOSI** → L4.6 | S | **strapping pin** (VDD_SPI select) |
| IO46 | `LCD_MISO` | **LCD SPI MISO** → L4.7 | S | **strapping pin** (boot mode) |
| IO47 | `I2S_DIN` | audio data | S | |
| IO48 | `I2S_BCK` | audio bit clock | S | |

### Connectors

- **L4** — LCD FPC, 0.5 mm 18-pin: `1 GND, 2 RST, 3 SCK, 4 RS(D/C), 5 CS,
  6 SDA, 7 SDO, 8 GND, 9 VDD(3V3), 10 LEDA, 11–14 LEDK, 15–18 XL/YU/XR/YD`.
  Pins 15–18 are resistive-touch connections, unused by this product.
- **L5** — touch FPC, 6-pin: `GND, RST, INT, SDA, SCL, VDD`. Fitted but empty
  on SKU 30223.
- **J8** — SH1.0 12-pin header: GND, D_N, D_P, GND, SCL, SDA, UART_TXD,
  UART_RXD, GPIO18, GPIO15, GND (+3V3/VBus). Matches the board silkscreen
  `IO15 IO18 RXD TXD SDA SCL 3V3 GND D+ D- VBus GND`.

### Bus sharing

The SD signals reach the module through 0 Ω links (R26/R27/R37/R39/R41) and
carry 10 kΩ pull-ups (R30–R35). Three further positions — **R38, R40, R42,
marked NC** — would bridge `SDIO_CMD/SCK/D0` onto `LCD_MOSI/LCD_SCK/LCD_MISO`.
They are **not fitted**, so the **LCD SPI bus is dedicated** and is not shared
with the microSD slot on this board.

### Strapping pins in use

ESP32-S3 strapping pins are IO0, IO3, IO45 and IO46. On this board **IO45
(`LCD_MOSI`) and IO46 (`LCD_MISO`) are strapping pins carrying LCD traffic**,
and IO0 is the BOOT button. This is safe because the panel is a passive slave
driven only after boot, but it constrains Phase C: the LCD lines must not be
given external pulls or be driven during reset, and a panel that held these
lines at reset could change boot mode or VDD_SPI voltage. IO3 (`TP_SCL`) is
strapping but unpopulated.

### Genuinely free GPIOs

**None are unallocated** — the schematic assigns every available pin. The
realistically reusable ones are the four touch nets **IO1, IO2, IO3, IO4**,
which are electrically idle because no touch controller is fitted; each keeps a
short stub trace to the empty L5 connector, and IO3 is a strapping pin. Pins
belonging to the microSD slot, IMU, RTC, audio codec and battery circuit are
attached to populated components and must not be treated as free.

### Note 2 — flash capacity is genuine, not aliased

A full `read_flash 0 0x1000000` completed successfully (16,777,216 bytes,
1459 s, 92 kbit/s). The image splits into four 4 MB quarters: quarter 0
contains the factory image, and quarters 1–3 are each byte-identical to 4 MB
of `0xFF` (md5 `2b7a70fa59f8173635bcbe956bad56c6`). A part that merely
claimed 16 MB would have wrapped its address space and returned quarter 0's
content at `0x400000` or `0x800000`; it returned erased flash instead, so the
upper address lines are real.

### Note 3 — `--flash_mode dio` in flash_args is expected

The generated `flash_args` writes `--flash_mode dio` even though
`CONFIG_ESPTOOLPY_FLASHMODE_QIO=y`. This is by ESP-IDF design: the image
header uses DIO (universally supported by the ROM loader) and the
second-stage bootloader upgrades itself to quad mode during initialisation
(ESP-IDF `components/esptool_py/Kconfig.projbuild`: *"dio mode for QIO/QOUT,
bootloader then upgrades itself to quad mode"*). The runtime mode is QIO, as
the boot log confirms. No configuration change is required.

## Validation log

Newest first.

### 2026-08-04 — Phase G, Milestone 1 closure verification

Purpose: confirm the **committed** repository state still works. Every earlier
phase was validated from a dirty working tree, so this is the first run of an
image built from a clean checkout — `c6b574e`, `dirty=false`, which is also the
first time the §14.4 release gate has been satisfied without a workaround.

**Reproduction against Phase F.** The committed image was re-flashed and run for
6.9 minutes, then the button was exercised separately.

| Measurement | Phase F | Phase G | |
|---|---|---|---|
| `internal_free` | 195,887 | 195,887 | identical |
| `internal_min` | 195,075 | 195,075 | identical |
| `lvgl_peak` | 9,884 | 9,884 | identical |
| `lvgl_used` | 7,320 | 7,320 | identical |
| `psram_free` | 8,385,732 | 8,385,732 | identical |
| Heap consumed by renderer | 77,880 | 77,880 | identical |
| `prd_ui` min free stack | 3,860 (47%) | 3,860 (47%) | identical |
| `prd_ui_val` min free stack | 2,932 (35%) | 2,932 (35%) | identical |
| `prd_button_val` min free stack | 2,672 (43%) | 2,672 (43%) | identical |
| Flush mean | 2,861 µs | 2,870 µs | within noise |
| Byte-swap mean | 56 µs | 57 µs | within noise |
| Flush failure counters | all 0 | all 0 | identical |
| `peak_in_flight` | 1 | 1 | identical |

8,106 flushes over the run with `submitted == completed`, 0 unexpected reboots,
0 aborts, 0 panics, 0 watchdog events, 0 cross-check divergences, 0 project-tag
errors or warnings.

**Button gesture classification re-confirmed on the committed build**

| Gesture | Result |
|---|---|
| Short press | classified `short-press` |
| Long press | classified `long-press` (×2) |
| Very-long press | classified `very-long-press`, fired while still held |
| Raw edges | **8 for 4 presses** — exactly two each, no bounce |
| `raw_dropped` / `events_dropped` | **0 / 0** |

One press initially came out `long-press` rather than `very-long-press`: the hold
had not reached the 8 s threshold, and the firmware classified what it actually
saw. A deliberate 12 s hold produced `very-long-press` at the threshold, while
still held, with nothing further on release — the emission rule from Phase E,
unchanged.

The renderer continued throughout the button test with every flush counter clean,
so input and rendering coexist without interference.

**Tooling note, third occurrence of the same class.** Closing one passive capture
and opening the next resets the board over USB-Serial/JTAG. `capture_passive.py`
clears DTR/RTS before `open()`, but not on `close()`. Two earlier instances are
recorded in the Phase C and Phase E entries. A first button attempt also recorded
`raw_edges=0` simply because the 150 s capture window closed before the presses
were made — a measurement-window error, not a board or firmware fault.

Evidence: [Phase G closure](evidence/2026-08-04-phase-g-closure.log).

### 2026-08-04 — Phase F, LVGL foundation

First rendering engine on this hardware. LVGL 9.5.0, pinned exactly, integrated
directly rather than through `esp_lvgl_port`; one project-owned task (`prd_ui`) is
the only code in the firmware that calls LVGL. Decisions and their rationale are in
[ADR-0005](../../docs/decisions/0005-lvgl-integration.md). This is the first flow
that redraws the panel continuously rather than every few seconds, and several
findings below exist only because of that.

**Configuration validated**

| Item | Value |
|---|---|
| Renderer | LVGL 9.5.0 (`lvgl/lvgl ==9.5.0`, hash `184e532558c1c45f…`) |
| Geometry | 320×240 landscape, `LV_DISPLAY_ROTATION_0` — the panel transform is already done in hardware |
| Pixel format | `LV_COLOR_FORMAT_RGB565` + `lv_draw_sw_rgb565_swap()` in the flush callback |
| Render mode | `LV_DISPLAY_RENDER_MODE_PARTIAL` |
| Draw buffers | 2 × 25,600 B (320×40 px), internal DMA-capable RAM — placement **verified** with `esp_ptr_internal()` / `esp_ptr_dma_capable()`, not assumed |
| LVGL heap | built-in allocator, fixed 64 kB pool in `.bss` |
| Tick | `lv_tick_set_cb()` reading the project monotonic clock; no periodic tick timer |

**Colour format — a defect found only by looking at the panel**

`LV_COLOR_FORMAT_RGB565_SWAPPED` was tried first, to render directly in the
panel's big-endian byte order and avoid a conversion pass. It renders **visibly
wrong colours**: every grey and white came out blue, the near-black background came
out navy, and the accent orange looked approximately correct.

`lv_display_set_color_format()` documents exactly four valid values — RGB565,
RGB888, XRGB8888, ARGB8888 — and its note directs callers to
`lv_draw_sw_rgb565_swap()` in the flush callback for endianness.
`RGB565_SWAPPED` is an image and blend format, not a display format.

**Every counter, log line and timing figure was clean while this was wrong.** The
fault was invisible to the firmware's own instrumentation and was caught by
photographing the screen. Corrected to the documented approach, measured at
**57–64 µs mean per flush** and about 390 µs for a full 40-row band against a
5.4 ms transfer — roughly 7%.

**Flush accounting**

Two independent views are compared: `ui` counts what LVGL asked for, `display`
counts what the panel did. They are produced by different components through
different code paths, so agreement is evidence.

| Check | Result |
|---|---|
| `submitted` (ui vs panel) | **exact match** at every sample |
| `completed` (ui vs panel) | panel leads by at most 1 — see the sampling note below |
| Buffers in flight | **`peak_in_flight = 1`** throughout — never two buffers lent at once |
| `submit_failures` | **0** |
| `spurious_signals` | **0** |
| `recovered_flushes` | **0** |
| `deadline_misses` | **0** |
| `abandoned_flushes` | **0** |
| `panel_wait_timeouts` | **0** |

`waits_blocked` reached 3,743 of 38,106 flushes (**9.8%**), so for about 90% of
bands the next band's rendering took longer than the previous band's transfer. The
SPI bus is not the bottleneck at this buffer size.

**Two counter-comparison bugs of my own, both fixed**

Neither was a firmware defect; both were wrong invariants in the *reporting*, and
both produced false ERROR lines that would have made a clean soak look faulty.

1. `submitted == completed` is wrong as an invariant. LVGL waits for a flush
   *before* starting the next one, never after the last one, so a sample taken
   while the final band of a refresh is still on the wire legitimately sees one
   outstanding. The correct bound is `submitted - completed <= 1`.
2. `ui.completed == panel.completed` is wrong for the same class of reason. The
   panel's counter is incremented in the transfer interrupt; `ui`'s is incremented
   when the render task next runs and consumes the signal. A third task sampling
   both can land between them, so the panel may lead by one. `completion_lag` is
   now reported explicitly rather than inferred.

**Timing and cadence**

| Measurement | Value |
|---|---|
| Flush duration, steady state | min 2,615 µs · mean 2,861 µs · max 7,980 µs |
| Flush duration, first full-screen frame | ~9.9–10.4 ms per 40-row band |
| Byte-swap cost | 56 µs mean over 38,106 flushes; ~390 µs for a full band |
| Update cadence | **19.99 flushes/s** (38,106 over 1,906 s), one per counter tick |
| Bands per full-screen frame | 6 (240 rows ÷ 40) |

The steady-state figure is below the 5.1 ms a full 40-row band takes at 40 MHz
because partial mode flushes only the dirty area — here the small counter label,
not a whole band. That is the redraw discipline the Production UI Specification §11
requires, working: a full-screen redraw would be six bands per update rather than
one.

**Stack high-water marks** — every project-owned task, measured at its deepest
observed path:

| Task | Configured | Min free | Peak used | Headroom |
|---|---|---|---|---|
| `prd_ui` | 8192 | 3,860 | 4,332 | **47%** |
| `prd_ui_val` | 8192 | 2,932 | 5,260 | **35%** |
| `prd_button_val` | 6144 | 2,672 | 3,472 | 43% |

Worst case across the full soak, not a single reading: `prd_ui`'s mark settled from
4,012 to 3,860 free as deeper paths ran.

**`prd_ui_val` was first written as 4096 and aborted on hardware.** The abort
landed inside `backlight_init()`'s `std::string` log line, decoded as
`operator new` failing and `__cxa_allocate_exception` aborting because C++
exceptions are disabled. Cause: `display::init()` plus one `std::string` is deeper
than 4 kB — the same fault the ESP-IDF main task hit at 3584 bytes in Phase C, and
the FreeRTOS canary misses it for the same reason, because the overflow faults
immediately rather than at a context switch. Raised to 8192 to match
`prd_display_val`, which does the same `display::init()` work and had already
measured 41% headroom at that size.

**Memory**

| Measurement | Value |
|---|---|
| Internal heap before LVGL | 273,767 B |
| Internal heap after LVGL | 195,887 B |
| Consumed by the renderer | **77,880 B** (51,200 draw buffers + LVGL structures + render task stack) |
| Largest free internal block | 122,868 B |
| Minimum free internal since boot | 195,075 B |
| PSRAM free | 8,385,732 B — essentially untouched, confirming the buffers really are internal |
| LVGL pool used | 7,320 / 63,644 B |
| LVGL pool peak | 9,884 B |
| LVGL pool fragmentation | 1% |

The 64 kB LVGL pool is heavily oversized for one scene; it is left at LVGL's
default because Milestone 1 has only this scene to size it against.

**Soak — 31.9 minutes continuous (1,911 s), 38,106 flushes**

| Check | Result |
|---|---|
| Unexpected reboots | **0** (the single `rst:0x15` is the post-flash boot) |
| Aborts, panics, `Guru Meditation` | **0** |
| Task watchdog events | **0** |
| Project-tag ERROR or WARNING lines | **0** |
| Cross-check divergences | **0** |
| Every flush failure counter | **0** |
| `submitted` vs `completed`, final | 38,106 = 38,106 |

Heap over 128 samples spanning the full soak:

| Metric | Values observed |
|---|---|
| `internal_free` | **195,887 — a single value, all 128 samples** |
| `lvgl_peak` | **9,884 — a single value, all 128 samples** |
| `lvgl_used` | 7,316 then 7,320; a 4-byte step at the first counter reformat, flat after |
| `lvgl_frag` | 1% throughout |
| `internal_min` | 195,407 → 195,075, then flat |
| `psram_free` | 8,385,732 throughout |

No leak trend exists to extrapolate: the free-heap figure did not move at all
across half an hour and 38,106 render-and-transfer cycles.

**Verbose logging is a workload at this redraw rate.** The hardware-validation
profile logs at verbose, which was harmless for every earlier phase because they
redrew every few seconds. At 20 redraws/s, `esp_driver_spi` and `spi_bus_lock`
emitted roughly **570 lines/s — 99% of all serial output**, enough to bury the
measurement and, over USB CDC, to perturb it. Raising those two tags to warning at
run time removes every `ESP_LOGV`/`ESP_LOGD` line and takes the rate to ~68
lines/s. What remains is one line per transfer from `spi_master.c`'s
**`ESP_EARLY_LOGV`**, which is compile-time only and ignores runtime tag levels by
design; removing it would mean lowering `CONFIG_LOG_MAXIMUM_LEVEL` for the whole
profile, which is an accepted setting from an earlier phase and was deliberately
left alone. The committed evidence log is filtered.

**Visual checks** — validation scene, deliberately not the accepted Printing or
Ready composition:

| Check | Result |
|---|---|
| Orientation | correct — every element in its intended corner, matching the Phase C landscape transform |
| Background `#111315` | correct near-black |
| Primary `#F2F3F4` | correct near-white |
| Accent `#F77F2F` | correct, and matches the Phase C1 colour chart |
| Arc, 4 px, round caps | correct — both ends visibly rounded |
| Anti-aliasing | smooth on the arc and on 48 px glyph curves; no stair-stepping |
| Clipping at the right edge | clean — 56 of 96 px visible, no wrap, no spill onto the adjacent row, no scroll |
| Tearing | none, including through the 20 Hz numeric field |
| Layout stability | none — the fixed-width counter format keeps the surrounding pixels still |

Evidence: [Phase F](evidence/2026-08-04-phase-f-lvgl.log).

### 2026-08-04 — Pre-Phase F hardening: transfer ownership and stack measurement

Narrow batch closing the two Phase F blockers from the Milestone 1 architecture
audit, plus the log-tag inconsistency that Phase F would otherwise extend. No
LVGL, no product UI, no display colour/gamma/backlight change.

**Pixel-transfer ownership.** `esp_lcd_panel_draw_bitmap()` submits over DMA and
returns while the transfer is still reading the caller's buffer; completion is
reported only through `on_color_trans_done`, which this firmware had never
registered. The test-pattern renderer reused a single band buffer across six
blits per frame, so the CPU could rewrite bytes DMA was still reading. It had not
produced visible corruption, because esp_lcd incidentally drains its transaction
queue once full — but that is accidental backpressure, not a guarantee.

The callback is now registered and the public contract is explicit: `blit()` lends
the buffer to the panel until completion is signalled, either by
`wait_for_transfers()` returning true or by the caller's registered callback. The
callback runs **in interrupt context** (esp_lcd invokes it from the SPI
transaction post-callback), which the header states along with the ISR
restrictions. `blit_blocking()` submits and waits, and the renderer uses it.

`peak_in_flight` is the measurement that proves the fix: **1** throughout, where
before the change up to six transfers could be outstanding against one buffer.

| Check | Result |
|---|---|
| Corner/border markers | **Pass** — 1 px border intact on all four edges, corners correct |
| Colour chart (comparison chart) | **Pass** — no torn bands, no garbage rows |
| 1 px checkerboard | **Pass** — crisp uniform grain |
| Gradient | **Pass** — smooth, no band discontinuities at the 40-row boundaries |
| Repeated init/draw/deinit | **Pass** — 6 full cycles across the SPI clock sweep, 0 failures |
| Drain at deinit | **Pass** — 0 occurrences of transfers still in flight at teardown |
| submitted vs completed | **Match** — 20 reports across two runs, never unequal |
| Spurious completions | **0** |
| Wait timeouts | **0** |
| Peak transfers in flight | **1** |
| Errors or warnings | **none** |

Transfers per frame are 6, which is 240 rows at 40 rows per band — the counts are
exact, not approximate, because esp_lcd raises its trans-done flag only on the
final chunk of each draw.

**Task stack high-water marks.** `uxTaskGetStackHighWaterMark` had never been
called anywhere in the repository; all three stack sizes were guesses. Note that
ESP-IDF's version returns **bytes**, where upstream FreeRTOS returns words —
scaling the result would have overstated free stack fourfold and produced
undersized stacks.

Worst case observed across the pattern cycle, the SPI clock sweep and button use:

| Task | Configured | Min free | Peak used | Headroom | Action |
|---|---|---|---|---|---|
| `prd_display_val` | 8192 | 3364 | 4828 | 41 % | unchanged |
| `prd_button` | 4096 | 1540 | 2556 | 37 % | unchanged |
| `prd_button_val` | 4096 → **6144** | 640 → **2688** | 3456 | 15 % → **43 %** | **raised** |

Only `prd_button_val` was changed, and only because 640 bytes free is too thin to
absorb a deeper log path. Its peak usage is unchanged at 3456 bytes after the
increase, confirming the extra stack is margin rather than a behaviour change. The
other two are left exactly as they were: measurement did not call for a change,
and reducing a stack that has not been exercised on every path would be guesswork
in the opposite direction.

`prd_button`'s mark converged 2980 → 1540 bytes free as deeper paths ran, which is
the high-water mark behaving correctly — a single early sample would have
overstated the available headroom.

Deepest path not yet exercised, and therefore not represented above: the
queue-full branch of the button component's `publish()`, which builds a longer log
string than anything measured. It is unreachable without a stuck consumer.

The Phase C main-task overflow remains recorded in the Phase C log entry below as
the historical reason this instrumentation exists. No display work has been
returned to the ESP-IDF main task.

Evidence: [hardening](evidence/2026-08-04-pre-phase-f-hardening.log).

### 2026-08-03 — Phase E, button bring-up and event classification

First application use of IO0. Owner decision: the MVP multifunction button is the
BOOT button (`Key2`, IO0). BAT_PWR (`Key1`, IO6) is explicitly not a product
input — the same net gates the battery-path P-FET — and IO7 (`BAT_Control`) is
never driven. Observer: project owner, on the physical unit.

**Input configuration**

| Item | Value | Basis |
|---|---|---|
| GPIO | IO0 | `board::button_pins()`; the button component holds no pin number |
| Active level | low | schematic; switch shorts to GND |
| Pull-up | external only | R20, 10 kΩ to 3V3. The internal pull-up is left **disabled** so two pull-ups are never placed in parallel on a strapping pin |
| Interrupt | `GPIO_INTR_ANYEDGE` | both edges needed to time a press |
| Configured | after the boot report and the `phase=ready` transition | matches the accepted initialisation order (Architecture Spec §12, step 12). Strapping is sampled at reset, so configuring an input later cannot affect it, but the ordering is kept regardless |

Boot log confirms the ordering and the wiring in one line:

```
I (883) prd.boot: phase=ready
I (892) prd.button: button ready gpio=0 active=low pull_up=external hw_debounce=no
                   debounce_ms=25 long_ms=1500 very_long_ms=8000 level=released
```

**Provisional thresholds.** debounce 25 ms, long press 1500 ms, very long press
8000 ms. No accepted Project Source specifies button timing, so these are
engineering defaults recorded as **provisional implementation values**, not
product behaviour. They must be revisited when the UI that consumes them is
designed.

**Results**

| Check | Required | Result |
|---|---|---|
| Short presses | ≥10 | **15** — all classified `short-press` |
| Long presses | ≥10 | **10** — all classified `long-press`, none leaking into either neighbour |
| Very-long presses | ≥5 | **6** — all classified `very-long-press` |
| Duplicate classified events | none | **none** — 62 raw edges produced 31 events; one gesture, one event |
| Missed events | none | **none** — every press the observer made appears in the log |
| False events while untouched | none | **none** — `raw_edges` stayed frozen through every idle interval, including a 20 s window immediately after init and multiple 10 s idle reports |
| Queue drops | none | **none** — `raw_dropped=0 events_dropped=0` throughout |
| Warnings or errors | none | **none** across the whole session |
| Reset or boot-mode entry during use | none | **none** — 814 s of continuous uptime across 28 presses, with a single reboot which was the initial flash |
| Button input affects normal boot | must not | **does not** — boot reaches `phase=ready` before the button is configured, and a clean boot with the button untouched classifies nothing |
| BOOT flashing recoverable | must be | **yes**, two independent ways — see below |

The bounce result deserves emphasis because it is unusually clean: **62 interrupt
edges for 31 press/release cycles is exactly two per press.** Not one bounce edge
was captured on this switch, across taps as short as ~250 ms and gaps as short as
~530 ms. The 25 ms debounce is margin here rather than a working necessity, and
the bounce handling is proven by host tests rather than by this bench run.

The emission rule was confirmed arithmetically rather than by inspection. Across
the very-long stage the counters moved by **+14 raw edges and +7 events**:
fourteen edges is seven press/release pairs, and seven presses produced seven
events. Had `VeryLongPress` also fired on release, those six holds would have
produced twelve events instead of six.

**BOOT recovery.** Both paths validated:

1. *Normal flashing* needs no button at all. esptool auto-reset connected,
   identified the chip and ran its stub with IO0 configured as an application
   input, and a full `flash` completed afterwards.
2. *Manual entry* works: holding BOOT across a USB-C power cycle reaches the ROM
   loader, proven by `esptool --before no_reset` connecting successfully — which
   only succeeds if the chip is already in the loader — and by the ROM banner
   `rst:0x15 (USB_UART_CHIP_RESET),boot:0x22 (DOWNLOAD(USB/UART0))`,
   `waiting for download`.

**Finding — download mode is sticky.** Once in USB-Serial/JTAG download mode the
`boot:0x22` latch survives both an RTS reset and `esptool run`; the board keeps
re-entering the loader. Only unplugging and replugging USB-C, without BOOT held,
returns it to the application. This is chip behaviour rather than a fault, and it
belongs in any recovery instructions written for this product: *"if the screen
stays black after a BOOT recovery, unplug and replug the cable."*

A power cycle without BOOT held also served as the discriminator that IO0 is not
stuck low: the board booted straight into the application and the button
classified three further presses cleanly afterwards.

**Not validated on hardware.** Bounce handling, the release-before-debounce case,
the glitch-rejection case, threshold boundaries and reset-while-held are covered
by 22 host tests against a fake clock rather than by the bench, because a human
finger cannot produce a 1 ms glitch or a press exactly 1500 ms long on demand.

Evidence: [phase-e-button](evidence/2026-08-03-phase-e-button.log).

### 2026-08-03 — Phase D, backlight PWM and brightness validation

First PWM use of IO5. The Phase C on/off GPIO control was replaced by LEDC PWM
owned by the display component. Observer: project owner, on the physical unit.

**LEDC configuration**

| Item | Value | Why |
|---|---|---|
| Timer / channel | `LEDC_TIMER_0` / `LEDC_CHANNEL_0` | first of 4 timers and 8 channels; the backlight is the only LEDC consumer in MVP scope |
| Speed mode | `LEDC_LOW_SPEED_MODE` | forced — the ESP32-S3 implements no high-speed channel group |
| Clock source | `LEDC_USE_XTAL_CLK`, 40 MHz | ESP-IDF lists XTAL as Dynamic-Frequency-Scaling compatible and APB as not, and the LEDC driver holds no PM lock. APB would give one more resolution bit but would let the PWM frequency drift if power management were enabled for battery operation |
| Frequency | **5 kHz** | see selection below |
| Duty resolution | **12 bits, 4096 steps** | the maximum 5 kHz allows from a 40 MHz source |
| Full scale | duty `1 << 12` = 4096 | ESP-IDF duty range is `[0, 2**resolution]`; at `2**resolution` the output is held constant high with no switching, so 100 % has no switching artefact at all |
| Polarity handling | LEDC `flags.output_invert`, driven from `board` | keeps duty semantics absolute — duty 0 is off and full scale is on for either polarity — so no code above the adapter reasons about wiring. This board is active HIGH, so the inverter is off |
| Startup | channel created at duty 0 with output disabled | the panel cannot show a partially initialised frame; the requested brightness is pre-loaded so lighting the panel is one call |

**Brightness curve.** `duty = (percent/100)^2.2 × full_scale`, precomputed into a
101-entry table at init and interpolated in Q8 for fades. Perceived lightness
follows roughly a cube-root law while LED luminance is near-linear in duty, so a
straight mapping would make the bottom of the range change violently and the top
barely change. 2.2 is the sRGB display gamma — the same exponent the accepted UI
colour tokens are encoded against, which keeps brightness and pixel encoding on
one curve. Measured at 5 kHz / 12 bits:

| Percent | 0 | 1 | 2 | 3 | 4 | 5 | 10 | 25 | 50 | 75 | 100 |
|---|---|---|---|---|---|---|---|---|---|---|---|
| Duty /4096 | 0 | 1 | 1 | 2 | 3 | 6 | 26 | 194 | 891 | 2175 | 4096 |

1 % and 2 % share duty 1 because that is the resolution floor, not a defect. The
curve's properties (0 → 0, 100 → full scale, never decreasing, no non-zero
request rounding to darkness) are asserted by nine host tests, so they are
protected by CI rather than only by observation.

**PWM frequency selection.** Each frequency was held at 25 % then 5 % on a white
field carrying a marker strip identifying the step, so an unattended sweep could
be tied to a frequency. The LEDC timer was re-timed in place; the panel, its SPI
bus and its contents were untouched.

| Frequency | Duty resolution | Full scale | Flicker (eye) | Camera banding | Audible noise | Corruption |
|---|---|---|---|---|---|---|
| 200 Hz | 14 bits | 16384 | none | none | none | none |
| 1 kHz | 14 bits | 16384 | none | none | none | none |
| 2 kHz | 14 bits | 16384 | none | none | none | none |
| **5 kHz** | **12 bits** | **4096** | none | none | none | none |
| 10 kHz | 11 bits | 2048 | none | none | none | none |
| 20 kHz | 10 bits | 1024 | none | none | none | none |
| 40 kHz | 9 bits | 512 | none | none | none | none |

**The perceptual axes did not discriminate.** Nothing was visible at any
frequency, including the 200 Hz negative control that was expected to fail. The
camera test used automatic exposure, which integrates over many PWM cycles, so it
is weak evidence and is recorded as such rather than as a clean bill of health.

Selection therefore rests on the axis that did discriminate — duty resolution:

- 12 bits is demonstrably sufficient: the fade was observed smooth with no
  stepping, and 1 % is visible in a dark room. More resolution buys nothing
  measurable.
- At 20 kHz and above the entire 1–5 % range collapses onto duty 1, which would
  destroy the dark-room range a bedside/desk display needs.
- 5 kHz keeps roughly 25× margin over the 200 Hz region, for observers more
  flicker-sensitive than one person and for cameras with a faster shutter than
  automatic exposure. This product will be photographed and filmed, so that
  margin is worth keeping.
- 5 kHz is also the frequency at which every brightness and fade observation was
  actually made, rather than one swept for twelve seconds.

Higher frequencies were not chosen for silence because silence was already
measured at every frequency, and the backlight path contains no magnetics that
could produce audible noise.

**Physical validation results**

| Check | Result |
|---|---|
| Polarity under PWM | **Pass** — brightness rises monotonically with percentage |
| 0 % is true full-off | **Pass** — completely black in a dark room, no residual glow |
| Monotonic 0→100 % | **Pass** — every step brighter than the last, no reversal |
| Lowest visible brightness (dark room) | **1 %** (duty 1/4096) |
| Comfortable in a dark room | **2–3 %** |
| Comfortable with room lights on | **50 %** — adopted as the startup brightness |
| Maximum brightness | **Pass** — duty 4096 is constant-high, no switching |
| Fade smoothness | **Pass** — smooth across the whole range, no stepping, including near black |
| Corruption while changing PWM | **None** — frame redrawn at each re-time, no tearing or garbage |
| Flicker during static patterns | **None** at any frequency |
| Audible coil/transistor noise | **None** at any frequency |
| Thermal | **Pass** — board and T3 stayed cool throughout, including repeated 100 % holds |
| Repeated init/deinit with LEDC teardown | **Pass** — 15 attempts, 0 failures, 4 sweeps |

`display::deinit()` now releases the LEDC channel and timer as well as the
panel, so the Phase C repeated-initialisation path was re-run to confirm the
backlight comes back up cleanly each time. 15 full init/deinit cycles, 0
failures, no LEDC error at any cycle.

Selected values now encoded in `display.hpp`: `kValidatedPwmFrequencyHz = 5000`,
`kMinimumVisibleBrightnessPercent = 1`, `kDefaultBrightnessPercent = 50`.

**Non-linearity.** None beyond the intended gamma curve and the duty-resolution
floor at 1–2 %. Brightness tracked the curve monotonically at every step.

Fades are software-stepped at ~62 Hz rather than using ESP-IDF's hardware fade.
The hardware fade is linear in duty, which is perceptually wrong — it would rush
through the dark end where the eye is most sensitive. The ESP32-S3 has no
gamma-curve fade support (`SOC_LEDC_GAMMA_CURVE_FADE_SUPPORTED` is not defined
for this target), so a perceptual ramp has to be stepped in software. This also
avoids installing the LEDC fade service, which would allocate an interrupt this
component has no other use for.

Evidence: [phase-d-backlight](evidence/2026-08-03-phase-d-backlight.log).

The captured `backlight ledc ok` lines show `startup_percent=60`, which was the
provisional value in force during the session. 50 % was adopted afterwards from
the room-lit comfort observation above; the log is left as captured rather than
regenerated to match the conclusion it produced.

### 2026-08-02 — Phase C1, vendor panel tuning A/B (colour investigation)

Question: does the Waveshare panel-specific initialisation materially improve
colour rendering versus ESP-IDF's generic ST7789 init? Four deterministic
builds, one per mode, each drawing the same static comparison chart
(`#F77F2F`, `#111315`, `#7C848E`, white, 25/50/75 % grey, pure R/G/B, gradient)
with a marker strip identifying the mode. Observer: project owner, panel and a
matching on-screen reference viewed straight-on under constant lighting.

Reference: ST7789T3_SPEC_V1.0.pdf, SHA-256
`7a91ce95b3f743111b975f9189c97d99a8c6bcaa0ed026dcaaf6c2979adf9d17`.

| Mode | Build SHA-256 (first 16) | Registers added beyond generic init |
|---|---|---|
| A generic (control) | `fefb9a8fa4b8f874` | none |
| B vendor exact | `a42910d7c181cf34` | 36, 3A, B0, B2, B7, BB, C0, C2, C3, C4, C6, **D0 (1 param — vendor defect, reproduced)**, E0, E1 |
| C vendor gamma only | `06b649a2b91db467` | **E0, E1 only** |
| D vendor analogue + gamma | `fb4d3fba628bea0b` | B2, B7, BB, C0, C2, C3, C4, C6, **D0 (2 params — datasheet-corrected)**, E0, E1 |

Exact bytes transmitted are in
[evidence/2026-08-02-colour-ab-modes.log](evidence/2026-08-02-colour-ab-modes.log).

**Results**

| Mode | Accent `#F77F2F` | Black `#111315` | Orientation | Verdict |
|---|---|---|---|---|
| A | dull, "dirty" | elevated — reads mid blue-grey, not near-black | reference | baseline |
| B | **wrong — renders azure blue** | good | **flipped** | rejected |
| C | **correct and better than A** | **good** | unchanged | **best** |
| D | **wrong — renders azure blue** | good | **flipped** | rejected |

**Conclusion.** The vendor **gamma tables (E0h/E1h) are the entire benefit**;
the vendor **analogue block is harmful on this integration**. B and D both send
the analogue block and both corrupt the accent colour and flip orientation; C
omits it and improves both accent and black level with no side effect. Mode C
touches no undocumented register and needs no correction of vendor defects.

**Probable cause of the B/D fault, not proven.** Mode D transmits no MADCTL,
COLMOD or RAMCTRL, so the orientation change must originate within
`B2/B7/BB/C0/C2/C3/C4/C6/D0`. `LCMCTRL (C0h)` is the only register there whose
documented function is XOR-ing MADCTL bits — including `XBGR`, which swaps red
and blue, and `XMY`, which flips an axis — which would explain both symptoms
together. The vendor writes `C0h = 0x80`; datasheet §9.2.10 maps that parameter
as `D7=0, D6=XMY, D5=XBGR, D4=XINV, D3=XMX, D2=XMH, D1=XMV, D0=XGS`, so `0x80`
sets a bit documented as fixed zero while leaving every documented XOR bit
clear. Writing a reserved bit is outside the documented contract and its effect
is unspecified. Isolating C0h individually would confirm this; it was not done
because the analogue block is being rejected regardless.

**Vendor defect recorded.** Waveshare's `panel_st7789t_init()` calls `0xD0`
with length 1 for the two-byte array `{0xA4, 0xA1}`, so the AVDD/AVCL/VDS
parameter never reaches the panel — their shipping product runs with it
dropped. Datasheet §9.2.21 defines `0xD0` as taking two parameters. Mode B
reproduces the defect verbatim (`tx cmd=0xD0 params=A4`); mode D sends the
corrected form (`params=A4 A1`), whose fields all decode to documented,
in-range enumerations with the reserved bits zero. Correcting it changed
nothing observable — D behaved like B.

**Not affected by any mode:** clipping, inversion, and repeatability were
unchanged; every mode initialised first time and held a stable image. Black
level improved in B, C and D. No flicker or artefact was reported in any mode.

## Accepted panel initialisation for this board

**Gamma-only tuning is the accepted default for `waveshare_esp32_s3_lcd_2_8`**
(owner decision, 2026-08-02, closing Phase C1). Selected in
`firmware/sdkconfig.defaults.waveshare_esp32_s3_lcd_2_8` via
`CONFIG_PRD_DISPLAY_TUNING_VENDOR_GAMMA_ONLY=y`, and the Kconfig default, so it
applies to **debug, hardware-validation and release** alike.

The default display path applies **exactly two registers** after ESP-IDF's
generic ST7789 init, and nothing else:

**PVGAMCTRL (0xE0) — positive voltage gamma, 14 parameters**

```text
D0 0D 14 0D 0D 09 38 44 4E 3A 17 18 2F 30
```

**NVGAMCTRL (0xE1) — negative voltage gamma, 14 parameters**

```text
D0 09 0F 08 07 14 37 44 4D 38 15 16 2C 2E
```

Both tables are panel-specific values taken from Waveshare's own
`Vernon_ST7789T.c`; the parameter counts match ST7789T3 datasheet §9.2.24 and
§9.2.25.

**The vendor analogue/power block is rejected and is never applied on the
default path:** `B2h`, `B7h`, `BBh`, `C0h`, `C2h`, `C3h`, `C4h`, `C6h`, `D0h`.
Reason: in modes B and D it **changed the display orientation and corrupted
colour rendering** — the accent rendered azure instead of orange — while
gamma-only delivered the same black-level improvement with no side effect. The
probable mechanism is `LCMCTRL (C0h) = 0x80`, which sets a bit the datasheet
maps as fixed zero in a register whose documented function is XOR-ing MADCTL
bits; this was not isolated individually because the block is rejected
regardless.

**Generic initialisation is retained only as the A/B control.** It, mode B and
mode D remain in the tree solely so the Phase C1 comparison can be reproduced,
and all three are gated in Kconfig on `PRD_DISPLAY_TEST_PATTERNS`, which only
the hardware-validation profile sets. Verified empirically: forcing
`CONFIG_PRD_DISPLAY_TUNING_VENDOR_EXACT=y` into a debug build is ignored and
the effective mode remains gamma-only, so **no debug or release image can
select a rejected experimental mode**.

**Residual limitation.** Even with gamma-only the panel does not match a
reference sRGB display exactly: the backlight is cool-tinted and RGB565
gradient banding remains (32 grey levels). **Colour-token compensation is
deferred until the production UI is rendered on hardware** — it is a Milestone
2 design decision, to be judged against the real composition rather than test
swatches. **No Project colour token was changed in this phase.**

### 2026-08-02 — Phase C, display bring-up (first GPIO driven on this board)

Firmware: `0.1.0-dev`, profile `hardware-validation`, ESP-IDF v5.5.4.
Panel driven through `esp_lcd` (ST7789 driver) on **SPI2 (FSPI)** via the GPIO
matrix. This is the first phase in which this project has driven any board
GPIO.

**SPI host choice.** SPI0 and SPI1 are reserved by the ESP32-S3 for the SPI
flash and the octal PSRAM, both proven active in Phase B, so they were never
candidates. SPI2 was chosen over SPI3 because it is the general-purpose host
ESP-IDF's own LCD support targets and it leaves SPI3 free for any later
peripheral. No LCD pin is a dedicated FSPI pad, so the bus routes through the
GPIO matrix — confirmed at runtime by `spi: SPI2 use gpio matrix`.

**SPI frequencies tested.** Each step re-initialised the panel from scratch and
displayed a 1 px checkerboard, colour bars and corner markers for 4 s each.

| Clock | Init | Image | Verdict |
|---|---|---|---|
| 10 MHz | ok | clean | pass |
| 20 MHz | ok | clean | pass |
| 40 MHz | ok | clean | pass — **selected default** |
| 80 MHz | ok | clean | pass (demonstrated ceiling, held in reserve) |

40 MHz is the configured default rather than the 80 MHz ceiling. Rationale: the
bus runs through the GPIO matrix rather than IOMUX, which erodes timing margin
at the top of the range; SPI stability with Wi-Fi active is still unvalidated
and explicitly deferred by Architecture Spec §25.5; and the product is a
mostly-static glanceable display (Architecture Spec §17.6) that needs nothing
close to the available bandwidth — 40 MHz already redraws a full 320×240 frame
in roughly 31 ms. Raising to 80 MHz stays available once Wi-Fi coexistence is
proven.

**Repeated initialisation.** 23 consecutive full init → draw → deinit cycles
across all four frequencies: **23 attempts, 0 failures**, no faults, no
watchdog events, and the main-task heartbeat continued uninterrupted
throughout.

**Visual results** (observer: project owner, panel viewed straight-on, ribbon
exiting right):

- orientation, offsets, clipping — corner markers landed in the correct logical
  corners and the 1 px border was intact on all four edges, so `gap 0,0` is
  right and nothing is clipped;
- colour order and byte order — red rendered red and blue rendered blue;
- inversion — dark field with white border, so `invert_colour = on` is correct;
- pixel alignment — 1 px checkerboard crisp and uniform, no shimmer or moiré;
- colour bars — eight clean bars in the expected order (white, yellow, cyan,
  green, magenta, red, blue, black). Their direction independently confirms the
  horizontal axis: the white bar (logical x=0) appears on the edge **away from
  the board/USB** and the black bar (logical x=319) on the **board/ribbon**
  edge. **Enclosure consequence: the PCB sits to the right of the viewed
  image.** Flipping that would require inverting `mirror_x`/`mirror_y`;
- backlight — illuminates when IO5 is driven high, confirming the
  schematic-derived active-high polarity.

**Findings recorded, not defects:**

1. **Panel mutes the accent colour.** Compared side by side against `#F77F2F`
   rendered on a reference sRGB display, the panel's orange reads visibly
   duller and more yellow. The pipeline is proven correct (the value
   round-trips to R247 G125 B41), so this is the panel's own rendition —
   a cool-tinted backlight plus esp_lcd's generic ST7789 init, which applies
   no panel-specific gamma table. Waveshare's own demo does apply one; adopting
   it is a candidate improvement that needs its own approval and validation
   pass. Whether to adjust `color.accent` for this hardware is a **Milestone 2
   design decision** and no accepted colour token was changed here.
2. **Gradient banding is visible.** Inherent to RGB565: a neutral ramp has only
   32 distinct grey levels, so across 320 px each step spans ~10 px. Not a
   defect, but relevant to Milestone 2 because the accepted artwork uses soft
   glows and a horizon gradient; dithering or gradient-light compositions may
   be needed.
3. **Backlight non-uniformity / glow** is visible on dark fields. Expected for
   a panel of this class; quantified properly in Phase D.

**Bug found and fixed during this phase.** The first attempt initialised the
panel directly on the ESP-IDF main task and crashed with
`Guru Meditation Error: Cache error / MMU entry fault` inside
`esp_intr_alloc`. Root cause: the main task stack is 3584 bytes, which
esp_lcd initialisation plus this project's `std::ostringstream`-based logging
overflows; the FreeRTOS canary only checks at context-switch time, so the
overflow faulted before it could be detected. Fixed by running the validation
flow on a dedicated 8 KB task, which also matches the task model in
Architecture Spec §10. **Carry-forward for later phases: any component doing
esp_lcd, LVGL or networking work needs its own task with an explicitly sized
stack; the main task is too small.**

**Tooling note.** One capture appeared to show the board dropping out after
~92 s into `DOWNLOAD(USB/UART0)` mode. That was a host-side artifact — two
capture processes held the serial port simultaneously and their DTR/RTS
toggling pushed the chip into the ROM loader. Not a board fault; the clean
re-run completed 23/23 cycles.

Evidence: [clock sweep and repeated initialisation](evidence/2026-08-02-display-clock-sweep.log).
Added during Phase G closure — the file was written in Phase C but never cited,
which broke this record's own rule that every evidence file be referenced from a
log entry or table row.

### 2026-08-02 — Phase C0, schematic pin mapping (analysis only)

No GPIO driven, no hardware exercised. Method: `poppler 26.07.0` installed;
`pdftotext -bbox-layout` used to obtain text with coordinates, from which net
labels were paired with MCU pin labels by row alignment; `pdftoppm` renders at
300 dpi used for visual confirmation of the GPIO allocation table, connector
pinouts, backlight driver, button circuits and power latch. Schematic SHA-256
re-verified before use.

Cross-checks performed:

- **Two independent methods agree.** Coordinate-based net/pin pairing and the
  schematic's own GPIO allocation table produce identical results for every LCD
  and SD signal.
- **Board photographs.** The J8 header silkscreen
  (`IO15 IO18 RXD TXD SDA SCL 3V3 GND D+ D- VBus GND`) matches the schematic's
  J8 net list; the empty 6-pin connector beside the LCD ribbon matches L5; the
  three buttons match `Key2` (BOOT), `Key3` (RESET) and `Key1` (BAT_PWR).
- **Official documentation.** The onboard-resources list (QMI8658 IMU,
  PCF85063 RTC, PCM5101A DAC, TF slot, battery management) matches the
  peripherals the schematic attaches to IO6–IO18, IO38 and IO47/48. The
  documented "I²C cannot be remapped" note matches IO10/IO11 being shared by
  IMU, RTC and header.
- **Phase B runtime evidence.** "Internal occupancy" on IO33–IO37 is consistent
  with the octal PSRAM that Phase B proved active; IO19/IO20 as USB D−/D+
  matches the native USB Serial/JTAG used for all Phase B work.
- **Repository.** `board_config.h` and `board_pins.h` updated; no prior
  constant contradicted the schematic (all pins were previously unassigned).
- **Project Sources.** Prototype Hardware Selection v0.1 expects an
  ST7789-family SPI panel with GPIO-controlled backlight through a transistor
  stage — the schematic confirms a 4-wire SPI connector and an NPN low-side
  backlight driver, so no Project Source is contradicted.

### 2026-08-02 — Phase B, first hardware bring-up (Bootstrap Spec §19.8)

Firmware under test: `0.1.0-dev`, git `88d7b3e095b4`, `dirty=false`,
profile `hardware-validation`, ESP-IDF v5.5.4, board target
`waveshare_esp32_s3_lcd_2_8`. Host: macOS on Apple silicon, ESP-IDF v5.5.4,
esptool v4.12.0.

| §19.8 criterion | Result |
|---|---|
| Board enumerates over USB-C | ✅ VID `0x303A` / PID `0x1001`, native USB Serial/JTAG, no driver needed |
| Image flashes without external programmer | ✅ direct over the board's USB-C connector |
| Serial monitor works | ✅ boot log captured over the same interface |
| Firmware boots repeatedly | ✅ 25/25 clean boots — 20 soft resets + 5 cold power cycles |
| Actual flash capacity reported | ✅ 16,777,216 bytes, `result=pass` |
| Actual PSRAM capacity reported | ✅ 8,388,608 bytes, `result=pass` |
| Stable ≥ 30 minutes | ✅ 31.8 min continuous uptime, 192 heartbeats, 0 reboots, 0 faults, 0 link drops |
| No display or unvalidated GPIO driven | ✅ firmware drives none; all pins remain `kUnassignedPin` |
| Findings recorded | ✅ this document + `evidence/` |

Procedure and observations:

1. Read-only identification (`esptool.py flash_id`) before any write —
   [chip-identity](evidence/2026-08-02-chip-identity.log).
2. Full factory-image backup read (16 MB) prior to overwriting; kept outside
   the repository, md5 `09d82cab47464fc7e48ff0138ba5d72e`. Doubles as the
   capacity-aliasing check in note 2.
3. `erase_flash` (4.0 s) before installing, because the factory partition
   layout differs from ours and its residue would otherwise land inside our
   `nvs`, `coredump` and `assets` regions. Confirmed effective — the first
   boot reported `Blank core dump partition!`.
4. `write_flash @flash_args`: bootloader `0x0`, partition table `0x8000`,
   initial otadata `0x10000`, app `0x20000`. All four regions hash-verified
   by esptool. Total write time 3.8 s.
5. First boot from `ota_0` at `0x20000`; full log in
   [first-boot](evidence/2026-08-02-first-boot.log). Printed partition table
   matches `firmware/partitions.csv` exactly.
6. 20 soft-reset cycles, each asserting `flash…result=pass`,
   `psram…result=pass`, `phase=ready` and no fault output —
   [soft-reset-cycles](evidence/2026-08-02-soft-reset-cycles.log).
7. 5 cold power cycles (physical unplug/replug) —
   [cold-power-cycles](evidence/2026-08-02-cold-power-cycles.log). Cold boots
   are identified by the firmware uptime counter restarting; observed values
   10,821 / 10,829 / 10,830 / 10,829 / 10,829 ms, a 9 ms spread indicating a
   deterministic power-on path.
8. 32-minute soak — [stability-soak](evidence/2026-08-02-stability-soak.log).

Accepted deviations and recorded limitations of this run:

- **Cold cycles: 5 instead of the 20 in the approved Milestone 1 plan —
  owner-approved deviation, closed.** The project owner explicitly accepted
  the 5 recorded cold power cycles together with the 20 soft-reset cycles and
  the 30+ minute soak as sufficient evidence for §19.8 (2026-08-02). This is
  a settled decision, not an outstanding task: **no further cold-cycle
  testing is required for Phase B acceptance.** Recorded for transparency:
  power-on repeatability rests on a 5-sample basis, so a rare power-up
  failure mode would be less likely to surface than under the originally
  planned 20 cycles. The 5 observed cold boots were tightly deterministic
  (uptime spread of 9 ms), and soft-reset repeatability is unaffected.
- **Cold-boot reports are not directly capturable.** With native USB
  Serial/JTAG, anything printed before the host opens the port is discarded,
  and bootloader output is unrecoverable in principle. Cold boots are
  therefore evidenced by the uptime counter restarting, which proves the
  firmware traversed the full boot path (the heartbeat loop is only reachable
  after `probe_hardware()` and `phase=ready`). It does **not** independently
  re-verify the capacity checks on those five boots, because the firmware
  deliberately stays alive on a failed check. Capacity-check evidence rests
  on the 20 captured soft-reset reports and the first-boot log.
- `usb_console` self-reports `unvalidated` in firmware output by design —
  the firmware cannot know whether a human successfully flashed over the
  port. Human evidence in this run confirms it works; the runtime line is
  expected to keep saying `unvalidated`.

## First-flash record (Bootstrap Spec §16.8)

- **Board SKU and visible revision:** SKU 30223, `ESP32-S3-T…-LCD-2.8` silkscreen, QC sticker "16"
- **USB device identity:** Espressif `0x303A:0x1001`, "USB JTAG_serial debug unit", CDC; `/dev/cu.usbmodem2101`
- **Flash command used:** `esptool.py -p /dev/cu.usbmodem2101 write_flash @flash_args` from `firmware/build/hardware-validation/`, preceded by `esptool.py -p … erase_flash`
- **Detected flash size:** 16 MB (16,777,216 bytes)
- **Detected PSRAM size:** 8 MB (8,388,608 bytes), octal, 80 MHz
- **Reset behaviour:** esptool auto-reset via RTS; board resets and re-enumerates reliably
- **Manual BOOT action required:** none

## Evidence convention

Evidence files live under `hardware/validation/evidence/` and are named:

```text
YYYY-MM-DD-<short-topic>.<ext>
```

Rules (Bootstrap Spec §16.8):

- redact machine paths, usernames and serial numbers before committing
  (the chip MAC address is redacted as `<redacted-mac>`);
- no credentials, Wi-Fi names or real printer payloads;
- each evidence file is referenced from at least one Validation log entry or
  table row;
- large binaries (for example the 16 MB factory backup) and vendor-copyrighted
  documents are kept outside the repository and cited by checksum.

---

## Milestone 2 Phase C — Inter typography and the first production scene

Date: 2026-08-06
Image: hardware-validation, app 0xe8020
Result: **PASS**, after six defects found by running it — four of which nothing else could
have caught.

### What was validated

- Boot sequence unchanged from Milestone 1: `phase=initializing`, `hardware-initializing`,
  `starting-services`, `ready`, with flash and psram `result=pass`.
- **Typography.** All seven generated Inter faces report `tabular=yes` with digit
  `min == max == advance`, and `fits_row=yes` against their §4 line-height tokens.
  Measured advances: hero_68 44 px, display_36 23, title_20 13, section_16 10, body_14 9,
  caption_12 8, micro_10 7.
- **The Printing composition** rendering the real fixture data through the real store,
  reducer, projection and renderer: ring, percentage, state title, remaining duration and a
  three-row telemetry column.
- ui/display cross-check agreeing exactly across the soak — `ui_submitted == panel_submitted`
  and `ui_completed == panel_completed`, with no spurious completions and no wait timeouts.
- Task stacks, LVGL pool, internal heap and presentation intake over a continuous soak.

### Defects found by running it

1. **The progress ring drew nothing at all.** `lv_arc_set_bg_end_angle()` contains
   `if(end > 360) end -= 360;` — one subtraction, not a normalising loop — so
   `lv_arc_set_bg_angles(arc, 270, 270 + 360)` set start equal to end and produced a
   zero-extent arc. The composition rendered with no ring on it and **passed a clean build,
   350 host tests, clang-tidy, the token guard and the flush/transfer cross-check**, because
   none of them can see that a correctly flushed band contained no arc. Found by looking at
   the panel. Fixed with `lv_arc_set_rotation()`; a static_assert now carries the trap.

2. **A device fault pinned the panel permanently.** The `recovery-and-device-fault` scenario
   raises a fault, which is terminal by design (UIAD 001 §10, §16.1) because on real hardware
   the recovery is a reboot. `FakeSource::load()` restarts the script but not the model, so
   every later scenario resolved to `layer=device-fault` and no scene was ever drawn again:
   **160 presentations at device-fault against 58 at subject-presentation over ten minutes,
   with the last Printing frame at t=47 s of 600.** Fixed in the reducer — entering
   `BootPhase::Starting` clears the device-owned conditions, which is a no-op on real hardware
   because a boot starts from a default `AppState`, and a genuine fault is re-raised by the
   hardware probe moments later. Eight host tests added.

3. **The hardware-validation profile would have built an image with no renderer.** The panel
   ownership exclusion moved to compile time, and the profile still set
   `PRD_DISPLAY_TEST_PATTERNS=y` — harmless under Milestone 1's run-time precedence, fatal
   under the new gate. Caught by the measured image size and nothing else. Both contradictory
   combinations are now `#error`s, verified to fire.

4. **§4's `line.micro` cannot hold its own face.** Inter at 10 px rasterises to a 13 px line
   box; the token says 12. Found by a start-up check comparing each face's actual
   `line_height` against its token. The other five sizes match §4 exactly.

5. **The hero line-height token was set from a predicted ratio and was 3 px short.** Inter's
   tabular advance is 0.653 em, not the 0.594 the font tables implied. Same check caught it.

6. **The percentage sat 20 px right of the ring's centre.** A fixed three-digit field padded
   the unused hundreds column. Measured, not guessed: the layout report gives each element's
   box and its clearance against the arc's chord.

### Composition review outcome

Reviewed against the accepted UI Design Decision 001 artwork as a whole, on the owner's
instruction, after per-element tuning stopped converging. Four changes:

- the ring's track returned to `opacity.low` so the ring reads as an **arc**, not a closed
  circle — a legible track had made the whole frame heavy, and a quiet track is also what
  lets the ring overlap the telemetry column, which is how the artwork buys its width at
  320 px;
- the telemetry column **right-aligned** to the content edge, which gives the frame an edge
  and is the better reading of §4's "units shall not alter numeric alignment";
- telemetry values to `color.secondary` and labels to `tertiary`, so the primary value is the
  only element at full luminance;
- the state title stepped from 20 px to 16 px, taking the numeral-to-title ratio to 4.25:1.

### Measured figures

| | |
|---|---|
| Draw buffers | 2 × 25,600 B, 40 rows, internal DMA |
| Internal heap consumed by display + renderer | 65,976 B |
| Generated Inter face data in the image | 29,323 B (six faces, before hero_68) |
| `main` | 1,672 / 3,584 (53 % headroom) |
| `prd_app` | 6,588 / 12,288 (46 %) |
| `prd_ui` | 5,148 / 8,192 (37 %) |
| `prd_render_rep` | 3,012 / 8,192 (63 %) |
| ui/display cross-check | agree, 0 spurious, 0 wait timeouts |

### Differences from the artwork, carried to Phase D

Not defects; deliberately not built in Phase C.

- No icons in the telemetry column. §8's Icon Rules constrain icons that exist but require
  none, and §12's colour-independence rule is satisfied by the text label.
- No hairline dividers between telemetry rows.
- No bloom or gradient on the progress arc. §6 prohibits decorative geometry and no token
  describes a glow; the artwork clearly has one.
