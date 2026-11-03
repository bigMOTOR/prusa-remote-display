<!-- SPDX-FileCopyrightText: 2026 Mykola Fiantsev -->
<!-- SPDX-License-Identifier: CC-BY-4.0 -->

# ADR-0007: Keep the local web server alive after provisioning, read-only over the LAN

## Status

Accepted

## Date

2026-08-19

## Context

The device serves a small web interface — `index.html`, `style.css`, `app.js` and two JSON routes
— from flash, over HTTP on port 80. Until this decision it existed only while the setup access
point did: the server was started once the access point came up, and stopped two seconds after
provisioning succeeded, at which point the interface ceased to exist until the next factory-fresh
boot.

A product requirement recorded on 2026-08-14 changed that. The local web interface must not
disappear after provisioning; after the device joins the user's Wi-Fi, the same interface must
remain reachable over the LAN through the station interface; stable local discovery is preferred
over making the user find the DHCP-assigned address; the setup access point should still stop after
successful provisioning; and the web UI is to be treated as a persistent local device management
interface with provisioning as one *mode*, not as a disposable setup page.

A second requirement, recorded the same day, set the security boundary that first one creates. The
persistent interface may expose non-sensitive read-only device and status information without
authentication on the trusted LAN. Any endpoint that mutates sensitive configuration — Wi-Fi
credentials, printer address or credentials, entering recovery, reset operations — must require
explicit authorization. No user accounts and no permanent web passwords in the MVP. The exact
mechanism is to be designed across this checkpoint and the next, with a strong preference for
short-lived authorization established through deliberate physical presence on the device.

Those two requirements pull in opposite directions on exactly one endpoint. `POST /api/setup` is a
mutating endpoint, and the moment the server outlives the access point that endpoint is reachable
from the LAN. The physical-presence mechanism the security requirement prefers depends on a device
button gesture that the *following* checkpoint owns and this firmware does not yet have.

Three further constraints bear on the shape of the change:

- The existing CSRF design compares the request's `Origin` header against exactly one string, the
  setup access point's own origin. A persistent interface answers on more than one address.
- The page detects a mid-flow device crash by observing that the session token changed, since the
  token is minted when the server starts. Anything that restarts the server therefore looks to a
  browser exactly like a crash.
- Firmware Architecture Specification §22 requires every dependency to be pinned,
  license-compatible, justified and upgrade-reviewed, and rules out general web frameworks. It
  lists "built-in NVS, Wi-Fi, OTA and mDNS facilities" among expected dependencies — but in
  ESP-IDF v5.x mDNS is no longer bundled in the framework and ships from the component registry.

## Decision

**1. The server stays alive after provisioning; the access point still stops.** The two-second
grace period that lets the browser read the outcome is unchanged, and at the end of it the access
point stops and the server does not. Persistence is implemented as the *absence* of the call that
used to stop it.

**2. No restart and no rebind is performed, because none is required.** ESP-IDF's `httpd_config_t`
has no bind-address field and its listener binds `INADDR_ANY`; the running server already answers
on every interface the device holds, including the station interface as soon as it has an address.
Restarting the server to "rebind" it would mint a new session token and the page would report,
correctly by its own rules, that the display had restarted mid-flow. This is recorded explicitly
because the plain reading of the requirement suggests a rebind that would actively cause a defect.

**3. The server is also started on a boot that is already provisioned.** Otherwise persistence
would last until the next power cycle rather than beyond it, which is not what a persistent
management interface means. On such a boot there is no access point at all.

**4. The LAN surface is read-only in this checkpoint.** Every request that mutates sensitive
configuration is refused unless it arrived on the setup access point. This is the temporary
implementation of the security requirement, not a weakening of it: refusing is what that
requirement asks for in the absence of authorization, and the alternative — accepting a credential
change from anyone on the LAN behind nothing but CSRF and a rate limit — is the thing it exists to
forbid. The physical-presence mechanism is not pulled forward from the next checkpoint.

**5. The arrival surface is read from the socket, never from a header.** `httpd_req_to_sockfd()`
followed by `getsockname()` reports the local address the connection was actually accepted on,
which the caller does not choose. `Host` and `Origin` are written by the caller, so authorizing on
either would let anyone on the LAN claim the privileged surface by typing an address into a header.
An address that cannot be resolved yields an unidentified surface, which is refused.

**6. Origin validation is widened to an explicit set, and stays exact.** The accepted rule becomes
membership of a set the device genuinely owns: the setup-AP origin, the current station-IP origin,
and the `.local` hostname origin, each present only while it is actually active. No wildcard, no
`*`, no prefix, suffix or substring match; the scheme is fixed and the comparison is
length-checked first. An inactive slot matches nothing, which is how the access point's origin
stops being accepted in the same operation that stops serving it.

**7. The device advertises a stable, brand-neutral mDNS identity.** The hostname is
`display-<suffix>.local` — for example `display-0c01.local` — where `<suffix>` is the existing
four-hex-character device suffix already used for the setup SSID, derived from the last two bytes
of the SoftAP MAC. The advertised service instance is `Remote Display <SUFFIX>`, and the service is
`_http._tcp` on the one port constant the listener also uses. The suffix derivation is shared with
setup identity rather than duplicated, so the network a user joined during setup and the name they
reach the device at afterwards identify the same board. The name deliberately does not carry the
project or product brand: `LICENSING.md` reserves those as all rights reserved, and a hostname
coupled to a brand would be a product decision made by a firmware constant.

**8. mDNS is a pinned managed component.** `espressif/mdns` is pinned exactly at 1.11.3 in the
`network` component's own manifest, resolved through the ESP-IDF Component Manager, and declared
privately so no other component can reach `mdns.h` through a transitive requirement. Its licence is
Apache-2.0 — verified from the licence text the registry serves for that exact version, since the
registry metadata reports only "Custom" — which is compatible inbound to GPL-3.0-only. Nothing from
it is redistributed here: `managed_components/` is fetched at build time and is not tracked.

**9. Bounded idle shutdown is reinterpreted rather than postponed again.** Architecture
Specification §14.4 asks the setup server to "stop after a bounded idle period when safe", and an
earlier ruling deferred that to the recovery checkpoint. The persistence requirement has since made
this server permanent, so the rule is recorded as scoped: §14.4's bounded idle shutdown applies to
temporary provisioning exposure and to privileged authorization windows, **not** to the persistent
read-only LAN server. The access point still stops after successful provisioning. The persistent
server does not stop merely because it is idle. The bounded lifetime of re-entry and of
physical-presence authorization belongs to the recovery checkpoint.

**10. The page stops offering controls it cannot honour.** An earlier ruling kept a Save action off
this page for as long as nothing could be saved. The same rule applies to a form whose submission
is guaranteed to be refused, so on a read-only surface the forms are hidden and a short notice
explains why. The page learns which surface it is on from one non-secret boolean on the status
response it already polls — the mechanism already accepted for reporting flow outcomes, not a second
one. The notice deliberately names no button gesture, because the gesture that would re-enter setup
does not exist in this firmware yet and naming it would be the same lie in a different place. The
session token is withheld on the read-only surface, where it would authorize nothing.

## Consequences

- The local interface survives provisioning and reboots, and is reachable at a name a person can
  write down rather than at a DHCP lease.
- Configuration cannot be changed from the LAN in this checkpoint. On a provisioned device it
  cannot be changed at all until the recovery checkpoint adds deliberate re-entry — which is the
  state that already existed, so no capability is lost.
- The refusal is not distinguishable in the response from any other refusal: every one answers the
  same 403, so a caller learns that it was refused and nothing about which rule it broke or where
  the privileged surface is. The distinction lives in the device's own log.
- The surface check runs before the rate limiter, so a refused caller on the LAN cannot exhaust the
  request budget of the person standing at the device, and before the token comparison, so nothing
  about the token can be measured from the LAN.
- One managed dependency is added, and `dependencies.lock` changes accordingly.
- The advertisement carries no TXT records. A TXT record is a broadcast to the whole segment, and
  the temptation it creates — publishing a serial, a firmware version, a printer address, a
  provisioning state — is what §13.5 and §13.6 rule out putting on a wire the product does not
  control.
- If mDNS fails to start, the device stays reachable by address; this is logged as degraded and is
  not a fault.

### Divergences from Project Sources, recorded and not resolved here

The two 2026-08-14 requirements are explicit owner instructions, which outrank Project Sources in
the project's priority order. No Project Source has been amended, and none may be without a separate
owner decision. Three passages are therefore temporarily diverged from the implementation:

- **§10.5** states that the provisioning server task "Exists only in setup mode." The server now
  survives provisioning. What §10.5 requires and this decision preserves intact is the
  discipline that the server performs no direct application state mutation and posts events to the
  application task — which matters more once the server is long-lived, not less.
- **§5.11** scopes `provisioning` to "the local initial-configuration experience." The web UI is now
  a persistent management surface of which provisioning is one mode. §5.11's rule that the web UI
  must never return stored secrets to the browser is preserved unchanged.
- **§14.4's** bounded idle shutdown was written for a short-lived setup server. Its scope is
  recorded in decision 9 above.

## Alternatives Considered

**Pull the physical-presence gesture forward and implement authorization now.** Rejected: the
gesture is owned by the recovery checkpoint, where it is shared with deliberate re-entry into
provisioning, and defining it here would settle a product interaction in the checkpoint least able
to validate it. Refusing LAN mutations costs nothing today because re-entry does not exist either.

**Authorize by mode rather than by surface** — accept mutations over the LAN while a provisioning
session is active. Rejected: it would leave a credential-mutating endpoint reachable from the LAN
behind only CSRF and a rate limit during that window, which is what the security requirement
forbids, and the window is not one the user can see.

**Relax `Origin` validation to accept any origin, or to a prefix or suffix test.** Rejected: it
would delete the CSRF protection rather than widen it. An explicit set of active origins gives the
persistent interface the addresses it needs while keeping every comparison exact.

**Serve only the JSON status route on the LAN and refuse the page.** Rejected: reachability of the
interface is the point of the requirement, and a management surface with no human-readable page is
not one.

**Restart the server on the station interface to "rebind" it.** Rejected as both unnecessary and
harmful — see decision 2.

**Hard-code the access point's well-known address instead of reading it.** Rejected on the existing
rule that the address is a property of the netif; a constant would be a second source of truth that
could disagree with the address a client is actually handed.

**Skip mDNS and show the DHCP address on the panel.** Rejected: the requirement explicitly prefers
stable discovery over making the user find the assigned address, and a leased address is not
something a person can rely on between reboots.

**Couple the hostname to the product brand.** Rejected: the brand is reserved, and the hostname is
also what a browser resolves — a rename would break every bookmark.

## Validation / Follow-up

Verified locally: 867 host tests (838 before this checkpoint), including 29 new ones covering the
surface rules and the naming rules, each negative-controlled — removing the surface check fails six,
and relaxing origin membership to a wildcard fails seven. The browser suite runs 24 scenarios at a
genuine 375×812 viewport, two of them new and both negative-controlled against a page that ignores
the read-only signal. All three firmware profiles build warning-free.

Not yet verified on hardware. The physical-validation checkpoint already names these items — "LAN
reachability and mDNS after provisioning" — and Architecture Specification §25 already records
"hostname and `.local` resolution reliability" as a decision deferred for physical validation.
Findings belong in `hardware/validation/waveshare-esp32-s3-lcd-2.8.md`.

The recovery checkpoint owns what completes this decision: deliberate re-entry into provisioning,
the button gesture it shares with physical-presence authorization, and the bounded lifetime of that
authorization window. When it lands, the LAN surface gains a way to become mutable, and decision 4
above is what it replaces.
