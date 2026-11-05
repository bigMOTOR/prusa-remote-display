<!-- SPDX-FileCopyrightText: 2026 Mykola Fiantsev -->
<!-- SPDX-License-Identifier: CC-BY-4.0 -->

# Installation, updates and recovery

This guide is for the **Waveshare ESP32-S3-LCD-2.8, non-touch** display board,
the tested configuration. The touch version has not been tested with this
firmware or enclosure; compatibility has not been established. Other ESP32
boards remain outside the supported configuration. Installation changes the
display, not your printer. You should not need to compile firmware.
For parts, assembly and printer setup, start with [Get started](get-started.md).

Use Chrome or Edge (Safari and Firefox are not supported). Use the firmware
and browser installer identified by the project's official release notes;
source tags and development builds are not substitutes for a signed release.

## Choose installation or update

| What you need | Route | What happens to settings |
| --- | --- | --- |
| Install on a new board | Browser installer over USB | Initial installation erases existing display settings |
| Update a working display | **Install update** on its own setup network, if an update is offered | A firmware update is separate from factory reset; check the release notes for any setup changes |
| Change Wi-Fi or printer details | Hold the display button for about two seconds, then release | Existing settings stay until you save a successfully checked replacement |
| Deliberately remove saved details | The separate two-hold factory reset described below | Saved Wi-Fi and printer settings are erased; firmware is not |
| Recover a display that will not start | Start with the checks below; use the matching release recovery route | Reinstallation can erase settings; do not assume they can be recovered |

## Initial installation over USB

Use a computer running **Chrome or Edge**, a USB data cable and the
browser installer linked in the official release notes.

Before starting, have your 2.4 GHz Wi-Fi details and local PrusaLink details to
hand. You will need them after an initial installation. The tested printer scope
is the Prusa CORE One; MK4S and MINI compatibility has not been established.

To install:

1. Open the project's official [GitHub Releases page](https://github.com/bigMOTOR/prusa-remote-display/releases)
   and follow the browser-installer link in the matching release. Read that
   release's supported-board and installation notes.
2. Connect the supported non-touch board directly to the computer with a
   data-capable USB cable. Close serial monitors and other installers that may
   already be using it.
3. Confirm the exact board model, choose the connected display's USB port when
   the browser asks, and read the erase warning before proceeding. ESP32-S3
   detection alone cannot identify the exact board. If you cannot identify
   the board or its port, stop rather than select another device.
4. Start installation only after confirming that the display's existing settings
   may be erased. Keep the USB connection and power in place until the installer
   finishes. Use the release's supplied image, not an unsigned build or a file
   for another board.
5. Check that the display actually starts. A successful write message alone
   does not prove a healthy boot. If it remains in download mode after the
   write has finished, see the USB recovery checks below.
6. Follow the setup screen and the [Wi-Fi and printer setup instructions](get-started.md#connect-to-wi-fi-and-your-printer).
   Compare the display with the printer once setup is complete.

## Firmware updates over Wi-Fi

When online, the display checks GitHub roughly once a day. It installs nothing
without your choice. Printer monitoring stays on the local network and does
not depend on a successful update check.

Normal update discovery requires an official stable, immutable firmware release.
A draft, prerelease or mutable release is not accepted for that route. No update offer does not
by itself mean the display is faulty; there may be no eligible newer release,
or its internet check may not have succeeded.

### Install an offered update

The display's update notice reads **Update available** and **Hold for settings**.
You can also open setup to look for an offer. The offered version is shown on
the setup page.

1. Keep the display powered and connected to your home Wi-Fi for its internet
   download. Hold its button for about **two seconds, then release it** to open
   setup. Do not continue holding into the factory-reset prompt.
2. Join `PRD-Setup-…` on your phone or computer with the setup-network password
   currently shown on the display, not your home Wi-Fi password. Stay connected
   if warned that this network has no internet. The display can still use its
   own home Wi-Fi connection; your phone's setup connection is local only.
3. Open the address on the display, normally `http://192.168.4.1`. Under
   **Update available**, note the offered version. If there is no offer, use
   [Leave setup without saving](#leave-setup-without-saving) below.
4. Return your phone or computer to home Wi-Fi and read that exact version's
   notes on the project's official [GitHub Releases page](https://github.com/bigMOTOR/prusa-remote-display/releases).
   If you cannot find its notes, do not install it yet.
5. Rejoin `PRD-Setup-…` and reopen the displayed address. If the network has
   expired, hold the display button for two seconds and release, then join with
   the password now shown. Check that the offered version still matches the
   notes you read, then choose **Install update**. No new Wi-Fi or printer
   configuration needs to be saved to request an offered update.
6. Keep the display powered while it downloads, verifies, installs and restarts.
   The setup network and page connection can disappear on restart; return your
   phone or computer to home Wi-Fi. Success means the display shows the current
   printer state and agrees with the printer itself. To read **Firmware** again,
   hold for two seconds and release, join the newly opened setup network with
   its current password, and open the displayed address. Then leave setup below.

There is no manual-file upload or **Check for updates** button. No offer does
not call for reinstalling or resetting a working display.

### Leave setup without saving

If a configured display was working and you only opened setup to look, without
choosing **Check this Wi-Fi** or saving anything, close the page and return your
phone to home Wi-Fi. After an idle period without choosing **Check this Wi-Fi**
or saving, the setup network closes and the display returns to monitoring.
Reading the page does not extend it.
A short button press does not close setup.

If you already checked a different Wi-Fi network and then abandoned setup, do
not assume that this idle exit restores the old connection. To restart using
the last successfully saved configuration, wait until checks finish and make
sure **no update, save or settings erase is running**, then unplug and reconnect
the display's power once **without holding BOOT**. Unsaved changes are lost.
This is a restart, not a settings erase or a guarantee of recovery from a fault.
Do not interrupt a running operation to leave setup.

### If an update is rejected or interrupted

The update path checks the release, board compatibility, image size, integrity
and application signature before selecting a new image to boot. A wrong,
corrupt or incompatible candidate should be rejected; do not bypass a refusal
or substitute an arbitrary firmware file. Keep the version and exact message
for a report.

An interrupted **download** and an unsuccessful **first boot** are different:

- Before the new image is selected, an incomplete or rejected download should
  leave the running firmware in service. Restore connectivity and check the
  display's state. Do not erase settings to fix a download problem.
- After a new image is selected and starts, first-boot health checks and
  bootloader rollback handling are intended to allow a return to a usable
  previous image when the new boot is not confirmed. This is conditional on
  a valid previous image and the boot state; it is not a guarantee for every
  failure, nor a backup of your settings.

If the display does not return to normal service, or its state is unclear,
record what happened and use the recovery checks below rather than repeatedly
installing, resetting or disconnecting it.

## Recovery: start without erasing

Do not use factory reset as the first response to an update or connection
problem. It removes credentials and cannot repair a damaged firmware image.

| What you see | What to try first |
| --- | --- |
| No USB device on the computer | Use a known data cable, connect directly without a hub, and try another USB port. Close another program that may hold the port. |
| A previously working display has gone blank or stopped responding | Briefly press and release the display button; if it stays dark, press once more. A very dim screen may switch off on the first press. If it stays unresponsive, reconnect power once **without BOOT**, only when no update, save or erase is running. If unsure, stop and report. |
| Blank screen after a completed USB write or a download-mode attempt | Once writing has finished and no update, save or erase is running, unplug and reconnect USB **without holding BOOT**. This exits the sticky download mode observed on the reference board. |
| Setup Wi-Fi has no internet | Stay connected; open the address displayed on the screen. Internet access on the phone is not needed for the local setup page. |
| Printer unavailable | Check that the printer is on, its current local address is correct, and both devices can communicate on the same network. Use the [printer-details guide](get-started.md#where-to-find-the-printer-details). |
| Wi-Fi or printer details have changed | Open setup with a two-second hold and release, then check and save the replacement details. |
| **Settings cannot be used** | Nothing has been erased. Hold for about two seconds and release to set up again. |
| Update rejected or failed | Preserve the message and version; check connectivity and the release notes. Do not bypass verification or erase the board. |
| Still blank, repeatedly restarting or not recovered after these checks | Stop and report the symptoms; use only the matching release recovery instructions. |

On the reference board, USB download mode can persist despite a software reset;
unplugging and reconnecting without BOOT was the way out during bench testing.
Do not disconnect power during an update,
configuration save or settings erase.

If a maintainer asks you to enter ROM download mode for an available, matching
recovery package: hold **BOOT**, unplug and reconnect USB, then release BOOT.
BOOT is on the edge opposite the USB-C connector; it is not RESET or BAT_PWR.
Do not force access through a stiff case button. A developer can follow the
[bench first-flash guide](development/first-flash.md), but its build tools and
commands are not a required user installation route. A whole-flash erase is
not a default recovery step.

## Factory reset — only when you want to erase settings

Opening setup with a two-second hold and release does **not** factory-reset
the display. To deliberately clear saved Wi-Fi and printer details:

1. Hold the display button for about **eight seconds**, until **Erase all settings?**
   appears, then release it.
2. To confirm, hold it again until erasing starts, about **eight seconds into
   the second press**. That point must be reached while the confirmation is
   visible; the prompt expires fifteen seconds after appearing. A short press
   cancels it too.
3. Follow the setup screen again and enter your Wi-Fi and printer details.

Factory reset removes saved configuration, not firmware. It does not install
an earlier version or guarantee recovery from a failed update. Do not force a
stiff printed button; disconnect power and check the assembly.

## Asking for help

Use [feedback and contributions](../CONTRIBUTING.md). Include the board's exact
non-touch model, firmware version if available, computer and browser, the route
you used, the exact visible message and whether a completed write was followed
by a USB unplug/reconnect. Say whether the display reaches setup or monitoring.

Never share Wi-Fi passwords, PrusaLink keys, raw settings backups or unredacted
serial logs. Review screenshots before posting them.

## Source and licences

The project is by **Mykola Fiantsev (bigMOTOR)**. Software and tools are
[GPL-3.0-only](../LICENSE), hardware designs are
[CERN-OHL-S-2.0](../LICENSE-hardware), and documentation is
[CC-BY-4.0](../LICENSE-documentation). The project name, logo and brand assets
remain reserved; this is an independent project, not endorsed by Prusa Research.
See [Licensing](../LICENSING.md) and [third-party notices](../THIRD_PARTY_NOTICES.md).

The [release validation notes](release-validation.md) keep the technical
validation history and publication checks separate from these operating
instructions.
