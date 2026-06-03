# RempyRadar — Flight Radar Display

A sonar-style flight radar desk toy. Fetches live ADS-B data and renders aircraft as fading pings
on a rotating radar sweep, with nearby airports, a live clock, wind widget, and a compass-stabilised
display that always shows magnetic north correctly.

---

## Hardware

| Component | Part |
|-----------|------|
| MCU | Seeed Studio XIAO ESP32-S3 (8MB OPI PSRAM — required) |
| Display | Adafruit 1.28" 240×240 Round TFT GC9A01A (EYESPI connector) |
| Magnetometer | Adafruit MMC5603 Triple-axis Magnetometer (STEMMA QT / Qwiic) |

## Wiring

| Signal | XIAO Pin |
|--------|----------|
| TFT CS | D3 |
| TFT DC | D1 |
| TFT RST | D0 |
| TFT SCK | D8 |
| TFT MOSI | D10 |
| MMC5603 SDA | D4 |
| MMC5603 SCL | D5 |

The MMC5603 uses 3.3V and has a fixed I2C address of `0x30`. With a STEMMA QT cable it's a
straight 4-wire connection (VIN → 3V3, GND, SDA → D4, SCL → D5).

---

## Build & Flash

**Build system: PlatformIO** (not Arduino IDE)

1. Install [PlatformIO](https://platformio.org/) (VS Code extension or CLI)
2. Open the project folder
3. Build: `pio run`
4. Flash firmware: `pio run --target upload`
5. Flash setup UI: `pio run --target uploadfs`

Both steps are required on a fresh device. The setup web page lives in a separate LittleFS
partition — if `uploadfs` is skipped the captive portal serves a 503 error.

**When to re-run `uploadfs`:** only when files in `data/` change (web UI). Firmware-only changes
just need `upload`.

### OTA partition layout

The project uses a custom `partitions.csv` with two equal 3 MB app slots for OTA updates.
A fresh USB flash writes the new partition table automatically. After that, firmware updates
are delivered over WiFi — no cable needed.

PSRAM is enabled via `platformio.ini`:
```ini
board_build.arduino.memory_type = qio_opi
```
Do not remove this — without it the sketch crashes after a few fetches.

### Dependencies (declared in platformio.ini)
- Adafruit GC9A01A
- Adafruit GFX Library
- Adafruit MMC56x3
- Adafruit Unified Sensor
- ArduinoJson

---

## Configuration

On first boot with no saved config the device starts a WiFi access point named `RempyRadar-Setup`.
Connect to it from any device — the captive portal fires automatically — and fill in the web form.
Config is saved to NVS and the device reboots into radar mode.

Settings available via the portal (also reachable at the device IP after setup):

| Field | Description |
|-------|-------------|
| WiFi SSID / password | Network to connect to |
| Location | Home lat/lon, or a street address geocoded on first connect |
| Radar radius | 1–50 km (default 25 km) |
| Timezone | POSIX timezone string, picked from a dropdown |
| Show wind widget | Arrow + speed in knots |
| Show clock | NTP-synced local time |
| Show trails | Fading path behind each aircraft |
| Altitude colours | Colour-code aircraft by altitude |
| Show airports | Draw nearby airport markers |
| Altitude palette | Classic Rainbow / Fire / Ocean / Monochrome / Custom |

---

## Compass Calibration

The display is compass-stabilised: the N marker always points to magnetic north and the radar
rotates to match. This requires a one-time setup.

**Before calibration:** north is locked to the top of the display (safe default).

### Step 1 — Hard-iron calibration (optional but recommended)

Open the device IP in a browser → **Compass** section → tap **Calibrate**.
Slowly rotate the device flat through a full 360° in 20 seconds.
This removes the constant magnetic bias from nearby PCB components.

The device also runs **continuous background calibration** automatically — the longer it's used,
the more accurate it becomes, with no user action needed.

### Step 2 — Set North (required once)

1. Use a phone compass to find north
2. Point the **USB port of the XIAO toward north**
3. Open the device IP in a browser → **Compass** section → tap **Set North**

That's it. The reference is saved to NVS and survives every reboot. Point the device at a plane
in the sky and it will appear at the corresponding position on the radar.

---

## OTA Updates

Firmware updates are delivered automatically over WiFi — no USB cable required after the first flash.

- **Nightly check** — at 3 am local time the device queries the GitHub Releases API. If a release
  tag newer than the running firmware is found it downloads and flashes the `.bin`, then reboots.
- **Manual check** — a **Check for updates** button in the settings page triggers the same check
  immediately. Status is shown on the device display in a retro green popup.
- **Releasing a new version** — push a git tag matching `v*.*.*`. GitHub Actions builds the
  firmware with that version string baked in and attaches `firmware.bin` to the release.
  Devices in the field pick it up that night.

---

## What It Does

- **Compass-stabilised radar** — the display rotates so magnetic north is always correctly
  indicated. Point the device at a plane and it appears at the top of the radar.
- **Radar sweep** — continuous rotating sweep
- **Aircraft pings** — aircraft appear as labelled icons when the sweep crosses them; hold full
  brightness briefly then fade. Colour indicates altitude tier. Climb / descent indicators.
  Emergency squawk highlights (7500 / 7600 / 7700).
- **Aircraft trails** — fading path behind each aircraft
- **Nearby airports** — auto-discovered via OpenStreetMap Overpass on first boot; up to 30 shown,
  labelled with IATA code where available (MEL, MBW…). Cached in NVS.
- **Clock** — current local time arced in the SW ring gap, NTP-synced to configured timezone
- **Wind widget** — wind direction arrow + speed in knots sourced from Open-Meteo, updated every
  ~10 minutes. Weathervane convention (arrow points toward wind source). Rotates with compass.
- **Double-buffered display** — full frame rendered to a PSRAM canvas each loop, pushed in a
  single `drawRGBBitmap()` call — no flicker

---

## Architecture

- `loop()` on core 0 — display and magnetometer reads, never blocks
- `fetchTask` on core 1 — WiFi, TLS, HTTP, JSON parsing every 20 seconds
- `watchdogTask` on core 1 — silently restarts `fetchTask` if it hangs >120 seconds
- 24-hour automatic reboot to clear long-term heap fragmentation
- Plane data and wind data exchanged via mutex-protected staging buffers
- Airports fetched once per boot; re-fetched when config changes

### Data sources

| Data | Source | API key |
|------|--------|---------|
| Aircraft | `opendata.adsb.fi` | None |
| Wind | `api.open-meteo.com` | None |
| Airports | `overpass-api.de` (OpenStreetMap) | None |
| Geocoding | `nominatim.openstreetmap.org` | None |
| Time | `pool.ntp.org` NTP | None |
| OTA update check | `api.github.com` (GitHub Releases) | None |

---

## Known Behaviour

- TLS handshake can take up to 16 seconds — normal for ESP32 with mbedTLS
- `opendata.adsb.fi` occasionally drops connections silently (Cloudflare CDN) — retry logic handles it
- Heap drops ~40 KB during TLS and isn't fully returned — mitigated by the 24-hour reboot
- Clock shows nothing until NTP sync completes (typically within the first fetch cycle)
- Wind shows nothing until the first successful Open-Meteo fetch
- Airports appear ~30 seconds after boot (first Overpass fetch); cached airports from a previous
  boot at the same location appear instantly
- Compass north is locked to display top until "Set North" calibration is performed
