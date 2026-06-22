#pragma once

// 0.2.41 refactor: fallback web-UI assets (HTML/CSS/JS) extracted from
// web_server_manager.cpp into web_assets.cpp. These are the embedded
// copies served when LittleFS doesn't have the matching file at /data/.
// Per architecture.md §"UI serving model", the fallback UI must stay
// behaviorally aligned with the LittleFS assets.
//
// Declared extern so web_server_manager.cpp's serveFileOrFallback()
// calls resolve at link time. The blobs themselves are in
// web_assets.cpp's PROGMEM section — no RAM cost, same as before.

extern const char FALLBACK_INDEX_HTML[];
extern const char FALLBACK_STYLE_CSS[];
extern const char FALLBACK_APP_JS[];
