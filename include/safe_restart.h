#pragma once
#include <Arduino.h>

// 0.2.33 #205: pre-restart wait that doesn't yield to the SYS task.
//
// Pre-fix every `delay(N); ESP.restart();` site (proactive heap-pressure,
// recovery-provisioning, device_restart hub command, button factory
// reset, four /api/system/* HTTP-triggered reboots) was at risk of the
// same race the 0.2.26 fix closed at the proactive-restart site:
// delay() yields to lwIP/BearSSL/SYS background, which can `alloc-fail
// → REASON_EXCEPTION_RST` inside the delay window. Each path that uses
// plain delay() before ESP.restart() can produce an Exception-class
// secondary crash instead of the clean Software/System restart the
// caller intended.
//
// safeRestartWait(ms) keeps the CPU on the current thread (no scheduler
// yield) while feeding both watchdogs so the device doesn't hardware-
// WDT-reset mid-wait. The LittleFS metadata-commit budget the original
// delay() was buying is preserved.
inline void safeRestartWait(uint32_t ms) {
  const uint32_t start = millis();
  while (millis() - start < ms) {
    ESP.wdtFeed();
    delayMicroseconds(1000);  // busy-wait, no scheduler yield
  }
}
