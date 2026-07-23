# Bug Log

This file tracks confirmed bugs, likely bugs needing confirmation,
quality weaknesses, and release-hardening findings for the firmware
project.

## 2026-05-17 Low-Heap Power Upload Characterization

### 1. Low-heap ESP8266 devices still crash when standalone HTTPS power uploads are enabled

- date: 2026-05-17
- title: `central + active power uploads` remains unstable on low-heap Sonoff S31 units
- severity: high
- area/component: central client / power upload transport / ESP8266 heap pressure
- environment/context:
  - live devices `.225` (`http://192.168.1.225`) and `.69` (`http://192.168.1.69`)
  - firmware candidates `0.1.38-dev-central-safe`, `0.1.39-dev-central-safe`, `0.1.40-dev-central-safe`
- reproduction steps:
  1. Keep `central.enabled=true` and `power.enabled=true` on a low-heap ESP8266 unit.
  2. Allow the device to leave boot warmup and begin central work.
  3. Observe the device through local `/api/status` or repeated status sampling.
- expected result:
  - device remains healthy while central heartbeats and power uploads both run.
- actual result:
  - device survives with:
    - `central=true, power=false`
    - `central=false, power=true`
  - but later drops off the LAN or reboots with `reset_reason="Exception"` when
    both central and power uploads are active together.
- evidence:
  - `C:\Users\Administrator\Documents\Codex\2026-04-18-all-projets-on-this-windows-pc\watch-225-69-0.1.38-2026-05-17-live.ndjson`
  - `C:\Users\Administrator\Documents\Codex\2026-04-18-all-projets-on-this-windows-pc\watch-225-0.1.39-central-power-live.ndjson`
  - `C:\Users\Administrator\Documents\Codex\2026-04-18-all-projets-on-this-windows-pc\watch-69-0.1.40-central-power-live.ndjson`
  - OTA result artifacts under the same workspace for `.225` and `.69` on `0.1.38` through `0.1.40`
- likely cause if known:
  - the remaining instability is not the local power monitor itself and not the
    base heartbeat/poll loop by itself; it is the additional HTTPS power-upload
    path on low-heap devices.
  - repeated BearSSL/HTTP client allocation plus request/response handling for
    `/device/power-samples` remains the best current suspect.
- recommended fix:
  - firmware-side:
    - minimize or eliminate standalone power-upload transport cost on low-heap
      ESP8266 devices
    - consider heartbeat-carried compact power summaries or a lighter upload path
      instead of separate power POSTs
  - hub-side:
    - be prepared to consume a lighter transport shape if firmware moves power
      summaries into heartbeats for constrained devices
- status: open; narrowed significantly on 2026-05-17

## 2026-05-14 QA Regression Pass

### 1. Unauthenticated config endpoint leaks central enrollment data

- date: 2026-05-14
- title: `GET /api/config` exposes central enrollment token without auth
- severity: high
- area/component: local API / auth / configuration
- environment/context: live bench device `http://192.168.1.48` on `0.1.19-dev-central-safe`
- reproduction steps:
  1. Send `GET /api/config` with no auth header.
  2. Inspect returned JSON.
- expected result:
  - unauthenticated callers should either be rejected or receive a redacted config shape.
- actual result:
  - endpoint returns full device configuration including `central.enrollment_token`,
    device identifiers, watchdog configuration, and other internal state.
- evidence:
  - `C:\Users\Administrator\Documents\Codex\2026-04-18-all-projets-on-this-windows-pc\2026-05-14-firmware-api-regression-results.json`
- likely cause if known:
  - `/api/config` is intentionally left open while the UI depends on it, but the
    response shape was not reduced when auth-protected central configuration was added.
- recommended fix:
  - either require auth for `/api/config` when provisioned, or add a safe public
    read model that strips enrollment and central identity data.
- status: fixed in `0.1.20-dev-central-safe`, deeply retested on `0.1.21-dev-central-safe`

### 2. Local UI cannot perform protected actions once auth is provisioned

- date: 2026-05-14
- title: browser UI never sends `X-Rebooter-Auth`, so protected actions fail
- severity: high
- area/component: local web UI / auth integration
- environment/context: Edge Playwright-style browser automation against live bench device
- reproduction steps:
  1. Open the device root page after local admin credentials are provisioned.
  2. Click relay toggle.
  3. Attempt config save.
- expected result:
  - UI should support an auth flow or stored session/token and successfully perform protected actions.
- actual result:
  - relay toggle fails with `unauthorized`
  - config save fails with `unauthorized`
  - browser console shows 401s for protected requests
- evidence:
  - `C:\Users\Administrator\Documents\Codex\2026-04-18-all-projets-on-this-windows-pc\2026-05-14-firmware-browser-regression-results.json`
  - `C:\dev\rebooter-firmware\data\app.js` does not set `X-Rebooter-Auth`
- likely cause if known:
  - frontend fetch/XHR helpers never attach the auth header used by protected endpoints.
- recommended fix:
  - implement a real local auth flow and attach the auth token/header for protected requests.
- status: fully fixed in `0.1.21-dev-central-safe`

### 3. Default secondary central base URL is misconfigured and produces repeated 404/transport noise

- date: 2026-05-14
- title: `www2.voipguru.org` fallback path is unhealthy by default
- severity: medium
- area/component: central integration / operational defaults
- environment/context: live central diagnostic on bench device
- reproduction steps:
  1. Call `GET /api/system/central-diagnostic` with auth.
  2. Inspect the second entry under `targets`.
- expected result:
  - both default central base URLs should serve the expected API paths.
- actual result:
  - primary URL returns 200 from `/api/v1/version`
  - secondary URL returns 404 from `/rebooter/api/v1/version`
  - event log also contains repeated transport or fallback noise tied to the second URL
- evidence:
  - `C:\Users\Administrator\Documents\Codex\2026-04-18-all-projets-on-this-windows-pc\2026-05-14-firmware-api-regression-results.json`
  - `GET /api/events` output on 2026-05-14
- likely cause if known:
  - fallback host is not provisioned with the same route tree as the primary.
- recommended fix:
  - either fix the secondary host or remove it from default firmware config until it is valid.
- status: fixed in `0.1.20-dev-central-safe`, deeply retested on `0.1.21-dev-central-safe`

### 4. Persisted event log uses ambiguous non-monotonic timestamps across reboots

- date: 2026-05-14
- title: event history becomes hard to interpret after reboot or recovery cycles
- severity: medium
- area/component: event log / observability
- environment/context: live `GET /api/events` response
- reproduction steps:
  1. Trigger multiple reboots/OTA/recovery cycles over time.
  2. Fetch `/api/events`.
  3. Observe repeated low `ts` values and interleaved entries from different boots.
- expected result:
  - persisted event history should remain chronologically interpretable.
- actual result:
  - entries appear to use uptime-relative timestamps and are interleaved after reboots,
    producing a non-monotonic timeline that is difficult to reason about.
- evidence:
  - `C:\Users\Administrator\Documents\Codex\2026-04-18-all-projets-on-this-windows-pc\2026-05-14-firmware-api-regression-results.json`
- likely cause if known:
  - event log stores relative timestamps without a boot/session discriminator or wall-clock anchor.
- recommended fix:
  - include a boot/session id and/or absolute timestamp when available, or clearly expose
    boot-relative semantics in the API shape.
- status: fixed in `0.1.20-dev-central-safe`, deeply retested on `0.1.21-dev-central-safe`

### 5. UI produces avoidable console noise from missing favicon

- date: 2026-05-14
- title: root page requests missing `/favicon.ico`
- severity: low
- area/component: local web UI / asset completeness
- environment/context: browser automation against live device root page
- reproduction steps:
  1. Open `/` in a browser.
  2. Observe console/network output.
- expected result:
  - page loads without missing-asset noise.
- actual result:
  - browser console records a 404 for `/favicon.ico`
  - direct request returns `Not found: /favicon.ico`
- evidence:
  - `C:\Users\Administrator\Documents\Codex\2026-04-18-all-projets-on-this-windows-pc\2026-05-14-firmware-browser-regression-results.json`
- likely cause if known:
  - no favicon route or asset exists.
- recommended fix:
  - add a tiny favicon asset or suppress the request via explicit markup.
- status: fixed in `0.1.20-dev-central-safe`, deeply retested on `0.1.21-dev-central-safe`

### 6. Unsupported methods return generic 404 instead of 405

- date: 2026-05-14
- title: method mismatch on API routes is not distinguished cleanly
- severity: low
- area/component: local API / protocol polish
- environment/context: direct HTTP checks
- reproduction steps:
  1. Send `GET /api/relay/on`.
- expected result:
  - endpoint should ideally return `405 Method Not Allowed`.
- actual result:
  - device returns `404 Not Found`.
- evidence:
  - direct `curl -i -X GET http://192.168.1.48/api/relay/on`
- likely cause if known:
  - routes are only registered per-method and fall through to the generic not-found handler.
- recommended fix:
  - add explicit method handling or a thin router-level 405 response where practical.
- status: fixed in `0.1.20-dev-central-safe`, deeply retested on `0.1.21-dev-central-safe`

### 7. OTA reboot briefly returned to recovery mode once during remediation retest

- date: 2026-05-14
- title: one OTA remediation retest came back in `recovery_mode` until a normal reboot cleared it
- severity: medium
- area/component: OTA / recovery interaction
- environment/context: live bench device `http://192.168.1.48` during `0.1.20-dev-central-safe` remediation pass
- reproduction steps:
  1. Upload the remediation build over local OTA.
  2. Wait for the device to come back.
  3. Inspect `GET /api/status`.
- expected result:
  - device should return in normal mode after successful OTA reboot.
- actual result:
  - one retest returned with `recovery_mode=true`, `last_known_good_restored=true`,
    and `central_state="recovery_mode"` until a normal authenticated reboot was issued.
- evidence:
  - live `GET /api/status` capture during this remediation pass
- likely cause if known:
  - unclear; may be a stale recovery flag or an OTA/restart edge case rather than
    the newly remediated auth/config work.
- recommended fix:
  - reproduce intentionally before changing code; inspect recovery-flag lifecycle
    across OTA completion and restart.
- status: not reproduced in 3-cycle OTA stress on `0.1.21-dev-central-safe`; continue monitoring

### 8. Live device-served UI asset lagged the repo fix for protected fetch headers

- date: 2026-05-14
- title: actual device-served `app.js` still dropped auth headers on option-bearing protected requests
- severity: medium
- area/component: local web UI / LittleFS-served assets / release artifact validation
- environment/context: live bench device `http://192.168.1.48` on `0.1.20-dev-central-safe`
- reproduction steps:
  1. Compare the repo `data/app.js` helper against the actual device-served `/app.js`.
  2. Inspect the `fetchJson(...)` request shape.
  3. Exercise protected UI actions that also supply request headers, such as config save.
- expected result:
  - the device-served UI should match the repo fix and preserve `X-Rebooter-Auth`
    when extra request headers are present.
- actual result:
  - the live device-served `app.js` still used the pre-fix merge order, allowing
    `options.headers` to overwrite the auth-bearing header set on protected requests.
- evidence:
  - live `GET http://192.168.1.48/app.js` inspection during deep retest
  - actual device-served UI retest on `.48`
- likely cause if known:
  - the repo source had been corrected, but the deployed LittleFS-served artifact
    on the device was still one build behind.
- recommended fix:
  - rebuild and OTA a new firmware artifact, then verify the actual device-served
    `/app.js` rather than relying only on repo source or proxy-served assets.
- status: fixed in `0.1.21-dev-central-safe`

### 9. Destructive-path proof exposed ambiguous recovery/factory-reset progression

- date: 2026-05-15
- title: explicit recovery boot plus LAN-only harness produced an ambiguous "half-reset" bench state
- severity: medium
- area/component: recovery / factory reset / destructive QA harness
- environment/context: live bench device `http://192.168.1.48` during destructive-path proof on `0.1.21-dev-central-safe`
- reproduction steps:
  1. Trigger the scripted destructive-path proof against a LAN-reachable bench device.
  2. Let the script continue after the device leaves LAN reachability for setup AP / recovery behavior.
  3. Reprovision the device manually from a phone and inspect returned state.
- expected result:
  - the proof harness should stop cleanly when manual provisioning becomes necessary,
    and the firmware should make recovery-vs-reset progression obvious.
- actual result:
  - the device returned on the LAN still carrying prior config/auth because the
    proof run did not actually prove a full factory reset.
  - explicit recovery provisioning also left the device in a recovery boot until
    a subsequent reboot cleared it.
- evidence:
  - `C:\Users\Administrator\Documents\Codex\2026-04-18-all-projets-on-this-windows-pc\destructive-proof-2026-05-15-101206`
  - live `GET /api/status` after manual reprovisioning showed `recovery_mode=true`
- likely cause if known:
  - the proof harness assumed uninterrupted LAN control even after forcing the
    device into setup AP / recovery flows.
  - recovery and factory-reset cleanup semantics were not explicit enough around
    provisioning credentials, recovery markers, and handoff back to a normal boot.
- recommended fix:
  - make successful explicit recovery provisioning auto-reboot back into a normal boot
  - clear recovery markers, event log state, and provisioned Wi-Fi credentials on factory reset
  - make the destructive QA harness fail fast and require manual operator steps when LAN reachability is intentionally lost
- status: fixed in `0.1.22-dev-central-safe`; live assisted destructive proof passed on 2026-05-15

### 10. First boot after OTA to `0.1.25-dev-central-safe` can fall into recovery mode on field devices

- date: 2026-05-15
- title: some healthy long-uptime field devices auto-enter recovery on the first boot after OTA to `0.1.25`
- severity: high
- area/component: OTA / boot-health / recovery interaction
- environment/context:
  - live field devices `http://192.168.1.30`, `http://192.168.1.67`
  - target firmware `0.1.25-dev-central-safe`
- reproduction steps:
  1. Start from a healthy long-uptime device on `0.1.18-dev-central-safe`.
  2. Upload `0.1.25-dev-central-safe` over local OTA.
  3. Wait for the device to come back and inspect `/api/status` and `/api/events`.
- expected result:
  - device should return in normal mode on the first post-OTA boot.
- actual result:
  - `.30` and `.67` both returned with `recovery_mode=true`,
    `last_known_good_restored=true`, and `central_state="recovery_mode"` on the
    first observed boot after OTA.
  - on `.30`, a plain authenticated reboot cleared recovery mode and immediately
    restored live power telemetry.
  - on `.67`, recovery mode also cleared after an explicit reboot, but the path
    remains inconsistent enough that it needs a focused root-cause pass.
- evidence:
  - `C:\Users\Administrator\Documents\Codex\2026-04-18-all-projets-on-this-windows-pc\power-capture-2026-05-15-live\events-192.168.1.30-20260515-154926.json`
  - `C:\Users\Administrator\Documents\Codex\2026-04-18-all-projets-on-this-windows-pc\power-capture-2026-05-15-live\events-192.168.1.67-20260515-154926.json`
  - live `/api/status` captures during the 2026-05-15 power-validation pass
- likely cause if known:
  - this turned out to be a layered problem:
    - older firmware images could hand the next image an inherited incomplete-boot strike
    - intentional restarts were not explicitly marked as planned, so some
      software-triggered reboots could still be counted like crashes
    - `RuntimeStatus` was not being explicitly reset at boot, so soft restarts
      could leak stale `recoveryMode` / central-state values into later normal boots
- recommended fix:
  - make boot-state firmware-version-aware
  - mark intentional restarts explicitly in boot-state
  - reinitialize runtime status at startup instead of relying on warm-restart behavior
- status: partially mitigated through `0.1.29-dev-central-safe`; the original boot-state / planned-restart issues were fixed in `0.1.27`, and a new central-path heap-churn mitigation landed in `0.1.29`, but `.225`, `.30`, and `.67` still need fresh revalidation on the new build
  - 2026-05-15 update:
    - `.69` upgraded cleanly to `0.1.28-dev-central-safe` and remained healthy, which
      gives us a good control device for the new boot/timing instrumentation.
    - `.225` was upgraded to `0.1.28-dev-central-safe` and one OTA cycle reproduced a
      delayed `Exception` reset: the device stayed normal until roughly the 60-75s
      window, then rebooted and auto-entered recovery with
      `auto_recovery_triggered=true` / `last_known_good_restored=true`.
    - a split test then showed `.225` is stable for 130+ seconds with
      `central.enabled=false`, which narrows the remaining fault to the central-enabled
      path rather than generic boot, Wi-Fi, or CSE7766 startup.
    - after disabling central, stabilizing, then re-enabling it and rebooting, `.225`
      later survived multiple 60-second heartbeat windows on `0.1.28`, so the
      central-path crash appears intermittent / heap-sensitive rather than strictly
      deterministic.
    - later serial and ELF decoding on `.48` showed the delayed crash pattern was
      landing in:
      - BearSSL validator allocation
      - `LittleFS.open(...)`
      - event-log JSON serialization to file
    - that combination points to heap churn / fragmentation in the central-enabled
      steady-state path rather than a pure boot-health bug.
    - `0.1.29-dev-central-safe` now:
      - defers event-log persistence instead of rewriting the file on every event
      - flushes explicitly only before restart-sensitive transitions
      - removes routine successful power-upload event spam from the persisted log
      - staggers central heartbeat / poll / firmware-check / power-upload work so
        the ESP8266 does not burst several TLS-heavy operations back-to-back
    - live verification on `.48` after OTA to `0.1.29`:
      - device stayed healthy for 210+ seconds with `central.enabled=true`
      - `central_state` advanced through heartbeat and returned to idle
      - the old `60-90s` crash window did not reproduce
    - live verification on `.69` after OTA to `0.1.29`:
      - device upgraded cleanly and stayed healthy through the normal post-OTA window
      - this gives us a second verification target on the new line before widening
        to the rest of Erica's devices
    - live verification on the former recovery-line devices after OTA to `0.1.29`:
      - `.67` upgraded cleanly on retry and is now healthy in normal mode with
        live low-load speaker telemetry (`~2.3W`, `~119.6V`)
      - `.30` also upgraded cleanly and is now healthy in normal mode with
        live power telemetry (`~4.9W`, `~119.8V`)
      - `.225` is the outlier: after `0.1.29` plus `central.enabled=true` and
        `power.enabled=true`, it again returned to `recovery_mode` with
        `reset_reason="Exception"`, `power_chip_seen=false`, and
        `power_source="none"`
    - current best read:
      - `0.1.29` generalized well enough to clear `.67` and `.30`
      - the residual fault is now narrower and should be treated as a `.225` /
        subset-of-devices problem rather than a proven whole-line regression

### 11. Event log can contain corrupted text under sustained power-upload activity

- date: 2026-05-15
- title: one `.30` power-upload event message contained binary garbage in the middle of the URL text
- severity: medium
- area/component: event log / string lifetime / serialization under power telemetry load
- environment/context:
  - live field device `http://192.168.1.30` on `0.1.25-dev-central-safe`
  - sustained 10-second power batch uploads after real telemetry came online
- reproduction steps:
  1. Enable real power telemetry and 10-second batch uploads.
  2. Let the device run under sustained upload traffic.
  3. Fetch `/api/events` and inspect the central-upload messages.
- expected result:
  - event log messages should remain valid UTF-8 / ASCII text and preserve the
    full upload URL and count.
- actual result:
  - one event message in boot `3`, sequence `35`, contains embedded binary
    garbage in the middle of `https://www.voipguru.org/rebooter`.
- evidence:
  - `C:\Users\Administrator\Documents\Codex\2026-04-18-all-projets-on-this-windows-pc\power-capture-2026-05-15-live\events-192.168.1.30-20260515-154926.json`
- likely cause if known:
  - unknown; possible string-buffer corruption or serialization misuse while
    recording high-frequency central-upload events.
- recommended fix:
  - reproduce with event logging left enabled under sustained power uploads
  - inspect event-log string ownership / serialization boundaries for central
    upload messages
- status: mitigated in `0.1.29-dev-central-safe`; routine successful power-upload
  events are no longer persisted every batch, and event-log persistence is now
  deferred / coalesced instead of rewriting the log file on every add
  - follow-up verification still needed under longer steady-state soak on
    central-enabled devices

### 12. Low-load telemetry reports nonzero watts with zero current because current is clamped below 50 mA

- date: 2026-05-15
- title: low-current CSE7766 loads keep `i_ma = 0` even when `p_w` is nonzero
- severity: medium
- area/component: power telemetry / CSE7766 low-load handling
- environment/context:
  - live field devices `http://192.168.1.30`, `http://192.168.1.225`
  - `0.1.25-dev-central-safe`
- reproduction steps:
  1. Enable real power telemetry on a lightly loaded device.
  2. Observe `/api/status`.
- expected result:
  - low-load devices should either report a small nonzero current or clearly
    surface that current is only estimated/unknown.
- actual result:
  - `.30` reported about `4.9-5.0W` at about `119V` while `power_current_ma`
    stayed `0`.
  - `.225` later reported about `2.2W` at about `118.8V` while
    `power_current_ma` stayed `0`.
- evidence:
  - live `/api/status` captures and `power-capture-2026-05-15-live\summary.json`
- likely cause if known:
  - `src/power_monitor.cpp` intentionally suppresses measured current when the
    estimated current is below `MIN_MEASURED_CURRENT_A = 0.05f`, which zeroes
    current for light standby loads.
- recommended fix:
  - decide whether to expose estimated low-load current separately, lower the
    clamp, or keep the clamp but make the "power valid / current not trusted"
    semantics explicit in hub-side analytics
- status: additive estimated-current semantics implemented in `0.1.27-dev-central-safe`; live loaded-standby verification confirmed on `.225` under `0.1.28-dev-central-safe`

### 13. OTA stress harness can report upload failure even when the device actually updates successfully

- date: 2026-05-15
- title: local OTA harness sometimes records `upload_accepted=false` when the target has already rebooted into the new firmware
- severity: low
- area/component: QA tooling / OTA stress harness
- environment/context:
  - live field devices `http://192.168.1.30`, `http://192.168.1.67`
  - local script `scripts/qa-ota-stress.ps1`
- reproduction steps:
  1. Use the local OTA stress harness against a recovery-line device.
  2. Observe the HTTP client side of the multipart upload during the reboot boundary.
  3. Compare the harness result to the actual post-boot firmware version.
- expected result:
  - the harness should distinguish a real upload failure from a connection that
    is cut short because the device has already accepted the update and is rebooting.
- actual result:
  - `.30` updated to `0.1.29-dev-central-safe` and stayed healthy, but the
    harness still recorded `upload_accepted=false` with a canceled task.
  - the first `.67` attempt also produced a transport-side failure shape while
    the device rebooted out of recovery, which made the result ambiguous.
- evidence:
  - `C:\Users\Administrator\Documents\Codex\2026-04-18-all-projets-on-this-windows-pc\ota-30-to-0.1.29-results.json`
  - `C:\Users\Administrator\Documents\Codex\2026-04-18-all-projets-on-this-windows-pc\ota-67-to-0.1.29-results.json`
  - `C:\Users\Administrator\Documents\Codex\2026-04-18-all-projets-on-this-windows-pc\ota-67-to-0.1.29-retry-results.json`
- likely cause if known:
  - the client can see a canceled / interrupted request at the same moment the
    device has already accepted the firmware and torn down the connection to reboot.
- recommended fix:
  - teach the harness to weigh the post-boot firmware version and reboot timing
    more heavily than the raw HTTP client exception in this edge case.
- status: mitigated in `scripts/qa-ota-stress.ps1`; the harness now records an
  effective acceptance result when the target clearly rebooted and came back

### 14. Central-enabled long soak still degrades into recovery mode under repeated transport failures

- date: 2026-05-16
- title: repeated central transport failures still drive later auto-recovery on `0.1.29`
- severity: high
- area/component: central client / long-run transport failure handling
- environment/context:
  - `0.1.29-dev-central-safe`
  - observed across `.48`, `.67`, `.69`, `.30`, `.225`
  - split-tested most directly on `http://192.168.1.225`
- reproduction steps:
  1. Run the central-enabled line through a long soak with the device enrolled.
  2. Allow repeated heartbeat / poll / firmware-check / power-upload work to continue.
  3. Observe device status and captured event snapshots over time.
- expected result:
  - the device should remain stable even if some central transport attempts fail.
- actual result:
  - `.225` stays healthy with `central.enabled=false` and `power.enabled=false`,
    but still returns to `recovery_mode` with `central.enabled=true` even when
    `power.enabled=false`.
  - the completed overnight soak later showed `.48`, `.67`, `.69`, and `.30`
    also ending in `recovery_mode=true`.
- evidence:
  - `C:\dev\rebooter-firmware\docs\overnight-capture-and-root-cause-readout-2026-05-16.md`
  - `C:\Users\Administrator\Documents\Codex\2026-04-18-all-projets-on-this-windows-pc\power-capture-2026-05-15-live\summary.json`
  - captured event snapshots under `power-capture-2026-05-15-live\events-*.json`
- likely cause if known:
  - repeated central transport failures still create enough heap churn and/or
    fragmentation to drive later early-boot failures and auto-recovery, even
    after the earlier `0.1.29` event-log persistence mitigation.
- recommended fix:
  - reduce transport-failure amplification under sustained outage
  - widen failure backoff for firmware-check / heartbeat / poll / power transport
  - trim repeated failure logging on the hot path
  - rerun a small controlled soak after the next mitigation
- status: open; this is now the primary firmware blocker
- **2026-06-17 closure (P2 triage):** the recommended fixes have all
  shipped across the 0.2.x line and the underlying recovery_mode
  signal is gone from the fleet.
  - `scheduleTransportFailureCooldown()` (introduced post-0.1.29)
    unifies retry backoff for announce / register / heartbeat / poll
    / firmware-check / power transport under a single `retryBackoffMs_`
    curve, with firmware-check floored at 120s. Closes recommended
    fix #1 + #2 (reduce amplification + widen backoff).
  - `logThrottled()` (introduced 0.2.7-era) rate-limits each
    transport-failure event-log line via a per-call-site
    `lastXFailureLogAtMs_` at the 120s
    `TRANSPORT_FAILURE_LOG_INTERVAL_MS`. Closes recommended fix #3
    (trim repeated failure logging on the hot path).
  - 0.2.6 EventLog RAM cap (30 entries × 80 chars) removed the heap
    erosion source that fed the late-boot recovery cascade.
  - 0.2.31 + 0.2.32 killed two NULL-deref crashes inside the
    transport-fail logger itself — those crashes were a likely
    contributor to the .48/.67/.69/.30/.225 recovery_mode results
    captured in 2026-05-16's overnight soak.
  - Live data 2026-06-17: zero `recovery_mode=true` heartbeats and
    zero `device.auto_recover*` events across all fleet MACs in the
    last 30 days. The original failure signal is gone.
  - Soak (recommended fix #4) has effectively run continuously
    since 0.2.34's predecessors landed (5+ weeks of fleet runtime
    on the .190 unit alone, plus the historical .185 / .188 data
    through 2026-06-13). No new failure signal has emerged.
- status: **fixed across v0.2.6 / v0.2.7 / v0.2.31 / v0.2.32** —
  closed in this triage as part of P2 on 2026-06-17. If a future
  recovery_mode cluster reappears, file a fresh entry rather than
  reopening this one; the failure surface has materially shifted.

## 2026-07-23 BUG-088 — proactive-restart cooldown persistence across non-planned reboots — 0.2.44

Operator report 2026-07-22: "i've noticed the relays resetting."

Field data at pause:
- .190: fully unresponsive since 2026-07-09 (no hub check-in, no ping
  response). Physical intervention required — outside firmware scope.
- .188: 6 consecutive `Power On` resets in ~1h with proactive-restart
  fires interleaved every ~32 min. Power On is a WALL POWER LOSS
  event, not the firmware restarting itself — likely the device is
  plugged into its own controlled outlet or the outlet is bad. Flagged
  to operator; not a firmware issue.
- .185: `Exception` boot loop with proactive-restart fires between —
  device runs 5-50 min then crashes with an Exception, reboots, runs
  again, repeats. 12 cycles in ~3h.

Root cause on .185's pattern (and the fleet in general when the
crash-then-proactive sequence happens): the 0.2.38 BUG-085 cooldown
in `central_client_heap.cpp:159-164` gated on
`status_->lastPlannedRestartReason == "heap_pressure_proactive"`.
That state is populated on boot from `bootstate.json` where
`beginBootSession()` reads `plannedRestart` + `plannedRestartReason`.
Any of Exception / Power On / WDT reboots the device WITHOUT calling
`prepareForPlannedRestart` first, so `bootstate.json` stays in the
"planned=false" state it was left in by the prior planned restart —
which then gets read as "prior was NOT proactive" on the current
boot. Cooldown blank-slates. Proactive fires again within ~30 min.

**Fix in 0.2.44**: dual-strategy cooldown check.

1. New persistent field `lastProactiveFireUnixSeconds` on
   `StoredBootState` + `BootHealthSnapshot` + `AppStatus`, serialized
   into `bootstate.json` as `last_proactive_fire_unix_s`. Missing on
   pre-0.2.44 files is treated as 0 (never fired) — matches the
   pre-fix behavior for the first proactive after upgrade.
2. `prepareForPlannedRestart(reason, proactiveFireUnixSeconds = 0)` —
   new optional 2nd argument. When the reason is
   `heap_pressure_proactive` AND the caller passes a non-zero value
   (i.e. SNTP is synced), the field is updated. All other planned-
   restart callers pass 0 (default) and don't touch it — no
   perturbation to non-heap paths.
3. `central_client_heap.cpp` reads `status_->wallClockUnixMs` +
   `status_->timeSynced` at cooldown-check time. If both are set AND
   the persisted timestamp is > 0 AND `now - persisted < 4h`, suppress
   the fire. Otherwise fall through to the legacy in-status
   `priorWasProactive` check — pre-fix behavior remains the floor
   when SNTP hasn't come up yet.

Storage cost: 4 bytes in bootstate.json. Runtime cost: one uint32
subtraction per heap-pressure check. No new failure modes: a
never-synced clock is exactly the pre-fix path.

- built: yes (27.5s, size unchanged material)
- shipped: fleet OTA push after CI green
- verify: watch .185's diag stream — should NOT see a proactive fire
  within 4h of the prior one, even if intervening resets are Exception
  or Power On

Operator report 2026-07-02: "i just re-added 2 devices to wifi again
manually that didn't auto-join voipguru" — despite 0.2.42's two-pass
walk being live fleet-wide.

Impossible to root-cause from telemetry: `walkCandidates()` logs its
decisions via `Serial.print` only, and Sonoff S31 production units
have no accessible UART. Which pass tried which SSID, how long the
scan took, and whether the retry saved the boot — all invisible.

0.2.43 ships two things:

1. **Walk-decision trace** captured in `WifiManagerService::walkTrace_`
   during `begin()` and emitted from `main.cpp` as a
   `DiagSyslog::sendEvent("wifi_walk", ...)` packet AFTER WiFi comes
   up. Records: candidate count, scan duration, scan network count,
   each attempt (pass label + SSID + connect duration + OK/FAIL),
   retry flag, and portal-fallback flag. String is capped at ~400
   chars to fit one UDP packet.

2. **Walk retry before portal.** If the first walk (Pass 1 + Pass 2)
   fails, wait 3s (radio cooldown, DHCP settle, AP beacon window)
   and walk one more time before dropping to captive portal. Common
   case (walk succeeds first time) cost zero. Bad-scan-that-happens-
   to-recover cost 3s + one more walk. Total worst case for a fully-
   deaf boot: `2 × (scan ~2s + 2 candidates × 15s) + 3s ≈ 67s`
   before portal — still well under any operator-perceived timeout.

Both changes are non-destructive: they add observability + resilience
without altering the successful boot path. Fleet retest: watch for
`wifi_walk` events at each boot and confirm the trace matches expected
happy-path (`p1:SpectrumSetup-4D...OK` or similar). Any failed boot
now leaves a forensic record.

**Empirical retry-path validation 2026-07-04.** Built a debug variant
`0.2.43-dev-central-testretry` guarded by `-DWIFI_WALK_TEST_RETRY` that
forces the first `attemptCandidate` call of the boot to return false
after 500 ms. Flashed to .190 and observed the trace:

```
cand_n=2;scan_ms=2287;scan_n=12;
p1:VoIPguru_wifi;ms=500;FAIL;
p2:SpectrumSetup-4D;ms=15266;FAIL;
retry;
scan_ms=2182;scan_n=9;
p1:VoIPguru_wifi;ms=3170;OK;
result=connected
```

End-to-end confirmed: first walk failed → retry-before-portal fired →
second walk connected on first attempt → device online without
operator intervention. `.190` reflashed back to plain 0.2.43 after
the test. Total dead-time from Power On to online: ~35s. Test hook
removed from `wifi_manager.cpp`; production tree unchanged.

The trace also incidentally confirmed **the operator's environment
has SpectrumSetup-4D unreachable** — `p2:SpectrumSetup-4D;ms=15266;
FAIL` shows the full 15s connect timeout on the scan-absent secondary.
Only VoIPguru_wifi is a viable candidate at these device locations,
so any transient VoIPguru scan miss + connect flake is the single
credible root cause for the historical "keep having to re-join"
symptom the operator reported.

**Real-prod validation 2026-07-08.** 3 days after fleet-wide 0.2.43
deploy, `.188` shows 2 boots where the retry-before-portal path fired
in production (grep for `;retry;` in `/data/diag/c4d8d50d2c1f.jsonl`):

```
2026-07-06T06:00:08Z:
  cand_n=2;scan_ms=2286;scan_n=10;
  p1:VoIPguru_wifi;ms=15330;FAIL;      ← first connect TIMED OUT
  p2:SpectrumSetup-4D;ms=15267;FAIL;   ← fallback also timed out (unreachable)
  retry;
  scan_ms=2180;scan_n=11;
  p1:VoIPguru_wifi;ms=3167;OK;         ← retry succeeded in 3.2s
  result=connected

2026-07-06T14:10:02Z: (same shape, ~8 hours later on same device)
```

Notable: on both retry boots, VoIPguru_wifi **was in the scan result**
on the first walk (Pass 1 attempted it) but the connect itself timed
out at ~15.3s. That's a transient-connect-failure mode, distinct from
the hidden-SSID / scan-miss failure mode the 0.2.42 two-pass walk
was designed for. Root cause is likely momentary AP unresponsiveness,
bad-radio-state after boot, or slow DHCP — the 3s retry cooldown
resets it and the second attempt connects in 3.2s.

That closes the "does retry ever actually fire in prod" question:
**yes, 2 times in 3 days on `.188`, saving 2 operator-intervention
events**. Zero `portal_fallback` events across all 3 devices in the
same window. The fix is doing what it was designed for and also
catching a failure mode the design didn't originally target.

## 2026-06-30 BUG-087 — walkCandidates skips scan-absent SSIDs including hidden — fixed in 0.2.42

Operator-reported 2026-06-30: "if the rebooters are supposed to have
2 built-in wifi networks, one of which i keep having to rejoin them
to, why do i have to keep rejoining them to it?"

Root cause: `wifi_manager.cpp::walkCandidates()` used the boot-time
`WiFi.scanNetworks()` result as a HARD GATE. Any candidate SSID not
in the scan result was skipped ("Skipping absent SSID" in the diag
log), even if the SSID was actually reachable.

Two failure modes fell out of that:
1. `scanNetworks()` on the ESP8266 is unreliable — busy 2.4 GHz,
   weaker beacon APs, or partial-scan results routinely under-report.
2. **Hidden SSIDs never show up in a scan.** A built-in network with
   broadcast disabled was permanently unreachable at boot until
   captive-portal re-provisioning. Every proactive-restart cycle
   (still firing occasionally on the fleet even with BUG-085's 4h
   cooldown) rolled the credential-loss dice again.

Combined with `begin()` dropping to `State::Portal` on
walkCandidates failure — and `loop()` returning early when in
Portal state so the runtime supervisor never re-tries — the device
would sit in captive portal until manually re-provisioned. That's
the "have to keep rejoining" symptom.

**Fix in 0.2.42**: two-pass walk.
- Pass 1: scan-flagged-visible candidates only (preserves the
  pre-fix fast path — ~3s to connect on the common happy case).
- Pass 2: candidates the scan flagged as absent — try them anyway.
  Catches hidden SSIDs + scan-underreport cases. Skipped entirely
  if the scan itself failed (Pass 1 already tried everyone).

Worst case (fully cold boot, all candidates absent): (2 built-in +
saved) × 15s ≈ 60-105s before falling through to captive portal.
Common case unchanged from prior behavior. Steady-state cost zero.

- size: built and verified
- staging: 0.2.42-dev-central-safe to be published + assigned to all
  3 fleet devices (.185, .188, .190)
- status: open; verify by watching a fleet reboot cycle and
  confirming no captive-portal drops on units currently on 0.2.39.

## 2026-06-21 BUG-086 — WiFi-credential loss on reboot — fixed in 0.2.39

**Operator-reported 2026-06-21**: "why are these rebooters needing
to be reset on wifi all the time? i'm resetting 3 now."

Live diagnosis traced to `config_manager.cpp::loadFromPath()`
line 270-274. The pre-fix code self-destructed any config file
with size 0 or > 8192 bytes:

    if (fileSize == 0 || fileSize > maxSafeBytes) {
      f.close();
      LittleFS.remove(path);  // ← active aggression
      return false;
    }

**Failure chain:**
1. Save sequence (config_manager.cpp:531-539) is atomic only when
   the rename steps complete. A reboot during the window between
   `LittleFS.rename(configPath_, LAST_KNOWN_GOOD_PATH)` and
   `LittleFS.rename(TEMP_CONFIG_PATH, configPath_)` leaves
   `/config.json` missing and `/config.tmp` and `/config.lkg.json`
   on disk.
2. Next boot, `load()` sees `/config.json` missing, falls back to
   LKG. **If the LKG file is itself 0-byte** (from a prior similar
   interruption), loadFromPath enters the size-check branch and
   DELETES the LKG too.
3. Now BOTH primary and LKG are gone. load() falls through to
   `out = AppConfig()` defaults (line 525) — which have an empty
   `wifi.savedNetworks` list — and `save()` writes the empty-creds
   config to disk.
4. Device boots with no WiFi credentials, enters captive portal,
   requires operator physical reset + re-provision.

**Amplifier**: the BUG-077 / 0.2.36-era proactive-restart burst
loop. .190 logged 19 reboots in 24h on 0.2.36. Each reboot has
some probability of landing mid-save; with 19/day the credential-
loss event compounds across the fleet. BUG-085's 4h cooldown
(0.2.38) reduces the reboot frequency but doesn't fix the
underlying credential-wipe vector.

**Fix in 0.2.39**: don't delete malformed files from
`loadFromPath()`. Return false (recovery chain still tries the
alternate), leave the truncated file on disk. A subsequent
successful save() atomically replaces it via the rename
sequence. Only delete on confirmed-good replacement, not on
first-glance malformation. Self-healing without self-destruction.

The flash-wear angle (different root cause, same symptom) is
unchanged by this fix — a genuinely-corrupt LKG that returns
`DeserializationError` from ArduinoJson still triggers the
defaults fallback. That's a separate hardening pass for a
later ship.

- size: built and verified
- staging: 0.2.39-dev-central-safe to be published + assigned to .190
- status: open; need .190 promotion + soak + zero-credential-loss
  observations across the fleet to call it fixed.

## 2026-06-20 BUG-085 — proactive-restart burst still firing 19×/24h on 0.2.36 — fixed in 0.2.38

After 0.2.36's BUG-084 semantic flip, .190 still logged 19 proactive
restarts in 24h. Trajectory data confirmed the discriminator IS
working correctly — it sees real ≥1024B drops (single-event BearSSL
allocations dropping mfb 18184 → 13472) and fires.

The deeper issue: the 30-min `HEAP_PRESSURE_MIN_UPTIME_S` gate is
the actual cadence. mfb is sustained sub-threshold from boot on
this device; the OUTER debounce passes the moment the gate clears;
the discriminator correctly identifies erosion; we fire. Reboot,
repeat — every 30 minutes for 24+ hours.

The fix is NOT smarter discrimination — the discriminator is right.
The fix is a burst-suppressor: if the PRIOR boot was already a
proactive restart, give the device 4 hours of runtime before the
next proactive fire is eligible. Caps the worst-case cadence at
one proactive every ~4.5 hours (30-min gate + 4h suppression)
instead of every 30 min.

**Protection preserved:** the FIRST proactive after any boot still
fires after 30 min of sustained pressure. The .185 WiFi-SDK
NULL-deref scenario fired within 80s of sustained sub-13K mfb —
the protection still wins by a factor of ~22x on cold boots.

**Sanity check:** .190 has been crash-free since 0.2.34 install
(no Hardware Watchdog, no Exception). The proactive was firing on
DEFENSIVE pressure not actual incidents. The burst-suppressor
trades modest WiFi-SDK risk in hour 1-4 of any post-proactive run
against eliminating the 30-min reboot loop.

- size: RAM 57.0% / Flash 67.5% (no change)
- build: clean
- staging: 0.2.38-dev-central-safe published to dev channel
- status: open; need .190 promotion + 24h soak to verify cadence
  drops from 19/day to ≤6/day.

## 2026-06-18 BUG-084 — 0.2.34 trajectory discriminator misclassifies FLAT mfb as erosion — fixed in 0.2.36

The 0.2.34 BUG-077 fix (b) — `heapTrajectoryRecovering()` — had the
wrong semantic. The original guard returned `true` only when mfb
had trended UP by ≥1024 bytes over the ring window. A FLAT mfb
(newest == oldest, stuck at e.g. 12456 for 60s) returned `false`,
so the caller fired the proactive restart even though no actual
erosion was happening.

That's exactly the .190 fragmented-boot pattern BUG-077 was meant
to suppress. Live data confirms: .190 logged 10 reboots between
0.2.34 install at 2026-06-18 10:54 UTC and 21:41 UTC — once it
got into the bursting pattern (around 18:00, triggered by my
relay-test traffic) it kept firing every ~30 minutes.

Captured trajectory right before the 21:05:54 proactive restart:
mfb=12456 across all 12 ring samples (60 seconds of dead-flat
readings, fh=20696 healthy, frag=36%). Not eroding. Pure
post-fragmentation steady state.

**Fix in 0.2.36:** semantic flip.
- TRUE (suppress) when mfb is FLAT or recovering (`newMfb >= oldMfb`,
  or any drop < `HEAP_PRESSURE_RECOVERY_DELTA`).
- FALSE (fire) only when an actual drop of ≥1024 bytes is observed
  over the 30-60s window — real erosion.

The .185-style WiFi-SDK NULL-deref protection still lives in the
13K threshold + 15s debounce; the discriminator only suppresses
the FLAT-low pattern that 0.2.34 was supposed to solve but didn't.

**Status: fixed in 0.2.36.** .190 promotion + 24h soak window
re-starts at 0.2.36 install time.

---

## 2026-06-17 P3 — firmware-side next-tier code review (BUG-079..083)

Parallel to the hub-side #201 sweep that filed BUG-072..076. Four
parallel reviewer angles over `git diff v0.2.6..HEAD` (28 versions,
~1700 LOC of changes). Five candidates survived verification by
direct file read; six others were de-prioritized as theoretical or
not actually reproducible (heap-ring init concern dropped because
`heapRingCount_ < 6` gate already covers it).

### BUG-079 — CRITICAL — NULL-deref in 3 more transport-failure paths — fixed in 0.2.35

Same shape as BUG-208 / BUG-209 (fixed in 0.2.31 / 0.2.32), in three
sites the 0.2.32 fix did NOT reach: `announceDevice`,
`registerDevice`, and `pollCommands`.

Pattern at `central_client.cpp:431-433`, `:842-844`, `:1052-1054`:
```cpp
const String detail = response.isEmpty()
    ? String("unknown transport error")
    : summarizeResponse(response);
Serial.print("Central <op> transport failed: ");
Serial.println(detail);
```

`summarizeResponse()` returns a `String` built from
`response.replace(...).substring(0, maxLen)` — each operation can
allocate a NULL-buffer String under heap pressure. `Serial.println
(String)` then calls `detail.c_str()` which returns the NULL buffer
and `Serial.println(const char*)` derefs NULL → EXCEPTION_RST 28/29.

Plus three concat'd `eventLog_->add("central", "X failed (" +
String(code) + "): " + summarizeResponse(response))` calls at
`:443`, `:536`, `:883` that chain 3+ String operations.

**Status: fixed in 0.2.35.** All three sites (announce L431,
register L863, poll L1087) mirror the 0.2.32 heartbeat-path flow:
literal `Serial.println` (no allocation), literal `logThrottled`
(eventLog::add bails internally if mfb<4K), forensic detail packed
via `snprintf` into a stack `char detailBuf[220]` and sent through
`DiagSyslog::sendEventCStr` (stack-only path). Three concat'd
`eventLog_->add(..., "X failed (" + String(code) + "): " +
summarizeResponse(...))` sites at L443, L558, L905 replaced with
`snprintf(failBuf, ..., "X failed (code=%d, mfb=%u)", code,
ESP.getMaxFreeBlockSize())` into a new `EventLog::add(const char*,
const char*)` overload. The new overload runs `DiagSyslog::
sendEventCStr` BEFORE the mfb<4K bail, then constructs the
internal Strings only if heap is healthy enough — eliminates the
implicit `char*→String` allocation at the call site which was the
remaining bug surface even with the in-function bail.

### BUG-080 — MEDIUM — `logThrottled` sentinel collision at millis()==0 — fixed in 0.2.35

`central_client.cpp:258`:
```cpp
if (lastAtMs != 0 && (now - lastAtMs) < minIntervalMs) return;
```

After `millis()` rollover (~49 days), if `lastAtMs` was set near
`UINT32_MAX` and `now` wraps to a small number, `now - lastAtMs`
underflows to a huge `uint32_t` (rollover of unsigned subtraction
gives the small positive delta — actually this is CORRECT for
`uint32_t`). Re-checking: `now=10, last=0xFFFFFFF0` →
`now - last = 10 - 0xFFFFFFF0 = 0x14` = 20 → comparison works.
So the OBVIOUS rollover concern is actually safe due to unsigned
arithmetic semantics.

The real bug is the `lastAtMs != 0` short-circuit: if a log
happens to fire at exactly `millis() == 0` (statistically possible
at boot or rollover), `lastAtMs` is set to 0, the next call
treats it as "never throttled before" and re-logs immediately.
Minor; logs once-extra per ~49 days per call site.

**Status: fixed in 0.2.35.** Sentinel switched from `0` to
`UINT32_MAX`. `millis()` rolls 0xFFFFFFFF→0 (never lands on
0xFFFFFFFF naturally because the increment from there wraps), so
UINT32_MAX is a true "never fired" marker that doesn't collide
with any valid timestamp. All seven `lastXFailureLogAtMs_` member
initializers in `central_client.h` updated to `UINT32_MAX`. The
guard in `logThrottled` changed from `if (lastAtMs != 0 && ...)`
to `if (lastAtMs != UINT32_MAX && ...)`.

### BUG-081 — LOW — `shouldUseCompactHeartbeat` dual-gate can oscillate

`central_client.cpp:265-273` (the 0.2.34 (a) fix added an `mfb<14K`
gate next to the existing `fh<20K`). A device near both thresholds
can flip between compact and verbose modes every cycle: verbose
peaks slice mfb, compact lows let fh recover. Each mode-switch
costs JSON re-serialization.

**Status: fixed in 0.2.37.** Add hysteresis: separate "enter compact"
thresholds (`fh<20K` / `mfb<14K`) from "exit compact" thresholds
(`fh>22K AND mfb>16K`). Defer; no observed regression in the
fleet data so far.

### BUG-082 — LOW — `prepareForPlannedRestart` not cleared on failure paths

`config_manager.cpp::prepareForPlannedRestart()` stages a reason
into `bootstate.json` so the next boot's classifier knows it was
planned. Cleared in `checkFirmwareAssignment` on
`HTTP_UPDATE_FAILED` / `HTTP_UPDATE_NO_UPDATES`. NOT cleared in:
- `web_server_manager.cpp` factory-reset endpoint when the user
  kills power between `prepareForPlannedRestart` and the
  `ESP.restart()` call.
- Hub-issued `device_restart` command path.
- The proactive heap-pressure restart path (always reaches
  `ESP.restart()` so this is fine — included for completeness).

**Status: fixed in 0.2.37.** Failure mode: diagnostic misattribution — a
brown-out gets logged as the planned reason on next boot. Fix:
guarantee `clearPlannedRestart()` runs whenever the stage is set
but the restart doesn't fire (set a stack-allocated RAII guard
that clears on scope exit unless explicitly disarmed before the
`ESP.restart()` call).

### BUG-083 — CLEANUP — HTTPS client setup duplicated across 3+ sites

`central_client.cpp:578-581`, `:644-650`, `:715-721` (approx) all
do:
```cpp
std::unique_ptr<BearSSL::WiFiClientSecure> client(new ...);
client->setInsecure();
client->setBufferSizes(512, 512);
HTTPClient http;
```
plus the boilerplate around `http.setTimeout`, `http.begin`,
`http.addHeader`, response handling, and the 9-line "0.2.5
pool-revert" comment.

**Status: fixed in 0.2.37.** Real cleanup candidate. Extract into
`scopedHttpsClient()` that returns a small RAII wrapper carrying
the configured `BearSSL` + `HTTPClient` pair. Future TLS-buffer
tuning changes one site instead of three. Defer to a refactor
ship; not blocking BUG-079.

---

## 2026-06-16 0.2.34 — BUG-077 proactive-restart burst loop (.190)

See `rebooter-droids/docs/bug-log.md` BUG-077 for the full diagnosis.
Firmware side ships fixes (b) + (c) here; (a) follows in 0.2.35.

**(b) Trajectory-slope discriminator in
`CentralClient::maybeHeapPressureRestart`.** After the existing
debounce confirms mfb < 13K for 3 consecutive samples, consult the
heap-trajectory ring (12 × 5s = 60s of history). If mfb has been
trending UPWARD across the window — newest sample exceeds the oldest
by ≥1024 bytes — the pressure is fragmentation resolving on its own.
Suppress the proactive restart and reset the debounce. If mfb is flat
or trending DOWN, that's real erosion; fire as before. Distinguishes
.190 fragmented-boot oscillation (8K↔15K) from .185-class erosion
(steady decay). Constant `HEAP_PRESSURE_RECOVERY_DELTA = 1024`,
ring-min-samples 6 (30s window).

**(c) Pre-crash breadcrumb coverage for LittleFS writes.** .190's
Hardware Watchdog reset on 2026-06-14 18:33 produced NO breadcrumb
event — the prior opcode set was HTTPS-only. Added three new opcodes
(OP_FS_EVENT_LOG_WRITE, OP_FS_CONFIG_WRITE, OP_FS_BOOT_STATE_WRITE)
and wrapped every persist() call site in
`PreCrashBreadcrumb::Scope`. Next watchdog crash will surface which
FS write was in flight at the moment the loop got starved.

**(a) Compact heartbeat on fragmentation, not just on low free-heap.**
Deeper trajectory analysis flipped the original "find an 8K alloc"
framing. mfb dips with EVERY heartbeat (per-call BearSSL handshake +
JSON body) and recovers in the inter-beat gap — not a persistent
leak. The verbose heartbeat is the dominant per-call fragmenter when
mfb is already low. Pre-fix `shouldUseCompactHeartbeat()` gated on
`free_heap < 20K` only; .190's fh sat 20-21K (above the gate) while
mfb sat 8-15K (well into danger). Added an `mfb < 14K` companion
gate so compact engages when fragmentation alone warrants it.

- size: RAM 56.9% / Flash 67.7% — no growth (matches 0.2.33).
- build: clean against sonoff_s31 env, zero warnings.
- staging: 0.2.34-dev-central-safe; -dev-central-safe-badboot also
  bumped for the bad-boot test build.
- status: open; ship to .190 first, soak 24h to confirm bursts no
  longer fire on fragmented post-watchdog boots. Then promote to
  fleet-stable.
