# Hardware Notes: Sonoff S31

## Target Device

- Device: Sonoff S31 smart plug
- MCU family: ESP8266
- Firmware build target: PlatformIO `esp12e`
- Serial monitor: 115200 baud

## Provisional Pin Mapping

Verify these against the exact board revision before first live test:

| Function | ESP8266 GPIO | Notes |
| --- | ---: | --- |
| Relay | GPIO12 | Common Sonoff S31 relay mapping in the starter scaffold. |
| Status LED | GPIO13 | Usually active-low. Scaffold writes `LOW` for on. |
| Button | GPIO0 | Usually active-low with internal pull-up. Also used for bootloader mode. |

## Serial Flashing Notes

Use a USB-TTL serial adapter at 3.3 V logic. Do not connect mains power while the device is open or connected to the programmer.

Typical ESP8266 serial connections:

| USB-TTL Adapter | Sonoff / ESP8266 |
| --- | --- |
| TX | RX |
| RX | TX |
| GND | GND |
| 3.3 V | 3.3 V, only if the adapter can safely supply enough current |

To enter bootloader mode, hold GPIO0/button low during reset or power-up, then release after the serial flashing tool connects.

## Pogo / Temporary Flash Setup

- Prefer a pogo-pin jig or secure temporary wires for repeated flashing.
- Keep contacts stable during erase/write operations.
- Label TX/RX/GND/3V3 on the jig once verified.
- Capture board photos in `docs/` before and after any hardware changes.

## Windows Flashing

PlatformIO upload once the project builds:

```powershell
pio run -e sonoff_s31 -t upload --upload-port COM3
```

LittleFS upload:

```powershell
pio run -e sonoff_s31 -t uploadfs --upload-port COM3
```

Low-level esptool flash notes from the proof-of-concept script are preserved in `docs/poc/flash.ps1`.

## Safety

The Sonoff S31 is a mains-powered device. Never flash or probe the board while connected to mains. Enclose the device before testing with line voltage, and use a safe test load during early relay validation.
