#include "diag_syslog.h"

#include <WiFiUdp.h>
#include "firmware_version.h"

namespace DiagSyslog {

namespace {

constexpr uint32_t PERIODIC_HEAP_MS = 15000;  // heap snapshot every 15s
constexpr uint32_t MIN_PACKET_INTERVAL_US = 100000;  // ≥100ms between throttled packets

WiFiUDP gUdp;
IPAddress gCollector;
uint16_t gPort = 0;
String gDevMac;
bool gActive = false;

uint32_t gPacketsSent = 0;
uint32_t gPacketsDropped = 0;
uint32_t gLastHeapSampleMs = 0;
uint32_t gLastThrottledSendUs = 0;

// Truncated firmware version so packets stay small.
const char* truncatedFw() {
  static char buf[24];
  static bool done = false;
  if (!done) {
    snprintf(buf, sizeof(buf), "%s", FIRMWARE_VERSION);
    done = true;
  }
  return buf;
}

// Escape a string for safe JSON embedding into a fixed-size buffer.
// Returns the number of bytes written (not including NUL).
int escapeJsonInto(char* out, size_t cap, const char* src) {
  size_t i = 0;
  for (size_t s = 0; src[s] != '\0' && i + 2 < cap; ++s) {
    char c = src[s];
    if (c == '"' || c == '\\') {
      out[i++] = '\\';
      out[i++] = c;
    } else if ((unsigned char)c < 0x20) {
      // Drop control chars rather than expanding — keeps the packet small.
      out[i++] = ' ';
    } else {
      out[i++] = c;
    }
  }
  if (i < cap) out[i] = '\0';
  return (int)i;
}

bool sendRaw(const char* json) {
  if (!gActive) return false;
  if (!gUdp.beginPacket(gCollector, gPort)) {
    ++gPacketsDropped;
    return false;
  }
  size_t n = strlen(json);
  gUdp.write(reinterpret_cast<const uint8_t*>(json), n);
  if (!gUdp.endPacket()) {
    ++gPacketsDropped;
    return false;
  }
  ++gPacketsSent;
  return true;
}

// Common JSON header. Caller continues building fields after the comma.
// Returns position of cursor inside `out`.
int writeHeader(char* out, size_t cap, const char* kind) {
  return snprintf(out, cap,
                  "{\"dev\":\"%s\",\"fw\":\"%s\",\"ms\":%lu,\"k\":\"%s\"",
                  gDevMac.c_str(), truncatedFw(),
                  static_cast<unsigned long>(millis()), kind);
}

}  // namespace

void begin(IPAddress collectorIp, uint16_t collectorPort, const String& macHex) {
  gCollector = collectorIp;
  gPort = collectorPort;
  gDevMac = macHex;
  // Pick a random ephemeral source port. WiFiUDP.begin needs a local
  // port — anything in 49152-65535 works. Use the high byte of MAC
  // to keep it stable per-device for the hub-side flow analysis.
  uint16_t srcPort = 49152 + (macHex.length() > 0 ? (uint8_t)macHex[macHex.length() - 1] : 0);
  if (gUdp.begin(srcPort)) {
    gActive = true;
  }
}

void loop() {
  if (!gActive) return;
  const uint32_t now = millis();
  if (now - gLastHeapSampleMs < PERIODIC_HEAP_MS) return;
  gLastHeapSampleMs = now;
  // Throttle gate.
  const uint32_t nowUs = micros();
  if (nowUs - gLastThrottledSendUs < MIN_PACKET_INTERVAL_US) return;
  gLastThrottledSendUs = nowUs;
  char buf[256];
  int n = writeHeader(buf, sizeof(buf), "heap");
  if (n <= 0 || (size_t)n >= sizeof(buf)) return;
  snprintf(buf + n, sizeof(buf) - n,
           ",\"free\":%lu,\"mfb\":%u,\"frag\":%u,\"up\":%lu}",
           static_cast<unsigned long>(ESP.getFreeHeap()),
           ESP.getMaxFreeBlockSize(),
           ESP.getHeapFragmentation(),
           static_cast<unsigned long>(millis() / 1000));
  sendRaw(buf);
}

void sendEvent(const String& type, const String& message) {
  // 0.2.32 #209: defensive NULL-check on c_str(). Arduino String sets
  // buffer=NULL after allocation failure; c_str() returns NULL in that
  // state. Without this guard we'd dereference NULL inside escapeJsonInto
  // exactly when the device was under the heap pressure we wanted to
  // forensically capture.
  const char* typeStr = type.c_str();
  const char* msgStr = message.c_str();
  sendEventCStr(typeStr ? typeStr : "?", msgStr ? msgStr : "?");
}

void sendEventCStr(const char* type, const char* message) {
  if (!gActive) return;
  if (!type) type = "?";
  if (!message) message = "?";
  char buf[512];
  int n = writeHeader(buf, sizeof(buf), "event");
  if (n <= 0 || (size_t)n >= sizeof(buf)) return;
  char typeBuf[40];
  char msgBuf[200];
  escapeJsonInto(typeBuf, sizeof(typeBuf), type);
  escapeJsonInto(msgBuf, sizeof(msgBuf), message);
  snprintf(buf + n, sizeof(buf) - n,
           ",\"type\":\"%s\",\"msg\":\"%s\"}",
           typeBuf, msgBuf);
  sendRaw(buf);
}

void sendWifiState(const char* what, const String& detail) {
  if (!gActive) return;
  char buf[384];
  int n = writeHeader(buf, sizeof(buf), "wifi");
  if (n <= 0 || (size_t)n >= sizeof(buf)) return;
  char whatBuf[40];
  escapeJsonInto(whatBuf, sizeof(whatBuf), what);
  // RSSI + IP + connect-status at point-of-send — captures what we
  // know about the radio at the moment of the state transition.
  int rssi = WiFi.RSSI();
  String ssid = WiFi.SSID();
  String ip = WiFi.localIP().toString();
  char ssidBuf[40], ipBuf[20], detailBuf[160];
  escapeJsonInto(ssidBuf, sizeof(ssidBuf), ssid.c_str());
  escapeJsonInto(ipBuf, sizeof(ipBuf), ip.c_str());
  escapeJsonInto(detailBuf, sizeof(detailBuf), detail.c_str());
  snprintf(buf + n, sizeof(buf) - n,
           ",\"what\":\"%s\",\"ssid\":\"%s\",\"rssi\":%d,\"ip\":\"%s\",\"status\":%d,\"detail\":\"%s\"}",
           whatBuf, ssidBuf, rssi, ipBuf, (int)WiFi.status(), detailBuf);
  sendRaw(buf);
}

void sendBreadcrumb(uint8_t op) {
  if (!gActive) return;
  char buf[160];
  int n = writeHeader(buf, sizeof(buf), "breadcrumb");
  if (n <= 0 || (size_t)n >= sizeof(buf)) return;
  snprintf(buf + n, sizeof(buf) - n,
           ",\"op\":%u,\"mfb\":%u}",
           op, ESP.getMaxFreeBlockSize());
  sendRaw(buf);
}

void sendResetReason(const String& reason) {
  if (!gActive) return;
  char buf[256];
  int n = writeHeader(buf, sizeof(buf), "reset");
  if (n <= 0 || (size_t)n >= sizeof(buf)) return;
  char reasonBuf[100];
  escapeJsonInto(reasonBuf, sizeof(reasonBuf), reason.c_str());
  snprintf(buf + n, sizeof(buf) - n,
           ",\"reason\":\"%s\"}",
           reasonBuf);
  sendRaw(buf);
}

uint32_t packetsSent() { return gPacketsSent; }
uint32_t packetsDropped() { return gPacketsDropped; }

}  // namespace DiagSyslog
