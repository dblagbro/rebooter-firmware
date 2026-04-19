# Next Steps

## Current Goal

Build a production-capable local-first Sonoff S31 firmware prototype for:

1. Smart plug mode
2. Internet watchdog mode
3. Device watchdog mode

## Current Status

- Initial proof-of-concept files are preserved under `docs/poc/`.
- The authoritative firmware design spec is now in `SPECS.md`.
- PlatformIO scaffold has been split into real source, include, and data files.
- PlatformIO CLI is installed: PlatformIO Core 6.1.19.
- Git for Windows is installed: 2.53.0.windows.3. The current shell may need a restart for PATH updates; use `C:\Program Files\Git\cmd\git.exe` meanwhile.

## Immediate Tasks

1. Verify Sonoff S31 pin mapping against the actual board revision.
2. Split watchdog logic into clearer Internet and Device watchdog modules.
3. Add OTA manager and local authenticated web UI.
4. Create first serial flash log in `docs/`.
5. Validate relay, LED, and button behavior on isolated hardware before mains testing.

## Completed Checkpoints

- Initial scaffold committed and build-verified.
- Added config schema/default validation, last-known-good config recovery, relay restore persistence, boot warm-up, Wi-Fi-loss watchdog pause, retry limits, and cooldown lockout.
- Added persisted recent event-log storage across reboots.
