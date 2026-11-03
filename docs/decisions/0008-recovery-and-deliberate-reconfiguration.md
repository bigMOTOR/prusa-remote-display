<!-- SPDX-FileCopyrightText: 2026 Mykola Fiantsev -->
<!-- SPDX-License-Identifier: CC-BY-4.0 -->

# ADR-0008: Recovery, deliberate reconfiguration, and factory reset

## Status

Accepted

## Date

2026-08-19

## Context

ADR-0007 made the local web interface persistent and left it read-only over the LAN. That was
deliberate but incomplete: with no way to re-enter setup, a configured device could not be
reconfigured at all, and the authorization mechanism the persistent surface implies was named but
not built.

Three requirements bear on closing that gap. A 2026-08-14 product requirement establishes that
later re-entry into provisioning requires deliberate physical presence through the device button,
with the exact gesture defined by the checkpoint that owns recovery. A second requires explicit
authorization for any endpoint that mutates sensitive configuration, with no user accounts and no
permanent web passwords, and states a strong preference for short-lived authorization established
by deliberate physical presence. A third, from the same day, forbids automatically erasing storage
and places user-consented recovery here.

The Architecture Specification adds four constraints: §14.1 lists deliberate user request and
migration needing intervention among the conditions that enter provisioning, and forbids erasing
configuration after temporary failures; §13.4 requires migration failure not to erase secrets
automatically, sends unrecoverable corruption to provisioning or recovery, and keeps factory reset
user-initiated; §5.6 proposes three button gestures and requires factory reset to be "difficult to
trigger accidentally" behind an on-screen confirmation sequence; §14.2 and §14.4 want the setup
access point stopped after setup and bounded when idle.

Two things the sources leave open had to be settled by the owner before implementation: the
confirmation sequence's actual inputs, which §5.6 names without defining, and the recovery
presentation, for which the Production UI Specification specifies no screen.

## Decision

**1. A long press is deliberate reconfiguration entry.** It settles §5.6's "long press: enter local
setup/diagnostics flow" in favour of setup rather than the Settings surface an earlier milestone
read it as. Entering reconfiguration is not destructive: nothing is erased or replaced until a new
validated transaction commits. UIAD 001's Settings surface remains in the architecture, has no
Production UI §9 screen, and is no longer reachable from the button. The development fixture that
demonstrated it was retired rather than left unable to reach its own state.

**2. The reducer records no state for that gesture.** Re-entry starts a radio and a privileged
server, which are side effects the reducer may not perform, so the composition root observes the
same event after it is applied and acts. The reducer's only job for a long press is to refuse it
meaning while a destructive confirmation is on screen.

**3. Recovery Mode may be entered automatically; privileged setup may not.** A stored configuration
that is invalid, needs a migration this firmware cannot perform, or cannot be read places the device
in Recovery Mode on its own. It raises no access point, exposes no mutable surface and erases
nothing — least of all when storage itself has failed, where writing to a store that has just failed
would be the worst available response. A deliberate long press is still required before anything
becomes mutable. This reconciles §13.4's "unrecoverable non-secret corruption enters
provisioning/recovery mode" with the physical-presence boundary the earlier rulings protect.

**4. The setup access point is the only surface a sensitive mutation may arrive on, and a long press
is what brings it into existence.** ADR-0007's arrival-surface rule is unchanged: the persistent
station/LAN interface stays read-only even while the access point is up. No LAN authorization token,
no grant window, and no relaxation of the surface check. Origin membership, JSON-only content type,
declared body size, the session token and the rate limit remain layered exactly as they were, in the
same order.

**5. The privileged surface has a bounded idle lifetime — ten minutes as a working baseline.**
Automatic polling is not activity: the served page fetches its status route once a second for as
long as a browser is open, and counting that would let a forgotten tab hold the privileged access
point open indefinitely. Only a submission the request guard allowed *and* the provisioning flow
accepted refreshes the deadline. A validation or commit in progress suspends expiry outright, and
accounting resumes when it settles. On expiry the access point stops, the persistent read-only
server continues, the active configuration is untouched and nothing is erased.

**6. Every re-entry mints a fresh ephemeral setup session.** A new random access-point password from
the hardware RNG, a new panel session and a new QR code built from it, never persisted and never
logged in any profile. A re-entry attempted while an access point is somehow still up is refused
rather than served with the previous password.

**7. Factory reset is confirmed by a second very long press within fifteen seconds.** The first arms
an on-screen confirmation; the second, inside that window, performs the reset. A short press cancels;
the window expiring cancels; a long press neither confirms nor cancels. **No brief press can ever
reach a wipe**, and a stale confirmation cannot be completed — the window is checked at the moment of
the second press, so a prompt left on screen is not a prompt that still works. Exactly one function
in the reducer sets the state the executor acts on, which makes the full set of preconditions
readable in one place and assertable by tests.

**8. The reset is explicitly key-scoped, and its inventory is closed.** It removes seven named keys —
the two activation markers, the pair-activation recovery record, both credential slots and both
configuration slots — plus its own journal. No partition is erased, no key is enumerated, no wildcard
is used, and no eFuse operation is performed. The OTA data, RF calibration, manufacturing partition,
coredump evidence and both app images survive, as do the two storage namespaces that are declared
but never written.

*Amended 2026-08-23, owner-approved, to add `system/pair.rec`.* Milestone 4 introduced a
pair-activation recovery record so that a power loss between the configuration and credential
activations can be resolved rather than leaving a mixed pair. Its payload is five `u32` fields — one
profile id and four generation numbers — so unlike the other six keys it carries no credential — and it is in the inventory anyway,
because **it changes what the next boot does.** Boot reconciliation reads it before the configuration
is loaded and will activate a pair from it. A reset that left it behind would produce one of two
outcomes, neither of which is a factory-fresh device: an activation rolled forward onto records the
reset had removed, or an unresolved recovery that sends the device to the storage-failure path. True
first-boot behaviour is the guarantee this decision makes, and after Milestone 4 that guarantee needs
this key. The amendment preserves an existing promise; it authorises nothing wider, and the six
original keys, the exclusions and the prohibition on wildcards and partition erasure are unchanged.

**9. The reset is interruption-safe, and journalled because R10 requires it to be.** The activation
markers are cleared first, so from that moment the device is un-provisioned and everything after is
inert cleanup rather than a window of half-configuration; the recovery record goes next, before the
records it names can be re-activated from it; and the credential records are then overwritten before
their keys are removed. A power loss partway would otherwise leave a readable
password on a device presenting as factory-fresh — but the store state it leaves is indistinguishable
from an abandoned first provisioning, which the automatic-erasure prohibition forbids touching. So a
journal key is written before anything is removed and cleared only on success, and resumption on the
next boot happens if and only if that key is present. A journal that cannot be written aborts the
reset before anything is removed.

*Amended 2026-08-23 alongside decision 8.* The recovery record is removed after the two activation
markers and before the records themselves. After, because until the markers are gone the device is
still configured and discarding the record would throw away the only means of repairing an activation
this reset had not yet reached. Before, because between those two points a surviving record still
names two provable generations, which reconciliation would roll forward into an activated pair. That
window is safe only because the journal is already armed and the boot resumes the reset **before** the
configuration load reaches reconciliation — so the ordering of those two boot steps is part of this
decision, not an implementation detail. Failure to remove the record leaves the journal armed, on the
same rule as every other step.

*Amended 2026-08-28, owner-approved, after audit item 5.* The journal is read as **four states, not
a boolean**, and only one of them authorises anything.

- **Absent** — the key is genuinely not found. No reset was authorised; the boot continues normally.
- **Armed** — the key reads back carrying exactly the value this firmware writes. **This is the only
  state that authorises erasure.** A foreign non-zero value does not: presence is not proof of
  consent, and the automatic-erasure prohibition asks for proof.
- **Unreadable** — the store returned a read failure rather than an absence.
- **Invalid** — the key reads back holding zero, or a value this firmware did not write.

A **readable armed journal authorises exactly one resume attempt per boot**, before configuration
loading. Per boot, not per lifetime: a later boot probes again, which is what makes an explicit
restart a real recovery for a transient fault, and why no automatic retry and no automatic reboot
exist anywhere on this path.

**Unreadable or Invalid performs no erase and blocks ordinary configuration loading.** The executor is
not called at all, because authorisation was never established. Neither may the boot proceed as though
nothing were owed: that is precisely what could not be established. The original rule stated only the
first half, and the resulting boolean probe reported an armed journal whose read had failed as an
ordinary absence — so the device loaded a valid configuration, presented normally, permitted every
mutation, and erased whatever had been configured in the meantime at a later boot. Both halves are the
decision.

**A failed resume does not cross into reconciliation.** Decision 9's ordering — resume before the
configuration load — was implemented and its OUTCOME was discarded, so a resume that failed fell
through into the very reconciliation the ordering exists to get in front of. The ordering and the
outcome are one answer: the configuration load is not reachable without it.

**A pre-journal write failure also faults until restart.** Nothing was removed and no resumable job is
owed, but a store that has just refused a write is not trusted for continued mutable operation, and
the device cannot distinguish a transient refusal from a persistent one without the retry this
decision forbids.

**No automatic retry and no automatic reboot loop.** A reset that could not finish stays where it is
and says so. The recovery is an explicit restart by the user.

This amendment governs the incomplete-reset lifecycle only. The erase inventory of decision 8 is
unchanged, and nothing here authorises any wider erasure or any new automatic action.

**11. The reset holds the product state exclusively while it runs.** *Added 2026-08-23 alongside
decision 8.* Milestone 4 gave the device a second thing that writes configuration: a printer
validation that activates a candidate pair when the printer answers. Those two operations must not
overlap. Before the first destructive step, and synchronously on the application task, every printer
and provisioning activity that could reach storage is quiesced — both worker sessions disarmed, the
worker's lease superseded without waiting on a socket, the queued reports drained, and the
transaction's retained credential wiped without publishing a setup failure over the reset's own
presentation. For the whole interval, including the post-erase presentation hold, no report may be
acted on, no candidate may be activated and no worker may be launched. The reset outranks both.

*Amended 2026-08-28 alongside the amendment to decision 9 above.* **The interval does not end when
the reset fails.** An incomplete reset retains this ownership, and a boot that could not establish the
journal's state establishes it, in both cases until an explicit restart. The barrier extends to those
two conditions and to no other device fault; a quarantined Wi-Fi driver in particular does not take
the device, because the setup surface is deliberately kept alive under that fault.

**And the retention does not rest on the presentation reason.** The device-fault reason is a single
mutable field that several paths assign, so an unrelated observation arriving after the failure could
replace it — releasing the barrier and re-admitting the destructive executor. Two separate guarantees
therefore hold: a **terminal reset-failure fact**, set when the failure is raised and cleared only by
an actual boot, which is what the executor consults; and a **precedence rule** on the fault reason
itself, so a reason that holds the device is not replaced by one that does not. Neither is derived
from the other.

**12. The reset is unreachable from every failure path.** Migration failure, Wi-Fi failure, storage
failure and every automatic recovery route reach Recovery Mode and stop. The executor runs only when
the confirmed state is set, and from the boot-time resumption a prior confirmation authorised.

*Clarified 2026-08-28 alongside the amendment to decision 9.* **"Reach Recovery Mode" describes the
routes that read a stored configuration and find it unusable, and those are unchanged.** Two routes
this decision did not distinguish do NOT reach Recovery Mode, and must not:

- a **confirmed reset that could not finish** — the store is not merely unusable, a destructive
  operation the user requested is unfinished;
- an **unreadable or invalid reset journal at boot** — the configuration was never read at all, so
  there is nothing to report as unusable.

Both settle into a device-owned fault instead. **The reason is that Recovery Mode's own copy says
nothing has been erased**, which is true on its routes and false on the first of these; and Recovery
Mode invites a long press that raises setup, which is precisely what must not happen while a journal
may still be armed. **The unreachability this decision protects is unaffected: neither new route can
start a reset, and the executor still runs only from a confirmed state or an authorised resumption.**

**13. Recovery shares the provisioning composition and gains Production UI §8's Status Message.**
UIAD 001 §12.3 already grouped Recovery Mode with the provisioning phases in one presentation tier,
and UIDD 003 holds that a failure state does not establish a different visual identity — but the
composition had no slot for an instruction, and an automatically-entered state on a blank panel is
not acceptable. §8 already specifies a Status Message component for Setup and Failure among its
uses; it is built here, in Body typography and existing semantic tokens, with no new role, colour,
animation or layout concept. It is drawn only in recovery, occupying the space the session rows
would use, so the two are mutually exclusive by construction. Its copy describes only behaviour
this firmware has: that nothing was erased, and that holding the button starts setup.

*Amended 2026-08-28 alongside the amendment to decision 9.* Two device faults gain the same
composition — one title and §8's Status Message, in the existing tier, with no new state, screen,
colour or layout concept:

| Fault | Title | Status Message |
|---|---|---|
| A confirmed reset that could not finish | **Reset could not finish** | **Restart the display.** |
| An unreadable or invalid journal, or a store that would not initialise | **Storage unavailable** | **Restart the display.** |

**Neither carries a persistence claim, in either direction.** The first cannot say nothing was
erased — on the route where the removals began, records really are gone — and it cannot promise the
old or new settings are safe, because neither is established.

**What the two may and may not share.** The storage route may **not** borrow the reset-specific
title: the journal was never read there, so no reset is proved to exist and "Reset could not finish"
would assert an operation the user may never have requested. The **actionable Status Message is
deliberately identical** — "Restart the display." is the one thing that works on both, and saying it
differently would be variation for its own sake. They nonetheless carry **separate text identifiers**,
so a future rewording of one cause cannot silently reword the other.

**The storage fault had no copy before this and drew a blank panel.** Giving it one is part of this
amendment, because the journal route settles there and a fault the user cannot see is not a fault
they can act on.

## Consequences

- A configured device can be reconfigured again, and a device whose stored configuration is unusable
  explains itself and offers a way forward instead of failing silently.
- The privileged window cannot be held open by an idle browser, and cannot be taken away
  mid-validation.
- Two corrections to the previous checkpoint were required and are included: the access point now
  selects combined access-point-and-station mode when a station is already running, so re-entry no
  longer drops the LAN interface it was reached over; and the setup URL is derived from the currently
  active access-point address rather than cached at server start, which is what makes the panel show
  an address on re-entry at all.
- Settings is unreachable until a checkpoint builds it a screen. That is a reduction in reachable
  surface, not a regression: it had no screen and no implementation beyond a state enum.
- The Wi-Fi password and API key are removed rather than abandoned, within the honest limit of the
  storage abstraction: entries are overwritten and then erased, but NVS reclaims pages on
  compaction, so under plaintext storage a dump taken before compaction may still contain residual
  bytes. **No physical flash scrubbing is claimed.**
- One code comment was corrected: the `system` namespace was documented as holding facts that
  survive a factory reset, and the two activation markers live there and must not.

### Divergences from Project Sources, recorded and not resolved here

None new. ADR-0007's three recorded divergences — §10.5's setup-only server, §5.11's provisioning
scope, and §14.4's bounded idle shutdown — stand unchanged, and this checkpoint implements the
bounded lifetime within the scope that ADR already recorded for it. §5.6's proposed long-press
semantics are settled toward setup rather than diverged from; no Project Source has been amended.

## Alternatives Considered

**A short or long press confirms the reset.** Rejected: a brief press one gesture away from a wipe
contradicts §5.6's "difficult to trigger accidentally", and a long press confirming would make its
meaning depend on modal state while it also means reconfiguration.

**A fourth button gesture for re-entry.** Rejected: §5.6 defines three, and adding one changes the
input contract to avoid making a decision the sources already point at.

**A presence-granted authorization window on the LAN surface.** Rejected: it satisfies the
authorization requirement no better than the access point already does, while making the LAN surface
mutable — the exposure that requirement exists to bound — and adding a grant, a lifetime and a
change to the arrival-surface rule.

**Automatically raising the access point on a faulted record.** Rejected: a device that cannot read
its configuration would start advertising a setup network and invite credentials it may also fail to
store, which is the concern that deferred these states to this checkpoint in the first place.

**Deactivating only — clearing the activation markers and leaving the records.** Rejected: the device
would look factory-fresh while the Wi-Fi password stayed readable in flash, which is not what a
person resetting a device before selling or lending it is asking for.

**Erasing the NVS partition.** Rejected outright: the storage layer was deliberately built with no
partition-level operation so that the automatic-erasure prohibition could not be broken by
forgetting, and a reset is not a reason to widen it.

**Resuming an interrupted reset without a journal.** Rejected: the store state it leaves is
identical to an abandoned first provisioning, so resuming unconditionally would erase records no user
asked to lose.

**A new recovery screen, or title-only recovery.** The first is the redesign the accepted rulings
forbid; the second ships an automatically-entered state with nothing the user can act on.

## Validation / Follow-up

Verified locally: 903 host tests, up from 892, including the confirmation sequence, the idle
lifetime, the reset inventory and interruption at every step of the reset. Negative-controlled:
removing the confirmation window fails the two staleness tests, and disabling the reset journal fails
the three interruption tests. One property test asserts that no sequence of short and long presses,
of any length, reaches a wipe. All firmware profiles build warning-free.

The bounded idle lifetime and the confirmation window are both reported as working baselines rather
than product policy; no accepted source determines either value.

Not yet verified on hardware. Physical acceptance belongs to the validation checkpoint, which already
schedules the items this touches. Findings belong in the board validation record.
