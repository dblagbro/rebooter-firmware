#pragma once
#include <Arduino.h>

// 0.2.22 (#183) Pre-crash breadcrumb.
//
// Hardware-WDT and SDK-initiated system_restart bypass the user-level
// custom_crash_callback. So for the .188-class "sudden crash with healthy
// mfb" failures we have no idea WHICH operation the device was inside
// when it died. This module writes a 1-byte opcode + 32-bit timestamp
// to RTC user memory BEFORE each risky operation and clears it on
// success. On the next boot, an uncleared breadcrumb tells us exactly
// where the device crashed.
//
// RTC user memory layout (32-bit words):
//   eboot       : 0–31  (SDK reserved-ish)
//   breadcrumb  : 80–83 (this module — 4 words = 16 bytes)
//   crash dump  : 96–139 (CrashRecorder)
//
// Survives REASON_EXCEPTION_RST, REASON_SOFT_WDT_RST, REASON_WDT_RST,
// and REASON_SOFT_RESTART. Wiped on REASON_DEFAULT_RST (cold power).

namespace PreCrashBreadcrumb {

enum Op : uint8_t {
  OP_NONE                  = 0,
  OP_HTTPS_HEARTBEAT       = 1,
  OP_HTTPS_POST_RESULT     = 2,
  OP_HTTPS_ANNOUNCE        = 3,
  OP_HTTPS_REGISTER        = 4,
  OP_HTTPS_FETCH_COMMANDS  = 5,
  OP_OTA_FINALIZE          = 6,
};

// Read RTC; if a valid uncleared record is present, log it via the
// caller-supplied callback (e.g. EventLog::add). Then clear the slot.
// Returns true if a breadcrumb was found.
using LogCallback = void(*)(const char* type, const String& message);
bool processPending(LogCallback log);

// Stamp the breadcrumb to `op` before a risky operation. Cheap (~1µs).
void set(Op op);

// Clear the breadcrumb after a successful operation.
void clear();

// RAII scope helper: set on construction, clear on destruction. Use at
// the top of any function whose body is a "risky operation."
struct Scope {
  explicit Scope(Op op) { set(op); }
  ~Scope() { clear(); }
};

}  // namespace PreCrashBreadcrumb
