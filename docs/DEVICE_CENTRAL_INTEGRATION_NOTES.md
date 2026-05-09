# Device Central Integration Notes

## Purpose

This document defines the firmware-side work needed to integrate a Rebooter device with the central platform described in `CENTRAL_SERVER_SPEC.md`.

## Device Requirements

The device should expose local config for:

- central management enabled
- central server base URL
- enrollment token
- device alias
- optional site/group defaults
- poll interval
- heartbeat interval

## Device Runtime Responsibilities

1. Register once with central service
2. Persist returned device identity/token locally
3. Send periodic heartbeats
4. Poll for commands
5. Execute commands safely
6. Report results
7. Check assigned firmware metadata
8. Download and install firmware when instructed

## v0.1 Central Command Support

The device-side implementation for v0.1 should support these command types:

- `relay_on`
- `relay_off`
- `relay_toggle`
- `relay_cycle`
- `device_restart`
- `factory_reset`
- `set_mode`
- `apply_config`
- `check_firmware`
- `start_firmware_update`

The exact payload schemas are defined in `CENTRAL_SERVER_SPEC.md`.

Important:

- `set_mode` changes only the active mode field
- `apply_config` uses partial-update semantics
- local admin credentials are not writable from central commands in v0.1
- unsupported keys in `apply_config` should be ignored and logged

## Safety Rules

1. Central commands must still obey local relay safety rules.
2. OTA updates initiated by central control must reuse the same internal OTA safety path as local OTA.
3. A failed central request must not block local operation.
4. If WAN is down, watchdog logic must behave according to the local rules already defined in the firmware spec.

## Suggested Firmware Modules

Future device-side modules:

- `central_client.cpp`
- `central_client.h`
- `firmware_update_client.cpp`
- `firmware_update_client.h`

## Suggested Initial Device Milestones

1. Config model can store central settings.
2. Device can register and persist a device token.
3. Device can send heartbeat.
4. Device can poll and log commands without executing them yet.
5. Device can execute simple relay commands from central queue.
6. Device can perform central-triggered firmware update.
