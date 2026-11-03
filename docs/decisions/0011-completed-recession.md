<!-- SPDX-FileCopyrightText: 2026 Mykola Fiantsev -->
<!-- SPDX-License-Identifier: CC-BY-4.0 -->

# Completed display recession

## Status

Accepted by the owner on 2026-10-04. This records a narrow amendment to UI Behaviour
Decision 001 section 10.3; the synced Project Source remains unchanged.

## Date

2026-10-04

## Context

The owner found that a completed print disappeared before they next looked at the
desk display. The earlier universal 300-second stable Dimmed duration is too short
for that residual terminal context. This amendment does not retain an old job over
new live information.

## Decision

While the live, presentable printer state is Completed, keep the already dimmed
display visible for twenty minutes before fading to Off. Select this duration
centrally in the display policy. Do not add a separate scene, retained notification,
brightness level or timer that competes with the current printer state.

There are two explicit owner exceptions to UBD-001 section 10.3:

- The 2026-09-08 Completed acknowledgement uses Active brightness rather than
  the ordinary quiet idle tier. This pre-existing owner decision is unchanged.
- The 2026-10-04 Completed stable Dimmed hold is 1,200 seconds rather than the
  universal 300 seconds in the original Source.

The twenty-second acknowledgement and two-second fades are unchanged.
Stable Dimmed begins after the first fade, so sleep starts 1,222 seconds after the
last meaningful change and true Off is reached at 1,224 seconds. Ordinary eligible
idle states retain 300 seconds of stable Dimmed and true Off at 324 seconds.

Ready and a new print replace Completed immediately. Current-to-Degraded freshness
changes and routine polls do not restart recession. Unavailable uses the ordinary
schedule. Actionable conditions still hold the display awake. Explicit manual Off
is not overridden; a genuine state change or explicit wake uses the existing wake
policy. No historical Completed message survives a new live state.

## Consequences

Completed stays visible longer at the existing low brightness. It uses more panel
on-time than the ordinary idle schedule, without making all idle states linger.
The narrow owner exception must remain explicit rather than silently broadening
the old universal rule.

## Alternatives Considered

Lengthening the global idle timer would also keep Ready and unavailable screens
lit longer; the owner requested only Completed. A historical notification would
compete with Ready or the next print and is not needed.

## Validation / Follow-up

Host tests cover duration selection, exact boundaries, freshness, state changes,
holds and the ordinary schedule. The app adapter uses this central selector;
target compilation is required separately. The ESP-dependent app adapter call
site is not exercised by the host policy test or the renderer's separate policy
harness. On the exact installed image, check that live Completed is still Dimmed
600 seconds after the last meaningful change, starts sleeping at 1,222 seconds,
and is Off at 1,224 seconds; then verify Ready/new print wakes and replaces it.
This physical check needs an explicitly authorized firmware installation;
host tests alone do not establish it. Updating the synced UBD-001 remains a
separate owner/source-maintenance action, not an edit to the read-only Source.
