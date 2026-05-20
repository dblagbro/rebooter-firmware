# Rebooter Firmware — Tier-2 Feature Design & Implementation Plan

Date: 2026-05-20
Author: D. Blagbrough
Baseline: `master` / `9797704`, firmware 0.1.40, Sonoff S31 (ESP8266 / esp12e)
Status: DESIGN ONLY — no source changes made. This document is a design pass; it is not committed.

---

## 0. Context and constraints

### 0.1 Memory budget (the dominant constraint)

The ESP8266 on the S31 has roughly 80 KB of usable heap. Real-world observed free heap on the
fleet runs much lower once Wi-Fi, TLS, the web server, ArduinoJson, and the event log are live.
Hard evidence already in the tree:

- `central_client.cpp`: compact-heartbeat kicks in below **20000** bytes free; compact power
  upload below **21000** bytes free. The code treats ~20 KB free as "already in trouble."
- `event_log.cpp`: refuses to grow the log below **12000** bytes free.
- `bug-log.md` / `NEXT_STEPS.md`: `central=true` + `power=true` together still crash low-heap
  units. The crash is heap churn / fragmentation in the central TLS path, not a single leak.
- Every outbound HTTPS call allocates a `BearSSL::WiFiClientSecure` with `setBufferSizes(512,512)`
  — even reduced, each TLS session is multi-KB and is the single largest transient allocation.

**Design rule for every feature below:** no feature may add a *standing* heap cost that
materially shrinks the steady-state floor, and no feature may add a *second concurrent* TLS
session or a large concurrent buffer alongside the central client. Where a feature needs
network I/O, it must reuse the existing serialized request cadence, never run in parallel.

### 0.2 Current architecture facts that shape these designs

- Config is a single `AppConfig` struct (`include/types.h`), persisted as `/config.json` via
  `ConfigManager` with last-known-good (`/config.lkg.json`) + temp-file atomic rename.
- `CONFIG_SCHEMA_VERSION` is currently **3**. Loader is tolerant: every field uses
  `doc[...] | default`, so adding fields is backward-compatible; removing/renaming is not.
- Wi-Fi today: `wifi_manager.cpp` first tries the two hardcoded `DevWifiConfig::NETWORKS`,
  then hands off to `tzapu/WiFiManager` (`autoConnect` / `startConfigPortal`) which keeps its
  *own* single SSID/password in ESP SDK flash. `BootstrapConfig` (stage-1 OTA loader) has a
  parallel hardcoded list.
- Central hub URLs already support a **list**: `CentralConfig::baseUrls` is a `std::vector<String>`,
  loader accepts `base_urls[]` (and legacy scalar `base_url`), `validateConfig` caps it at **4**
  entries and drops a known-legacy URL. `postWithFallback`/`getWithFallback` iterate the list.
- Power: `PowerMonitor` already does **real CSE7766 frame parsing** — 24-byte frame, 4800 8E1,
  checksum, voltage/current/power/energy. It is started lazily (`main.cpp`: only after 10 s
  uptime, Wi-Fi up, `power.enabled`, not in recovery). It uses `SoftwareSerial` RX-only on
  `GPIO3` (`Pins::POWER_MONITOR_RX = 3`).
- Crash capture: none. Exceptions only reach `Serial`. There is a boot-health state machine
  (`beginBootSession` / consecutive-unhealthy-boots / auto-recovery) but it records *that* a
  boot failed, never *why*.
- Web/API: single `ESP8266WebServer` on port 80. Public reads: `/api/status`, `/api/config`
  (redacted), `/api/events`. Protected (header `X-Rebooter-Auth`): relay, config save, OTA,
  reboot/recovery/factory-reset, diagnostics. UI is served from LittleFS `data/` with
  embedded fallbacks in `web_server_manager.cpp`.

### 0.3 Two separate Wi-Fi front-ends

There are **two** captive-portal paths: the runtime firmware (`wifi_manager.cpp` + tzapu
WiFiManager) and the stage-1 bootstrap loader (`bootstrap_main.cpp`). This design changes the
**runtime** firmware. The bootstrap loader keeps its hardcoded list as a deliberate
disaster-recovery floor and is explicitly out of scope except where noted in §1.7.

---

## 1. Feature 1 — Multi-Wi-Fi

### 1.1 Goal

Keep the two `dev_wifi_config.h` networks as **built-in defaults**. Add a user-managed list
of saved networks (primary / secondary / tertiary and more). On boot, try saved networks in
priority order, then the built-in dev networks, then fall back to AP-mode captive portal.
The AP portal and the device WebUI both edit the saved list.

### 1.2 Decision: stop relying on tzapu WiFiManager as the credential store

Today the *only* persisted user network is whatever tzapu WiFiManager wrote to SDK flash —
exactly one SSID. tzapu cannot store a prioritized multi-network list. We keep tzapu **only**
as the AP/captive-portal HTTP server (it is already a dependency and handles DHCP/DNS), but
we **own the credential list ourselves** in `AppConfig` and drive `WiFi.begin()` directly.

This also fixes a latent issue: tzapu's stored credential and our config can currently
disagree. After this change, our config is the single source of truth.

### 1.3 Config schema changes

Add to `types.h`:

```cpp
struct WifiNetwork {
  String ssid;          // <= 32 chars
  String password;      // <= 64 chars ("" = open network)
  // priority is implicit = vector index (0 = highest)
};

struct WifiConfig {
  std::vector<WifiNetwork> savedNetworks;   // user-managed, max 5
  uint32_t connectTimeoutMs = 20000;        // per-network attempt budget
  bool preferStrongestKnown = false;        // optional: scan-first ordering
};
```

Add `WifiConfig wifi;` to `AppConfig`. Bump `CONFIG_SCHEMA_VERSION` to **4**.

JSON shape (new `wifi` object in `/config.json`, `/api/config`, `/api/config/save`):

```json
"wifi": {
  "saved_networks": [
    {"ssid": "HomeNet",  "has_password": true},
    {"ssid": "ShopNet",  "has_password": true}
  ],
  "connect_timeout_ms": 20000,
  "prefer_strongest_known": false
}
```

- Public `/api/config` returns `has_password` only — **never** the plaintext password.
- `/api/config/save` accepts `password` per entry; an entry with `"password"` omitted but
  matching an existing `ssid` keeps the stored password (edit-without-retype). A sentinel
  empty `"password": ""` explicitly sets an open network — distinguish "field absent" from
  "field present and empty" using `doc[...].is<const char*>()`, which the codebase already does.
- `validateConfig`: trim SSIDs, drop empty SSIDs, dedupe by SSID (keep first/highest priority),
  cap at **5** saved networks, clamp `connect_timeout_ms` to 5000–60000.

`dev_wifi_config.h` stays as-is. The dev networks are **not** copied into `savedNetworks`;
they remain a compile-time fallback tier so a wiped config still joins the bench networks.

### 1.4 Connection state machine

Replace the linear `tryDevWifi()` then-portal flow with an explicit state machine in
`WifiManagerService`. States:

```
INIT -> CONNECTING(idx) -> CONNECTED
                       \-> next idx ... -> PORTAL -> CONNECTED (after provisioning)
CONNECTED -> (link lost) -> RECONNECTING -> CONNECTED | PORTAL
```

Boot-time ordered candidate list, built once:

1. `wifi.savedNetworks[0..n]` in priority order.
2. `DevWifiConfig::NETWORKS[0..n]` (built-in bench fallback).
3. If all fail → `PORTAL` (tzapu AP, captive portal at 192.168.4.1).

Per-candidate attempt: `WiFi.begin(ssid, pass)`, poll `WiFi.status()` with `yield()` until
`connectTimeoutMs` or success. This mirrors the existing `tryDevWifi()` loop; total worst-case
boot delay = `(savedNetworks + devNetworks) * connectTimeoutMs`. With 5 saved + 2 dev at 20 s
that is up to 140 s — **a red flag** (see §1.8): mitigate by defaulting the timeout to 12–15 s
and short-circuiting on a scan (`WiFi.scanNetworks()`) so we only attempt SSIDs actually
present. The scan adds a one-time ~2 s and a transient allocation but avoids waiting full
timeouts on absent networks; do the scan **once** at boot, free the result immediately.

**Runtime reconnect:** today `WifiManagerService::loop()` is empty and reconnection is left to
the SDK auto-reconnect against the last SSID. Add a lightweight reconnect supervisor: if
`WiFi.status()` is disconnected for > `connectTimeoutMs`, re-walk the candidate list (non-blocking,
one `WiFi.begin` per N seconds). Do **not** drop into the AP portal on a transient runtime drop —
SPECS §16.1 explicitly says do not reboot/relinquish on transient Wi-Fi loss. Only enter the
portal from the *boot* path or on explicit operator request.

### 1.5 AP captive portal editing

tzapu's stock portal cannot render a multi-network list editor. Two options:

- **Option A (recommended):** keep tzapu only for the "join an AP, get DHCP, capture DNS"
  mechanics, and register **custom HTTP handlers** on tzapu's web server (tzapu supports
  `setCustomHeadElement` / custom parameter pages, but cleanest is a custom `setSaveConfigCallback`
  plus our own `/wifi` page). The portal page POSTs a small form: add one SSID+password at a
  time, list current saved SSIDs with delete buttons. Each submit appends to `wifi.savedNetworks`
  and re-attempts connection. This keeps the portal minimal (memory-friendly) and avoids
  shipping a second full UI.
- **Option B:** drop tzapu entirely, run our own `ESP8266WebServer` in AP mode + `DNSServer`
  for the captive redirect. More code, more control, removes a dependency, but more risk.
  Defer to a later tier.

Go with **Option A**. The AP portal form is intentionally spartan (no JS framework, server-rendered)
so it costs almost nothing in flash/heap.

### 1.6 Device WebUI editing

In normal (connected) mode, the existing authenticated WebUI gets a "Wi-Fi Networks" card:

- Lists saved networks with priority order, SSID, `has_password`, and (live) current RSSI/SSID.
- Add / delete / reorder (reorder = move up/down, which just reindexes the vector).
- Password field is write-only; blank = keep existing.
- Saved via the existing `POST /api/config/save` (the `wifi` object). No new endpoint needed
  for editing the list.
- Add one **action** endpoint `POST /api/wifi/scan` (protected) returning nearby SSIDs+RSSI to
  populate a picker. Scanning while connected briefly disrupts the link; gate it behind an
  explicit button, run synchronously, free the scan result immediately.

### 1.7 Bootstrap loader note

`BootstrapConfig::WIFI_NETWORKS` stays hardcoded — it is the recovery floor. Optionally, a
later change can have the bootstrap loader read `/config.json`'s `wifi.saved_networks` from
LittleFS before falling back to its hardcoded list, so a user who changed networks can still
receive stage-1 OTA. Flagged as a **follow-up**, not part of this tier (LittleFS access in the
bootstrap image adds flash + risk to the smallest, most safety-critical binary).

### 1.8 Memory impact

- Standing cost: `std::vector<WifiNetwork>` of up to 5 × (2 `String`s). Each `String` is ~16 B
  overhead + heap for content. 5 networks ≈ 5 × (32+64+~40) ≈ **~700 B** standing. Acceptable.
- Transient: one-time `WiFi.scanNetworks()` result (~100 B/AP, freed immediately). Fine.
- No new TLS session, no new server. **Net assessment: low risk.**
- RED FLAG: serial boot delay if many saved networks all time out. Mitigate with scan-gating
  and a 12–15 s default per-network timeout.

### 1.9 Implementation steps

1. Add `WifiNetwork` / `WifiConfig` to `types.h`; add `wifi` to `AppConfig`; bump schema to 4.
2. `config_manager.cpp`: load/save/validate the `wifi` object; dedupe + cap at 5; clamp timeout.
   Keep the `has_password` redaction in the web layer, not the config layer (config stores plaintext).
3. Rewrite `WifiManagerService` as a state machine: ordered candidate list (saved → dev →
   portal), per-candidate `WiFi.begin` with timeout, boot-time single scan to skip absent SSIDs.
4. Implement non-blocking runtime reconnect supervisor in `WifiManagerService::loop()`.
5. Wire tzapu portal custom `/wifi` page (Option A) that appends to `savedNetworks`.
6. WebUI: add "Wi-Fi Networks" card to `data/index.html` + `app.js` + embedded fallbacks;
   serialize/deserialize the `wifi` object in `sendConfigJson` / `config/save`.
7. Add protected `POST /api/wifi/scan`.
8. Test: wiped config joins dev nets; saved nets win priority; all-fail → portal; portal add →
   connect; reorder persists; transient drop reconnects without entering portal.

---

## 2. Feature 2 — Configurable hub URLs (up to 10)

### 2.1 Goal

Up to **10** hub/central URL slots (index 0–9). Defaults = current hub URLs. Settable via the
AP portal **and** the device WebUI ("add another", up to 10). The hub is a public multi-tenant
SaaS, but users may self-host — so the list must be fully user-editable, not just reorderable.

### 2.2 Current state — most of this already exists

`CentralConfig::baseUrls` is **already** a `std::vector<String>`, already loaded from a JSON
array, already iterated by `postWithFallback` / `getWithFallback` / `postWithoutResponseWithFallback`.
The work is mostly **raising a cap and surfacing UI**, not building failover from scratch.

Current limits to change:
- `validateConfig` (`config_manager.cpp`): `if (cleanedBaseUrls.size() >= 4) break;` → raise to **10**.
- `validateConfig` hard-drops the legacy URL `https://www2.voipguru.org/rebooter`
  (`isLegacySecondaryCentralUrl`). Keep this drop (it is a one-time migration cleanup) but it
  must not silently delete a *self-hoster's* URL — it only matches that one exact string, so safe.
- Per-URL length cap is 192 — keep, but document it in the UI (self-hosted URLs can be long).

### 2.3 Schema

No struct change needed — `baseUrls` is already a vector. Only the **cap** changes (4 → 10) and
the validation. Schema version bump to 4 (shared with Feature 1) is sufficient; the loader is
already array-aware and legacy-scalar-aware, so old configs upgrade transparently.

Keep defaults centralized: today the default `"https://www.voipguru.org/rebooter"` is
hardcoded in three places (`types.h` initializer, `validateConfig` empty-fallback,
`config_manager.cpp` migration). **Recommendation:** define the default hub URL list once in a
new `include/hub_defaults.h` (or extend `bootstrap_config.h` semantics) and reference it
everywhere, so "defaults = current hub URLs" stays true after future URL changes.

### 2.4 Failover logic

Already correct in shape; document and tighten the contract:

- `*WithFallback` iterates `baseUrls` in order, returns on first 2xx, treats 429 as
  "reached, rate-limited," treats a 4xx with a JSON envelope as "reached" (server-level
  decline, not a transport failure → stop, don't try the next URL), and only *continues* to
  the next URL on 5xx or transport failure. This is sound. Keep it.
- With 10 URLs, a fully-down list means 10 sequential TLS handshakes per cycle. Each handshake
  is the most expensive heap event in the firmware. **RED FLAG** — see §2.6. Mitigation:
  - Track a `lastGoodBaseUrlIndex_` and start the iteration **from the last known-good URL**,
    not always index 0. Most cycles then do exactly one handshake.
  - Cap the *attempted* URLs per cycle (e.g. try at most 3 per loop iteration, rotate the
    starting point), so a 10-entry all-down list never does 10 handshakes back-to-back.
  - The existing transport-failure cooldown / backoff already throttles repeated full failures.

### 2.5 AP portal + WebUI editing

- **WebUI:** the config form already round-trips `central.base_urls[]` through `/api/config`
  and `/api/config/save`. Add an "Add another hub URL" repeating-row control (up to 10) to
  `data/index.html` / `app.js` and the embedded fallback. The save path already deserializes
  the array. Minimal new firmware code.
- **AP portal:** add hub-URL fields to the custom portal page from §1.5 (one URL field +
  "add another", capped low for portal simplicity — e.g. portal allows setting the first 2,
  full 10-slot editing lives in the WebUI). Rationale: the portal is used pre-connectivity by
  a user who almost always wants the default SaaS hub or one self-hosted URL; 10-slot editing
  on a memory-constrained server-rendered page is not worth the cost.

### 2.6 Memory impact

- Standing: up to 10 `String` URLs ≈ 10 × (~16 B + ~60 B content) ≈ **~760 B**. Acceptable.
- Transient: unchanged per request (one TLS session at a time) — but **worst-case cycle cost
  rises** if all 10 are down. The "start from last-good" + "max 3 attempts/cycle" mitigations
  in §2.4 are **required**, not optional, given the documented `central` heap fragility.
- **Net assessment: low standing risk; medium transient risk without the iteration cap.**

### 2.7 Implementation steps

1. Add `include/hub_defaults.h` with the canonical default URL list; reference it from
   `types.h`, `config_manager.cpp` (both fallback sites).
2. `config_manager.cpp`: raise the `baseUrls` cap 4 → 10 in `validateConfig`.
3. `central_client.cpp`: add `lastGoodBaseUrlIndex_`, start `*WithFallback` iteration there;
   cap attempted URLs per cycle at 3 with a rotating start.
4. WebUI: "Add another hub URL" repeating rows in `data/` assets + embedded fallback.
5. AP portal: add 1–2 hub-URL fields to the custom portal page.
6. Test: 10 URLs persist; failover picks a healthy URL; last-good is preferred next cycle;
   all-down does not exceed 3 handshakes/cycle; self-hosted `http://` URL works.

---

## 3. Feature 3 — LAN discovery beacon

### 3.1 Goal

After AP setup, a mobile app must find the device on the LAN (it just moved from the device's
own AP to the user's Wi-Fi and no longer knows the IP).

### 3.2 Option evaluation (ESP8266)

| Option | Pros | Cons / ESP8266 cost |
|---|---|---|
| **mDNS** (`ESP8266mDNS`) | Standard, `_rebooter._tcp` service + TXT records, well supported, app-side libs everywhere (Android NSD, iOS Bonjour) | Standing memory + a UDP listener; multicast can be flaky on some consumer routers / guest VLANs; responder runs continuously |
| **UDP broadcast beacon** | Tiny, full control of payload + cadence, no continuous listener if device only *broadcasts*, trivial app side (open a UDP socket, listen) | Non-standard; broadcast may be filtered on some networks; app must be actively listening in a window |
| **SSDP** (`ESP8266SSDP`) | Discoverable by UPnP-aware tooling | Heavier, XML device description, overkill, weakest app-side story for a custom app |

### 3.3 Recommendation: mDNS as primary + a lightweight UDP broadcast as a fallback/announce

- **Primary: mDNS.** Advertise hostname `rebooter-<chipid6>.local` and a service
  `_rebooter._tcp` on port 80 with TXT records. This is the cleanest app integration and
  `ESP8266mDNS` is a first-party library, no new third-party dependency. `MDNS.begin()` after
  Wi-Fi connects, `MDNS.update()` in the loop (cheap).
- **Fallback: a UDP "I just joined" announce.** Immediately after a fresh portal provisioning
  (`provisionedViaPortal()` is already tracked) and for the first ~2 minutes, send a small UDP
  broadcast every 5 s to a fixed port (e.g. 51999). This covers the exact AP-setup handoff
  window where the app is actively looking and where mDNS may not have propagated yet, and it
  works on networks that filter multicast. After the announce window, stop broadcasting — only
  mDNS remains as the steady-state discovery surface.

Rationale for both: mDNS gives the durable, standards-based path; the short post-setup UDP
burst gives a reliable, low-cost handoff exactly when the app needs it, then goes silent so
there is no permanent broadcast traffic or standing socket beyond mDNS.

### 3.4 Beacon payload

UDP announce payload — small JSON, well under one MTU:

```json
{
  "t": "rebooter-beacon",
  "v": 1,
  "id": "<chipid6>",
  "name": "<device_name>",
  "ip": "192.168.1.42",
  "fw": "0.1.41",
  "mode": "smart_plug",
  "api": 80,
  "auth_set": true
}
```

mDNS TXT records carry the equivalent compact subset: `id`, `name`, `fw`, `mode`, `auth`.

`auth_set` / `auth` lets the app show "this device still needs an admin password" vs "log in."
Do **not** put any secret, token, or password in the beacon — it is plaintext on the LAN.

### 3.5 Cadence

- UDP announce: every 5 s for the first 120 s after a fresh portal provisioning, then stop.
  Also send a 3-packet burst once on every normal boot (covers app re-discovery after a reboot).
- mDNS: continuous, passive (responder only answers queries; `MDNS.update()` is cheap).
- Optional: a protected `POST /api/discovery/announce` to trigger an on-demand UDP burst (so
  the app can ask "are you there?" via a previously-known IP, or a user can re-trigger it).

### 3.6 Memory impact

- mDNS responder: standing cost on the order of **~1–2 KB** heap while active (responder
  state + UDP socket). This is the main cost. Acceptable but not free — measure on a real
  unit with `central=true`.
- UDP announce: a transient `WiFiUDP` + a ~200 B buffer, only during the announce window.
  Negligible and time-bounded.
- **RED FLAG:** mDNS runs continuously alongside the central client. Given the documented
  `central+power` heap fragility, validate free-heap on a real S31 with mDNS + central enabled
  before shipping. If it erodes the floor below the ~20 KB compact-mode threshold, make mDNS
  **opt-in** via a config flag (`discovery.mdns_enabled`) and keep the UDP post-setup burst —
  which is nearly free — as the always-on discovery mechanism.

### 3.7 Schema

```cpp
struct DiscoveryConfig {
  bool mdnsEnabled = true;
  bool udpAnnounceEnabled = true;
  uint16_t udpPort = 51999;
};
```

Add `DiscoveryConfig discovery;` to `AppConfig`. JSON `discovery` object. Same schema bump (4).

### 3.8 Implementation steps

1. Add `DiscoveryConfig` to `types.h`; load/save/validate in `config_manager.cpp`.
2. New `discovery_manager.{h,cpp}`: owns `ESP8266mDNS` + `WiFiUDP`; `begin(status, config)`,
   `loop()`, `onWifiConnected()`, `onPortalProvisioned()`.
3. mDNS: `MDNS.begin(hostname)` + `addService("_rebooter","_tcp",80)` + TXT records on Wi-Fi up;
   `MDNS.update()` in loop.
4. UDP announce: timed burst window after `provisionedViaPortal()` and on boot.
5. Wire into `main.cpp` loop (after `g_wifi`), gated on Wi-Fi connected.
6. Add `POST /api/discovery/announce` (protected).
7. Measure free heap on a real S31 with central enabled; if marginal, make mDNS opt-in.
8. Test: device resolves as `rebooter-xxxxxx.local`; app receives UDP burst post-setup;
   burst stops after window.

---

## 4. Feature 4 — Power metering (complete the CSE7766 path)

### 4.1 What 0.1.40 already does

`power_monitor.cpp` is **not** a stub — it already implements real CSE7766 sampling:

- `SoftwareSerial` RX-only on `GPIO3` at 4800 baud, 8E1, with the documented header
  detection (`0x55` / `0xF0`-class / `0xAA`), `0x5A` second byte, and the 24-byte checksum.
- Parses voltage/current/power coefficients + cycle counts, computes V, A, W, apparent VA,
  power factor, and accumulates energy (Wh) from CF pulses.
- Handles the low-load case: when measured current is below `MIN_MEASURED_CURRENT_A` (0.05 A)
  it falls back to an **estimated** current (`P/V`) and flags it (`currentEstimated`,
  `POWER_SAMPLE_FLAG_CURRENT_ESTIMATED`). This matches the architecture note about standby loads.
- Live values land in `RuntimeStatus::power` and already surface via `StatusPayload::fillPowerStatus`
  → `/api/status` and the heartbeat.
- `central_client.cpp` already has `PowerSampleRecord`, `maybeQueuePowerSample`, batching, and
  `sendPowerSamples` (the upload path that is the known low-heap crash source).

### 4.2 What is actually incomplete / needs design

1. **Frequency is never produced.** `frequencyValid` is hardwired `false` and `frequencyHz`
   stays 0, even though `PowerAnalyticsConfig::includeFrequency` exists and the schema/UI
   advertise it. The CSE7766 frame does not directly give mains frequency; it can be derived
   from the power-cycle register timing. **Decision:** either (a) implement a derivation from
   the cycle registers, or (b) honestly drop `includeFrequency` from the schema/UI until it is
   real. Recommend (b) for now — exposing a field that is always 0/invalid violates the
   expose-real-capabilities principle in §6. Revisit (a) as a measured follow-up.
2. **The UART / serial-debug conflict.** `GPIO3` (RX) is shared between the CSE7766 and the
   USB-serial debug header. `Serial.begin(115200)` in `main.cpp` claims `GPIO1/GPIO3`. When a
   serial adapter is attached, CSE7766 reads are unreliable (documented in `NEXT_STEPS.md` #8
   and `bug-log.md`). `SoftwareSerial` on `GPIO3` and hardware `Serial` RX on `GPIO3` are
   fundamentally in contention.
   **Design:**
   - Keep `Serial` for **TX-only** debug logging (`GPIO1`) and treat `GPIO3` as owned by the
     CSE7766 whenever `power.enabled`. The HW UART RX is not used by the firmware anyway.
   - Document explicitly: with a physical serial adapter attached, power metering is expected
     to be degraded — this is a bench constraint, not a bug. Surface it: when
     `invalidFrameCount` dominates and `chipSeen` is false after N seconds, set a status field
     `power_uart_contended` so the UI can explain "attach no serial cable for metering."
   - Lazy start already helps (power begins only after 10 s, Wi-Fi up). Keep that.
3. **Power-upload transport is the real blocker**, not sampling. Per `NEXT_STEPS.md` and
   `bug-log.md`, `central=true` + standalone HTTPS `/device/power-samples` crashes low-heap
   units. **Design recommendation (consistent with the existing memos):** do not ship a
   standalone power-upload HTTPS path to low-heap S31 units. Instead **piggyback a compact
   power summary onto the heartbeat** (the heartbeat TLS session already runs). The heartbeat
   payload already calls `fillPowerStatus`; extend it with a small rolling summary
   (min/avg/max W, latest V/A/PF, energy Wh, sample counts) rather than uploading every raw
   sample. Keep raw 1 Hz samples available *locally* via the API for an app that wants detail,
   but do not push them off-device over a second TLS session.

### 4.3 Data model

Live (already exists, keep): `RuntimeStatus::power` (`PowerLiveStatus`).

Add a small **rolling aggregate** held in `PowerMonitor` (no flash, RAM only, fixed size):

```cpp
struct PowerAggregate {
  float minW = 0, maxW = 0, sumW = 0;
  uint32_t sampleCount = 0;
  float lastV = 0, lastA = 0, lastPF = 1.0f;
  uint32_t energyWh = 0;      // monotonic accumulator (already computed)
  uint32_t windowStartUptime = 0;
};
```

This is a fixed-size struct — **no heap growth**. The heartbeat reads and resets it each cycle
(or keeps a rolling window). Optionally expose a short in-RAM ring of the last N (e.g. 20) raw
samples for `/api/power/recent`; size N conservatively (20 × ~32 B ≈ 640 B) and make it
compile-time fixed, never a growing vector.

### 4.4 API / UI surface

- `/api/status` — already carries live power. Keep, and add `power_uart_contended` and a
  `power_aggregate` sub-object (min/avg/max W over the window).
- New `GET /api/power/recent` (public read, like other reads) — the fixed-size raw ring.
- WebUI: a "Power" card showing live V/A/W/PF/energy, the estimated-current flag, frame
  quality counters (`valid_frame_count` / `invalid_frame_count` — operationally meaningful per
  `NEXT_STEPS.md` B16 #2), and the UART-contended hint.
- Heartbeat: add the compact aggregate (§4.2.3), gated by `power.enabled` and by the existing
  compact-mode heap check.

### 4.5 Memory impact

- Aggregate struct: fixed ~40 B. Optional raw ring: fixed ~640 B. **No heap growth** — this is
  the point; replace the unbounded raw-sample vector upload with bounded aggregates.
- **RED FLAG resolved-by-design:** the existing `std::vector<PowerSampleRecord> powerSamples_`
  in `central_client.h` grows with batching and feeds the crashing standalone upload. The
  design recommendation is to **stop using the standalone upload path** for low-heap units and
  bound/aggregate instead. If the vector is kept for higher-heap devices, it must be hard-capped.
- **Net assessment: sampling is low risk (already working); the upload path is the documented
  high risk and the design deliberately removes the standalone TLS upload for S31-class units.**

### 4.6 Implementation steps

1. Decide frequency: drop `includeFrequency` from schema/UI (recommended) or implement
   derivation. If dropping, mark it removed-by-design in the schema doc.
2. `power_monitor.{h,cpp}`: add `PowerAggregate`; update it per valid frame; add a fixed-size
   raw ring; add UART-contention detection (`chipSeen` false + high `invalidFrameCount`).
3. `app_state.h`: add `power_uart_contended` and aggregate fields to `PowerLiveStatus` (fixed size).
4. `status_payload.cpp`: emit aggregate + contention into `/api/status` and the heartbeat.
5. `central_client.cpp`: route power off the standalone `/device/power-samples` path; piggyback
   the compact aggregate on heartbeat; hard-cap or retire `powerSamples_`.
6. WebUI: "Power" card in `data/` assets + embedded fallback; new `GET /api/power/recent`.
7. Test on a real S31 with a known downstream load so V/A/W are nonzero; verify with serial
   adapter detached; confirm `central=true` + `power=true` is now stable (the whole point).

---

## 5. Feature 5 — On-flash crash capture

### 5.1 Goal

Persist exception/crash dumps to flash so they are retrievable via the device API/WebUI with
no serial cable. Today crashes only print to `Serial`; the boot-health machine knows a boot
*failed* but never *why*.

### 5.2 Approach

Use the **EspSaveCrash pattern** but write to **LittleFS** (the project already uses LittleFS
for everything; mixing in raw-EEPROM crash storage adds a second storage model). The mechanism:

- The ESP8266 Arduino core exposes a weak `custom_crash_callback(struct rst_info*, uint32_t
  stack_start, uint32_t stack_end)` that the core calls from the exception/abort handler
  **before** reboot. Override it. From that callback we can read the reset reason, exception
  cause, exception address (`epc1`/`excvaddr`), and walk the stack range.
- **Constraint:** the crash callback runs in a fragile post-exception context — heap is
  suspect, interrupts off, must be fast and allocation-free. LittleFS writes from inside that
  callback are risky. **Two-tier design:**
  - In the callback: write a **small, fixed crash record** to a dedicated raw flash sector or
    to RTC user memory (`ESP.rtcUserMemory*` — survives a crash reboot, ~512 B, zero
    allocation, safe to touch in the handler). Store: reset reason, exception cause,
    `epc1`/`epc2`/`epc3`/`excvaddr`/`depc`, a short stack excerpt (e.g. 32 words), uptime,
    firmware version hash.
  - On the **next boot**, early in `setup()`, check RTC memory for a valid (CRC-checked) crash
    record. If present, copy it into a LittleFS file `/crash/last.json` (or a small ring of
    the last 3 crashes, `crash-0..2.json`) and clear the RTC slot. LittleFS writes here are
    safe — we are in a normal boot context.
- RTC user memory survives `abort()` / exception resets but **not** power loss. That is
  acceptable: a power-loss event is not a firmware crash. The "callback → RTC → next-boot →
  LittleFS" hop converts a fragile in-handler write into a safe normal-context write.

This integrates cleanly with the existing boot-health machine: `beginBootSession` already
detects "previous boot incomplete"; the crash record explains *why* and can be attached to
the boot event log entry.

### 5.3 Data model

Fixed-size RTC struct (no heap, CRC-protected):

```cpp
struct CrashRtcRecord {
  uint32_t magic;          // validity sentinel
  uint32_t crc;            // CRC32 of the rest
  uint32_t resetReason;
  uint32_t exceptCause;
  uint32_t epc1, epc2, epc3, excvaddr, depc;
  uint32_t uptimeMs;
  uint32_t fwVersionHash;
  uint32_t stack[32];      // top-of-stack excerpt
};
```

Persisted JSON `/crash/last.json` (or ring of 3): the decoded record plus a human reset-reason
string and an ISO timestamp if `timeSynced` was true at capture (best-effort — time may not be
known in the handler, so stamp it at the next-boot copy step instead).

### 5.4 API / UI surface

- `GET /api/system/crash` (protected — a crash dump is diagnostic, keep it behind auth like
  `central-diagnostic` and `heartbeat-preview`). Returns the stored crash record(s) as JSON.
- `POST /api/system/crash/clear` (protected) — delete stored crash files.
- WebUI: a "Diagnostics" / "Last Crash" panel showing reset reason, exception cause + address,
  and the raw stack words. Provide a "copy" button; offline ELF/`addr2line` decoding stays a
  developer-side step (no symbol table on-device — too big).
- Optionally include a one-line crash summary in `/api/status` (`last_crash_present: true`,
  `last_crash_reason: "Exception (28)"`) so the app/UI can surface a badge.

### 5.5 Memory impact

- RTC record: 512 B of RTC user memory — **not** heap. Zero heap cost at capture time.
- LittleFS crash files: a few hundred bytes each on flash; the next-boot copy uses a small
  transient `JsonDocument`. Bounded ring of 3 keeps flash use trivial.
- The override callback adds a small amount of flash code, no standing heap.
- **Net assessment: very low risk** — this is the safest of the six features memory-wise,
  because the fragile path touches only RTC memory and the heavy path runs in a normal boot.
- One caution: do not call `Serial` / `LittleFS` / allocate inside `custom_crash_callback`.
  Keep it to fixed-struct RTC writes + CRC.

### 5.6 Implementation steps

1. New `crash_recorder.{h,cpp}`: define `CrashRtcRecord`; implement `custom_crash_callback`
   (RTC write only, allocation-free, CRC).
2. Early in `setup()` (before `LittleFS.begin()` consumers, but after `LittleFS.begin()`):
   check RTC for a valid record; if present, write `/crash/last.json` (ring of 3), add a boot
   event-log entry, clear the RTC slot.
3. Add crash summary fields to `RuntimeStatus` / `/api/status`.
4. Add `GET /api/system/crash` + `POST /api/system/crash/clear` (protected).
5. WebUI "Last Crash" panel in `data/` assets + embedded fallback.
6. `config_manager.cpp` `reset()`: also remove `/crash/*` on factory reset (consistent with
   the existing "factory reset clears event history" rule).
7. Test: force a crash (the build already has `SAFE_FALLBACK_TEST_BAD_BOOT` which calls
   `abort()` — perfect test vector); confirm the dump appears in `/api/system/crash` after the
   next boot with no serial cable attached.

---

## 6. Feature 6 — Expose-all-device-features principle

### 6.1 Goal

The WebUI/API should expose every capability the hardware supports, memory permitting. Audit
where the code artificially limits, and note the headroom.

### 6.2 Where the code currently limits (audit)

1. **Frequency** — `PowerLiveStatus::frequencyValid` is hardwired `false`; `includeFrequency`
   config exists and the UI advertises it, but no value is ever produced. *Limit type:
   advertised-but-fake.* Fix per §4.2.1 (implement or remove).
2. **Power upload caps** — power telemetry is intentionally throttled/compacted on low-heap
   devices. This is a **legitimate, memory-justified** limit, not artificial. Keep, but make
   it explicit in the API (`power_upload_mode: "compact" | "full" | "heartbeat_piggyback"`)
   so the capability is *visible* even when constrained.
3. **`baseUrls` capped at 4** — artificial; raised to 10 in Feature 2.
4. **Single saved Wi-Fi network (via tzapu)** — artificial; fixed in Feature 1.
5. **Internet targets capped at 10** (`validateConfig`) — reasonable, keep; document it.
6. **No discovery surface** — gap; fixed in Feature 3.
7. **No crash visibility** — gap; fixed in Feature 5.
8. **Energy/`energyWh`** is computed and surfaced; good. But there is **no energy reset**
   endpoint. The hardware/firmware tracks a monotonic accumulator with no way to zero it from
   the UI. *Limit type: missing control for an existing capability.* Add `POST /api/power/energy/reset`.
9. **Relay** is fully exposed (on/off/toggle). Good. **Button behaviors** (3 s reboot, 10 s
   recovery, 30 s factory reset) are real capabilities only documented in code/specs — surface
   them read-only in the WebUI so users know they exist.
10. **`statusLedEnabled`** exists in `AppConfig` but is **not** in `sendConfigJson` or the
    `config/save` handler — a real config field with no UI/API path. *Limit type:
    config-field-with-no-surface.* Add it to the config JSON round-trip.
11. **`notificationCooldownSeconds`, `eventLogMaxEntries`, `timezone`** — `timezone` and
    `eventLogMaxEntries` are validated but `timezone` is not in `sendConfigJson`;
    `eventLogMaxEntries` and `notificationCooldownSeconds` are likewise missing from the
    web config round-trip. Audit `sendConfigJson` vs `AppConfig` and close every gap.
12. **Notification config** — only `enabled`, `webhookUrl`, `webhookAuthToken` are handled in
    `config/save`; `type`, `webhookMethod`, `sendOnTrigger/Recovery/MaxCycles`,
    `sendTestNotificationEnabled` are in the struct and in `fillReportedConfig` but **not**
    settable via `/api/config/save`. *Limit type: partially-wired config.* Wire them fully.
13. **Pushover** — `notifications.type` validation allows `"pushover"` but no Pushover
    transport exists in `notification_manager.cpp`. *Advertised-but-unimplemented.* Either
    implement or restrict `type` to `"webhook"` until it is real.

### 6.3 Principle for the design

For every hardware/firmware capability:
- If it is **real**, expose it in both `/api/config` (or `/api/status`) and the WebUI.
- If it is **constrained for memory**, expose it *with the constraint visible* (a mode flag),
  never silently.
- If it is **not implemented**, remove it from the schema/UI rather than advertise a fake.

The recurring anti-pattern in the codebase is **schema/struct fields that are not wired
through `sendConfigJson` + `config/save`**. A single audit pass aligning `AppConfig` ⇔
`sendConfigJson` ⇔ `config/save` ⇔ `fillReportedConfig` closes most of the gap and is the
highest-value, lowest-risk item in this tier.

### 6.4 Memory headroom

The honest headroom is **small and conditional**:
- Steady-state free heap on low-heap S31 units sits near the **20 KB** compact-mode trigger
  with `central=true`. With `central=true` + `power=true` (standalone upload) units crash.
- That means new *standing* allocations must stay in the low **hundreds of bytes**, and new
  *transient* allocations must not coincide with a TLS session.
- The Tier-2 features above were sized against this: Multi-Wi-Fi ~700 B, hub URLs ~760 B,
  mDNS ~1–2 KB (the one to watch), power aggregates fixed/no-growth, crash capture RTC-only.
- **The combined standing cost (~3–4 KB if mDNS is always on) is significant against a ~20 KB
  floor.** Recommendation: ship Multi-Wi-Fi, hub URLs, power aggregation, and crash capture
  first (all low/no standing heap); make mDNS opt-in and validate it last with real free-heap
  measurements; do the §6.3 config-audit pass throughout (it adds JSON size, not standing heap).

### 6.5 Implementation steps

1. Audit `AppConfig` vs `sendConfigJson` vs `config/save` vs `fillReportedConfig`; produce the
   field-by-field gap list; wire every missing field through both directions.
2. Add `POST /api/power/energy/reset` and `power_upload_mode` to status.
3. Resolve each advertised-but-fake item (frequency, Pushover) by implementing or removing.
4. Surface button-gesture documentation read-only in the WebUI.
5. Re-verify free heap after the config-audit changes (larger JSON = larger transient buffers).

---

## 7. Cross-cutting: schema versioning and rollout order

- All new config groups (`wifi`, `discovery`, expanded `central.baseUrls`) ride a **single**
  bump to `CONFIG_SCHEMA_VERSION = 4`. The loader's `doc[...] | default` pattern makes the
  upgrade from 3 → 4 transparent; no migration code needed beyond the existing legacy handling.
- Keep last-known-good recovery working: `overlayRecoveryPreservedState` currently preserves
  `central` identity across a recovery rollback. Decide whether `wifi.savedNetworks` should
  also be preserved across recovery — **recommended yes**, so an auto-recovery does not strand
  the device on a network it can no longer reach. Add `wifi` to `overlayRecoveryPreservedState`.
- Suggested ship order (lowest risk first):
  1. §6 config-audit pass (no standing heap, immediate correctness win).
  2. Feature 2 hub URLs (mostly raising a cap; failover already exists).
  3. Feature 1 Multi-Wi-Fi (self-contained, low standing heap).
  4. Feature 5 crash capture (RTC-only, very low risk, high diagnostic value).
  5. Feature 4 power completion (sampling done; the win is killing the standalone upload).
  6. Feature 3 discovery — last, because mDNS is the one standing-heap risk; validate on a
     real unit with `central=true` before committing it as always-on.

---

## 8. Questions for the product owner

1. **mDNS always-on vs opt-in.** mDNS is the only Tier-2 feature with a non-trivial standing
   heap cost (~1–2 KB) against a ~20 KB floor on the worst units. Acceptable as default-on, or
   opt-in with the near-free UDP post-setup burst as the always-on path?
2. **Frequency.** The schema/UI advertise mains frequency but the firmware never produces it.
   Implement a real derivation from the CSE7766 cycle registers (more work, needs bench
   validation), or remove `includeFrequency` until it is real?
3. **Pushover.** `notifications.type` accepts `"pushover"` but there is no Pushover transport.
   Is Pushover in scope for this tier, or should `type` be restricted to `"webhook"` for now?
4. **Power upload contract.** This design recommends *retiring* the standalone HTTPS
   `/device/power-samples` path for S31-class units and piggybacking a compact aggregate on
   the heartbeat (consistent with the existing low-heap memos). This is a **hub-side contract
   change** — the hub must accept power data inside the heartbeat envelope. Is the hub team
   aligned, and is per-raw-sample upload still required for any device class?
5. **AP-portal scope for hub URLs.** Proposal: the AP portal edits only 1–2 hub URLs; full
   10-slot editing lives in the authenticated WebUI (portal memory + UX). Acceptable?
6. **Saved-Wi-Fi count.** Design caps user-saved networks at 5 (plus 2 built-in dev networks).
   Is 5 enough, or is a larger cap required (each extra network adds boot connect-time and a
   small standing cost)?
7. **Crash dump exposure.** Crash dumps are proposed as **auth-protected** reads (they reveal
   addresses/stack). Confirm they should not be on the public read surface.
8. **Bootstrap loader Wi-Fi.** Should the stage-1 bootstrap loader eventually read the user's
   saved Wi-Fi list from LittleFS (so OTA recovery survives a network change), or stay
   strictly hardcoded as the immutable disaster-recovery floor?
