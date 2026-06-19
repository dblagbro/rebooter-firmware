#pragma once

#ifdef SAFE_FALLBACK_TEST_BAD_BOOT
static constexpr char FIRMWARE_VERSION[] = "0.2.37-dev-central-safe-badboot";
#else
static constexpr char FIRMWARE_VERSION[] = "0.2.37-dev-central-safe";
#endif
