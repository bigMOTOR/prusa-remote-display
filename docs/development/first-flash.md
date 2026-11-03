<!-- SPDX-FileCopyrightText: 2026 Mykola Fiantsev -->
<!-- SPDX-License-Identifier: CC-BY-4.0 -->

# First Flash over USB-C

This is the developer bench route for flashing and monitoring the board from
macOS. It requires the development tools and is not the normal ready-made user
installation route. See [Installation, updates and recovery](../installation-and-recovery.md)
for user-facing status and recovery checks.

The bring-up observations below were recorded on the reference hardware during
Milestone 1; they do not validate a public installer or every recovery failure.
The measured results are in
[hardware/validation/waveshare-esp32-s3-lcd-2.8.md](../../hardware/validation/waveshare-esp32-s3-lcd-2.8.md).

**Board:** Waveshare ESP32-S3-LCD-2.8, non-touch (SKU 30223).

Complete [macos-setup.md](macos-setup.md) first.

---

## Build and run your own version over USB

You may modify and run your own firmware. The official OTA update checks the
publisher's signature, but hardware Secure Boot is not enabled in the reference
configuration. You do not need the publisher's private signing key for the
USB route below.

**Stage 5 validation pending:** the complete modified-build, USB install and
successful-run sequence on the final device will be checked during the physical
test. The earlier bring-up results do not certify this new sequence.

1. Follow [macOS setup](macos-setup.md), including **ESP-IDF v5.5.4** and its
   submodules. Get the reference source tag:

   ```bash
   git clone --branch v0.1.1-dev --recurse-submodules https://github.com/bigMOTOR/prusa-remote-display.git
   cd prusa-remote-display
   git switch -c my-firmware
   ```

2. Make your changes. For the physical test, choose a harmless visible change
   so that you can distinguish your build after reboot. Use the `debug` profile;
   the official `release` profile requires a clean tagged tree.

   ```bash
   . ~/Developer/esp/esp-idf/export.sh
   ./firmware/tools/build.sh debug
   ```

   The build configuration is saved as `firmware/build/debug/sdkconfig`.
   Keep the project's defaults for the Waveshare ESP32-S3-LCD-2.8 non-touch
   board. Do not enable hardware Secure Boot or flash encryption, and do not
   run eFuse provisioning commands.

3. Connect the board with a USB data cable, find its port as described below,
   and replace `<PORT>` with that port. Stop any other serial monitor first.

   ```bash
   idf.py -C firmware -B firmware/build/debug -p <PORT> flash monitor
   ```

   This writes your local build and boots it; it is not an official OTA
   installation. No publisher signing key or official installer package is
   involved. Do not erase the entire flash as a routine part of this route.

4. Check `profile=debug`, your build information and a healthy boot in the log.
   Confirm that the display runs your visible change, then unplug and reconnect
   power without holding BOOT and confirm that your build starts again.
   Exit the monitor with **Ctrl+]**.

If the board is in download mode, follow the manual recovery section below.
To return to the official firmware, use
[installation and recovery](../installation-and-recovery.md) with the matching
official USB package. An unsigned custom build is not promised to accept
official OTA updates; USB recovery remains the return route. Keep your current
settings before starting, and treat settings changes as part of the physical
test rather than assuming compatibility across arbitrary forks.

---

## What the hardware needs — validated answers

These were open questions before the board arrived. They are settled, and knowing
them saves a lot of guessing:

| Question | Answer |
|---|---|
| External programmer needed? | **No.** The exposed USB-C port flashes directly. |
| USB driver needed? | **No.** Native ESP32-S3 USB-Serial/JTAG enumerates as CDC with no vendor driver. |
| BOOT/RESET press needed to flash? | **No.** esptool's auto-reset connects, flashes and restarts unaided. |
| USB identity | Espressif `0x303A:0x1001`, "USB JTAG_serial debug unit" |
| Bus power sufficient? | **Yes.** Validated over a 31.8-minute soak with no resets or link drops. |

---

## 1. Connect

Use a **data-capable** USB-C cable directly between the Mac and the board. Avoid an
unpowered hub for first bring-up.

## 2. Find the port

Before connecting:

```bash
ls /dev/cu.usb*
```

Connect the board, then run it again. The device that appeared is the board.
Typically it looks like `/dev/cu.usbmodem2101` — the exact number depends on which
USB port you used, so discover it rather than assume it.

Substitute your port for `<PORT>` below.

Run the following build, flash and monitor commands from the repository root.
`-C firmware` selects the ESP-IDF project; the build wrapper does not change
your terminal's working directory.

## 3. Build

Use the `hardware-validation` profile for bring-up: it is the only profile that
compiles the validation flows and the LVGL validation scene.

```bash
. ~/Developer/esp/esp-idf/export.sh && ./firmware/tools/build.sh hardware-validation
```

## 4. Flash

```bash
idf.py -C firmware -B firmware/build/hardware-validation -p <PORT> flash
```

esptool resets the board, writes the four regions, verifies each by hash, and
restarts. No button press is involved.

## 5. Monitor

```bash
idf.py -C firmware -B firmware/build/hardware-validation -p <PORT> monitor
```

Exit the monitor with **Ctrl+]**.

Combined:

```bash
idf.py -C firmware -B firmware/build/hardware-validation -p <PORT> flash monitor
```

## 6. Confirm a healthy boot

The `hardware-validation` image should log, under the `prd.*` tags:

```text
prd.app:   Prusa Remote Display 0.1.0-dev
prd.boot:  version=... git=... dirty=... profile=hardware-validation idf=v5.5.4 board=...
prd.board: flash_detected=16777216 expected=16777216 result=pass
prd.board: psram_detected=8388608  expected=8388608  result=pass
prd.boot:  phase=ready
```

Both capacity checks must read `result=pass`. `usb_console=... result=unvalidated`
is expected and is not a fault — the USB console cannot verify itself from inside
the firmware that is using it.

The panel then lights and shows the validation scene.

---

## Serial capture, if you are recording evidence

Opening the port with default settings asserts DTR/RTS, which pulses the board and
can drop it into the ROM download loader. Clear both **before** `open()` — and
before `close()`, or closing the capture resets the board. This has been
self-inflicted three times in this project; it is not a hardware fault.

For anything longer than a few minutes, prevent the Mac from sleeping, or the
capture dies partway:

```bash
caffeinate -dimsu <your-capture-command>
```

Size the capture window for the human round trip. A window that closes before you
have finished pressing buttons records nothing and looks exactly like a firmware
fault.

---

## If the port does not appear

In order:

1. Confirm the cable carries data, not power only. This is the most common cause.
2. Connect directly to the Mac, not through a hub.
3. Try another USB-C port.
4. Press the board's RESET button once.
5. Check  → About This Mac → System Report → USB for the device.
6. Enter ROM download mode manually — see below.

---

## Manual recovery: ROM download mode

Only needed if the application is not running or the port never appears.

1. Hold **BOOT**.
2. While holding it, unplug and replug USB-C.
3. Release BOOT.

The board is now in the ROM loader. Confirm:

```bash
esptool.py -p <PORT> --before no_reset chip_id
```

Use the matching, verified firmware and its flash procedure. A whole-flash
erase is not a default recovery step: it destroys saved settings and can remove
data needed for diagnosis. Do not erase merely because an update failed or the
board is still in download mode. After a completed write, use the unplug/replug
step below to leave download mode.

**BOOT is the button on the edge opposite the USB-C connector.** Identified
electrically during Milestone 1 Phase E, by pressing it and observing GPIO0 edges.
The board has three buttons; the other two are RESET and BAT_PWR, and **BAT_PWR
does nothing visible** — if a press seems to have no effect, you are probably on
that one.

### Download mode is sticky — this is the important part

Once the ESP32-S3 has entered USB-Serial/JTAG download mode, it **stays there**.
Neither of these gets you out:

- an RTS reset from a serial tool;
- `esptool.py run`.

The only exit is to **unplug and replug USB-C without holding BOOT**.
Do this only after a flash write has finished, with no update, configuration
save or settings erase running; do not interrupt one to leave download mode.

You are in this state if the boot banner reads:

```text
rst:0x15 (USB_UART_CHIP_RESET),boot:0x22 (DOWNLOAD(USB/UART0))
```

A healthy application boot reads `boot:0x2a (SPI_FAST_FLASH_BOOT)` instead.

This surprised the project during bring-up and cost real time. If a board seems
bricked after a recovery attempt, power-cycle it before concluding anything.

---

## Recording a first flash

Bootstrap Spec §16.8 asks for the first successful log under
`hardware/validation/evidence/`, recording the board SKU and visible revision, USB
device identity, the flash command used, detected flash and PSRAM sizes, reset
behaviour, and any manual BOOT action required.

Before committing a log: **redact machine paths, usernames and serial numbers**, and
never commit credentials, Wi-Fi names or printer payloads. The MAC address counts
as a device identifier and is redacted in this repository's records.
