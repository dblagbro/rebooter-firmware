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

1. Review the new built-in local UI on the test device and iterate the UX.
2. Add local browser flow for setting admin credentials and protecting OTA/config actions.
3. Add central-management config fields to the device firmware.
4. Hand off `docs/CENTRAL_SERVER_SPEC.md` and `docs/DEVICE_CENTRAL_INTEGRATION_NOTES.md` to the backend developer.
5. Begin device-side registration and heartbeat client implementation.
6. Verify Sonoff S31 pin mapping against the actual board revision.
7. Split watchdog logic into clearer Internet and Device watchdog modules.

## Backlog

### Post-v0.1.18 safe follow-ups

1. Expand the central heartbeat payload to include the current
   status/recovery truth already present in local `/api/status`,
   including `central_enabled`, `central_registered`,
   `central_state`, `recovery_mode`,
   `auto_recovery_triggered`,
   `last_known_good_restored`,
   `consecutive_unhealthy_boots`, and useful holdoff/cooldown and
   power-config flags.
2. Add a non-secret `reported_config` block to heartbeat so hub-side
   desired-config drift can reconcile against real device truth
   instead of guessing from stale or partial state.
3. Reconcile the remaining asymmetry between local
   `POST /api/config/save` and central `apply_config`, especially
   around notifications and any fields the hub may assume are
   centrally writable when they are not.
4. Keep the recovery / status contract docs and the hub-side schema
   notes in sync whenever firmware status or config fields change,
   so the hub UI and drift logic do not fall behind device reality
   again.
5. After physical verification on the bench, record the final truth
   for short press, 3s reboot, 10s recovery entry, and 30s factory
   reset in both firmware docs and the hub-facing notes.

1. Move hosted firmware delivery under the central server path tree, preferably under `/rebooter/`, instead of depending on a root-level site file.
2. Add bootstrap/main firmware support for trying multiple firmware download locations in order, not just one fixed URL.
3. Include at least one backup firmware-hosting location that is operationally independent from the primary business infrastructure so firmware recovery still works if the primary service is unavailable or the project is no longer maintained.
4. Evaluate practical fallback hosts for long-term resilience, such as public object storage, GitHub Releases, or other durable public file hosting. If a consumer-friendly option such as Google Drive is considered, verify direct-download stability, file size limits, rate limiting, and OTA compatibility before relying on it.
5. Define a canonical firmware URL strategy that cleanly separates:
   - bootstrap/stage-1 firmware source
   - current stable production firmware
   - development/canary firmware
   - fallback/disaster-recovery firmware source

## Completed Checkpoints

- Initial scaffold committed and build-verified.
- Added config schema/default validation, last-known-good config recovery, relay restore persistence, boot warm-up, Wi-Fi-loss watchdog pause, retry limits, and cooldown lockout.
- Added persisted recent event-log storage across reboots.
- Added OTA manager plus local reboot, factory-reset, and firmware upload API endpoints.
- Added local AuthManager with salted password hashes and protected mutating API routes.
