#include "crash_recorder.h"

#include <Arduino.h>
#include <LittleFS.h>
#include <ArduinoJson.h>

#include "firmware_version.h"

extern "C" {
#include <user_interface.h>
}

namespace {

constexpr uint32_t CRASH_RTC_MAGIC = 0x52424352;  // 'RBCR'
// RTC user memory is addressed in 32-bit words. The first 64 words are used
// by the SDK / OTA; user memory starts at offset 64. Place the crash record
// well clear of any core usage.
constexpr uint32_t CRASH_RTC_WORD_OFFSET = 96;
constexpr uint8_t CRASH_RING_SIZE = 3;
constexpr char CRASH_DIR[] = "/crash";
constexpr char CRASH_LAST_PATH[] = "/crash/last.json";

uint32_t crc32Update(uint32_t crc, const uint8_t* data, size_t len) {
  crc = ~crc;
  for (size_t i = 0; i < len; ++i) {
    crc ^= data[i];
    for (uint8_t b = 0; b < 8; ++b) {
      crc = (crc >> 1) ^ (0xEDB88320UL & (-(int32_t)(crc & 1)));
    }
  }
  return ~crc;
}

uint32_t recordCrc(const CrashRtcRecord& rec) {
  // CRC covers every field after magic+crc.
  const uint8_t* start = reinterpret_cast<const uint8_t*>(&rec.resetReason);
  const size_t len = sizeof(CrashRtcRecord) -
                     offsetof(CrashRtcRecord, resetReason);
  return crc32Update(0, start, len);
}

bool readRtcRecord(CrashRtcRecord& out) {
  static_assert(sizeof(CrashRtcRecord) % 4 == 0,
                "CrashRtcRecord must be word-aligned for RTC memory");
  if (!ESP.rtcUserMemoryRead(CRASH_RTC_WORD_OFFSET,
                             reinterpret_cast<uint32_t*>(&out),
                             sizeof(CrashRtcRecord))) {
    return false;
  }
  if (out.magic != CRASH_RTC_MAGIC) return false;
  return out.crc == recordCrc(out);
}

void clearRtcRecord() {
  CrashRtcRecord blank;
  memset(&blank, 0, sizeof(blank));
  ESP.rtcUserMemoryWrite(CRASH_RTC_WORD_OFFSET,
                         reinterpret_cast<uint32_t*>(&blank),
                         sizeof(blank));
}

const char* resetReasonText(uint32_t reason) {
  switch (reason) {
    case REASON_DEFAULT_RST: return "power_on";
    case REASON_WDT_RST: return "hardware_watchdog";
    case REASON_EXCEPTION_RST: return "exception";
    case REASON_SOFT_WDT_RST: return "software_watchdog";
    case REASON_SOFT_RESTART: return "software_restart";
    case REASON_DEEP_SLEEP_AWAKE: return "deep_sleep_wake";
    case REASON_EXT_SYS_RST: return "external_reset";
    default: return "unknown";
  }
}

String crashFilePath(uint8_t index) {
  return String(CRASH_DIR) + "/crash-" + String(index) + ".json";
}

void ensureCrashDir() {
  if (!LittleFS.exists(CRASH_DIR)) {
    LittleFS.mkdir(CRASH_DIR);
  }
}

// Serializes a decoded crash record into a JsonObject.
void crashRecordToJson(JsonObject obj, const CrashRtcRecord& rec) {
  obj["reset_reason_code"] = rec.resetReason;
  obj["reset_reason"] = resetReasonText(rec.resetReason);
  obj["exception_cause"] = rec.exceptCause;
  obj["epc1"] = rec.epc1;
  obj["epc2"] = rec.epc2;
  obj["epc3"] = rec.epc3;
  obj["excvaddr"] = rec.excvaddr;
  obj["depc"] = rec.depc;
  obj["crash_uptime_ms"] = rec.uptimeMs;
  obj["fw_version_hash"] = rec.fwVersionHash;
  JsonArray stack = obj["stack"].to<JsonArray>();
  const uint32_t depth = rec.stackDepth > 32 ? 32 : rec.stackDepth;
  for (uint32_t i = 0; i < depth; ++i) {
    stack.add(rec.stack[i]);
  }
}

}  // namespace

// The ESP8266 Arduino core invokes this weak symbol from the panic/abort
// handler before the device reboots. It runs in a fragile post-exception
// context: interrupts are off and heap is suspect. So it ONLY writes a
// fixed-size, CRC-protected struct to RTC user memory. No allocation, no
// Serial, no LittleFS here.
extern "C" void custom_crash_callback(struct rst_info* resetInfo,
                                      uint32_t stackStart,
                                      uint32_t stackEnd) {
  CrashRtcRecord rec;
  memset(&rec, 0, sizeof(rec));
  rec.magic = CRASH_RTC_MAGIC;
  rec.resetReason = resetInfo ? resetInfo->reason : 0xFFFFFFFFUL;
  rec.exceptCause = resetInfo ? resetInfo->exccause : 0;
  rec.epc1 = resetInfo ? resetInfo->epc1 : 0;
  rec.epc2 = resetInfo ? resetInfo->epc2 : 0;
  rec.epc3 = resetInfo ? resetInfo->epc3 : 0;
  rec.excvaddr = resetInfo ? resetInfo->excvaddr : 0;
  rec.depc = resetInfo ? resetInfo->depc : 0;
  rec.uptimeMs = millis();
  rec.fwVersionHash = CrashRecorder::firmwareVersionHash();

  uint32_t depth = 0;
  // 0.2.25 CRITICAL fix: stack capacity reduced from 32 → 20 to make the
  // record fit in the 512-byte RTC user memory budget (see crash_recorder.h
  // note). The walking loop bound must match the array size.
  for (uint32_t addr = stackStart; addr < stackEnd && depth < 20; addr += 4) {
    rec.stack[depth++] = *reinterpret_cast<uint32_t*>(addr);
  }
  rec.stackDepth = depth;

  rec.crc = recordCrc(rec);
  ESP.rtcUserMemoryWrite(CRASH_RTC_WORD_OFFSET,
                         reinterpret_cast<uint32_t*>(&rec),
                         sizeof(rec));
}

namespace CrashRecorder {

uint32_t firmwareVersionHash() {
  // FNV-1a over the firmware version string.
  uint32_t hash = 2166136261UL;
  for (const char* p = FIRMWARE_VERSION; *p; ++p) {
    hash ^= static_cast<uint8_t>(*p);
    hash *= 16777619UL;
  }
  return hash;
}

bool processPendingCrash() {
  CrashRtcRecord rec;
  if (!readRtcRecord(rec)) return false;

  // Valid crash record found. Clear the RTC slot immediately so a failure
  // below cannot replay it endlessly.
  clearRtcRecord();

  ensureCrashDir();

  // Rotate the ring: crash-1 -> crash-2, crash-0 -> crash-1, new -> crash-0.
  if (LittleFS.exists(crashFilePath(CRASH_RING_SIZE - 1))) {
    LittleFS.remove(crashFilePath(CRASH_RING_SIZE - 1));
  }
  for (int8_t i = CRASH_RING_SIZE - 2; i >= 0; --i) {
    if (LittleFS.exists(crashFilePath(i))) {
      LittleFS.rename(crashFilePath(i), crashFilePath(i + 1));
    }
  }

  JsonDocument doc;
  JsonObject obj = doc.to<JsonObject>();
  crashRecordToJson(obj, rec);

  File f = LittleFS.open(crashFilePath(0), "w");
  if (f) {
    serializeJson(doc, f);
    f.close();
  }
  // Keep /crash/last.json as a stable alias for the newest crash.
  File last = LittleFS.open(CRASH_LAST_PATH, "w");
  if (last) {
    serializeJson(doc, last);
    last.close();
  }
  return true;
}

bool hasStoredCrash() {
  return LittleFS.exists(crashFilePath(0));
}

String lastCrashReason() {
  if (!LittleFS.exists(crashFilePath(0))) return "";
  File f = LittleFS.open(crashFilePath(0), "r");
  if (!f) return "";
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, f);
  f.close();
  if (err != DeserializationError::Ok) return "";
  String reason = String((const char*)(doc["reset_reason"] | "unknown"));
  const uint32_t cause = doc["exception_cause"] | 0;
  if (reason == "exception") {
    reason += " (" + String(cause) + ")";
  }
  return reason;
}

String storedCrashesJson() {
  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();
  for (uint8_t i = 0; i < CRASH_RING_SIZE; ++i) {
    if (!LittleFS.exists(crashFilePath(i))) continue;
    File f = LittleFS.open(crashFilePath(i), "r");
    if (!f) continue;
    JsonDocument entry;
    DeserializationError err = deserializeJson(entry, f);
    f.close();
    if (err != DeserializationError::Ok) continue;
    JsonObject row = arr.add<JsonObject>();
    row["slot"] = i;
    for (JsonPair kv : entry.as<JsonObject>()) {
      row[kv.key()] = kv.value();
    }
  }
  String out;
  serializeJson(doc, out);
  return out;
}

void clearStoredCrashes() {
  for (uint8_t i = 0; i < CRASH_RING_SIZE; ++i) {
    if (LittleFS.exists(crashFilePath(i))) {
      LittleFS.remove(crashFilePath(i));
    }
  }
  if (LittleFS.exists(CRASH_LAST_PATH)) {
    LittleFS.remove(CRASH_LAST_PATH);
  }
}

}  // namespace CrashRecorder
