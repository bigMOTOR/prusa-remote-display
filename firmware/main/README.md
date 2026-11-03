<!-- SPDX-FileCopyrightText: 2026 Mykola Fiantsev -->
<!-- SPDX-License-Identifier: CC-BY-4.0 -->

# main

The `main` component is the ESP-IDF composition root.

It contains only `app_main()` and delegates firmware startup to `app_core`.

Production application logic belongs in components under `firmware/components/`.
