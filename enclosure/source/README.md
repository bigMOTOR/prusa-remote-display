<!-- SPDX-FileCopyrightText: 2026 Mykola Fiantsev -->
<!-- SPDX-License-Identifier: CC-BY-4.0 -->

# Display enclosure CAD source

This is a hybrid editable CadQuery 2.6.1 design, not a fully parametric
reconstruction. There is no native Fusion timeline file in this package.
Five native functional STEP bases retain the shell, rear cover, carrier,
cassette and button. `geometry.py` adds the internal shell gussets and
three removable button pads, and defines the accepted assembly datum and print
poses. There are four print groups: shell, rear cover, carrier and cassette.
The cassette group contains the cassette, functional button and three pads.
The pads have nominal 0.25 mm separation from the button.

The base STEP files are required source inputs. Keep `bases/` beside
the two Python modules; the generator resolves these
from its own location and has no dependency on a project checkout or history.
Changing the functional design or datum requires updating the STEP bases and
validating the assembled geometry; the fixed numbers are not design sliders.

Use an existing Python environment with CadQuery 2.6.1 and its OpenCascade
dependencies. PrusaSlicer 2.9.6 provides the native 3MF project serialization.
No runtime is bundled. From this directory, run:

```sh
python generate.py --output ../../enclosure-build --prusa-slicer /path/to/prusa-slicer
```

Use a fresh output directory. The four STEP files use the assembly datum;
the four standard CadQuery STL files use the accepted print poses.
`display-enclosure.3mf` contains the same four print groups with geometry and build transforms only. These nine files are the distribution exports. In this repository they are
under `../step/`, `../stl/` and `../3mf/`; copy only those final files when
updating the distribution. Use a build directory outside the source folders. The geometry
precursor, native project precursor, export log and export report are build
provenance and need not be distributed.

No printer, print or filament presets are bundled. Select the official profile
for your own printer, nozzle and material in your slicer; then apply the author's
textual starting recommendations in `../printing.md` if appropriate. Settings
for another machine are not guaranteed.
The generator exports a project only; it does not slice or generate G-code.

The 3MF performs exact coordinate indexing separately for each named solid.
It does not tolerance-weld, round, move, smooth, decimate or fill triangles.
The final 3MF excludes slicer configuration, object-setting metadata and source
paths. Core model provenance fields may remain; STEP headers use only the output
basename. The native intermediate is build provenance, not a distribution file.

CAD design source and exports: CERN-OHL-S-2.0. This document: CC-BY-4.0.
