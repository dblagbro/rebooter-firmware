# Phase 3 design — UDP control channel for sub-50ms relay actions (#179)

## Goal
Beat 200ms reliably for browser-click → relay-flip. Phase 1 + 2 (SSE
push + LAN agent + HTTP POST to device) ships at ~400ms steady-state
because the ESP8266 HTTP stack (parse headers, build response, TCP
teardown) costs ~280ms per request even with keep-alive. UDP eliminates
that.

## Wire latency target
Per-packet round-trip on a LAN (sub-1ms ping):
- Agent serialize + send: <0.5ms
- Wire: <1ms
- ESP8266 parse + HMAC verify + relay GPIO write: <5ms
- ESP8266 send response: <1ms
- Wire: <1ms
- Agent receive + parse: <0.5ms

**Total: <10ms.** Adding browser→hub SSE→agent (~30ms hub-side, ~50ms
network) gives ~90ms end-to-end browser-click → relay-flip — meeting
the 200ms target with margin.

## Protocol

UDP port 31416 (1 over `IPPORT_USERRESERVED`, easy to recall).

### Request (29 bytes)

```
offset  bytes  field
  0     16     HMAC-SHA256(secret, nonce ‖ ts ‖ cmd)[:16]   truncated to 16
 16      8     nonce          random 64-bit, non-repeating
 24      4     ts             big-endian unix seconds
 28      1     cmd            0x01=relay_on 0x02=relay_off 0x03=relay_toggle
```

### Response (26 bytes)

```
offset  bytes  field
  0     16     HMAC-SHA256(secret, nonce ‖ ack ‖ relay)[:16]
 16      8     nonce          echo of request
 24      1     ack            0=ok 1=bad_hmac 2=unknown_cmd
                              3=stale_ts 4=replay 5=internal
 25      1     relay          0=off, 1=on (current state after command)
```

### Replay protection
Device tracks the last 32 received `nonce` values in a ring; reject
duplicates. Also reject `ts` more than ±60s from the device's current
time. Device's wall clock is set via NTP (already in firmware), so this
is enforceable.

### Secret
The HMAC secret is the same `central.deviceToken` the device uses for
hub HTTPS. Already provisioned, already kept secret, already 32+ bytes
of entropy. The agent fetches it from the hub once per device and
caches it in memory.

## Firmware module — `src/udp_control.cpp`

~80-100 LOC:
- `UdpControl::begin(RelayController*, const String& secret)` — bind socket
- `UdpControl::loop()` — drain `udp_.parsePacket()`, verify, dispatch
- HMAC via BearSSL `br_hmac_*` (already linked via
  `WiFiClientSecureBearSSL.h`)
- Constant-time comparison for the 16-byte tag
- Ring buffer of 32 recent nonces

Heap impact: ~300 bytes static + transient buffers — orders of magnitude
under our heap-pressure floor.

## Hub side
New `/api/v1/admin/services/devices/<id>/control-secret` endpoint
(token-bearer admin auth) returns the device's `central.deviceToken`.
Agent fetches lazily on first UDP attempt per device, caches per
process.

## Agent side
~40 LOC change in `lan-relay-agent.py`:
- Per-device socket pool
- Mint nonce + timestamp; compute HMAC
- `socket.sendto`, `socket.settimeout(0.05)`, `recvfrom`
- On timeout / EAGAIN / bad ACK: retry up to 3× then fall back to HTTP
- HTTP fallback already exists (current code path)

## Why not WebSocket?
- WS over TCP needs TLS to be safe over the LAN — back to BearSSL
  fragmentation territory.
- Plain `ws://` is fine for LAN-only — but TCP handshake every
  reconnect (~50ms) + framing overhead is comparable to the UDP path
  with none of the simplicity.
- Persistent WS connection on the ESP8266 = persistent state = heap
  cost. UDP is stateless.

## Why not raw TCP keep-alive?
- TCP keep-alive on the ESP8266 is fiddly; many implementations
  silently drop the connection on transient WiFi interruption.
- UDP recovers gracefully — every request is independent.

## Test plan
1. Ship firmware 0.2.19 with `udp_control` enabled on port 31416.
2. Bench: agent script that sends 100 commands back-to-back, prints
   latency histogram.
3. Validate replay rejection (replay a captured packet → expect ack=4).
4. Validate bad-HMAC rejection.
5. Soak: 24h continuous control with heap-trajectory streaming
   confirming UDP path does NOT contribute to fragmentation.

## Ship cadence
- 0.2.19: firmware UDP server.
- 0.6.24: hub control-secret endpoint.
- Agent: incremental update — adds UDP path with HTTP fallback.

## Risks
- ESP8266 single-thread: `UdpControl::loop()` adds work to every
  main-loop tick. If `parsePacket()` is fast (it's a non-blocking
  syscall), negligible. If WiFi RX queue is busy, may add 1-2ms
  per tick. Acceptable.
- Port 31416 conflict with operator firewall: configurable.
- HMAC computation cost: SHA-256 is ~10μs on ESP8266 hardware path. Fine.

## Open questions for operator
- Confirm port choice (31416) doesn't conflict with any existing
  service.
- Should the UDP path also support `device_restart` and `relay_cycle`,
  or stick to instant-on/off/toggle to keep the protocol simple?
- Replay window of ±60s — too tight for clock drift on devices that
  haven't NTP'd in a while?
