<!-- SPDX-FileCopyrightText: 2026 Mykola Fiantsev -->
<!-- SPDX-License-Identifier: CC-BY-4.0 -->

# Architecture Decision Records

Technical decisions for the Prusa Remote Display firmware, one file per decision,
in the format required by Firmware Repository Bootstrap Specification v0.3 §18.

An ADR records a decision that is expensive to reverse and whose reasoning would
otherwise be lost — a version pin, a partition layout, a component boundary, a
security posture. It is not a design document and not a place for work in
progress.

## What lives here, and what does not

These are **tracked** and public. The accepted Project Sources (Product Vision,
Product Brief, the Firmware Architecture and Bootstrap Specifications, the UI
sources) and the working development notes are deliberately untracked by owner
decision, 2026-08-04; publishing them is a later open-source release milestone.
So an ADR may cite a Project Source by name and section, but must not assume the
reader can open it: state enough context to stand alone.

## Index

| ADR | Title | Status |
|---|---|---|
| [0001](0001-pin-esp-idf-v5.5.4.md) | Pin ESP-IDF to v5.5.4 | Accepted |
| [0002](0002-dual-ota-slots-no-factory-app.md) | Dual OTA slots without a factory application | Accepted |
| [0003](0003-separate-host-testable-logic-from-adapters.md) | Separate host-testable product logic from ESP-IDF adapters | Accepted |
| [0004](0004-stage-security-features.md) | Stage security features; do not burn eFuses during bootstrap | Accepted |
| [0005](0005-lvgl-integration.md) | Integrate LVGL 9.5.0 directly, owned by a single ui task | Accepted |
| [0006](0006-browser-regression-suite-toolchain.md) | Run the browser regression suite in CI on system Chrome and a pinned Node | Accepted |
| [0007](0007-persistent-lan-management-interface.md) | Keep the local web server alive after provisioning, read-only over the LAN | Accepted |
| [0008](0008-recovery-and-deliberate-reconfiguration.md) | Recovery, deliberate reconfiguration, and factory reset | Accepted |
| [0009](0009-warning-colour-and-recovery-heading-on-glass.md) | The warning colour and the recovery heading, decided on glass | Accepted |
| [0010](0010-direct-prusalink-for-mvp-connect-local-deferred.md) | Direct PrusaLink for the MVP; Prusa Connect Local deferred | Accepted |
| [0011](0011-completed-recession.md) | Twenty minutes of stable Dimmed for live Completed | Accepted |
| [0012](0012-browser-installation.md) | Static browser installation preparation | Accepted for local preparation |
| [0013](0013-desktop-installation.md) | Desktop installation application | Accepted for local implementation |

## Numbering

Sequential, four digits, never reused. `0000-template.md` is the template and is
not a decision.

0001–0004 are the records §18 requires of Milestone 0. They were written after
0005 — the LVGL decision was made during Milestone 1 Phase F, when the
Milestone 0 documentation batch was still outstanding, and it took 0005
deliberately so these four kept the numbers the specification assigns them. The
gap was intentional, not an accident of ordering.

## Superseding

A decision that replaces another sets its own Status to `Accepted`, names what it
supersedes, and the superseded record is edited to `Superseded` with a pointer
forward. Superseded ADRs are kept, not deleted: the reasoning that was once
correct is what explains the code a reader is looking at in an old tag.

ADR-0005 narrowly supersedes Firmware Architecture Specification §5.5 on where
LVGL integration lives. That is a Project Source rather than an ADR, so the
supersession is recorded in 0005 and in the development status notes.

## Writing one

Copy `0000-template.md`. Keep the section order — it is specified. Cite accepted
sources by section number. Record the costs alongside the benefits, and if a
decision rests on a measurement, give the measurement.
