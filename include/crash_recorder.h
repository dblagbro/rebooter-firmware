#pragma once

#include <Arduino.h>

// On-flash crash capture.
//
// The ESP8266 Arduino core calls a weak `custom_crash_callback` from the
// exception/abort handler before reboot. That context is fragile: heap is
// suspect and allocation is unsafe. So the handler only writes a small,
// fixed, CRC-protected record to RTC user memory (survives a crash reboot,
// not power loss). On the next normal boot we copy that record into a
// LittleFS JSON file where allocation is safe.

// Fixed RTC record layout. Kept word-aligned because ESP.rtcUserMemory*
// operates on 32-bit words.
//
// 0.2.25 CRITICAL fix (firmware code review F1): stack was [32] which made
// sizeof(CrashRtcRecord) = 12*4 + 32*4 = 176 bytes. At RTC word offset 96
// the record would have occupied bytes 384..559 — but ESP8266 RTC user
// memory caps at 512 bytes (128 words). The Arduino core's rtcUserMemory*
// silently rejects out-of-range writes and returns false; crash_recorder.cpp
// ignored the bool. EVERY crash record since this module shipped was lost.
// Shrinking stack[20] → sizeof becomes 12*4 + 20*4 = 128 bytes = 32 words,
// fitting exactly at offset 96..127 with the breadcrumb at 80..83 below.
struct CrashRtcRecord {
  uint32_t magic;          // validity sentinel
  uint32_t crc;            // CRC32 of every field after this one
  uint32_t resetReason;    // rst_info.reason
  uint32_t exceptCause;    // rst_info.exccause
  uint32_t epc1;
  uint32_t epc2;
  uint32_t epc3;
  uint32_t excvaddr;
  uint32_t depc;
  uint32_t uptimeMs;       // millis() at crash
  uint32_t fwVersionHash;  // hash of FIRMWARE_VERSION at crash
  uint32_t stackDepth;     // number of valid words in stack[]
  uint32_t stack[20];      // top-of-stack excerpt (was [32], see 0.2.25 note above)
};
static_assert(sizeof(CrashRtcRecord) == 128, "CrashRtcRecord must fit RTC byte budget");

namespace CrashRecorder {

// Called early in setup() after LittleFS.begin(). If a valid CRC'd crash
// record is present in RTC memory, decode it into the LittleFS crash ring
// (/crash/crash-0..2.json, /crash/last.json) and clear the RTC slot.
// Returns true if a crash record was found and persisted this boot.
bool processPendingCrash();

// True if at least one crash file exists on flash.
bool hasStoredCrash();

// One-line human reason for the most recent stored crash ("" if none).
String lastCrashReason();

// Serializes the stored crash ring as a JSON array string for the API.
String storedCrashesJson();

// Removes all /crash/* files. Used by the API clear endpoint and factory reset.
void clearStoredCrashes();

// Stable 32-bit hash used to stamp the firmware version into a crash record.
uint32_t firmwareVersionHash();

}  // namespace CrashRecorder
