#pragma once
#include <Arduino.h>
#include <IPAddress.h>
#include <ESP8266WiFi.h>

// 0.2.27 (#206) Diagnostic UDP syslog.
//
// Sends one UDP packet per significant event to a hub-side collector.
// Bypasses HTTPS/BearSSL/TLS entirely so it keeps reporting through
// the very heap-pressure cascade that's the primary suspect for the
// .185 silent-failure pattern (heartbeats fail → device retries →
// device falls into setup-AP → all forensics lost).
//
// Each packet is a compact JSON object on one line. Format example:
//   {"dev":"0CF74B...","fw":"0.2.27","ms":12345,"k":"event","type":"central","msg":"Heartbeat..."}
//
// Field keys are 1-3 chars to keep packets small (<=1500B Ethernet frame).
//   dev  = MAC address, lowercase, no separators
//   fw   = firmware version (truncated)
//   ms   = millis() at packet emit
//   k    = kind: event | wifi | heap | breadcrumb | reset
//   plus per-kind fields (type/msg for events, ssid/rssi for wifi, etc.)
//
// Throttling: max ~10 packets per second to avoid drowning the collector
// and the local UDP stack. Events/wifi/breadcrumb bypass the throttle
// (low-rate by nature); periodic heap snapshots are throttled.

namespace DiagSyslog {

// Configure target IP+port. Empty IP disables. Call from setup() after
// WiFi is up but the bind itself is deferred to first send.
void begin(IPAddress collectorIp, uint16_t collectorPort, const String& macHex);

// Heap snapshot every PERIODIC_MS — called from loop().
void loop();

// Tap points — fire-and-forget, never block.
void sendEvent(const String& type, const String& message);
// 0.2.32 #209: raw-pointer overload for the critical-failure path
// where building Arduino Strings would itself allocate and risk the
// NULL-buffer crash. Pass stack-buffer or string-literal pointers
// directly; never call this with a c_str() of a possibly-NULL-buffer
// String.
void sendEventCStr(const char* type, const char* message);
void sendWifiState(const char* what, const String& detail = String());
void sendBreadcrumb(uint8_t op);
void sendResetReason(const String& reason);

// Packet stats for debugging the diag harness itself.
uint32_t packetsSent();
uint32_t packetsDropped();

}  // namespace DiagSyslog
