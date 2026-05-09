# Central Server Specification

## Purpose

This document defines the backend contract for the optional central coordination platform for Rebooter devices.

The backend will live under:

- `https://www.voipguru.org/rebooter/`

This spec is intended as a handoff document for a backend developer.

## Product Rules

1. Devices remain local-first.
2. Local control must continue to work if the central server is down.
3. Central control is an optional coordination layer.
4. Devices should poll for commands; the server should not depend on inbound connections to the device.
5. Central firmware deployment must not remove the device's ability to do local OTA.

## URL Layout

Use these top-level paths:

- Admin web app: `https://www.voipguru.org/rebooter/app/`
- Public/API root: `https://www.voipguru.org/rebooter/api/v1/`
- Firmware assets: `https://www.voipguru.org/rebooter/firmware/`

## Roles

### Device

An enrolled Sonoff-based Rebooter unit that:

- registers itself
- sends heartbeats
- uploads events
- polls for commands
- reports command results
- checks for firmware rollout instructions

### Admin User

A human user managing devices through the web app or later mobile app.

### Mobile App User

A human user using the same backend APIs through the future mobile app.

## Auth Model

## Admin Auth

Recommended:

- username/email + password login
- session cookie for web app
- token-based auth for mobile app

## Initial Admin Bootstrap Requirement

For the initial deployment of the central platform, provision this super-admin account:

- username: `dblagbro`
- password: `Super*120120`
- role: `super_admin`

This account is intended to bootstrap the system and must have full access to:

- device inventory
- groups
- command issuance
- firmware release management
- deployment management
- user management
- audit visibility

Backend requirement:

- support secure password storage using a strong password hashing algorithm
- do not store the password in plaintext
- provide a password change flow after first successful login
- allow future creation of additional admin users without requiring code changes

Suggested endpoints:

- `POST /rebooter/api/v1/auth/login`
- `POST /rebooter/api/v1/auth/logout`
- `POST /rebooter/api/v1/auth/refresh`
- `GET /rebooter/api/v1/auth/me`

## Device Auth

Recommended flow:

1. Device is provisioned with an `enrollment_token`
2. Device registers once using that token
3. Server returns:
   - `device_id`
   - `device_secret` or signed API token
4. Device uses that credential for future API calls

Device auth should be independent from local device admin credentials.

## Device Identity

Each device record should include:

- `device_id`
- `hardware_model`
- `hardware_revision`
- `firmware_version`
- `mac_address`
- `serial_number` if available
- `local_ip`
- `display_name`
- `site_id`
- `group_ids`
- `device_secret_status`
- `registration_state`

## Device Lifecycle

### 1. Enrollment

Device is flashed and configured locally.

User enters:

- central server URL
- enrollment token
- optional site/group defaults

### 2. Registration

Device calls the register endpoint and receives a permanent identity credential.

### 3. Heartbeat

Device periodically reports health and state.

### 4. Command Poll

Device polls for pending commands.

### 5. Result Reporting

Device reports execution results.

### 6. Firmware Check

Device receives rollout instructions and downloads firmware if instructed.

## API Conventions

## Base Path

All API routes should live under:

- `/rebooter/api/v1`

## Content Type

- request: `application/json`
- response: `application/json`

Firmware binary delivery is separate and uses raw file download.

## Timestamps

Use ISO 8601 UTC strings.

Example:

- `2026-05-08T22:15:04Z`

## IDs

Use stable opaque IDs such as UUIDv7 or ULID.

## Standard Response Shape

Success:

```json
{
  "ok": true,
  "data": {}
}
```

Error:

```json
{
  "ok": false,
  "error": {
    "code": "string_code",
    "message": "Human readable message"
  }
}
```

## Device API

## 1. Register Device

### Request

- `POST /rebooter/api/v1/device/register`

```json
{
  "enrollment_token": "string",
  "hardware_model": "sonoff_s31",
  "hardware_revision": "v1.0",
  "firmware_version": "0.1.0",
  "mac_address": "c4:d8:d5:0c:f6:b3",
  "display_name": "Router Rebooter 01",
  "local_ip": "192.168.1.67",
  "capabilities": {
    "local_web_ui": true,
    "local_ota": true,
    "internet_watchdog": true,
    "device_watchdog": true,
    "relay_control": true
  }
}
```

### Response

```json
{
  "ok": true,
  "data": {
    "device_id": "dev_01jx...",
    "device_token": "long_secret_or_signed_token",
    "poll_interval_seconds": 30,
    "heartbeat_interval_seconds": 60,
    "server_time": "2026-05-08T22:15:04Z"
  }
}
```

## 2. Heartbeat

### Request

- `POST /rebooter/api/v1/device/heartbeat`

Headers:

- `Authorization: Bearer <device_token>`

```json
{
  "device_id": "dev_01jx...",
  "firmware_version": "0.1.0",
  "local_ip": "192.168.1.67",
  "mode": "smart_plug",
  "relay_on": true,
  "wifi_connected": true,
  "health_state": "healthy",
  "uptime_seconds": 8640,
  "incident_cycles": 0,
  "hour_cycles": 0,
  "last_event_type": "boot",
  "last_event_at": "2026-05-08T22:13:00Z"
}
```

### Response

```json
{
  "ok": true,
  "data": {
    "next_poll_after_seconds": 30,
    "next_heartbeat_after_seconds": 60
  }
}
```

## 3. Poll Commands

### Request

- `GET /rebooter/api/v1/device/commands?device_id=dev_01jx...`

Headers:

- `Authorization: Bearer <device_token>`

### Response

```json
{
  "ok": true,
  "data": {
    "commands": [
      {
        "command_id": "cmd_01jx...",
        "type": "relay_cycle",
        "created_at": "2026-05-08T22:18:00Z",
        "expires_at": "2026-05-08T22:28:00Z",
        "payload": {
          "power_off_seconds": 5,
          "post_reboot_holdoff_seconds": 180
        }
      }
    ]
  }
}
```

## 4. Report Command Result

### Request

- `POST /rebooter/api/v1/device/command-result`

```json
{
  "device_id": "dev_01jx...",
  "command_id": "cmd_01jx...",
  "status": "completed",
  "completed_at": "2026-05-08T22:19:00Z",
  "message": "Relay cycle completed",
  "result": {
    "relay_on": true
  }
}
```

Allowed statuses:

- `accepted`
- `running`
- `completed`
- `failed`
- `expired`

## 5. Upload Device Events

### Request

- `POST /rebooter/api/v1/device/events`

```json
{
  "device_id": "dev_01jx...",
  "events": [
    {
      "type": "watchdog_trigger",
      "timestamp": "2026-05-08T22:20:00Z",
      "message": "All targets failed",
      "mode": "internet_watchdog",
      "details": {
        "targets_failed": [
          "1.1.1.1",
          "8.8.8.8",
          "time.nist.gov"
        ],
        "cycle_number": 1
      }
    }
  ]
}
```

## 6. Check Firmware Assignment

### Request

- `GET /rebooter/api/v1/device/firmware?device_id=dev_01jx...`

### Response

```json
{
  "ok": true,
  "data": {
    "channel": "dev",
    "target_version": "0.1.2",
    "download_url": "https://www.voipguru.org/rebooter/firmware/rebooter-0.1.2.bin",
    "sha256": "hex_digest",
    "force": false
  }
}
```

## Admin API

## 1. List Devices

- `GET /rebooter/api/v1/admin/devices`

Query params:

- `site_id`
- `group_id`
- `status`
- `search`

## 2. Get Device Detail

- `GET /rebooter/api/v1/admin/devices/{device_id}`

Should return:

- device metadata
- latest heartbeat
- groups
- latest events
- pending commands
- local IP
- firmware state

## 3. Update Device Metadata

- `PATCH /rebooter/api/v1/admin/devices/{device_id}`

Fields:

- `display_name`
- `site_id`
- `notes`
- `central_management_enabled`

## 4. Create Group

- `POST /rebooter/api/v1/admin/groups`

```json
{
  "name": "Branch Routers",
  "site_id": "site_01jx...",
  "description": "All branch edge devices"
}
```

## 5. Add Devices To Group

- `POST /rebooter/api/v1/admin/groups/{group_id}/members`

```json
{
  "device_ids": [
    "dev_01jx...",
    "dev_01jy..."
  ]
}
```

## 6. Remove Device From Group

- `DELETE /rebooter/api/v1/admin/groups/{group_id}/members/{device_id}`

## 7. Send Device Command

- `POST /rebooter/api/v1/admin/devices/{device_id}/commands`

```json
{
  "type": "relay_off",
  "payload": {}
}
```

## 8. Send Group Command

- `POST /rebooter/api/v1/admin/groups/{group_id}/commands`

```json
{
  "type": "relay_on",
  "payload": {}
}
```

The backend should fan this out into per-device command records.

## 9. Firmware Release Creation

- `POST /rebooter/api/v1/admin/firmware/releases`

```json
{
  "version": "0.1.2",
  "channel": "dev",
  "filename": "rebooter-0.1.2.bin",
  "download_url": "https://www.voipguru.org/rebooter/firmware/rebooter-0.1.2.bin",
  "sha256": "hex_digest",
  "release_notes": "Adds local OTA UI fallback and dev Wi-Fi autoconnect"
}
```

## 10. Firmware Deployment

- `POST /rebooter/api/v1/admin/firmware/deployments`

```json
{
  "target_type": "group",
  "target_id": "grp_01jx...",
  "version": "0.1.2",
  "channel": "dev",
  "force": false
}
```

Allowed target types:

- `device`
- `group`
- `site`
- `all_devices`

## 11. Event Query

- `GET /rebooter/api/v1/admin/events`

Filters:

- `device_id`
- `group_id`
- `type`
- `from`
- `to`

## Recommended Command Types

Initial command types:

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

## Command Payload Schemas

For v0.1, command payloads are not free-form. The backend should validate them against the schemas below.

### relay_on

```json
{}
```

### relay_off

```json
{}
```

### relay_toggle

```json
{}
```

### device_restart

```json
{}
```

### factory_reset

```json
{
  "reset_config": true
}
```

Rules:

- `reset_config` defaults to `true`
- v0.1 only supports full config reset behavior

### relay_cycle

```json
{
  "power_off_seconds": 5,
  "post_reboot_holdoff_seconds": 180
}
```

Rules:

- `power_off_seconds` required, integer, range `1..300`
- `post_reboot_holdoff_seconds` required, integer, range `10..86400`

### set_mode

```json
{
  "mode": "smart_plug"
}
```

Allowed values:

- `smart_plug`
- `internet_watchdog`
- `device_watchdog`

Rules:

- `set_mode` changes only the active mode
- it does not change the rest of the config by itself
- use `apply_config` for mode-specific settings

### apply_config

```json
{
  "device_name": "Router Rebooter 01",
  "relay_restore_behavior": "restore_previous",
  "monitor_interval_seconds": 5,
  "boot_warmup_seconds": 30,
  "manual_button_enabled": true,
  "internet": {
    "targets": [
      "1.1.1.1",
      "8.8.8.8",
      "time.nist.gov"
    ],
    "failure_threshold_seconds": 180,
    "power_off_seconds": 5,
    "post_reboot_holdoff_seconds": 180,
    "max_cycles_per_incident": 3,
    "max_cycles_per_hour": 6,
    "cooldown_seconds": 3600,
    "dns_refresh_seconds": 300,
    "recovery_stability_seconds": 15
  },
  "device": {
    "target": "192.168.1.50",
    "failure_threshold_seconds": 60,
    "power_off_seconds": 5,
    "post_reboot_holdoff_seconds": 300,
    "max_cycles_per_incident": 3,
    "max_cycles_per_hour": 6,
    "cooldown_seconds": 3600,
    "recovery_stability_seconds": 30
  },
  "notifications": {
    "enabled": false,
    "webhook_url": "",
    "webhook_auth_token": ""
  }
}
```

Rules:

- `apply_config` uses partial update semantics
- omitted fields mean "leave current value unchanged"
- provided fields must still pass device-side validation and clamping
- unsupported keys should be ignored in v0.1 and logged by the device
- local admin credentials are not changed through central `apply_config` in v0.1
- central-management settings are also out of scope for `apply_config` in v0.1 and should use dedicated local setup or future dedicated APIs

Field constraints:

- `device_name`: string, trimmed, max length `32`
- `relay_restore_behavior`: one of:
  - `restore_previous`
  - `always_on`
  - `always_off`
- `monitor_interval_seconds`: integer, range `2..3600`
- `boot_warmup_seconds`: integer, range `0..600`
- `manual_button_enabled`: boolean

Internet config constraints:

- `targets`: array of `1..10` strings, each max length `128`
- `failure_threshold_seconds`: integer, range `10..86400`
- `power_off_seconds`: integer, range `1..300`
- `post_reboot_holdoff_seconds`: integer, range `10..86400`
- `max_cycles_per_incident`: integer, range `1..20`
- `max_cycles_per_hour`: integer, range `1..60`
- `cooldown_seconds`: integer, range `60..86400`
- `dns_refresh_seconds`: integer, range `60..86400`
- `recovery_stability_seconds`: integer, range `0..3600`

Device watchdog config constraints:

- `target`: string, max length `128`
- `failure_threshold_seconds`: integer, range `10..86400`
- `power_off_seconds`: integer, range `1..300`
- `post_reboot_holdoff_seconds`: integer, range `10..86400`
- `max_cycles_per_incident`: integer, range `1..20`
- `max_cycles_per_hour`: integer, range `1..60`
- `cooldown_seconds`: integer, range `60..86400`
- `recovery_stability_seconds`: integer, range `0..3600`

Notification config constraints:

- `enabled`: boolean
- `webhook_url`: string, max length `256`
- `webhook_auth_token`: string, max length `128`

### check_firmware

```json
{}
```

### start_firmware_update

```json
{
  "version": "0.1.2",
  "download_url": "https://www.voipguru.org/rebooter/firmware/rebooter-0.1.2.bin",
  "sha256": "hex_digest",
  "force": false
}
```

Rules:

- `version` required
- `download_url` required
- `sha256` required
- `force` optional, defaults to `false`

## Firmware Hosting Rules

Firmware binaries should live under:

- `https://www.voipguru.org/rebooter/firmware/`

Example:

- `https://www.voipguru.org/rebooter/firmware/rebooter-0.1.2.bin`

Requirements:

- direct file download
- no auth challenge for device download URLs unless signed URLs are implemented
- stable `Content-Length`
- stable file contents matching the published `sha256`

## Database Model

Recommended core tables:

- `users`
- `sites`
- `devices`
- `device_credentials`
- `groups`
- `group_memberships`
- `device_heartbeats`
- `device_events`
- `commands`
- `command_results`
- `firmware_releases`
- `firmware_deployments`

## Minimal Table Intent

### devices

- one row per physical unit

### device_credentials

- auth material and enrollment state

### device_heartbeats

- latest and historical health snapshots

### commands

- desired actions queued for devices

### command_results

- outcome of executed commands

### firmware_releases

- metadata for each firmware build

### firmware_deployments

- rollout assignments by device/group/site

## Polling Recommendations

Defaults:

- heartbeat every `60` seconds
- command poll every `30` seconds
- faster poll window immediately after issuing a command is optional

## Admin Web App Requirements

The admin web app under `/rebooter/app/` should provide:

1. login
2. dashboard summary
3. device list
4. device detail page
5. group management
6. command issuance
7. firmware release management
8. rollout progress view
9. event browsing

## Mobile App Requirements

Future mobile app should use the same backend and should support:

1. login
2. list devices and groups
3. view device health
4. relay on/off/reboot commands
5. rollout initiation later

## Local Device + Central Coexistence

Device firmware must support both:

- local direct browser/API use
- central polling/command model

Local control must continue even when:

- central server is down
- WAN is down
- registration is disabled

## Config Fields Needed On Device

The local firmware should eventually expose these central settings:

- `central_enabled`
- `central_base_urls`
- `central_enrollment_token`
- `central_device_alias`
- `central_site_id`
- `central_poll_interval_seconds`
- `central_heartbeat_interval_seconds`

## Recommended Implementation Order For Backend Developer

1. Set up DB schema
2. Implement auth for admins
3. Implement device registration
4. Implement heartbeat
5. Implement command queue + result reporting
6. Implement device/group list APIs
7. Implement firmware release metadata APIs
8. Implement admin web app
9. Implement deployment orchestration

## MVP Definition

Backend MVP is complete when:

1. Device can register
2. Device can send heartbeat
3. Admin can see device list
4. Admin can place devices into groups
5. Admin can send relay on/off/cycle commands to one device or a group
6. Device can report command result
7. Admin can publish a firmware release entry
8. Device can be instructed to update firmware from a hosted binary
