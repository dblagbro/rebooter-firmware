#include "pre_crash_breadcrumb.h"

namespace PreCrashBreadcrumb {

namespace {

constexpr uint32_t RTC_WORD_OFFSET = 80;
constexpr uint32_t MAGIC = 0xB12E4C03;  // unique sentinel

struct __attribute__((packed)) Record {
  uint32_t magic;
  uint32_t crc;
  uint32_t op_and_timestamp;  // [31:24]=opcode [23:0]=lower 24 bits of millis()
  uint32_t reserved;
};
static_assert(sizeof(Record) == 16, "Record must be 16 bytes / 4 words");
static_assert(sizeof(Record) % 4 == 0, "Record must be word-aligned");

uint32_t crcOf(const Record& rec) {
  // Simple Fletcher-style sum over op_and_timestamp + reserved. Strong
  // enough to detect uninitialized RTC garbage, which is the only
  // failure mode we care about; collisions don't matter (a false-positive
  // breadcrumb just logs a noise line).
  return rec.op_and_timestamp ^ (rec.reserved + 0x9E3779B9);
}

const char* opName(uint8_t op) {
  switch (op) {
    case OP_HTTPS_HEARTBEAT:      return "https-heartbeat";
    case OP_HTTPS_POST_RESULT:    return "https-post-result";
    case OP_HTTPS_ANNOUNCE:       return "https-announce";
    case OP_HTTPS_REGISTER:       return "https-register";
    case OP_HTTPS_FETCH_COMMANDS: return "https-fetch-commands";
    case OP_OTA_FINALIZE:         return "ota-finalize";
    case OP_FS_EVENT_LOG_WRITE:   return "fs-event-log-write";
    case OP_FS_CONFIG_WRITE:      return "fs-config-write";
    case OP_FS_BOOT_STATE_WRITE:  return "fs-boot-state-write";
    default:                      return "unknown";
  }
}

bool readRtc(Record& out) {
  return ESP.rtcUserMemoryRead(RTC_WORD_OFFSET,
                               reinterpret_cast<uint32_t*>(&out),
                               sizeof(out));
}

void writeRtc(const Record& rec) {
  ESP.rtcUserMemoryWrite(RTC_WORD_OFFSET,
                         const_cast<uint32_t*>(reinterpret_cast<const uint32_t*>(&rec)),
                         sizeof(rec));
}

}  // namespace

bool processPending(LogCallback log) {
  Record rec;
  if (!readRtc(rec)) return false;
  if (rec.magic != MAGIC) return false;
  if (rec.crc != crcOf(rec)) return false;
  const uint8_t op = static_cast<uint8_t>(rec.op_and_timestamp >> 24);
  if (op == OP_NONE) return false;
  if (log) {
    const uint32_t ts_lo = rec.op_and_timestamp & 0xFFFFFFu;
    log("crash", String("Pre-crash breadcrumb: ") + opName(op) +
        " (last seen at uptime " + String(ts_lo) + " ms)");
  }
  // Clear so this isn't replayed on every subsequent boot.
  clear();
  return true;
}

void set(Op op) {
  Record rec;
  rec.magic = MAGIC;
  // Pack opcode + low 24 bits of millis() into one word — gives ~16777s
  // (~4.5h) of timestamp range; for breadcrumb forensics that's plenty.
  rec.op_and_timestamp = (static_cast<uint32_t>(op) << 24) | (millis() & 0xFFFFFFu);
  rec.reserved = 0;
  rec.crc = crcOf(rec);
  writeRtc(rec);
}

void clear() {
  Record rec;
  rec.magic = 0;
  rec.crc = 0;
  rec.op_and_timestamp = 0;
  rec.reserved = 0;
  writeRtc(rec);
}

}  // namespace PreCrashBreadcrumb
