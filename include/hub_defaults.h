#pragma once

#include <Arduino.h>
#include <vector>

// Canonical default hub / central base URLs.
//
// This is the single source of truth for the factory hub URL list. Reference
// it from every place that previously hardcoded "https://www.voipguru.org/rebooter"
// (types.h initializer, config_manager.cpp empty-list fallback) so that
// "defaults = current hub URLs" stays true after any future URL change.
namespace HubDefaults {

  // Maximum number of user-configurable hub URL slots.
  static constexpr size_t MAX_BASE_URLS = 10;

  // Per-URL length cap. Self-hosted URLs can be long, so this is generous.
  static constexpr size_t MAX_BASE_URL_LENGTH = 192;

  static constexpr char PRIMARY_BASE_URL[] = "https://www.voipguru.org/rebooter";

  // The default list. Today this is a single SaaS hub URL; additional
  // defaults can be appended here without touching any other file.
  inline std::vector<String> defaultBaseUrls() {
    std::vector<String> urls;
    urls.push_back(String(PRIMARY_BASE_URL));
    return urls;
  }

}
