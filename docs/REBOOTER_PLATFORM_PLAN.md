# Rebooter Platform Plan

## Product Direction

Build the project in three layers:

1. Minimal serial bootstrap firmware
2. Full local-first device firmware with local web UI and OTA
3. Optional central coordination platform at `https://www.voipguru.org/rebooter/`

The device must remain useful even if the central platform is offline.

## What We Already Have

- A working bootstrap pattern:
  - serial flash once
  - join known Wi-Fi
  - download full firmware from `voipguru.org`
- A working main firmware OTA path over local HTTP
- A main firmware skeleton with:
  - relay control
  - watchdog scaffolding
  - config persistence
  - event logging
  - local API
  - local OTA upload handler

## Phase Plan

## Phase 0: Bring-Up Path

Purpose:

Make new devices easy to get onto the network and into the real firmware.

Flow:

1. Flash a tiny bootstrap image over serial.
2. Bootstrap joins known Wi-Fi.
3. Bootstrap downloads full firmware from `https://www.voipguru.org/<firmware>.bin`.
4. Device reboots into the main firmware.

Requirements:

- minimal code size
- conservative serial flash path
- very clear serial logging
- no relay automation during bootstrap

## Phase 1: Full Local-First Firmware

Purpose:

Deliver the original device behavior from the design spec through local web UI and local API.

Core features:

- Smart Plug mode
- Internet Watchdog mode
- Device Watchdog mode
- local config persistence
- local web UI
- local OTA upload
- local auth
- event log

Important rule:

This phase must work without the central platform.

### Phase 1A: Device UX Completion

Needed work:

- upload LittleFS UI assets automatically during dev builds
- finish setup pages
- finish dashboard
- finish mode configuration pages
- finish system page
- expose OTA upload in the UI

### Phase 1B: Local Security Completion

Needed work:

- complete user/password setup flow
- improve local auth token/session flow
- lock down OTA path behind configured credentials
- add rate limiting and safe defaults

## Phase 2: Central Coordination Platform

Purpose:

Let devices optionally register to a central hub for inventory, grouping, remote commands, and centralized firmware rollout.

Base URL:

- `https://www.voipguru.org/rebooter/`

Suggested split:

- device API:
  - `https://www.voipguru.org/rebooter/api/`
- admin web app:
  - `https://www.voipguru.org/rebooter/app/`
- mobile app backend:
  - same API family under `/rebooter/api/`

## Central Platform Design Principles

1. Device remains local-first.
2. Central server is optional enhancement, not a requirement.
3. Local web and local API continue to work without cloud/hub access.
4. Central commands should be auditable and explicit.
5. Device should poll for instructions rather than require inbound firewall exceptions.

## Central Device Lifecycle

### Registration

Each device should be able to register with the central service using:

- device ID
- hardware model
- firmware version
- MAC address
- local IP
- current mode
- capabilities
- optional user-assigned site/group labels

Suggested identity fields:

- `device_id`
- `hardware_model`
- `firmware_version`
- `mac_address`
- `registration_token`
- `site_id`
- `group_ids`

### Heartbeat

Device periodically sends:

- online status
- current IP
- relay state
- mode
- health status
- last event summary
- watchdog incident counters
- firmware version

### Command Pull

Device polls central hub for pending commands such as:

- reboot relay
- relay on
- relay off
- apply config
- check in now
- update firmware
- restart device

The server should not require direct inbound access to the device.

## Grouping Model

Support:

- sites
- device groups
- tags
- device collections

Examples:

- `Chicago Office`
- `All Modems`
- `Branch Routers`
- `Lab Devices`

Commands should be targetable to:

- one device
- one group
- many selected devices
- one site

## OTA Update Strategy

There should be three supported update paths:

1. Local web upload
2. Central hub instructed download/update
3. Future mobile app initiated update through central coordination

### Recommended Device OTA Flow

The central server should store metadata about firmware releases:

- version
- channel
- filename
- sha256
- release notes
- minimum compatible hardware

Device receives an update instruction, downloads from a known firmware URL, verifies integrity, installs, and reports result.

Suggested channels:

- `bootstrap`
- `dev`
- `beta`
- `stable`

## Central Server Scope

## Phase 2A: Core Central Hub

Minimum server features:

- device registration
- device heartbeat
- device inventory list
- group management
- command queue
- command status tracking
- firmware release metadata
- event log ingestion

## Phase 2B: Central Admin Web App

Minimum admin UI features:

- login
- device list
- device detail page
- group management
- send command to device/group
- firmware publish screen
- firmware rollout screen
- event view

## Phase 2C: Mobile App

Mobile app should use the same central API as the web app.

Minimum mobile app features:

- login
- view devices
- view groups
- issue relay on/off/reboot
- view health
- later: launch firmware rollout

## Security Model

## Local Device

- local user/password auth
- local OTA upload requires auth
- local API mutating calls require auth
- optional HTTPS later if practical

## Central Server

- user accounts and roles
- device registration token or claim token
- per-device secret or signed API token
- TLS required
- audit log for all remote commands

## Device-Central Trust

Recommended:

- device gets provisioned with a unique registration token or enrollment key
- central hub returns a long-lived device credential after registration
- future polls use that credential

## Recommended API Shape

## Device -> Central

- `POST /rebooter/api/devices/register`
- `POST /rebooter/api/devices/heartbeat`
- `GET /rebooter/api/devices/{device_id}/commands`
- `POST /rebooter/api/devices/{device_id}/command-result`
- `POST /rebooter/api/devices/{device_id}/events`

## Admin / App -> Central

- `POST /rebooter/api/auth/login`
- `GET /rebooter/api/devices`
- `GET /rebooter/api/devices/{device_id}`
- `POST /rebooter/api/devices/{device_id}/commands`
- `POST /rebooter/api/groups`
- `POST /rebooter/api/groups/{group_id}/commands`
- `POST /rebooter/api/firmware/releases`
- `POST /rebooter/api/firmware/deployments`

## Suggested Data Model

Main entities:

- users
- sites
- devices
- groups
- group_memberships
- device_heartbeats
- device_events
- commands
- command_results
- firmware_releases
- firmware_deployments

## Build Order Recommendation

### Step 1

Finish local device experience:

- UI assets
- setup flow
- OTA from web UI
- auth

### Step 2

Add a device-side central client with feature flags:

- disabled by default
- registration endpoint configurable
- heartbeat interval configurable
- poll interval configurable

### Step 3

Build central server MVP under `/rebooter/`

Recommended MVP stack:

- backend API
- database
- simple admin web app

### Step 4

Add central firmware release management

### Step 5

Add mobile app

## Recommended Immediate Device Work

1. Make local UI fully usable after first boot.
2. Add simple local firmware upload screen.
3. Add LittleFS upload path to dev workflow.
4. Add central registration config fields to local config model:
   - server URL
   - enrollment token
   - device alias
   - site/group assignment
   - central management enabled
5. Add device heartbeat client stub.

## Recommended Immediate Central Work

1. Define central API schema.
2. Choose backend stack.
3. Choose database.
4. Stand up `/rebooter/` MVP admin page and API.
5. Implement registration and heartbeat first.

## Decisions To Lock Soon

1. Central backend stack
2. Database choice
3. Auth model for admins
4. Device enrollment model
5. Firmware release workflow
6. Whether local HTTPS is in scope for near-term device firmware

## Suggested Backend Stack

Conservative option:

- backend: Node.js + TypeScript
- DB: PostgreSQL
- admin app: same stack, server-rendered or SPA
- mobile app: React Native later

This is not mandatory, but it is a pragmatic default.

## Current Working Definition Of Success

The platform is on-track when:

1. A fresh Sonoff can be serial-flashed once with bootstrap.
2. Bootstrap installs the full firmware over Wi-Fi.
3. Full firmware joins Wi-Fi automatically in dev mode.
4. Full firmware serves a usable local web UI.
5. Full firmware can be updated over local OTA without serial.
6. Devices can later register with `https://www.voipguru.org/rebooter/`.
