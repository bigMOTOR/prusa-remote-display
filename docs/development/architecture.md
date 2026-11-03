<!-- SPDX-FileCopyrightText: 2026 Mykola Fiantsev -->
<!-- SPDX-License-Identifier: CC-BY-4.0 -->

# Firmware architecture map

If you want to change how the display behaves, start here. For building it,
use the [development setup](macos-setup.md); for assembling and using the device,
use the [build guide](../get-started.md).

## From printer to pixels

```text
PrusaLink HTTP response → decode and normalize → application event queue
  → app_store + app_model reducer → Presentation → UI queue → LVGL → display
```

`app_core` composes the services and drains the event queue on the application
task. That task is the single writer of the store. Network and button work post
events rather than changing the state directly. The model projects state into
a screen presentation; the separate UI task consumes the latest presentation,
runs LVGL and owns the backlight ramp. Slow network requests do not run inside
the renderer.

## Where to look

All paths below are under [firmware/components](../../firmware/components).

| Component | Job / useful starting point |
| --- | --- |
| [prusalink](../../firmware/components/prusalink) | Request construction, response decoding and printer-state normalization |
| [app_model](../../firmware/components/app_model) | State, events, reducer, presentation and display-power policy; host-testable logic |
| [app_store](../../firmware/components/app_store) | Authoritative state and revision/observer handling |
| [app_core](../../firmware/components/app_core) | Startup, event plumbing, service orchestration and policy-to-adapter wiring |
| [ui](../../firmware/components/ui) | Screen controller, scenes, formatting, theme, transitions and the LVGL task |
| [network](../../firmware/components/network) / [provisioning](../../firmware/components/provisioning) | Wi-Fi and the local setup/management web page |
| [config](../../firmware/components/config) / [storage](../../firmware/components/storage) / [secrets](../../firmware/components/secrets) | Configuration types, persistence and credential handling |
| [board](../../firmware/components/board) / [display](../../firmware/components/display) / [button](../../firmware/components/button) | Hardware-specific pins, panel transport and input |
| [ota](../../firmware/components/ota) | Firmware discovery, signed updates and rollback handling |
| [validation](../../firmware/components/validation) / [fake_source](../../firmware/components/fake_source) | Deliberate development fixtures, distinct from live printer observations |
| [diagnostics](../../firmware/components/diagnostics) / [json_reader](../../firmware/components/json_reader) / [platform](../../firmware/components/platform) | Logging and diagnostic records, bounded printer-JSON reading, and clock/system adapters |

Change a state rule in `app_model`, not in a drawing callback. Change a screen
composition in `ui`, not in the HTTP decoder. Keep ESP-IDF calls in adapters so
the same product logic can be tested on a host. Only the UI task calls LVGL;
only the display adapter calls `esp_lcd`; pin numbers belong to `board`.

## Checks and decisions

Use [Contributing](../../CONTRIBUTING.md) for the applicable gates. UI work also
needs [renderer checks and visible frame inspection](ui-rendering.md).
[ADR-0003](../decisions/0003-separate-host-testable-logic-from-adapters.md) explains
the logic/adapter boundary, and [ADR-0005](../decisions/0005-lvgl-integration.md)
explains LVGL ownership. The [decision index](../decisions/README.md) covers
configuration, recovery, updates and installation choices.

This map describes the checked-in implementation. It does not require access
to the project's unpublished design documents.
