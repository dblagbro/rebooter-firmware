#include <Arduino.h>
#include <Updater.h>
#include "ota_manager.h"
#include "event_log.h"

void OtaManager::begin(EventLog* eventLog) {
  eventLog_ = eventLog;
}

void OtaManager::handleUpload(HTTPUpload& upload) {
  if (upload.status == UPLOAD_FILE_START) {
    updateError_ = false;
    lastError_ = 0;
    updateStarted_ = true;
    const uint32_t maxSketchSpace = (ESP.getFreeSketchSpace() - 0x1000) & 0xFFFFF000;
    if (eventLog_) eventLog_->add("ota", "Firmware upload started");
    if (!Update.begin(maxSketchSpace)) {
      updateError_ = true;
      lastError_ = Update.getError();
      if (eventLog_) eventLog_->add("ota", "Firmware upload failed to begin");
    }
    return;
  }

  if (upload.status == UPLOAD_FILE_WRITE) {
    if (!updateError_ && Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
      updateError_ = true;
      lastError_ = Update.getError();
      if (eventLog_) eventLog_->add("ota", "Firmware upload write failed");
    }
    return;
  }

  if (upload.status == UPLOAD_FILE_END) {
    if (!updateError_ && !Update.end(true)) {
      updateError_ = true;
      lastError_ = Update.getError();
      if (eventLog_) eventLog_->add("ota", "Firmware image validation failed");
      return;
    }
    if (!updateError_ && eventLog_) eventLog_->add("ota", "Firmware upload complete, reboot required");
    updateStarted_ = false;
    return;
  }

  if (upload.status == UPLOAD_FILE_ABORTED) {
    Update.end();
    updateError_ = true;
    lastError_ = Update.getError();
    updateStarted_ = false;
    if (eventLog_) eventLog_->add("ota", "Firmware upload aborted");
  }
}

bool OtaManager::hasError() const {
  return updateError_;
}

String OtaManager::errorString() const {
  if (!updateError_) return "";
  return "OTA error " + String(lastError_);
}