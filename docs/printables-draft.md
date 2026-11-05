<!-- SPDX-FileCopyrightText: 2026 Mykola Fiantsev -->
<!-- SPDX-License-Identifier: CC-BY-4.0 -->

# Prusa Remote Display — desktop status screen for Prusa CORE One

Your print, at a glance.

A small screen for your desk that shows what your printer is doing, how far
the print has progressed and how much time is left. No need to reach for your
phone or walk over to the printer.

It reads status directly from your printer over home Wi-Fi using PrusaLink.
There is no cloud account, subscription or separate server. It monitors one
printer and never sends printer-control commands. When online, it also checks
GitHub roughly once a day for firmware updates; it installs nothing unless
you choose to update. Normal monitoring stays on your local network.

**[Build and set up your display →](https://github.com/bigMOTOR/prusa-remote-display/blob/main/docs/get-started.md)**

## Get the parts

Use the **Waveshare ESP32-S3-LCD-2.8, non-touch** board. On the
[official Waveshare product page](https://www.waveshare.com/product/mcu-tools/development-boards/esp32-s3-touch-lcd-2.8.htm),
select **Without Touch Function**. Any seller is fine; the exact non-touch
model matters. The family-page URL says “touch”, but the selected option must
be non-touch for this tested build. The touch version has not been tested with
this firmware or enclosure; compatibility has not been established. Other ESP32
boards remain outside the supported configuration.

| Part | Quantity | Details |
| --- | ---: | --- |
| Waveshare ESP32-S3-LCD-2.8, non-touch | 1 | Without Touch Function |
| PLA for the case | 4 printed groups | Shell, rear cover, carrier, captive-button cassette |
| USB data cable | 1 | Data + power, USB-C at the board end |
| USB power supply | 1 | Regulated 5 V USB; follow the board manufacturer's power requirements |
| M2.5×6 ISO 7380-1 button-head screws | 4 | Board to carrier, into stock metal posts |
| M3×8 ISO 7380-1 button-head screws | 2 | Rear cover |
| Thin square M3 nuts, Prusa M3nS / DIN 562 type | 2 | Cover nuts; ordinary hex nuts do not substitute |

You also need access to a 3D printer, fine pliers, 1.5 mm and 2 mm Allen keys, a
computer for installation and 2.4 GHz Wi-Fi that lets the display reach the
printer. Tested printer scope is **Prusa CORE One with local PrusaLink access**;
MK4S and MINI compatibility has not been established. No soldering, threaded
inserts or glue are required for the case assembly.

The main purchase is just the Waveshare board, at about US$20–26.
If you buy the other materials new too (filament, a cable, a USB charger and screws), budget about US$40–50 in total for one build, before shipping and tax.
See the [parts and cost breakdown](https://github.com/bigMOTOR/prusa-remote-display/blob/main/docs/parts-and-cost.md).

## Choose the print files

Use **display-enclosure.3mf** for the geometry-only four-group plate in PrusaSlicer.
It includes print poses, with no printer, print, filament or support settings.
For another slicer, use **shell.stl**, **rear-cover.stl**, **carrier.stl** and
**cassette.stl**. Keep their supplied print orientation, including the button
and three temporary pads inside the cassette. Keep all five cassette bodies
together. The pads are model geometry, not slicer-generated supports. Print one
of each group.
There are no printed nut keepers; the two square M3 nuts remain in the build.
STEP files use shared assembly coordinates and are not print-orientation guides.
Editable design sources and the illustrated build guide accompany the files.

For **Prusa CORE One HF / 0.4 mm nozzle**, the project author's starting recommendation
is **Devil Design PLA, 0.12 mm layers, 40 mm/s perimeters and 30 mm/s external
perimeters**. In your slicer, select the official profile for your own printer,
nozzle and material. For CORE One HF / 0.4 mm, choose its official 0.15mm
STRUCTURAL profile, then apply the layer height and speeds above as a starting
point. For standard CORE One / 0.4 mm, select that machine's official profiles.
No profiles or macros are bundled; your slicer's current selections or defaults
may apply on import. Check them before slicing. These recommendations are not a
guarantee for your machine or material. Do not add a brim or supports inside the
moving button's clearances.
Inspect the cassette layers and transitions in your own slicing.

The project author printed the rear cover with light supports under the screw cutout:
from the build plate only, a large separation distance and low density.
This is an optional slicer choice. The geometry-only 3MF saves no support or brim
settings and does not encode that rear-cover support recipe. See the complete
[printing guide](https://github.com/bigMOTOR/prusa-remote-display/blob/main/enclosure/printing.md).
No G-code is supplied: slice for your printer and material.

## Put it together

Work with USB and other power disconnected. The
[illustrated cleanup and assembly guide](https://github.com/bigMOTOR/prusa-remote-display/blob/main/enclosure/assembly.md)
shows the parts and the three removable pads.

1. Remove only the **three temporary cassette pads**. Keep the captive button,
   guide, legs and hooks. Do not pull the button out or force a fused pad.
2. Fit the cassette **from inside the empty shell, before the board**. Both
   hooks must engage; do not push the cassette through the exterior button hole.
3. Place the two square M3 nuts in their channels. Hold and align them until
   the cover screws engage; there are no printed keepers.
4. Outside the shell, fasten the board to the carrier with four M2.5×6 screws.
   Insert the module from the rear, screen forward and USB toward its cutout.
   Hold the frame and edges; do not press the LCD.
5. Engage the rear cover's upper tongues first, then seat its lower edge.
   Start both M3×8 screws and tighten gently in alternation. Do not use them
   to force a gap closed or sink their heads into the cover.
6. Without power, check that the board is flat, the cover sits evenly and the
   button moves freely to the first BOOT click and releases. Stop at a hard stop.

The cover photo shows the current printed and assembled enclosure. Only the
two printed nut keepers were removed from the plate; the rest of the geometry
is unchanged.

Screen examples, rendered in software, show paused, completed and error states.
The assembly pictures are CAD illustrations of the printed parts.

## Install and connect

Use a computer and USB data cable. Open the browser installer linked from the
project's official [release notes](https://github.com/bigMOTOR/prusa-remote-display/releases)
for the exact non-touch board and firmware version. You do not need to compile code.

Use Chrome or Edge (Safari and Firefox are not supported).

The installer asks you to confirm the board, USB connection and erase step
before writing. Initial installation erases the display's existing settings;
it writes to the display, not the printer. Keep USB connected until it finishes.

Then follow the display's setup screen: join **PRD-Setup-…** using the password
shown on the display, open its local setup address (normally
**http://192.168.4.1**), check your home Wi-Fi and enter the printer's IPv4
address, PrusaLink port and API key. On CORE One, the PrusaLink **Password** is
the credential for the setup page's **API key** field; it is not your Prusa
account password. Collect these details while still on home Wi-Fi. After saving,
return to your home network and compare the display with the printer.

For the complete steps, updates, changing settings and recovery, use
[Build and set up your display](https://github.com/bigMOTOR/prusa-remote-display/blob/main/docs/get-started.md)
and [Installation, updates and recovery](https://github.com/bigMOTOR/prusa-remote-display/blob/main/docs/installation-and-recovery.md).
Hold the display's button for about two seconds and release to reopen settings.
Do not force a stiff printed button; disconnect power and check assembly.

## Licences and attribution

Original enclosure design, editable CAD and printable geometry:
**[CERN-OHL-S-2.0](https://github.com/bigMOTOR/prusa-remote-display/blob/main/LICENSE-hardware)**,
Copyright 2026 Mykola Fiantsev. Commercial use is permitted under its terms.
When distributing products made from the design, provide Complete Source or
tell recipients its Source Location. Modified covered designs remain under
that licence, subject to its Available Component exception.

Original firmware/software is **GPL-3.0-only**. Documentation, listing text,
original photos and CAD illustrations are **CC-BY-4.0**, with attribution to
Mykola Fiantsev. Project name, logo and designated brand assets remain reserved.
Third-party material retains its own terms. No PrusaSlicer presets are bundled. Keep the supplied full licence texts,
attribution and [third-party notices](https://github.com/bigMOTOR/prusa-remote-display/blob/main/THIRD_PARTY_NOTICES.md)
with the files. See the [licensing breakdown](https://github.com/bigMOTOR/prusa-remote-display/blob/main/LICENSING.md).

## Help and follow along

[Project on GitHub](https://github.com/bigMOTOR/prusa-remote-display).

Feedback and fixes are welcome — no coding needed. See
[Contributing](https://github.com/bigMOTOR/prusa-remote-display/blob/main/CONTRIBUTING.md),
and leave passwords and printer keys out of screenshots. On GitHub, choose
**Watch → Custom → Releases** to follow firmware releases.

This is an independent community project by Mykola Fiantsev, not affiliated
with or endorsed by Prusa Research.
