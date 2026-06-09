#include "udp_control.h"

#include <Arduino.h>
#include <WiFiUdp.h>
#include <bearssl/bearssl_hmac.h>
#include <time.h>

#include "event_log.h"
#include "relay_controller.h"

namespace UdpControl {

namespace {

constexpr uint16_t PORT = 31416;
constexpr size_t REQ_LEN = 29;   // 16 HMAC + 8 nonce + 4 ts + 1 cmd
constexpr size_t RES_LEN = 26;   // 16 HMAC + 8 nonce + 1 ack + 1 relay
constexpr uint8_t HMAC_LEN = 16; // truncated SHA-256 tag
constexpr int64_t TS_WINDOW_S = 60;
constexpr size_t NONCE_RING_SIZE = 32;

enum Cmd : uint8_t {
  CMD_RELAY_ON     = 0x01,
  CMD_RELAY_OFF    = 0x02,
  CMD_RELAY_TOGGLE = 0x03,
};

enum Ack : uint8_t {
  ACK_OK             = 0,
  ACK_BAD_HMAC       = 1,
  ACK_UNKNOWN_CMD    = 2,
  ACK_STALE_TS       = 3,
  ACK_REPLAY         = 4,
  ACK_INTERNAL       = 5,
};

WiFiUDP gUdp;
RelayController* gRelay = nullptr;
EventLog* gEventLog = nullptr;
String gSecret;
bool gActive = false;

// 0.2.25 CRITICAL fix (code review F19): pre-fix the ring was zero-init'd
// AND used equality compare against the incoming nonce. A legitimate
// nonce of 0 (e.g. an agent counter starting from zero) collided with every
// zero-init'd slot and was ACK_REPLAYed on every boot until the agent's
// nonce overflowed. Use UINT64_MAX as the "empty" sentinel. Real nonces
// are 64-bit random tokens — collision with UINT64_MAX has 1-in-2^64 odds.
constexpr uint64_t NONCE_EMPTY = UINT64_MAX;
uint64_t gNonceRing[NONCE_RING_SIZE] = {
  NONCE_EMPTY, NONCE_EMPTY, NONCE_EMPTY, NONCE_EMPTY, NONCE_EMPTY, NONCE_EMPTY, NONCE_EMPTY, NONCE_EMPTY,
  NONCE_EMPTY, NONCE_EMPTY, NONCE_EMPTY, NONCE_EMPTY, NONCE_EMPTY, NONCE_EMPTY, NONCE_EMPTY, NONCE_EMPTY,
  NONCE_EMPTY, NONCE_EMPTY, NONCE_EMPTY, NONCE_EMPTY, NONCE_EMPTY, NONCE_EMPTY, NONCE_EMPTY, NONCE_EMPTY,
  NONCE_EMPTY, NONCE_EMPTY, NONCE_EMPTY, NONCE_EMPTY, NONCE_EMPTY, NONCE_EMPTY, NONCE_EMPTY, NONCE_EMPTY,
};
uint8_t gNonceRingHead = 0;

uint32_t gAcceptedTotal = 0;
uint32_t gRejectedTotal = 0;

void hmacSha256Truncated(const uint8_t* key, size_t key_len,
                         const uint8_t* msg, size_t msg_len,
                         uint8_t out[HMAC_LEN]) {
  br_hmac_key_context kc;
  br_hmac_key_init(&kc, &br_sha256_vtable, key, key_len);
  br_hmac_context ctx;
  br_hmac_init(&ctx, &kc, HMAC_LEN);
  br_hmac_update(&ctx, msg, msg_len);
  br_hmac_out(&ctx, out);
}

// Constant-time 16-byte compare — defeats timing oracles even though
// this is LAN-only.
bool ctEq16(const uint8_t* a, const uint8_t* b) {
  uint8_t diff = 0;
  for (uint8_t i = 0; i < HMAC_LEN; ++i) diff |= a[i] ^ b[i];
  return diff == 0;
}

uint64_t readU64BE(const uint8_t* p) {
  uint64_t v = 0;
  for (int i = 0; i < 8; ++i) v = (v << 8) | p[i];
  return v;
}

uint32_t readU32BE(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
         (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

bool nonceSeen(uint64_t nonce) {
  for (size_t i = 0; i < NONCE_RING_SIZE; ++i) {
    if (gNonceRing[i] == nonce) return true;
  }
  return false;
}

void rememberNonce(uint64_t nonce) {
  gNonceRing[gNonceRingHead] = nonce;
  gNonceRingHead = (gNonceRingHead + 1) % NONCE_RING_SIZE;
}

void sendAck(IPAddress peer, uint16_t peerPort, const uint8_t* nonce,
             uint8_t ack, uint8_t relayState) {
  if (gSecret.isEmpty()) return;
  uint8_t out[RES_LEN];
  memset(out, 0, sizeof(out));
  // Lay out the response payload first (offsets 16..25), then compute
  // HMAC over the payload, write tag at the front.
  memcpy(out + HMAC_LEN, nonce, 8);
  out[HMAC_LEN + 8] = ack;
  out[HMAC_LEN + 9] = relayState;
  hmacSha256Truncated(
      reinterpret_cast<const uint8_t*>(gSecret.c_str()), gSecret.length(),
      out + HMAC_LEN, RES_LEN - HMAC_LEN,
      out);
  gUdp.beginPacket(peer, peerPort);
  gUdp.write(out, RES_LEN);
  gUdp.endPacket();
}

void processPacket() {
  IPAddress peer = gUdp.remoteIP();
  uint16_t peerPort = gUdp.remotePort();
  uint8_t buf[REQ_LEN];
  size_t got = gUdp.read(buf, REQ_LEN);
  if (got != REQ_LEN) {
    // Drain any extra bytes from the same datagram so the next
    // parsePacket() doesn't trip on them.
    while (gUdp.available()) gUdp.read();
    ++gRejectedTotal;
    return;
  }
  while (gUdp.available()) gUdp.read();

  if (gSecret.isEmpty()) {
    // No secret configured yet — silently drop, don't ACK (can't
    // authenticate the response anyway).
    ++gRejectedTotal;
    return;
  }

  // Verify HMAC over (nonce ‖ ts ‖ cmd). The 16-byte tag is at offset 0.
  uint8_t expected[HMAC_LEN];
  hmacSha256Truncated(
      reinterpret_cast<const uint8_t*>(gSecret.c_str()), gSecret.length(),
      buf + HMAC_LEN, REQ_LEN - HMAC_LEN,
      expected);
  if (!ctEq16(buf, expected)) {
    // Don't ACK — we can't authenticate our own response, and an attacker
    // probing with bad HMACs shouldn't get a reflection.
    ++gRejectedTotal;
    return;
  }

  const uint64_t nonce = readU64BE(buf + HMAC_LEN);
  const uint32_t ts = readU32BE(buf + HMAC_LEN + 8);
  const uint8_t cmd = buf[HMAC_LEN + 12];
  uint8_t nonceBytes[8];
  memcpy(nonceBytes, buf + HMAC_LEN, 8);

  // 0.2.25 CRITICAL fix (code review F5): fail-closed when NTP isn't
  // synced. Pre-fix the entire skew check was wrapped in `if (now > 0)`
  // so a device with broken NTP/DNS/captive-portal trusted ANY timestamp
  // and the only replay defense was the 32-slot nonce ring (~1.6s of
  // coverage at design cadence). A captured relay_off packet could be
  // replayed for the device's entire offline-from-NTP lifetime. Now
  // we reject when the device doesn't know what time it is.
  const time_t now = time(nullptr);
  if (now <= 0) {
    sendAck(peer, peerPort, nonceBytes, ACK_STALE_TS, gRelay ? gRelay->isOn() : 0);
    ++gRejectedTotal;
    return;
  }
  {
    const int64_t skew = int64_t(now) - int64_t(ts);
    if (skew > TS_WINDOW_S || -skew > TS_WINDOW_S) {
      sendAck(peer, peerPort, nonceBytes, ACK_STALE_TS, gRelay ? gRelay->isOn() : 0);
      ++gRejectedTotal;
      return;
    }
  }
  if (nonceSeen(nonce)) {
    sendAck(peer, peerPort, nonceBytes, ACK_REPLAY, gRelay ? gRelay->isOn() : 0);
    ++gRejectedTotal;
    return;
  }
  rememberNonce(nonce);

  // Execute.
  if (!gRelay) {
    sendAck(peer, peerPort, nonceBytes, ACK_INTERNAL, 0);
    ++gRejectedTotal;
    return;
  }
  switch (cmd) {
    case CMD_RELAY_ON:     gRelay->set(true);  break;
    case CMD_RELAY_OFF:    gRelay->set(false); break;
    case CMD_RELAY_TOGGLE: gRelay->toggle();   break;
    default:
      sendAck(peer, peerPort, nonceBytes, ACK_UNKNOWN_CMD, gRelay->isOn() ? 1 : 0);
      ++gRejectedTotal;
      return;
  }

  sendAck(peer, peerPort, nonceBytes, ACK_OK, gRelay->isOn() ? 1 : 0);
  ++gAcceptedTotal;
  if (gEventLog) {
    const char* verb =
        cmd == CMD_RELAY_ON ? "on" :
        cmd == CMD_RELAY_OFF ? "off" : "toggle";
    // Throttle this — UDP can fire many times per second from a stuck
    // agent. The event log's own dedup window catches consecutive
    // identical entries within `suppressDuplicateWindowSeconds_`.
    gEventLog->add("udp", String("UDP relay ") + verb +
                          " from " + peer.toString());
  }
}

}  // namespace

void begin(RelayController* relay, EventLog* eventLog, const String& secret) {
  gRelay = relay;
  gEventLog = eventLog;
  gSecret = secret;
  gActive = gUdp.begin(PORT);
}

void setSecret(const String& secret) {
  gSecret = secret;
}

void loop() {
  if (!gActive) return;
  // Drain up to 4 packets per call to avoid back-pressure if a misbehaving
  // sender floods us, while still keeping the main loop responsive.
  for (int i = 0; i < 4; ++i) {
    int size = gUdp.parsePacket();
    if (size <= 0) break;
    processPacket();
  }
}

uint32_t packetsAcceptedTotal() { return gAcceptedTotal; }
uint32_t packetsRejectedTotal() { return gRejectedTotal; }

}  // namespace UdpControl
