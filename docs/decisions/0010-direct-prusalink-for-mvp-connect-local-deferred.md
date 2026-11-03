<!-- SPDX-FileCopyrightText: 2026 Mykola Fiantsev -->
<!-- SPDX-License-Identifier: CC-BY-4.0 -->

# ADR-0010: Direct PrusaLink for the MVP; Prusa Connect Local deferred

## Status

Accepted

## Date

2026-08-22

## Context

On 2026-08-13 Prusa announced **Prusa Connect Local** under the heading "Prusa Connect Local to
replace PrusaLink" — a successor built from scratch, described as open, extensible and
multi-protocol, currently in early access
(https://www.prusa3d.com/p/prusa-connect-local/early-access/). The public page confirms its
topology:

- an **external self-hosted server** — a single Go binary on a Raspberry Pi, PC, NAS, Docker or
  Kubernetes; nothing is installed inside the printer;
- the **printer connects out to the server** over the native xBuddy/Buddy WebSocket protocol,
  plain or TLS — the only printer-facing protocol, with **no HTTP-polling fallback**;
- clients consume the server's **REST/OpenAPI, Server-Sent Events and MQTT** interfaces;
- printer firmware **6.4.0 or newer** is required; the CORE One family is supported;
- the full documentation and API reference are **private to early access** (a private GitHub
  repository, and a docs site served by a running instance), under the Open Community License
  v1.1, which is not an OSI-approved open-source licence.

This is the reverse of PrusaLink's direction: PrusaLink is an HTTP server embedded in the printer
that the display polls; Connect Local is a server the printer registers with. The server-side
xBuddy WebSocket protocol has no official public specification.

Two facts bound the decision. First, the product contract: the Product Brief requires that the
display communicate directly with the printer and that normal operation require **no cloud
account, no external server, no mobile application and no Prusa Connect** — and it already
requires the internal architecture to separate the UI and printer-state model from the transport
so another data source can be added later. Second, the current reality: the embedded PrusaLink
HTTP API ships today in current CORE One firmware (6.5.7), is documented in the official Knowledge
Base as the built-in local interface, and its OpenAPI specification is public. No official
deprecation or removal notice exists; "replace" is so far a statement about the product line's
direction, not about removing the embedded API from firmware.

(Naming hazard: an unrelated, obsolete MINI firmware feature from 2020–2021 was also called
"Prusa Connect Local". Community material conflates the two; only the 2026 self-hosted server is
meant here.)

## Decision

**1. Milestone 4 and the MVP integrate direct PrusaLink on the owner's CORE One.** PrusaLink is
the required normal mode because it is the only integration that satisfies the accepted
no-cloud, no-account, no-external-server contract with nothing but the display and the printer on
the network. The first goal is the shortest real vertical slice:

```text
PrusaLink response → decoder → normalized observation → AppState → production display
```

covering `/api/version` and `/api/v1/status`, API-key authentication, live idle/printing/error
states, and connection and stale-data handling.

**2. Prusa Connect Local is an explicitly deferred optional backend** for users who already
operate its external server. It is never a requirement of the normal product experience.

**3. No Connect Local code is written now.** No dual-backend runtime selection, no generic
backend infrastructure, no xBuddy WebSocket support, no REST/SSE/MQTT client, and no speculative
compatibility code.

**4. The seam is the existing observation/domain boundary, unchanged.** The fake source built in
Milestone 3 drives the real store exclusively through typed observation events; nothing in the
domain, reducer or UI knows a transport. A PrusaLink adapter — and any later Connect Local
adapter — is another producer of the same observations. Whether a more explicit backend interface
is warranted is deliberately left to the evidence of the first real implementation, not decided in
advance.

**5. Provenance rule for early-access material.** Private early-access documentation, code,
schemas, examples, or material derived from them must not be copied into this public repository.
The legal and licensing effect of merely holding early access is unknown until the actual access
terms are reviewed; nothing stronger is claimed. Applying for early access is a separate owner
decision for strategic visibility — the application form explicitly lists REST/API integration and
third-party product use cases — and Milestone 4 proceeds regardless of application or acceptance.

**6. A formal API Research revision is deferred** until a sufficiently public and stable Connect
Local contract exists to write one against.

## Consequences

- Milestone 4 proceeds on the accepted, publicly documented contract, against hardware and
  firmware already on the bench — the shortest path to a real printer on the production display.
- Push-based freshness (SSE/MQTT) is deliberately forgone for now; polling remains the freshness
  mechanism, with the costs the accepted polling strategy already budgets.
- The deferral costs no code and no delay: the seam it needs already exists and is tested.
- If Prusa ever removes the embedded PrusaLink API from printer firmware, the normal mode needs a
  new answer; the revisit triggers below exist for exactly that.
- The API Research Report and this decision are the record of the divergence between "announced
  successor" and "implemented target"; nothing is silently reconciled.

## Alternatives Considered

**Pivot to Connect Local now.** Rejected. It requires an external always-on server, which the
product contract forbids for normal operation; its documentation is private; the product is in
early access with no published stability guarantees. A pivot would trade a working public
contract for an inaccessible one and add a third device to the owner's setup.

**Implement both backends now.** Rejected. It doubles the integration surface before the first
real slice exists, against a private and unstable second contract — a speculative abstraction by
definition.

**Build a generic backend interface now, ahead of need.** Rejected. The observation boundary
already isolates the domain from any transport; a second concrete backend is the evidence that
would justify more structure, and it does not exist yet.

## Validation / Follow-up

Revisit this decision when any of these occurs:

1. Connect Local's API/OpenAPI documentation becomes public and stable;
2. an official PrusaLink deprecation or removal notice appears for printer firmware;
3. material user demand emerges for a server-backed mode;
4. reviewed early-access terms are found to permit an appropriate public implementation.

Until then: no Connect Local work, and the M4 slice validates the PrusaLink decision on glass.
