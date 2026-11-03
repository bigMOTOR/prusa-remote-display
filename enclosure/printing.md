<!-- SPDX-FileCopyrightText: 2026 Mykola Fiantsev -->
<!-- SPDX-License-Identifier: CC-BY-4.0 -->

# Printing the enclosure

The enclosure fits the **Waveshare ESP32-S3-LCD-2.8 non-touch** board.
This is the version printed and assembled by the project author, confirmed
on 3 November 2026. Only the two printed nut keepers were removed from the
plate; the remaining geometry is unchanged.

## Choose your files

Open [display-enclosure.3mf](3mf/display-enclosure.3mf) in PrusaSlicer.
This geometry-only file contains the orientation and plate placement of all four
groups, with no printer, filament, print or support settings:

| Group | Part | Quantity |
| --- | --- | ---: |
| shell | Main shell | 1 |
| rear-cover | Rear cover | 1 |
| carrier | Display carrier frame | 1 |
| cassette | Cassette with moving button and three temporary pads | 1 |

The button prints inside the cassette and stays captive after cleanup.
You need two thin square M3 nuts for assembly; there are no separate printed
nut keepers in this set.

For another slicer, use the [STLs](stl/). Their print orientation and placement
match the 3MF. Keep that orientation; do not use STEP as a plate-placement
reference — the [STEP files](step/) share assembly coordinates.
For the small cassette, preserve the positions of the button and pads in particular.

### Recognise the supplied plate

In a top view with **X increasing to the right and Y toward the back of the
bed**, the shell is at the front left, rear cover at the front right, carrier
at the back left and the small cassette at the back right. The
[assembly figure key](assembly.md#enclosure-cleanup-and-assembly) identifies the
four shapes, but its exploded positions are not print poses.

| Group | Face toward the bed in the supplied files | What stays above it |
| --- | --- | --- |
| Shell | Front surround around the screen window | Open rear, internal shelves and side walls |
| Rear cover | Broad outer rear face | Inner features and upper tongues |
| Carrier | Flat rear face of the frame | Board-mounting seats |
| Cassette | The three temporary pads lie flat at the bed | Captive button, guide and hooked legs; keep all five bodies together |

The geometry-only 3MF and source print-pose definitions place every group at Z = 0.
Their approximate overall heights in this orientation are **26.56 mm shell,
10.79 mm cover, 5.60 mm carrier and 11.05 mm cassette**. This is a digital
orientation check, not evidence of a successful print. Do not use a slicer's
“lay on face” or automatic orientation to replace these poses.

Two warning signs to check before printing:

- **A group has tipped onto an edge or changed height:** for example the shell's
  rear opening faces the bed, or the cassette's pads no longer lie at Z = 0.
  Reload the supplied pose rather than rotating to match the assembly picture.
- **Your sliced preview fills the moving button's clearance with added brim or
  support material, or joins it to the guide.** Inspect those gaps and the three
  pad-to-button separations; remove the added material setting before printing.
  The pads are separate temporary model bodies, not slicer-generated supports.

## Settings

The project author's recommended starting point for a **Prusa CORE One HF with a 0.4 mm nozzle**:

| Setting | Recommendation |
| --- | --- |
| Material | Devil Design PLA |
| Layer height | 0.12 mm |
| Perimeter speed | 40 mm/s |
| External perimeter speed | 30 mm/s |

In your slicer, select the official profile for your own printer, nozzle and
material. For CORE One HF / 0.4 mm, choose its official 0.15mm STRUCTURAL print
profile, then apply the layer height and speeds above as a starting point.
For a standard CORE One / 0.4 mm, choose that printer's official profiles and
slice for your material. No profiles, macros or settings are included in this
3MF. Your slicer may use its current selections or defaults when importing it;
check them before slicing. These recommendations are not a guarantee for your
machine or material.

Do not add a brim or supports inside the moving button's clearances.
The three existing pads are part of the cassette's print preparation and are
removed after printing. Keep the functional legs, hooks, guide and button.
Before printing, inspect the cassette layers and transition areas in your own slicing.

### Choose supports in your slicer

The 3MF supplies geometry only: it saves no support generation, support painting,
per-object settings or brim choices. The three pads are separate model bodies,
not slicer-generated supports. Inspect the sliced preview with your own settings.

The project author printed the rear cover with light supports under the screw
cutout: from the build plate only, a large separation distance and low density.
That is an **optional choice to make in your slicer**, not a requirement already
encoded in the 3MF or a guarantee that printing with or without those supports
will succeed on your machine. Keep support material out of the moving button's
clearances in either case.

After printing, continue with [cleanup and assembly](assembly.md).
