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

1. Create the first Git checkpoint for the scaffold.
2. Verify Sonoff S31 pin mapping against the actual board revision.
3. Begin implementing missing v1 spec items: config schema, validation, retry limits, cooldown, OTA, and auth.
4. Split watchdog logic into clearer Internet and Device watchdog modules.
5. Create first serial flash log in `docs/`.
6. Validate relay, LED, and button behavior on isolated hardware before mains testing.
