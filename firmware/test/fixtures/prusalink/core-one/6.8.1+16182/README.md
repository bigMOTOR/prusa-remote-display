<!--
SPDX-FileCopyrightText: 2026 Mykola Fiantsev
SPDX-License-Identifier: CC-BY-4.0
-->

# CORE One · PrusaLink 2.1.2 · firmware 6.8.1+16182

Captured 2026-08-29 from the project owner's printer during the Milestone 4 live vertical slice,
over the LAN with `X-Api-Key`. Architecture Spec §20.3: "Every field discovered during physical
validation should first become a fixture, then a regression test."

`version.json` is `GET /api/version`, 183 bytes, byte-for-byte the body the display's own probe
received and rejected (`decode=missing-field field=version`, `bytes=183`).

## What it establishes

- The response carries **no `version` key**. The PrusaLink server version arrives as **`server`**.
- `api`, `printer`, `text` and `firmware` are all present, so `version` was the only absent member
  of §4.1's required set.
- `nozzle_diameter`, `hostname` and `capabilities` are additive fields §4.1 does not list; §3.2
  requires them to be ignored, and they are.

## Divergences from the Accepted Project Sources — reported, not applied

- **API Research Report §4.1** lists `version` as a required field of `GET /api/version`. This
  firmware does not send it. The decoder accepts either spelling; the Source is unchanged.
- **API Research Report §18.2 and §18.4** record firmware **6.5.7** as the dated initial
  prototype / Tier-A target. This fixture adds physical evidence for CORE One firmware
  **6.8.1+16182**. The Source remains historically accurate — §18.2 is explicitly dated
  12 July 2026 and correctly states the then-current baseline — and what this evidence extends is
  its tested compatibility matrix, which requires a later owner-controlled update.
- **§23 open question 3** — "What exact response does `/api/version` return on CORE One?" — is
  answered by this file.

No credential is present: the response carries version strings, a nozzle diameter, the default
hostname and a capability flag.
