# Bootstrap OTA Plan

## Purpose

Create a minimal first-stage firmware for Sonoff S31 that is small, conservative, and easy to serial-flash once. Its only job is to get the device onto the local Wi-Fi and then install a larger firmware image from a known HTTP URL.

This avoids repeated long serial flashes over the pogo setup.

## Verified Network Inputs

Verified on this Windows PC on May 7, 2026:

- SSID: `SpectrumSetup-4D`
- WPA2 key: `smalltruck536`

These values were verified from the active Windows Wi-Fi profile on this machine.

## Stage-1 Bootstrap Requirements

The bootstrap firmware should:

1. Boot safely on Sonoff S31 / ESP8266.
2. Join the known Wi-Fi network automatically using the verified credentials.
3. Expose clear serial logging during boot and update.
4. Show simple LED status for:
   - booting
   - connecting Wi-Fi
   - downloading firmware
   - update success
   - update failure
5. Download a full firmware image from:
   - `https://www.voipguru.org/<filename>`
6. Validate the download and invoke ESP8266 OTA update from the fetched binary stream.
7. Reboot into the installed firmware when the update succeeds.

## Explicit Non-Goals For Bootstrap

Do not include these in the bootstrap image:

- captive portal
- full web UI
- watchdog logic
- relay automation
- notifications
- event log persistence beyond basic serial output
- config management beyond compile-time constants

The bootstrap image should stay as small and simple as possible.

## Proposed Bootstrap Behavior

### Boot Sequence

1. Initialize serial logging.
2. Initialize LED.
3. Keep relay unchanged unless a hardware-safe default is required.
4. Connect to Wi-Fi using hardcoded bootstrap credentials.
5. Wait for DHCP address with bounded retry timing.
6. If Wi-Fi fails, retry with backoff and keep logging status.
7. Once Wi-Fi is connected, start firmware fetch.
8. Stream the firmware image directly into `ESPhttpUpdate`.
9. Reboot automatically after successful update.

### Failure Behavior

If update fails:

1. Log the reason to serial.
2. Blink LED in a distinct failure pattern.
3. Wait a short cooldown.
4. Retry download a limited number of times.
5. If all retries fail, remain in bootstrap firmware so the device can be power-cycled and retried without pogo rewiring changes.

## Delivery URL Strategy

Use a compile-time firmware filename constant so we can swap builds without changing the bootstrap logic.

Example:

- Base URL: `https://www.voipguru.org/`
- Filename: `rebooter-firmware-main.bin`
- Final URL: `https://www.voipguru.org/rebooter-firmware-main.bin`

This should be assembled from constants rather than hardcoding one long literal everywhere.

## Security Notes

For bootstrap only:

- HTTPS should be preferred if certificate validation is practical with ESP8266 memory limits.
- If certificate pinning is too heavy for the first bootstrap iteration, an HTTP fallback can be supported only as an explicit development mode.

Because the requested host is HTTPS, the first implementation attempt should use HTTPS update flow first.

## Suggested Build Layout

Add a dedicated PlatformIO environment for the bootstrap image, separate from the main firmware:

- environment: `sonoff_s31_bootstrap`
- entry file: `src/bootstrap_main.cpp`

This keeps the bootstrap firmware isolated from the production-capable main application.

## Suggested Source Shape

Minimal bootstrap modules:

- `src/bootstrap_main.cpp`
- `include/bootstrap_config.h`

Optional if needed:

- `src/bootstrap_led.cpp`
- `include/bootstrap_led.h`

## Compile-Time Constants

The bootstrap firmware should use compile-time constants for:

- Wi-Fi SSID
- Wi-Fi password
- firmware base URL
- firmware filename
- retry count
- retry delay

## Questions Already Answered

- We do not need a full 4 MB serial write for bootstrap.
- We only need one small serial write for the bootstrap image.
- The real firmware can then be installed over Wi-Fi.
- The working Sonoff UART on this board revision is the plain `TX` / `RX` pads, not `D-TX` / `D-RX`.

## Recommended Build Order

1. Finish enough backup work to feel comfortable.
2. Add bootstrap PlatformIO environment.
3. Implement smallest Wi-Fi + OTA downloader firmware.
4. Build and record binary size.
5. Serial-flash bootstrap once.
6. Host the larger firmware at `https://www.voipguru.org/<filename>`.
7. Let bootstrap fetch and install the larger firmware.
8. Use OTA for later iterations.

## Definition Of Done

Bootstrap is complete when all of the following are true:

1. Serial flash of bootstrap succeeds once.
2. Device joins `SpectrumSetup-4D`.
3. Device downloads a firmware image from the configured `voipguru.org` URL.
4. Device installs the image and reboots successfully.
5. Follow-up firmware updates no longer require pogo-pin serial flashing.
