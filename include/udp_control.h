#pragma once
#include <Arduino.h>

// 0.2.23 (#179) Phase 3 — UDP control channel for sub-50ms relay actions.
//
// LAN agent sends 29-byte HMAC-authed packets to port 31416; this module
// verifies, executes the relay command, and sends a 26-byte ACK. Wire
// time on a healthy LAN is <10ms; total browser-click → relay-flip <100ms
// (vs ~400ms for the HTTP-POST path). See
// docs/phase3-udp-control-design.md for the full protocol spec.

class RelayController;
class EventLog;

namespace UdpControl {

// Bind the UDP socket and remember dependencies. `secret` is the device's
// central.deviceToken — same shared secret already provisioned.
void begin(RelayController* relay, EventLog* eventLog, const String& secret);

// Re-arm the shared secret (e.g. after token rotation). Empty disables.
void setSecret(const String& secret);

// Drain at most a few packets per call. Designed to run from the main
// loop; non-blocking (no parsePacket() returns 0).
void loop();

// Stats for the heartbeat payload — let the operator see if UDP is being
// used in real life.
uint32_t packetsAcceptedTotal();
uint32_t packetsRejectedTotal();

}  // namespace UdpControl
