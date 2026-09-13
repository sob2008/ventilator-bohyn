# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project overview

Firmware + docs for "Ventilator PRO" (SOB electronic), an ESP8266-based WiFi thermostat/fan controller. The repo has two files:

- `ventilator-pro.ino` — the entire firmware (Arduino sketch, single file, ~770 lines). Comments and log strings are in Czech.
- `manual-ventilator-pro.html` — a static, print-ready (A4) HTML user manual for the end product, also in Czech. Opens directly in a browser or is printed/exported to PDF; there is no build step for it.

There is no package manager, build config, or test suite in this repo — it's flashed directly via the Arduino IDE / arduino-cli.

## Build / flash / verify

This is an Arduino sketch for ESP8266 boards, built with the Arduino IDE or `arduino-cli`. There is no CI, linter, or automated test suite configured in this repo.

Required libraries (all referenced via `#include` at the top of `ventilator-pro.ino`): `ESP8266WiFi`, `WiFiManager`, `ESP8266WebServer`, `ESP8266mDNS`, `NTPClient`, `WiFiUdp`, `OneWire`, `DallasTemperature`, `EEPROM`.

Typical arduino-cli workflow (adjust FQBN/port for the actual board):
```
arduino-cli compile --fqbn esp8266:esp8266:nodemcuv2 ventilator-pro.ino
arduino-cli upload -p <PORT> --fqbn esp8266:esp8266:nodemcuv2 ventilator-pro.ino
```
There's no way to unit-test this code outside real hardware; changes are validated by compiling successfully and, ideally, flashing to a device.

## Architecture (`ventilator-pro.ino`)

Single-file Arduino sketch structured as sequential sections (see the `// ---------- X ----------` comment banners):

- **Pin map / hardware**: relay on D1, mode switch on D2, DS18B20 (OneWire temp sensor, needs external 4.7k pull-up) on D4, manual fan switch on D5, CPU thermistor on A0 (NTC 10k + 10k series resistor, Steinhart-Hart approximation).
- **Modes** (`enum Mode`): `MODE_AUTO` (thermostat controls fan by threshold), `MODE_MANUAL` (fan toggled from the web UI), `MODE_HAND` (physical switch overrides everything). Priority is strictly `MODE_HAND > MODE_MANUAL > MODE_AUTO` — when the hand switch is engaged, web API mode/fan changes are rejected (see `handleApiMode`/`handleApiFan`).
- **Debounced switches**: `debounce()` is a generic helper used for both the mode switch and the manual fan switch (50 ms debounce), polled every `loop()` via `readSwitches()`.
- **Non-blocking temperature reads**: DS18B20 conversion (750 ms for 12-bit resolution) is kicked off and polled without `delay()`, driven by `tempRequested`/`tempRequestTime` state machine inside `loop()`, on a 2-second cadence. After `SENSOR_REINIT_THRESHOLD` (5) consecutive sensor errors, the OneWire bus is reinitialized.
- **Threshold persistence**: `thresholdOn`/`thresholdOff` (fan on/off setpoints) are stored in EEPROM with a magic-byte guard (`eepromLoadSettings`/`eepromSaveSettings`), validated on load (range 0–80°C, off < on), falling back to defaults (27.0/25.0) otherwise.
- **24h temperature history**: `histBuf` is a circular buffer of `HIST_SIZE` (720) `int16_t` values (temp × 10, sentinel `HIST_NO_DATA` = -9990), recorded every `HIST_INTERVAL` (2 min) in `histRecord()`. This lives only in RAM and is lost on reboot (documented as such in the manual).
- **Web server** (`ESP8266WebServer` on port 80, also exposed via mDNS as `ventilator.local`):
  - `GET /` → `handleDashboard()` serves a self-contained dashboard (`DASHBOARD_HTML`, a `PROGMEM` string literal embedding HTML/CSS/vanilla JS — no external assets, no build step, no framework). The JS polls `/api/status` every 2s and `/api/history` every 30s and draws the temperature chart on a `<canvas>` by hand (no chart library).
  - `GET /api/status` → current mode/fan/temperature/thresholds/CPU health/uptime as JSON, built with `snprintf` into a fixed buffer.
  - `GET /api/mode?m=auto|manual`, `GET /api/fan?state=on|off`, `GET /api/settings?on=..&off=..` → mutate state; all reject changes while `MODE_HAND` is active or fan isn't in manual mode, and validate threshold ranges before saving to EEPROM.
  - `GET /api/history` → streams the circular buffer as chunked JSON (`CONTENT_LENGTH_UNKNOWN` + `server.sendContent` in ~300-byte batches) to avoid building one large string in heap.
- **System self-monitoring**: tracks min free heap (`trackHeap`), heap fragmentation, WiFi RSSI, CPU frequency, and CPU temperature (via the A0 thermistor), all surfaced in `/api/status` and shown on the dashboard's "System" card as health thresholds (documented in the manual, section 9).
- **WiFi provisioning**: `WiFiManager` auto-connect with a 3-minute config portal timeout; on failure the device restarts (`ESP.restart()`).

When editing `ventilator-pro.ino`, keep new dashboard markup/JS inside the existing `DASHBOARD_HTML` PROGMEM literal (don't introduce external files/CDNs — the device serves everything itself), and keep JSON responses hand-built with `snprintf`/`String` concatenation consistent with the existing handlers rather than pulling in a JSON library, since the project has no dependency-management step beyond the Arduino library list above.

## `manual-ventilator-pro.html`

Pure static HTML/CSS document (no JS) styled for A4 print (`@page` rules, `page-break-*` classes) and responsive down to mobile widths. It documents user-facing behavior (modes, thresholds, dashboard cards, troubleshooting, spec table) that should stay in sync with `ventilator-pro.ino` — e.g., default thresholds (27°C/25°C), history interval/size (2 min / 24h), health thresholds shown on the System card, and mode-priority order. When firmware behavior changes (defaults, ranges, new modes/cards), update the corresponding section of this manual too.
