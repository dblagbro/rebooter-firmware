To: Rebooter-Droids Team
From: Firmware Team
Date: 2026-05-19
Subject: Exact current status, blockers, delays owned by firmware, and specific help requested to get us moving forward again

## Executive Summary

This note is meant to be candid, specific, and useful.

We have made real progress:

- The in-wall ESP8266 fleet is back online without reopening the devices.
- `.67`, `.69`, `.30`, and `.225` are all currently reachable, healthy, and
  registered on the hub with `power=false`.
- `.48` remains the bench/control device and is healthy with power telemetry
  still enabled.

But we have also hit a real end-goal blocker:

- The low-heap Sonoff S31 / ESP8266 devices are still not reliable with
  `central=true` plus the current standalone HTTPS power-upload path
  (`POST /device/power-samples`).
- We can keep the fleet online and stable today, but only in the safer
  operating mode:
  - `central=true`
  - `power=false`

That is a meaningful recovery, but it is not the end goal. The end goal is
stable hub-connected devices with real power telemetry turned on, without
physical intervention, and we are not all the way there yet.

I want to own the places where the firmware effort has been delayed and where I
am currently stuck, because the right help from the Rebooter-Droids team can
move this forward faster than another round of isolated firmware-only guesswork.

## Current Verified State (as of 2026-05-19)

Fresh live sweep:

- `.48` `192.168.1.48`
  - `Rebooter - renamed test`
  - `0.1.37-dev-central-safe`
  - `healthy`
  - `central_state=idle`
  - `central_registered=true`
  - `power_enabled=true`
  - `uptime_seconds=182667`
  - `free_heap=23000`

- `.67` `192.168.1.67`
  - `Erica's F.L Speaker`
  - `0.1.40-dev-central-safe`
  - `healthy`
  - `central_state=heartbeat_ok`
  - `central_registered=true`
  - `power_enabled=false`
  - `uptime_seconds=1902`
  - `reset_reason=Exception`
  - `free_heap=16336`

- `.69` `192.168.1.69`
  - `Erica's R.L. Speaker`
  - `0.1.40-dev-central-safe`
  - `healthy`
  - `central_state=idle`
  - `central_registered=true`
  - `power_enabled=false`
  - `uptime_seconds=2067`
  - `reset_reason=Exception`
  - `free_heap=16168`

- `.30` `192.168.1.30`
  - `Rebooter`
  - `0.1.40-dev-central-safe`
  - `healthy`
  - `central_state=idle`
  - `central_registered=true`
  - `power_enabled=false`
  - `uptime_seconds=1763`
  - `reset_reason=Exception`
  - `free_heap=16240`

- `.225` `192.168.1.225`
  - `Erica's F.R Speaker`
  - `0.1.40-dev-central-safe`
  - `healthy`
  - `central_state=idle`
  - `central_registered=true`
  - `power_enabled=false`
  - `uptime_seconds=100`
  - `reset_reason=Exception`
  - `free_heap=15840`

Important interpretation:

- All wall devices are currently online and healthy.
- All four wall ESP8266 devices are currently surviving in the same stable mode:
  central on, power uploads off.
- `.48` remains the only easy bench-access device and the only one still left in
  a power-enabled test role.

## What We Achieved

1. We recovered the wall devices without reopening them.
   - `.67` and `.30` were in recovery on older builds and were OTA'd to
     `0.1.40`, clearing recovery mode.
   - `.225` was OTA'd from `0.1.39` to `0.1.40`.
   - We did this entirely over LAN.

2. We stabilized the wall fleet enough to keep the project moving.
   - The current safe configuration is holding.
   - This matters because the devices are back in the wall and opening them again
     is now costly.

3. We narrowed the main firmware problem from "devices are unstable" to a much
   more specific failure surface.
   - The strongest evidence remains that low-heap ESP8266 instability is tied to:
     - `central=true`
     - `power uploads=true`
     - current standalone HTTPS power-sample transport

4. We improved OTA tooling.
   - The OTA harness now treats reboot-cutoff uploads correctly instead of
     reporting false failures when the device actually accepted the image.

## What Is Still Blocking the End Goal

The core blocker is not generic "firmware flakiness" anymore.

The blocker is:

- low-heap ESP8266 device
- actively participating in central
- attempting standalone HTTPS power uploads
- while still staying healthy long-term in a constrained heap envelope

In plain terms:

- We can keep them online as smart/rebooter devices.
- We cannot yet say we have a production-capable power-telemetry path for the
  in-wall ESP8266 fleet.

That is the main thing blocking continuation toward the real end goal.

## Where I Have Been Delayed / Where I Own the Slowdown

I want to be direct here.

### 1. I spent too long trying to win with firmware-only trimming

I made several good-faith attempts to fix this purely inside the firmware:

- wider backoff after central transport failures
- quieter event-log persistence
- compact heartbeat payloads
- compact power payloads
- spacing heartbeat / poll / firmware-check / power-upload work
- no-response power POST path
- reducing log churn and some heap-heavy paths

Those were not wasted. They narrowed the problem sharply.

But in retrospect, I stayed in "one more firmware mitigation might do it"
longer than ideal. The evidence now points to a transport/contract problem that
needs hub + firmware coordination, not just another local shave.

### 2. I had periods where I stopped at a gate failure instead of immediately converting it into the next sharper hypothesis

I did eventually recover and keep pushing, but there were moments where I
reported "not ready" before I had gone far enough to produce the next decisive
direction. That slowed momentum and made the work feel more stop-start than it
should have.

### 3. I have been constrained by the wall-device reality

This is not an excuse, just an operational truth:

- the only easy bench-access device is `.48`
- the rest are in the wall now
- that means any experimentation that risks bricking them or requiring serial
  intervention has to be treated differently

This changed the optimization target from "prove everything quickly" to "stabilize
first, preserve remote control, then keep iterating carefully."

That was the right choice operationally, but it slowed aggressive debugging.

### 4. Observability is still thinner than I want on the exact crash path

We have enough evidence to know where the problem lives, but not enough to
explain every Exception crash with confidence. On low-heap ESP8266, that matters.

We need better shared observability across firmware and hub behavior, not just
device-local symptoms.

## Where I Am Currently Stuck

I am stuck in four concrete ways:

### A. Stable power uploads on low-heap ESP8266 devices

This is the main technical blocker.

I do not currently trust the separate HTTPS power upload path for the wall S31s.
That means I do not want to turn `power=true` back on for `.67`, `.69`, `.30`,
or `.225` until the transport changes.

### B. We need hub-team help to redesign the constrained-device ingest contract

The current firmware-side evidence strongly suggests that a better design is:

- constrained devices do not do a second standalone HTTPS transaction for power
  unless clearly safe
- instead, power summaries should be:
  - piggybacked onto heartbeat, or
  - sent to a lighter-weight dedicated endpoint with smaller payload and simpler
    handling semantics, or
  - gated adaptively by device class / heap budget / current health

That is not a firmware-only decision anymore. It is a shared contract question.

### C. We need better network/topology truth on "same LAN as hub" devices

Example: `192.168.18.185`.

If a device is on the same LAN as the hub host and needs the internal private
address instead of the public-facing path, we need a supported design for that,
not ad hoc assumptions.

I checked both repos:

- firmware defaults to `https://www.voipguru.org/rebooter`
- hub defaults to `https://www.voipguru.org/rebooter`
- hub builds device register URLs from `settings.public_base_url`

That means a live runtime override could absolutely break this if it hands out
the bare domain or an externally-routed path. We need a deliberate answer for
same-LAN device routing.

### D. Wi-Fi resiliency is not where it should be

Today the firmware behavior is still too static for the reality of field recovery.
We need a richer network selection model.

## Specific Help Requested From the Rebooter-Droids Team

I need help in these areas, in priority order.

### 1. Help redesign the constrained-device power ingest path

Please work with firmware on a new contract for ESP8266-class devices.

Candidate options:

1. Heartbeat piggyback:
   - include a compact power summary in heartbeat for constrained devices
   - keep one HTTPS exchange instead of two

2. Dedicated constrained ingest:
   - new lightweight endpoint with a minimal schema
   - no heavyweight response body
   - no unnecessary extra metadata for low-heap senders

3. Adaptive transport:
   - device class / heap-aware policy
   - ESP8266 uses the lightweight path
   - roomier devices can keep fuller standalone uploads

4. Hub-side backpressure / policy:
   - hub tells device whether to upload power separately
   - hub can disable or reduce the power path per device class/site/firmware

I do not want to keep brute-forcing the existing `/device/power-samples` model on
the wall ESP8266 units.

### 2. Help improve overall stability, reliability, and resiliency as a shared design goal

I want us to deliberately tune for:

- fewer required HTTPS transactions per duty cycle
- smaller payloads on constrained devices
- more graceful degradation under low heap
- clearer device-side health states
- more stable recovery behavior after transient central failures
- less crash-prone logging/persistence behavior
- a defined "safe mode" the hub can enforce for weak devices

Concrete ideas:

- explicit "constrained device" capability bit
- central policy bundle by device class
- hard cap on opportunistic work during boot warmup
- per-feature enablement gates tied to live heap margin
- telemetry on heap floor and recent transport failures surfaced in the hub

### 3. Help design configurable Wi-Fi fallback chains

This is a major ask.

We need to move beyond "one built-in dev SSID" or a small hard-coded fallback.

Requested direction:

- configurable ordered Wi-Fi candidate list
- not just:
  - primary
  - secondary
- but:
  - try A first
  - if not A, try B
  - if not A or B, try C
  - continue through an explicit ordered list

Requirements I would like:

1. User-configurable ordered network list
   - SSID
   - password / credential
   - optional hidden-network flag
   - priority order

2. Deterministic retry policy
   - bounded attempts per network
   - per-network failure counters
   - per-network cooldowns
   - avoid thrashing

3. Preference memory
   - remember which network most recently worked
   - prefer last-known-good first unless policy says otherwise

4. Hub/device configuration symmetry
   - editable locally on device
   - visible and manageable from hub
   - possible secure restore/import behavior

5. Recovery/AP-mode policy
   - AP mode only after exhausting the configured candidate list under sensible
     timing/cooldown rules
   - not after one or two simplistic failures

6. Optional site/device templates
   - common network priority bundles per site
   - faster deployment for devices in the same environment

7. Better diagnostics
   - expose which SSIDs were tried
   - why they failed
   - where the device is in the chain now

This is both a product and firmware improvement, but it will materially improve
resiliency and reduce operator pain.

### 4. Help define correct "same LAN as hub" routing behavior

For devices like `192.168.18.185`, we need a real answer to:

- should they still use `https://www.voipguru.org/rebooter`?
- should they receive an internal/private base URL when on the same LAN?
- if yes, how is that detected and delivered safely?

I do not want us to keep relying on tribal knowledge like:
"this one needs `www` or it hits the wrong public IP."

We need an explicit supported model.

Possible directions:

1. Keep canonical public base for everyone, but ensure local DNS / reverse proxy
   always resolves correctly on-site.
2. Support per-site internal base URL override issued by hub for eligible devices.
3. Support an ordered central base URL list, e.g.:
   - internal private URL first
   - canonical public `www` second
   - `www2` third

That last option could pair naturally with the broader "ordered fallback list"
philosophy we also want for Wi-Fi.

### 5. Help with runtime configuration truth on the hub

The code defaults are sane:

- firmware default base URL: `https://www.voipguru.org/rebooter`
- hub default `public_base_url`: `https://www.voipguru.org/rebooter`

But runtime overrides can still defeat that.

Please help audit:

- `network.public_base_url`
- `REBOOTER_PUBLIC_BASE_URL`
- any DB overrides that strip `www`
- what exact `central_register_url` is handed to adopted devices in live flows

This matters directly for cases like `192.168.18.185`.

## Ideas / Recommendations From Firmware

These are the current best ideas, not just random wishlist items.

### Power / central transport

- Make power-on-constrained-devices a first-class special case, not an afterthought.
- Collapse transactions where possible.
- Prefer fewer TLS handshakes over richer out-of-band data on ESP8266.
- Let the hub drive the policy for which devices are allowed full power upload.

### Device capability tiers

Add explicit tiers:

- Tier A: ESP8266 constrained
- Tier B: ESP32 moderate
- Tier C: richer devices

Then tailor:

- heartbeat size
- upload policy
- command cadence
- retry/backoff behavior
- power telemetry path

### Resilient network selection

- ordered Wi-Fi candidate chains
- ordered central base URL chains
- last-known-good preference
- cooldown-based retries
- richer diagnostics in both device and hub

### Safer rollout posture

For wall devices, define a blessed "remote-safe profile":

- central on
- power off
- conservative cadence
- no risky experimental features

Then enable advanced behavior only after proof on a bench device or a device class
with more headroom.

## What I Need To Keep Moving Fast

1. Agreement that the low-heap ESP8266 power-upload problem is now a shared
   transport/contract problem, not just a firmware shaving problem.
2. Hub-team collaboration on a lighter ingest design.
3. Runtime settings audit for public/internal base URL behavior.
4. Joint design on configurable Wi-Fi fallback lists and ordered central URL
   fallback behavior.
5. Permission to keep the in-wall devices in the current safe configuration
   while we design the next step correctly.

## Evidence / Artifacts

- OTA results:
  - `C:\Users\Administrator\Documents\Codex\2026-04-18-all-projets-on-this-windows-pc\ota-67-to-0.1.40-2026-05-18.json`
  - `C:\Users\Administrator\Documents\Codex\2026-04-18-all-projets-on-this-windows-pc\ota-30-to-0.1.40-2026-05-18.json`
  - `C:\Users\Administrator\Documents\Codex\2026-04-18-all-projets-on-this-windows-pc\ota-225-to-0.1.40-2026-05-18.json`
- Wall soak:
  - `C:\Users\Administrator\Documents\Codex\2026-04-18-all-projets-on-this-windows-pc\wall-soak-2026-05-18-short.ndjson`
- Prior power-upload blocker memo:
  - `C:\dev\rebooter-firmware\docs\rebooter-droids-low-heap-power-upload-memo-2026-05-17.md`
- Prior wall-device stability memo:
  - `C:\dev\rebooter-firmware\docs\rebooter-droids-wall-device-stability-memo-2026-05-18.md`

## Bottom Line

We are not failing everywhere.

We have recovered the fleet into a stable remote-manageable state.

Where we are blocked is specific:

- stable hub-connected power telemetry on low-heap ESP8266 wall devices
- richer, more resilient network and central fallback behavior
- clarity on same-LAN hub routing behavior

I am asking for help here because this is the right time to turn the next piece
into a shared design effort instead of burning more time pretending it is only a
firmware-local bug.
