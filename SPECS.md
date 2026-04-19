# Firmware Design Specification

## Project Name

Router / Device Auto-Restarter Firmware

## Target Hardware

- Primary target: Sonoff S31 smart plug
- MCU: ESP8266
- Relay: onboard AC mains switching relay
- Button: onboard push button
- LED: onboard status LED
- Network: 2.4 GHz Wi-Fi
- Power switching: single controlled AC outlet

## Product Goal

Create firmware for a Wi-Fi smart plug that can operate in one of three mutually exclusive modes:

1. Standard Smart Plug Mode
2. Internet Connectivity Watchdog Mode
3. Single Device Reachability Watchdog Mode

The device must be self-contained, must not require a cloud dependency, and must expose a local web UI hosted on the device itself for setup, configuration, status, and control.

Optional external services may be used for notifications and future fleet management, but the device must remain fully functional without them.

## 1. Functional Overview

### 1.1 Core Principles

- Device must operate fully without any cloud platform.
- Device must provide a local captive portal and local web UI.
- Device must store configuration locally in persistent memory.
- Device must safely control relay power state.
- Device must support OTA firmware updates.
- Device must support factory reset and recovery behavior.
- Exactly one operating mode may be active at a time.

### 1.2 Primary Use Cases

#### Use Case A - Standard Plug

User wants to use device as a basic Wi-Fi smart outlet with manual on/off control.

#### Use Case B - Cable Modem / Router Auto-Restarter

User plugs modem/router into device. Device monitors multiple external targets. If all configured external checks fail continuously for a configurable threshold, device power cycles modem/router and waits before retesting.

#### Use Case C - Single Device Auto-Restarter

User plugs a server, PC, NAS, or other single host device into the outlet. Device monitors reachability of one configured IP/host. If target becomes unreachable for a configurable threshold, device power cycles the outlet and waits for boot stabilization before retesting.

## 2. Operating Modes

### 2.1 Mode 1 - Standard Smart Plug Mode

#### Purpose

Operate as a plain Wi-Fi switch.

#### Behavior

- Relay can be turned on/off manually through:
- local web UI
- API endpoint
- optional button behavior
- No watchdog monitoring runs.
- No automatic reboots occur.
- Event log records manual state changes.
- Optional future support for schedules is out of scope for v1.

#### Configurable Fields

Outlet default state after power loss:

- restore previous state
- always on
- always off

### 2.2 Mode 2 - Internet Connectivity Watchdog Mode

#### Purpose

Auto-restart modem/router/network edge device if internet connectivity appears down.

#### Monitoring Logic

- Device monitors between 1 and 10 configured targets.
- Default target count for first boot: 3.
- Default targets:
- `1.1.1.1`
- `8.8.8.8`
- `time.nist.gov`
- Targets may be IPv4 addresses or hostname / URL hostname portions.
- For v1, target checking method should be ICMP ping for IP or resolved hostname IP.
- Optional future support:
- DNS resolution check
- HTTP GET/HEAD check
- TCP port connect check

#### Trigger Condition

If all configured targets fail continuously for at least X seconds, outage condition is declared.

Default X = 180 seconds.

#### Recovery Action

Once outage condition is declared:

1. Turn relay off.
2. Wait Y seconds.
3. Turn relay on.
4. Enter post-reboot holdoff period for Z seconds.
5. After holdoff expires, resume monitoring.
6. If all targets still fail for trigger threshold, another cycle may occur, subject to retry limits.

#### Default Values

- X failure threshold: 180 seconds
- Y power off duration: 5 seconds
- Z post-reboot holdoff: 180 seconds
- Max cycles per incident: 3
- Max cycles per rolling hour: 6
- Cooldown after max cycles reached: 3600 seconds

#### Additional Rules

- A single successful target response resets the all-targets-failed condition.
- The device should track consecutive failure duration, not merely failure count.
- Hostname targets must be periodically re-resolved.
- A global monitoring interval should be configurable, default 5 seconds.

### 2.3 Mode 3 - Single Device Reachability Watchdog Mode

#### Purpose

Auto-restart a single endpoint device connected to the outlet when that device stops responding.

#### Monitoring Logic

- One target host only.
- Target may be IPv4 address or hostname.
- Default check method: ICMP ping.

#### Trigger Condition

If target fails continuously for at least X seconds, declare target down.

Default X = 60 seconds.

#### Recovery Action

1. Turn relay off.
2. Wait Y seconds.
3. Turn relay on.
4. Wait Z seconds before resuming checks to allow device boot time.
5. Repeat if still down, subject to configured retry limits.

#### Default Values

- X failure threshold: 60 seconds
- Y power off duration: 5 seconds
- Z post-reboot holdoff: 300 seconds
- Max cycles per incident: 3
- Max cycles per rolling hour: 6
- Cooldown after max cycles reached: 3600 seconds

#### Additional Rules

- Successful ping resets failure duration and incident state if target remains healthy for recovery-stability period.
- Recovery-stability period default: 30 seconds.

## 3. System Architecture

### 3.1 High-Level Components

Firmware should be structured into the following components:

- Hardware Abstraction Layer
- relay control
- LED control
- button input
- reset/factory reset handling
- Wi-Fi Manager
- initial AP mode
- captive portal
- station mode connection
- reconnect handling
- Configuration Manager
- schema versioning
- persistent storage
- validation
- import/export
- Monitoring Engine
- scheduling health checks
- evaluating trigger conditions
- handling mode-specific check logic
- Recovery Engine
- relay power cycle execution
- holdoff timers
- retry/cooldown enforcement
- Web UI Server
- device configuration pages
- status pages
- control endpoints
- local auth
- Notification Engine
- webhook calls
- optional Pushover support
- retry policy for outbound alerts
- Event Log Manager
- ring buffer storage
- in-memory + persisted summary
- export
- OTA Update Manager
- local firmware upload
- optional remote fetch in future
- Security Layer
- UI auth
- CSRF/basic protections
- config sanitization

## 4. Configuration Specification

### 4.1 Global Settings

```json
{
  "device_name": "string",
  "admin_username": "string",
  "admin_password_hash": "string",
  "timezone": "string",
  "current_mode": "smart_plug | internet_watchdog | device_watchdog",
  "relay_restore_behavior": "restore_previous | always_on | always_off",
  "status_led_enabled": true,
  "event_log_max_entries": 200,
  "monitor_interval_seconds": 5,
  "notification_enabled": false,
  "notification_type": "webhook | pushover",
  "notification_cooldown_seconds": 60
}
```

### 4.2 Smart Plug Mode Config

```json
{
  "manual_button_enabled": true
}
```

### 4.3 Internet Watchdog Mode Config

```json
{
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
}
```

### 4.4 Device Watchdog Mode Config

```json
{
  "target": "192.168.1.50",
  "failure_threshold_seconds": 60,
  "power_off_seconds": 5,
  "post_reboot_holdoff_seconds": 300,
  "max_cycles_per_incident": 3,
  "max_cycles_per_hour": 6,
  "cooldown_seconds": 3600,
  "recovery_stability_seconds": 30
}
```

### 4.5 Notification Config

```json
{
  "enabled": false,
  "type": "webhook",
  "webhook_url": "",
  "webhook_method": "POST",
  "webhook_headers": [],
  "webhook_auth_token": "",
  "pushover_user_key": "",
  "pushover_api_token": "",
  "send_on_trigger": true,
  "send_on_recovery": true,
  "send_on_max_cycles_reached": true,
  "send_test_notification_enabled": true
}
```

## 5. State Machine Design

### 5.1 Global Device States

- BOOTING
- AP_SETUP_MODE
- CONNECTING_WIFI
- ONLINE_IDLE
- MONITORING
- OUTAGE_DETECTED
- POWERING_OFF
- POWER_OFF_WAIT
- POWERING_ON
- POST_REBOOT_HOLDOFF
- COOLDOWN_LOCKOUT
- ERROR_STATE
- FACTORY_RESET_PENDING

### 5.2 Monitoring Substates

#### Internet Watchdog

- HEALTHY
- PARTIAL_FAILURE
- ALL_TARGETS_FAILED_TIMER_RUNNING
- TRIGGER_READY

#### Device Watchdog

- TARGET_HEALTHY
- TARGET_FAILED_TIMER_RUNNING
- TRIGGER_READY

### 5.3 Incident Lifecycle

1. Detection begins.
2. Trigger threshold reached.
3. Incident created.
4. Notification sent.
5. Recovery cycle executed.
6. Holdoff timer starts.
7. Checks resume.
8. Either target/service healthy -> incident closed, failure continues -> additional cycle, or retry limit reached -> cooldown lockout.

## 6. Health Check Design

### 6.1 Check Scheduler

- Use non-blocking scheduler / millis-based timing.
- No long blocking delays except relay timing state machine logic should also be timer-based.
- Each check should have timeout, default 2 seconds.
- Health checks should not block HTTP server responsiveness.

### 6.2 Target Resolution

For hostname targets:

- Resolve hostname to IP at startup.
- Re-resolve every `dns_refresh_seconds`.
- If resolution fails, count that target as failed.

### 6.3 Success/Failure Rules

#### Internet Watchdog

- If any target succeeds during monitoring interval, clear global outage timer and mark status as at least partially healthy.
- Only if all targets fail continuously for full threshold, trigger reboot.

#### Device Watchdog

- If target succeeds, clear target failure timer.
- If target fails continuously for threshold, trigger reboot.

### 6.4 Future Extensibility

Design interfaces so health check types can later support:

- ICMP ping
- DNS resolve
- HTTP HEAD/GET
- TCP connect
- custom vendor endpoint

## 7. Relay Control Requirements

### 7.1 Safety Rules

- Relay state changes must be serialized.
- No overlapping recovery operations.
- Power cycle action must confirm current state before toggling.
- During firmware update, relay must not be toggled except by explicit safe policy.
- After unexpected MCU reboot, relay state restore behavior must obey configured restore policy.

### 7.2 Timing Accuracy

- `power_off_seconds` resolution: 1 second minimum.
- `post_reboot_holdoff_seconds` resolution: 1 second minimum.

## 8. Local Web UI Specification

### 8.1 General Requirements

- UI hosted entirely on device.
- Responsive for mobile and desktop.
- No external JS/CSS dependencies required.
- All assets served locally from device flash.
- Basic authentication required once device is provisioned.
- Setup mode may initially be unauthenticated until admin credentials are set.

### 8.2 Required Pages

#### 8.2.1 Setup / Provisioning Page

Shown during first boot or after factory reset.

Functions:

- detect/set Wi-Fi SSID
- Wi-Fi password entry
- device name
- admin username/password
- timezone
- save and reboot

#### 8.2.2 Dashboard

Show:

- device name
- firmware version
- uptime
- Wi-Fi strength
- current relay state
- current mode
- monitoring status
- countdown timers if active
- last event
- last reboot cause
- notification status

#### 8.2.3 Mode Configuration Page

Allows choosing exactly one mode:

- Smart Plug
- Internet Watchdog
- Device Watchdog

#### 8.2.4 Smart Plug Settings Page

- manual relay control
- restore behavior
- button enable/disable

#### 8.2.5 Internet Watchdog Settings Page

- target list, 1 to 10 entries
- add/remove target
- failure threshold X
- power off duration Y
- post reboot holdoff Z
- max cycles per incident
- max cycles per hour
- cooldown seconds
- monitor interval
- test all targets button

#### 8.2.6 Device Watchdog Settings Page

- target host
- failure threshold X
- power off duration Y
- post reboot holdoff Z
- max cycles per incident
- max cycles per hour
- cooldown seconds
- monitor interval
- test target button

#### 8.2.7 Notifications Page

- enable notifications
- choose notification type
- webhook URL / token inputs
- Pushover settings
- send test notification
- event selection toggles

#### 8.2.8 Event Log Page

- timestamp
- event type
- message
- mode
- relay action
- trigger cause
- target list snapshot if relevant
- downloadable as JSON/text

#### 8.2.9 System Page

- reboot device
- check relay state
- firmware update
- export config
- import config
- factory reset

### 8.3 UX Requirements

- all forms validated client-side and server-side
- numeric ranges enforced
- confirmation modal for risky actions
- mode changes require explicit save/apply
- show warning if leaving page with unsaved changes

## 9. Local API Specification

### 9.1 Authentication

Use local session auth or HTTP basic auth for v1. CSRF protection recommended if session-based auth is used.

### 9.2 Endpoints

#### Read-only

- `GET /api/status`
- `GET /api/config`
- `GET /api/events`
- `GET /api/network`
- `GET /api/health`

#### Mutating

- `POST /api/relay/on`
- `POST /api/relay/off`
- `POST /api/relay/toggle`
- `POST /api/config/save`
- `POST /api/mode/set`
- `POST /api/test/targets`
- `POST /api/test/notification`
- `POST /api/system/reboot`
- `POST /api/system/factory-reset`
- `POST /api/system/ota`

## 10. Notification Design

### 10.1 Trigger Events

Notifications may be sent for:

- outage detected
- watchdog reboot initiated
- relay power restored
- incident resolved
- max cycles reached
- cooldown entered
- device booted
- Wi-Fi disconnected for prolonged duration
- firmware updated

### 10.2 Notification Payload

For webhook:

```json
{
  "device_name": "RouterRebooter-01",
  "event_type": "watchdog_trigger",
  "mode": "internet_watchdog",
  "timestamp": "ISO8601",
  "relay_state": "off",
  "reason": "all_targets_failed",
  "targets_failed": ["1.1.1.1", "8.8.8.8", "time.nist.gov"],
  "cycle_number": 1,
  "max_cycles_per_incident": 3
}
```

### 10.3 Retry Behavior

- notification send timeout: configurable, default 5 sec
- max retries per event: 2
- do not block recovery behavior if notification fails
- failed notification should log an event locally

## 11. Persistence and Storage

### 11.1 Requirements

- Use persistent storage for configuration.
- Use schema version number.
- Perform config migration on firmware upgrades if schema changes.
- Protect against corrupt config via checksum, fallback defaults, and last-known-good restore.

### 11.2 Event Log Storage

- Keep recent entries in ring buffer.
- Persist recent subset across reboots.
- Minimum retained entries: 200.
- Log rollover allowed.

## 12. LED and Button Behavior

### 12.1 LED

Suggested LED states:

- booting: fast blink
- AP setup mode: slow blink
- connecting Wi-Fi: medium blink
- healthy monitoring: solid or heartbeat blink
- outage detected: rapid double blink
- cooldown lockout: long pulse
- OTA update: fast continuous blink
- error state: repeating triple blink

### 12.2 Button

#### Short press

- toggle relay in Smart Plug mode
- optional no-op or manual override in watchdog modes

#### Long press 5 seconds

- reboot device

#### Long press 10 seconds

- factory reset network and config

#### Boot-hold behavior

- if held during power-up for defined time, force AP setup mode

## 13. Wi-Fi Provisioning

### 13.1 First Boot

- device boots as AP
- serves captive portal
- user connects to setup SSID
- user submits Wi-Fi credentials and admin setup
- device reboots into STA mode

### 13.2 Failure Behavior

- if Wi-Fi connection fails for N attempts or M seconds, continue retries
- optionally enable fallback AP after configurable window
- device should never silently become unrecoverable

### 13.3 Default Setup SSID

Format:

```text
Rebooter-<last3bytesMAC>
```

## 14. OTA Update Requirements

### 14.1 Supported Update Paths

- local upload through web UI
- optional authenticated API upload

### 14.2 Safety

- validate firmware image before apply
- retain rollback/failsafe strategy where practical
- log firmware version change
- do not trigger relay cycle during OTA except safe preserve-state handling

## 15. Security Requirements

### 15.1 Local Security

- set admin credentials on first setup
- password stored hashed, never plain text
- reject empty password unless explicit insecure mode is enabled for development only
- no hardcoded credentials

### 15.2 Network Security

- local HTTP acceptable for v1
- optional HTTPS out of scope unless practical
- rate limit login attempts
- sanitize all config inputs
- prevent command injection via webhook fields and hostname fields

### 15.3 Privacy

- no data leaves device unless notifications or update actions are configured by user
- no mandatory telemetry

## 16. Edge Cases and Failure Handling

### 16.1 Wi-Fi Loss

- if device loses Wi-Fi, it cannot reliably validate internet targets
- separate event should log Wi-Fi disconnect
- watchdog timers should pause or enter defined degraded mode until Wi-Fi returns
- do not instantly reboot outlet solely because device Wi-Fi dropped, unless future feature explicitly adds that option

### 16.2 DNS Failure

- if hostname targets fail due to DNS, IP targets may still succeed
- all-target logic prevents false positives if at least one IP target is reachable

### 16.3 Boot Storm Prevention

- after MCU restart, watchdog must not immediately trigger without fresh checks
- enforce minimum warm-up period before first automated action
- default warm-up: 30 seconds

### 16.4 Notification Endpoint Failure

- must not affect watchdog action execution

### 16.5 Repeated Unsuccessful Reboots

- enforce cooldown after max retry count reached
- require healthy signal before incident reset
- local dashboard should clearly display lockout reason

## 17. Default Values Summary

### 17.1 Global Defaults

- mode: smart_plug
- relay restore: restore_previous
- monitor interval: 5 sec
- event log max: 200

### 17.2 Internet Watchdog Defaults

- targets:
- `1.1.1.1`
- `8.8.8.8`
- `time.nist.gov`
- X failure threshold: 180 sec
- Y power off: 5 sec
- Z holdoff: 180 sec
- max incident cycles: 3
- max hourly cycles: 6
- cooldown: 3600 sec

### 17.3 Device Watchdog Defaults

- target: none until user configures
- X failure threshold: 60 sec
- Y power off: 5 sec
- Z holdoff: 300 sec
- max incident cycles: 3
- max hourly cycles: 6
- cooldown: 3600 sec

## 18. Non-Functional Requirements

### 18.1 Performance

- UI should remain responsive during monitoring.
- Watchdog checks must use non-blocking operations or bounded timeouts.
- Memory usage must fit ESP8266 constraints.
- Firmware must remain stable over long uptime.

### 18.2 Reliability

- must survive power loss
- must recover from transient Wi-Fi loss
- config corruption must not brick device
- relay control must remain deterministic

### 18.3 Maintainability

- modular code layout
- configuration schema versioning
- separation of UI, monitoring, and hardware logic
- log messages structured and consistent

## 19. Recommended Code Modules

Suggested source layout:

```text
/src
  main.cpp
  config_manager.cpp
  config_manager.h
  wifi_manager.cpp
  wifi_manager.h
  relay_controller.cpp
  relay_controller.h
  led_manager.cpp
  led_manager.h
  button_handler.cpp
  button_handler.h
  monitor_engine.cpp
  monitor_engine.h
  internet_watchdog.cpp
  internet_watchdog.h
  device_watchdog.cpp
  device_watchdog.h
  notification_manager.cpp
  notification_manager.h
  event_log.cpp
  event_log.h
  web_server.cpp
  web_server.h
  ota_manager.cpp
  ota_manager.h
  auth_manager.cpp
  auth_manager.h
  models/
  storage/
  ui/
```

## 20. Acceptance Criteria for v1

The firmware is considered acceptable for first release when all of the following are true:

- Device can be provisioned from factory reset through captive portal.
- Device can join Wi-Fi and serve local authenticated web UI.
- Device can manually toggle relay in Smart Plug mode.
- Internet Watchdog mode supports at least 3 targets.
- Internet Watchdog mode supports up to 10 targets.
- Internet Watchdog mode reboots only when all targets fail for configured threshold.
- Internet Watchdog mode respects retry limits and cooldown.
- Device Watchdog mode monitors one target.
- Device Watchdog mode power cycles according to X/Y/Z values.
- Device Watchdog mode respects retry limits and cooldown.
- Notification test and trigger notifications work.
- Event log records all major actions and trigger reasons.
- Settings persist across reboot/power loss.
- OTA update works from local upload.
- Factory reset returns device to provisioning state.

## 21. Suggested Future Enhancements

These are explicitly out of scope for v1 but should be kept in mind during design:

- HTTPS local UI
- remote fleet dashboard
- mobile app wrapper
- MQTT integration
- SNMP integration
- schedules/timers in Smart Plug mode
- multi-outlet hardware
- more check types:
- DNS-only
- HTTP status code
- TCP port
- advanced notification channels
- firmware rollback
- per-target weighting
- auto-discovery and device claiming flow

## 22. Developer Notes

### 22.1 Recommended Firmware Stack

For a custom build on Sonoff S31:

- Arduino framework for ESP8266 or ESP-IDF equivalent if practical
- Async web server if stable enough for chosen stack
- Wi-Fi manager / captive portal component
- JSON config storage
- non-blocking state machine architecture

### 22.2 Important Product Philosophy

This firmware should be designed as a local-first appliance, not a cloud-dependent IoT toy. That is one of its biggest differentiators.
