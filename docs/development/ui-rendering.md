<!-- SPDX-FileCopyrightText: 2026 Mykola Fiantsev -->
<!-- SPDX-License-Identifier: CC-BY-4.0 -->

# Render and inspect the display UI

The host logic tests do not draw the screen. This separate suite runs the
production LVGL controller and scenes with host hardware stubs. Start with the
[development setup](macos-setup.md) and [architecture map](architecture.md).

## Run the gate

From the repository root, with Python 3 and Clang/Clang++ available:

```sh
bash ./firmware/tools/test-render.sh
```

This is the same entry point used in CI. ESP-IDF and a connected board are not
needed. The first run downloads the pinned, checksum-checked LVGL dependency;
later runs reuse it. Both builds run with AddressSanitizer and
UndefinedBehaviorSanitizer: the fixed 64 KiB LVGL pool and a CLIB variant where
AddressSanitizer can also bound-check individual allocator allocations.
It checks rendered behaviour and runs deliberately failing mutation controls.

Results are under `firmware/build/controller-tests-1/` and
`firmware/build/controller-tests-2/`: logs plus `controller-frames/`,
`access-frames/`, `message-frames/`, `firmware-update-frames/`,
`access-review-frames/` and `review-frames/`. A successful run ends with the
verification script passing; inspect any earlier failed assertion or sanitizer
report rather than treating frames alone as a pass.

This is a substantial compiled suite, not a lightweight preview. The mutation
control scripts enforce a 4 GiB allocated-output budget per allocator; that is
not a cap on the whole build folder. Reuse the existing build paths. Do not
archive a full build copy after every run or purge the entire build folder:
it may contain signing material or irreplaceable hardware evidence.

## View a frame

Frames are raw 320 × 240 RGB565 pixels, not PNG files. List the frames, choose
one, then convert it with the included dependency-free Python utility:

```sh
ls firmware/build/controller-tests-1/controller-frames/*.rgb565
python3 firmware/tools/render-frame-to-png.py <chosen-frame.rgb565> <new-output.png>
```

Use an actual filename from the list in place of the placeholders. Open the PNG
in your normal image viewer. The converter defaults to the little-endian output
of the Mac and Linux renderer hosts and refuses to overwrite an existing file.
`--byte-order big` is available for frames from a big-endian host.

Inspect the relevant before/after states and transition frames, not just one
attractive still. The frames prove host rendering, not physical panel colour,
backlight, button feel, frame cadence or real-printer operation. Board checks
remain separate. See [Contributing](../../CONTRIBUTING.md) for other gates and
the [project overview](../../README.md) for current availability.
