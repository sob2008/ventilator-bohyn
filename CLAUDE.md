# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project overview

Firmware + docs for "Ventilator PRO" (SOB electronic), an ESP8266-based WiFi thermostat/fan controller running on a Wemos/LOLIN D1 mini (ESP-12F). The repo:

- `ventilator-pro.ino` — the main firmware (Arduino sketch, single file, ~790 lines). Comments and log strings are in Czech.
- `manual-ventilator-pro.html` — a static, print-ready (A4) HTML user manual for the end product, also in Czech. Opens directly in a browser or is printed/exported to PDF; there is no build step for it.
- `OtaConfig.h`, `OtaState.h/.cpp`, `OtaManager.h/.cpp`, `OtaVersion.h/.cpp`, `Sha256.h/.cpp` — the OTA (over-the-air update) client, next to the main sketch. See "OTA updates" below.
- `factory-sw/` — a second, standalone sketch used only to provision new/returned units (see "OTA updates").
- `.github/workflows/release.yml`, `scripts/release.ps1` — release automation for publishing new OTA firmware versions.

There is no package manager or test suite in this repo — it's flashed directly via the Arduino IDE / arduino-cli. `OtaVersion`/`Sha256` are Arduino-independent and can be unit-tested with a plain host `g++` (see the OTA section), but no such tests are currently checked into this repo.

## Build / flash / verify

This is an Arduino sketch for ESP8266 boards, built with the Arduino IDE or `arduino-cli`. There is no CI that runs on every push (only the manual release workflow, see below), no linter, and no automated test suite in this repo.

Board: **LOLIN(WEMOS) D1 R2 & mini** in the Arduino IDE board list, FQBN `esp8266:esp8266:d1_mini`.

Required libraries (`#include`s across `ventilator-pro.ino` and the `Ota*` modules): `ESP8266WiFi`, `WiFiManager`, `ESP8266WebServer`, `ESP8266mDNS`, `NTPClient`, `WiFiUdp`, `OneWire`, `DallasTemperature`, `EEPROM` (all except `ArduinoJson` and `WiFiManager` ship with the ESP8266 core — `LittleFS`, `ESP8266HTTPClient`, `WiFiClientSecureBearSSL`, `Updater` are core-bundled too). `ArduinoJson` (v7 API) must be installed separately — it's a new dependency introduced by the OTA client.

Typical arduino-cli workflow:
```
arduino-cli compile --fqbn esp8266:esp8266:d1_mini --warnings all ventilator-pro.ino
arduino-cli upload -p <PORT> --fqbn esp8266:esp8266:d1_mini ventilator-pro.ino
```
Note: Arduino requires the sketch folder name to match the main `.ino` file name (`ventilator-pro/ventilator-pro.ino`). The current repo checkout folder may not be named `ventilator-pro` — if `arduino-cli`/the IDE complains about this, compile by opening/pointing at a folder that *is* named `ventilator-pro` (the IDE offers to do this automatically when you open the `.ino` directly). The release workflow (`.github/workflows/release.yml`) already works around this by checking out into a `ventilator-pro/` subfolder.

There's no way to unit-test the main sketch outside real hardware; changes are validated by compiling successfully and, ideally, flashing to a device.

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
- **OTA updates**: `OtaState::begin()`/`OtaManager::begin()` run before WiFi connects; `OtaManager::notifyApplicationHealthy()` is called at the end of `setup()` once WiFi + the web server are up; `OtaManager::handle()` runs every `loop()`. See "OTA updates" below for the full picture.

When editing `ventilator-pro.ino`, keep new dashboard markup/JS inside the existing `DASHBOARD_HTML` PROGMEM literal (don't introduce external files/CDNs — the device serves everything itself). The hand-built `snprintf`/`String`-concatenation JSON in the `/api/*` handlers is a separate, deliberate style choice from the `ArduinoJson`-based OTA modules (kept minimal/allocation-light for a tiny response on every 2s poll) — don't convert those handlers to `ArduinoJson` without reason, and don't hand-rewrite OTA's JSON parsing to avoid the dependency.

## OTA updates

The device self-updates from **public GitHub Releases** of this repo. Ported from the reusable, hardware-tested [`sob2008/esp-ota`](https://github.com/sob2008/esp-ota) package — copied in verbatim (`OtaVersion`, `Sha256`, `OtaState`, `OtaManager`) rather than reimplemented; see that repo's `README.md` for the full design rationale (redirect-following on every GitHub HTTP request, `setInsecure()` TLS + mandatory SHA-256 as compensating control, why `pending_validation` is persisted *before* `Update.end()`, etc.) before changing any of these files.

- **`OtaVersion.h/.cpp`**, **`Sha256.h/.cpp`** — Arduino-independent (no `Arduino.h`), numeric semver comparison and a standalone streaming SHA-256. Host-testable with plain `g++`.
- **`OtaState.h/.cpp`** — persistent state on `LittleFS` under `/ota/` (`state.json` via ArduinoJson, `candidate.bin`, `last_good.bin`), written with a temp-file-then-rename pattern so a power loss mid-write can't corrupt it.
- **`OtaManager.h/.cpp`** — orchestration: polls `GET /repos/{owner}/{repo}/releases/latest`, compares `tag_name` numerically against `FIRMWARE_VERSION`, downloads `firmware.bin` while streaming it into both the OTA flash region (`Update`) and `/ota/candidate.bin`, verifies SHA-256, then reboots. On a boot loop (`OTA_MAX_BOOT_ATTEMPTS`, default 3) it reflashes `/ota/last_good.bin` and marks the failed version so it's never retried automatically.
- **`OtaConfig.h`** — all tunables in one place, no secrets (repo must stay public — anonymous GitHub REST API). `FIRMWARE_TARGET` here (`esp8266-d1mini-ventilator-pro`) must stay identical to `factory-sw/OtaConfig.h`'s.
- **ESP8266 has no native A/B rollback** (unlike ESP32) — `Update`/`eboot` copies the new image into the currently-unused app region and swaps it in on the *next* restart, which is why `OtaState` implements its own candidate/last-good backup at the application level. Don't port ESP32 `esp_ota_*` patterns here.

**Publishing a new firmware version** (after committing normal code changes):
```
./scripts/release.ps1 -Version 1.2.0 -SketchDir .
```
This bumps `FIRMWARE_VERSION` in `OtaConfig.h`, commits, tags `v1.2.0`, and (after confirmation) pushes — which triggers `.github/workflows/release.yml` to compile the firmware, compute its SHA-256, and publish a GitHub Release with `firmware.bin` + `firmware.json` + `firmware.bin.sha256`. The workflow only ever runs on a manually pushed `vX.Y.Z` tag, never on a plain push to `main` — this is a deliberate safety property (the user always controls what actually ships to devices in the field).

**`factory-sw/`** is a separate sketch (not built by the release workflow) flashed over USB onto new or returned units *instead of* `ventilator-pro.ino`. It has its own copies of the `Ota*` modules (Arduino compiles each sketch folder independently — cross-folder `#include` doesn't work) and its own `OtaConfig.h` with `FIRMWARE_VERSION "0.0.0"` (always lower than any real release, so OTA fires immediately) and a 20s check interval. It opens a `VentilatorPRO_Tovarni` WiFi config portal, connects to the technician's/customer's WiFi, then installs the latest published release and reboots into it — `factory-sw` never runs again after that (unless someone reflashes it over USB for a repair/return). `factory-sw/!flash/` is a project-independent flashing helper (`flash.py` / `flash.sh`, ESP8266 and ESP32) — copied unmodified from `esp-ota`; drop a compiled `.bin` into `!flash/bin/` and run it.

**Known limitation**: none of this has been verified with a real compile — this environment has neither `arduino-cli` nor `g++` installed, so `OtaState`/`OtaManager` (ESP8266-core-dependent) and the port of `OtaVersion`/`Sha256` (host-testable) haven't been compiled/run here. Verify with `arduino-cli compile --fqbn esp8266:esp8266:d1_mini --warnings all ventilator-pro.ino` (and the same for `factory-sw/`) before relying on this.

## `manual-ventilator-pro.html`

Pure static HTML/CSS document (no JS) styled for A4 print (`@page` rules, `page-break-*` classes) and responsive down to mobile widths. It documents user-facing behavior (modes, thresholds, dashboard cards, troubleshooting, spec table) that should stay in sync with `ventilator-pro.ino` — e.g., default thresholds (27°C/25°C), history interval/size (2 min / 24h), health thresholds shown on the System card, and mode-priority order. When firmware behavior changes (defaults, ranges, new modes/cards), update the corresponding section of this manual too.
