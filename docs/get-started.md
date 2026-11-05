<!-- SPDX-FileCopyrightText: 2026 Mykola Fiantsev -->
<!-- SPDX-License-Identifier: CC-BY-4.0 -->

# Build and set up your display

Build your display without compiling code: buy the parts,
print the case, put it together, install ready-made firmware and connect it to
your printer over Wi-Fi.

## Choose the right parts

Use the official [Waveshare ESP32-S3-LCD-2.8 product page](https://www.waveshare.com/product/mcu-tools/development-boards/esp32-s3-touch-lcd-2.8.htm)
and choose **Without Touch Function**. The board model is
**ESP32-S3-LCD-2.8, non-touch**. You can buy it from any seller; check the
exact model and option before ordering. The non-touch board is the tested
configuration. The touch version has not been tested with this firmware or
enclosure; compatibility has not been established. Other ESP32 boards remain
outside the supported configuration. Printer testing covers
the **Prusa CORE One** with local PrusaLink access; MK4S and MINI have not been tested.

You will need a USB data cable for installation, USB power for everyday use and
a 2.4 GHz Wi-Fi network that lets the display reach your printer.

The [case guide](../enclosure/README.md) lists the four printed groups and
links the model files. Fasteners are four M2.5×6 ISO 7380-1 screws for the board,
two M3×8 ISO 7380-1 screws and two thin square M3 nuts for the cover.
See the [assembly instructions](../enclosure/assembly.md) for their exact roles.
Use a regulated 5 V USB power supply and a data-capable USB cable.

The main purchase is just the Waveshare board, at about US$20–26.
If you buy the other materials new too (filament, a cable, a USB charger and screws), budget about US$40–50 in total for one build, before shipping and tax.
See the [parts list and cost estimate](parts-and-cost.md) for quantities and buying assumptions. No soldering,
threaded inserts or glue are required for this enclosure assembly.

## Print and assemble the case

Follow the [printing guide](../enclosure/printing.md) for orientation and the
recommended settings, and the [assembly guide](../enclosure/assembly.md) for
removing the three temporary button pads, fitting the cassette from inside
before the board, and fastening the cover. The square nuts remain; there are
no printed nut keepers. Hold and align the nuts until the cover screws engage.

Check the assembled button and cover fit with power disconnected. The button
should move freely; do not force a stiff button.

## Install the firmware

Use a computer, a USB data cable and the browser installer linked from the
project's official [release notes](https://github.com/bigMOTOR/prusa-remote-display/releases).
Use Chrome or Edge (Safari and Firefox are not supported).
Read the release notes for the exact board and firmware version before
connecting the display.

1. Connect the non-touch display board directly to the computer with the data
   cable. Close serial monitors and other installers using its USB port.
2. Open the release's browser installer. Confirm the exact board, select its
   USB port and read the erase warning. ESP32-S3 detection alone cannot identify
   the board model.
3. Start installation and keep USB connected until it finishes.
4. Check that the display starts and shows its setup screen. If the screen stays blank after the installer confirms the write has
   completed, the board may still be in download mode; unplug and reconnect USB without
   holding **BOOT**.

Installation writes to the display, not the printer. Initial installation erases
the display's existing settings; the installer asks you to confirm the board,
USB connection and erase step before writing. Use the firmware for the exact
supported non-touch board and the version named in the official release notes.

The [developer first-flash guide](development/first-flash.md) is for bench setup
and recovery. It is not a required step in the normal ready-made installation route.

## Connect to Wi-Fi and your printer

Before joining the display's setup network, stay on your home Wi-Fi and collect
your home Wi-Fi details, the printer's IPv4 address and its PrusaLink password
(see [Where to find the printer details](#where-to-find-the-printer-details) below).
Check the printer's address and read any linked Prusa help while still on your
home network; those pages are not reachable through the setup network.

On first startup, follow the display's setup screen:

1. Connect your phone or computer to the `PRD-Setup-…` Wi-Fi network, using the
   setup-network password currently shown on the display, not your home Wi-Fi
   password. This network has no internet; stay connected if your phone warns you.
2. Open the address shown on the screen, normally `http://192.168.4.1`.
3. Enter your home Wi-Fi details and choose **Check this Wi-Fi**.
4. Enter your printer's local address, PrusaLink port and API key, then save.
   The key comes from the printer's PrusaLink settings, not your Prusa account password.

When the page says **Saved**, the setup network will close. Return your phone
or computer to your home Wi-Fi. Check that the display shows the printer's
current state and that it agrees with the printer itself.

### Where to find the printer details

On the reference CORE One, open **Settings → Network** on the printer:

- **IPv4 Address** is the address to enter in the setup page. Copy the address
  alone, without `http://` or a trailing slash.
- Open **PrusaLink**. Its **Password** is the local PrusaLink credential used
  in the setup page's **API key** field, not your Prusa account password.
- Leave **Port** at its prefilled `80` for a normal direct PrusaLink connection.

You can check the address by opening `http://<printer-address>` in a browser
on your home network. If the page does not open, check the printer's network
connection using the guide below before retrying the display setup.
Prusa's [network guide](https://help.prusa3d.com/article/network-connection-core-one-l-core-one-mk4s-mk3-9s_736892)
shows where to find the address and login; its
[PrusaLink credentials guide](https://help.prusa3d.com/guide/sending-files-from-prusaslicer-to-prusalink_222961)
explains the password/API-key naming. The address can change after a network
restart, so check it again if a previously working connection stops.

If the connection check fails, the page explains whether the printer could not
be reached, the key was rejected or its response could not be used. Follow the
retry hint; the printer fields stay filled in. Leave passwords and API keys
out of any screenshots you share.

## Check your first print

Compare the display with the printer, then watch a print finish and check that
the display follows the next Ready or new-print state. The completed screen is
bright briefly, then dimly visible for twenty minutes, unless
a new live state replaces it. The display only asks for status; it never sends
printer-control commands.

## Change Wi-Fi or printer details later

Hold the display's button for about two seconds, then release it to open setup
again. Join `PRD-Setup-…` using the setup-network password currently shown on
the display; it can differ from last time and is not your home Wi-Fi password.
Follow the setup steps above. Opening setup does not erase your old settings;
they are replaced only when you save a successfully checked new configuration.

If you only opened setup to look and did not choose **Check this Wi-Fi** or save
anything, close the page and return your phone to home Wi-Fi. After an idle period
without choosing **Check this Wi-Fi** or saving, the setup network closes and a
configured display returns to monitoring. Reading the page does not keep it open; a short button press does not exit setup. If it expires before you finish, hold for two seconds
and release to reopen it, then use the currently displayed setup password.
If you already checked a replacement Wi-Fi network and then changed your mind,
see [leaving setup without saving](installation-and-recovery.md#leave-setup-without-saving).

A factory reset is a different action. Keep holding the button for about eight
seconds until **Erase all settings?** appears, then release it. To confirm, hold
it again until erasing starts, about eight seconds into that second press.
The second hold must reach that point while the confirmation is still visible;
it expires fifteen seconds after appearing. A short press cancels too. Reset
removes the display's saved Wi-Fi and printer settings, not its firmware.

If the display says **Settings cannot be used**, nothing has been erased — hold
the button for about two seconds and release it to set up again.
Do not force a stiff printed button; disconnect power and check the assembly.

## Updates and help

When online, the display checks GitHub roughly once a day for new firmware.
It installs nothing unless you choose to update. Printer monitoring does not
depend on that check.

For the **Install update** control, settings and reset behavior, and
checks to try if something goes wrong, see [Installation, updates and recovery](installation-and-recovery.md).

Something unclear in this guide? You can help without writing code. See
[feedback and contributions](../CONTRIBUTING.md), or return to the
[project overview](../README.md). This is an independent community project,
not affiliated with or endorsed by Prusa Research.
