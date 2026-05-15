# Rebooter Firmware

Rebooter is local-first firmware for a Sonoff S31 ESP8266 smart plug that can operate as a normal smart plug or as an automatic power-cycle watchdog for network equipment and other devices.

The initial target hardware is the Sonoff S31, flashed over a USB-TTL serial adapter using a pogo-pin or temporary serial setup. The firmware is built with PlatformIO and the Arduino ESP8266 framework.

## Device Modes

- Smart plug mode: manually switch the relay from the physical button, local API, or local web UI.
- Internet watchdog mode: monitor multiple internet targets and power-cycle the attached modem/router only when all targets fail for a configured duration.
- Device watchdog mode: monitor one LAN/IP/hostname target and power-cycle when that target stops responding for a configured duration.

## Local Interfaces

- Local web UI served from LittleFS.
- Captive portal for first setup and recovery.
- JSON API for status, relay control, event log, configuration, tests, and system actions.
- Serial monitor at 115200 baud for diagnostics.

## Design Priorities

- Fully functional without cloud services.
- Configuration stored locally with schema/version handling.
- Relay operations are serialized and deterministic.
- Monitoring uses timer-based state machines so the web UI stays responsive.
- OTA, local authentication, event logging, and factory reset are v1 requirements.

## Safety Notes

This project controls mains-powered hardware. Treat the Sonoff S31 as hazardous when opened, disconnect it from mains before flashing, and verify pin mappings on the exact board revision before energizing modified hardware.

## Current Status

This repo is beyond the original scaffold stage. The current firmware line
includes:

- local auth-protected control paths
- safe fallback and recovery work
- protected config backup/restore
- central heartbeat expansion with `reported_config`
- power telemetry configuration surfaces

## Current validated checkpoint

Current release-quality checkpoint:

- firmware: `0.1.21-dev-central-safe`
- SHA256:
  - `59D06F4F4279CC41ADA3A866B23E9CA4F64152D6399C08F109780FB91DE739F9`
- validated on live bench device `.48`

What was deeply retested on this line:

- public-vs-protected config behavior
- local UI auth/session flow
- protected relay and config-save actions from the actual device-served UI
- heartbeat preview and central diagnostics
- event-log sequencing metadata
- 3-cycle OTA stress pass without recovery-mode relapse

What is still intentionally not claimed as fully closed:

- physical button timing/behavior still needs bench-hands verification
- destructive recovery/factory-reset retest still needs an operator window
- power-sample upload still needs a live receiving-hub validation pass

The full product target is still broader than the current implementation, but
the repo is now an active working firmware codebase rather than only a starter.
