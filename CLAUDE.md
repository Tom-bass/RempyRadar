# RempyRadar — Claude Context

## What this project is
A flight radar desk toy running on a Seeed Studio XIAO ESP32-S3 with a 1.28" round TFT display.
It fetches live ADS-B data and renders a sonar-style radar with a rotating sweep and aircraft pings.
The display is compass-stabilised: it rotates so the N marker always points to magnetic north.
Intended to be distributed to friends and made publicly available on GitHub.

## Hardware
- **MCU:** Seeed Studio XIAO ESP32-S3
- **Display:** Adafruit 1.28" 240x240 Round TFT GC9A01A (EYESPI connector)
- **Magnetometer:** Adafruit MMC5603 Triple-axis Magnetometer — STEMMA QT / Qwiic (I2C, address 0x30, on D4/D5)
- **PSRAM:** 8MB OPI — critical, must be enabled or the sketch crashes

## Development environment
- **Build system:** PlatformIO (not Arduino IDE)
- **Editor:** VS Code with Remote SSH or WSL
- **Board ID:** `seeed_xiao_esp32s3`
- **PSRAM setting:** `board_build.arduino.memory_type = qio_opi` in `platformio.ini`
- **Flash via:** USB to the host running PlatformIO (Linux or WSL)
- **Two upload commands:** `upload` for firmware, `uploadfs` for the LittleFS web UI (`data/`).
  `uploadfs` is only needed when `data/` files change.

## Project structure
```
src/          C++ source files
include/      Header files
data/         LittleFS partition — captive portal / settings web UI
scripts/      PlatformIO pre-scripts (version.py injects git tag as FIRMWARE_VERSION)
partitions.csv  Custom 8MB OTA partition layout (two 3MB app slots + ~2MB LittleFS)
.github/      GitHub Actions CI — builds firmware and publishes releases on v*.*.* tags
```

| Module | Responsibility |
|--------|---------------|
| `main.cpp` | `setup()` / `loop()` — magnetometer read, heading smooth, calibration state machine |
| `radar.cpp` | Geometry: distance, bearing, screen projection (accepts `northOffset` param) |
| `display.cpp` | Canvas, sweep, pings, landmarks, grid, clock, wind widget, OTA popup |
| `fetch.cpp` | WiFi, TLS, HTTP, JSON parsing, FreeRTOS tasks, all data fetches |
| `storage.cpp` | NVS read/write via `Preferences` |
| `portal.cpp` | Captive portal (initial setup) + settings web server (post-setup) |
| `ota.cpp` | GitHub Releases version check, `HTTPUpdate` OTA flash, nightly + manual trigger |

## Credential & config philosophy
**No credentials or user-specific data exist anywhere in the codebase.**
- WiFi SSID/password and home location (lat/lon/radius) are stored in NVS only
- `include/config.h` contains only hardware pin defines and timing constants — safe to commit
- `storageLoad()` returns `configured = false` if NVS is empty; device launches captive portal

## Critical ESP32 memory constraints
- `heap_caps_malloc_extmem_enable(4096)` must be first in `setup()` — routes any `malloc(≥4096)` to PSRAM
- `StaticJsonDocument<16384> g_json` is a module-level global in `fetch.cpp` — never make it local or dynamic
- `WiFiClientSecure g_client` is a module-level global — same reason
- Internal RAM at runtime: ~134–151KB free; TLS handshake consumes ~40KB
- Large temporary buffers (airport body 128KB) use `malloc` → PSRAM automatically
- 24hr scheduled reboot (`esp_restart()`) mitigates long-term heap fragmentation

## FreeRTOS architecture
- `loop()` runs on core 0 — handles display and magnetometer reads, never blocks
- `fetchTask` runs on core 1 — WiFi, TLS, HTTP, JSON (stack: 65536 bytes)
- `watchdogTask` runs on core 1 — restarts `fetchTask` silently if it hangs >120s (stack: 4096 bytes)
- Staging plane data is mutex-protected; `fetchConsumeStagingIfReady()` is the only handoff point
- Wind data uses the same mutex: `fetchConsumeWindIfReady()` is the handoff point

## TLS / network behaviour (known quirks)
- Uses `WiFiClientSecure` with `setInsecure()` — no cert pinning
- TLS handshake can take up to 16 seconds — normal
- `opendata.adsb.fi` is behind Cloudflare and occasionally drops connections silently — retry logic handles it
- `g_client.stop()` + `setInsecure()` called before every connection attempt
- **`overpass-api.de` sends chunked transfer encoding even when the client sends `HTTP/1.0`.**
  Do not attempt to stream directly from `g_client` into ArduinoJson — `read()` returns -1 between
  TCP segments and ArduinoJson misreads that as EOF. Instead: read full body into a PSRAM malloc buffer,
  decode chunks with `decodeChunked()`, then parse from buffer.
- **Overpass JSON is pretty-printed.** Fields arrive as `"lat": -37.8` (space after `:`). Any custom
  stream scanner must skip whitespace before reading values.
- Open-Meteo also uses chunked encoding — handled by the `{` scan in `httpGet`.

## HTTP helper — `httpGet()` in fetch.cpp
Used for aircraft, wind, and geocoding fetches. Accumulates the full HTTP response in a `String`,
strips headers, strips chunked prefix by scanning for first `{`. Suitable only for responses that
fit comfortably in a String (~10–20KB). Not used for airports.

## Display geometry (important constants)
- Canvas: 240×240, `GFXcanvas16`, allocated on PSRAM
- `CENTRE_X = 120`, `CENTRE_Y = 120`, `RADAR_R = 115`
- Three rings: outer r=115, middle r=77 (2/3), inner r=38 (1/3)
- Angle convention: 0°=north/up, clockwise positive
- Position formula: `x = cx + r*sin(θ)`, `y = cy - r*cos(θ)`
- Draw order in `displayRenderFrame()`: reproject pings → fillScreen → sweep trail → sweep line →
  radar grid (includes airport landmarks) → expire pings → check sweep hits → fade pings → trails →
  ping icons → ping labels → clock → wind widget → drawRGBBitmap
- **Hardware rotation:** `tft.setRotation(3)` in `displayInit()` — 90° CCW, so the device can be
  mounted on its side (USB port facing a direction of the user's choice)
- **Blit offset:** `tft.drawRGBBitmap(-3, -3, ...)` — shifts the 240×240 canvas 3px left and 3px
  up to centre the radar circle within the physical round glass bezel

## Compass / north rotation architecture
- `g_northOffset` in `display.cpp` — the device's current compass heading (degrees). Subtracted
  from all geographic bearings before projecting to screen. When 0, north is at top (default).
- `displaySetNorthOffset(float deg)` — called every frame from `main.cpp` with the smoothed heading.
  If `g_northCalibrated` is false (no Set North done yet), `main.cpp` passes 0.
- **Ping positions stored in world space:** `Ping` struct holds `bearing` (geographic) and `distKm`.
  `x, y` are recomputed each frame via `pingScreenPos()` using the current `g_northOffset`.
- **Trail points** also store `geoBearing` + `distKm` (not screen x/y), recomputed at draw time.
- **N label** draws at `fmodf(360 - g_northOffset, 360)` — floats around the ring edge as you rotate.
- **Wind arrow direction** and **aircraft track arrows** both subtract `g_northOffset` so they point
  in the correct geographic direction on the rotated display.
- `planeToScreen()` in `radar.cpp` accepts a `northOffset` default param — used for airport landmarks.

## Magnetometer (`main.cpp`)
- **Sensor:** Adafruit MMC5603, I2C address 0x30, `mag.setDataRate(100)`, read every frame on core 0
- **Axis mapping:** The MMC5603 PCB is mounted standing on its long edge. This means the **Z axis**
  (perpendicular to the PCB face) is horizontal and rotates with the device on the table. **Y is
  vertical** (useless for compass). Heading uses `atan2(cz, cx)` — not `atan2(cy, cx)`.
- **Hard-iron calibration:** tracks running min/max of raw X and Z readings. Offsets =
  `(max + min) / 2`. Only applied once the observed span on both axes exceeds `CAL_MIN_SPAN = 15 µT`.
  **Calibration is LOCKED once established** — after a successful spin calibration completes, the
  min/max ranges are frozen. This prevents environmental magnetic anomalies (metal furniture,
  electronics) from expanding the range and drifting the heading when the device is moved around.
  Before first calibration (`g_calReady = false`), background updates are still allowed so the
  device can self-calibrate from normal use.
- **Spin calibration:** 20-second accelerated pass via web UI "Calibrate" button. Resets the range
  for a clean run. Includes outlier rejection: during the spin, readings whose field strength deviates
  more than 2.5× the current estimated radius from centre are discarded. Saves to NVS on completion.
- **Mounting-angle correction (`g_magCorrection`):** a single degree offset added to raw heading
  after hard-iron correction. Set once via web UI "Set North" — user points USB port toward north,
  taps the button. Saves `correction = -rawHeading` so that orientation becomes 0° (north).
  Persisted as `magCorr` in NVS.
- **`g_northCalibrated` flag:** loaded from NVS key `magNSet`. False until user completes Set North.
  While false, `displaySetNorthOffset(0)` — north locked to top of display.
- **`compassRotate` config flag:** even when calibrated, compass rotation can be disabled. Stored in
  NVS as `compassRotate`. Toggle shown in web UI Compass section only after Set North is done.
  Marked as WIP (⚠) in the UI — the feature is functional but experimental.
- **Heading smoothing:** exponential moving average with circular interpolation, alpha = 0.05
  (~270 ms to reach 50% of a step change at 50 fps).
- **Heading formula:** `atan2(cz, cx) * RAD_TO_DEG + g_magCorrection` where cx/cz are hard-iron
  corrected X/Z readings. North offset applied to display only when `g_northCalibrated && config.compassRotate`.

## NVS keys for magnetometer calibration
| Key | Type | Description |
|-----|------|-------------|
| `magMinX` | float | Running min of raw X field |
| `magMaxX` | float | Running max of raw X field |
| `magMinY` | float | Running min of raw Y field (tracked but not used for heading) |
| `magMaxY` | float | Running max of raw Y field (tracked but not used for heading) |
| `magMinZ` | float | Running min of raw Z field |
| `magMaxZ` | float | Running max of raw Z field |
| `magCorr` | float | Mounting-angle correction (degrees) |
| `magNSet` | bool | True once "Set North" has been completed |

## Clock widget (`drawClock()` in display.cpp)
- Arced "HH:MM" display in the SW ring gap (outer–middle, r=96)
- Arc centred at 225° (SW), spacing 10° between character centres, textSize=2
- Color: `color565(0, 60, 0)` (dark green, matches range labels)
- Characters placed individually: `setCursor(px - 6, py - 8)` centres each char on the arc point
- Timezone: POSIX TZ string stored in NVS as `config.timezone`, set via captive portal dropdown.
  Default is `"UTC0"` if no value is stored. Passed to `configTzTime()` in `setup()`.
- If `getLocalTime()` fails (NTP not yet synced), `drawClock()` returns silently — no error shown

## Wind widget (`drawWindWidget()` in display.cpp)
- Data source: Open-Meteo free API (`api.open-meteo.com`), no key required
- Fields: `wind_speed_10m` (knots), `wind_direction_10m` (degrees, meteorological FROM direction)
- Fetch frequency: on first fetch, then every `WIND_FETCH_EVERY_N_FETCHES` (=30) aircraft fetches (~10 min)
- Widget centred at 192° (SSW), sits just clockwise of the clock arc which ends at ~207°
- **Arrow:** thin line at r=90, ±6px shaft, ±4px arrowhead. Points toward the wind SOURCE direction
  (weathervane convention). Arrow direction rotates with compass heading.
- **Speed text:** arced at r=104, format `"%dkt"`. Spacing: 7° between digits, 4° between 'k' and 't'.
- Color: `color565(0, 60, 0)` — same dark green as clock
- Wind staging uses `static StaticJsonDocument<256> windFilter` and `static StaticJsonDocument<512> windJson`
  inside `fetchWindData()`. These must remain static — do not make them local non-static.

## ArduinoJson filter sizing (lesson learned)
- Filter documents must be large enough to hold all nested keys — a too-small filter silently drops fields
- Aircraft filter: `StaticJsonDocument<512>` for filter, `StaticJsonDocument<16384>` for parsed data
  (256 was too small for 10 nested fields — silently dropped alt_baro, filtering all aircraft)
- Wind filter: `StaticJsonDocument<256>` for filter, `StaticJsonDocument<512>` for parsed data
- Airport filter: `StaticJsonDocument<512>` for filter (8 nested fields including center.lat/lon)

## Arced text rendering pattern
Used by both clock and wind widget. Place each character individually on a circle:
```cpp
float startAngle = centreDeg + ((len - 1) / 2.0f) * spacing; // left-to-right reading
for (int i = 0; i < len; i++) {
    float angle = startAngle - i * spacing;
    float rad   = toRad(angle);
    int px = (int)roundf(cx + r * sinf(rad));
    int py = (int)roundf(cy - r * cosf(rad));
    canvas->setCursor(px - charHalfW, py - charHalfH);
    canvas->print(buf[i]);
}
// textSize=1: charHalfW=3, charHalfH=4
// textSize=2: charHalfW=6, charHalfH=8
```

## Aircraft filtering (fetch.cpp)
- Skip category starting with 'C' (ground service vehicles)
- Skip aircraft with no `alt_baro`, or where `alt_baro` is not an integer, or ≤ 0 (grounded)
- Skip aircraft outside `radiusKm` after lat/lon distance check
- Callsign priority: `flight` → `r` (registration) → `hex`
- Emergency squawks: 7500, 7600, 7700 → `isEmergency = true`

## Airport fetch (`fetchAirports()` in fetch.cpp)
- **Query:** `nwr["aeroway"="aerodrome"](bbox); out center;` via Overpass API
- **Flow:** direct TLS connect → HTTP/1.0 GET → skip headers → read full body into 128KB PSRAM buffer
  → `decodeChunked()` if body doesn't start with `{` → parse with `DynamicJsonDocument(16384)` + filter
- **Label priority:** IATA (`MEL`) → ICAO (`YMML`) → `ref` → `name`
- **Caching:** saved to NVS via `storageSaveAirports`; loaded at boot for instant display before first fetch
- **Fetch timing:** once per boot on first fetch cycle; re-fetched when `fetchUpdateConfig()` is called
  (i.e. when user saves new settings). `g_airportsFetched` flag controls this.
- **Limits:** `MAX_AIRPORTS = 30` (in `radar.h`); max radius 50km (enforced in portal + NVS)
- **URL encoding:** all special chars including spaces must be encoded (`' '` → `%20`). Missing space
  encoding in `out center` caused 400 Bad Request — fixed.
- **Node vs way/relation:** nodes have `lat`/`lon` directly; ways/relations have `center.lat`/`center.lon`.
  Both cases handled in parsing. Filter must include both or way/relation airports are silently skipped.

## Captive portal (`portalBegin()` in portal.cpp)
- AP name: **`"RempyRadar"`** (was `"RempyRadar-Setup"` in earlier versions)
- Runs a blocking `for(;;)` loop — never returns to `main.cpp`'s `loop()`
- **Must check `g_restartPending` inline inside the loop** — `portalRestartPending()` is only polled
  from `main.cpp::loop()` which is unreachable during the captive portal
- After-setup settings server (`portalStartSettingsServer()`) uses the normal loop flow and
  `portalRestartPending()` / `portalConfigChanged()` correctly
- **Live endpoints** (outside the main save form, use `fetch()` POST): `/set-north`,
  `/mag-calibrate`, `/set-compass-rotate`. These update state immediately without a page reload.
  `portalCompassRotatePending(bool &enabled)` — polled from `main.cpp::loop()` for the toggle.
- **Geocoding propagation:** `fetchGeocodeReady(float *lat, float *lon)` signals `main.cpp::loop()`
  when geocoding completes on core 1 so `config.homeLat/Lon` is updated without a restart.
  Airports are fetched and displayed immediately after geocoding on first boot.

## What's implemented

- [x] Radar display with sweep, pings, fade, draw order
- [x] Live ADS-B fetch every 20 seconds (`opendata.adsb.fi`)
- [x] FreeRTOS dual-core architecture + watchdog task
- [x] NVS storage layer (`storageLoad`, `storageSave`, `storageErase`)
- [x] PlatformIO project structure + GitHub Actions CI
- [x] Arced clock (NTP-synced, timezone user-configurable) — SW ring gap, 225°
- [x] Wind widget (Open-Meteo, arrow + kt label) — SSW ring gap, 192°
- [x] Altitude-based ping colour coding (4 built-in palettes + custom)
- [x] Climb/descent indicator on pings
- [x] Emergency squawk highlight (7500/7600/7700)
- [x] Captive portal (`RempyRadar` AP → web form → NVS → reboot)
- [x] Post-setup settings web server (accessible at device IP)
- [x] Airport landmarks — auto-fetched via Overpass, cached in NVS, IATA labels preferred; display updates immediately after geocoding on first boot
- [x] OTA firmware updates — nightly 3 am GitHub Releases check + manual "Check for updates" button; on-device status popup; CI publishes `.bin` on version tag push
- [x] **Compass-stabilised display** ⚠ WIP — MMC5603 magnetometer; display rotates so N always points to
  magnetic north. Locked hard-iron calibration (spin once to establish; frozen thereafter) using X+Z axes
  (PCB mounted on its side). One-time "Set North" tap via web UI. Compass rotation toggle in web UI.
  North locked to top until calibrated.
- [x] **Display 90° CCW rotation** — `setRotation(3)` + `drawRGBBitmap(-3,-3,...)` blit offset for centred bezel

## OTA update architecture

- **Partition table:** `partitions.csv` — two 3 MB OTA app slots (`ota_0` / `ota_1`) + ~2 MB LittleFS.
  First flash must be wired (USB); all subsequent updates can be over WiFi.
- **Version injection:** `scripts/version.py` pre-script runs `git describe --tags --exact-match`
  at build time and injects the result as `-DFIRMWARE_VERSION="v1.2.3"`. Non-tagged builds get `"dev"`.
- **`otaInit()`** called at top of `setup()` — calls `esp_ota_mark_app_valid_cancel_rollback()` to
  confirm the running slot is healthy.
- **`otaCheckIfDue()`** called at end of every `fetchTask` cycle:
  - **Scheduled:** at 3 am local time (once per calendar day via `g_lastCheckedDay`)
  - **Forced:** if `g_forcePending` is set by `otaTriggerCheck()` — jumps the fetch queue, runs
    before `fetchAircraft()` and other fetches so the popup appears immediately
- **Check flow:** TLS connect to `api.github.com` → parse `tag_name` + `.bin` asset URL →
  compare to `FIRMWARE_VERSION` → if newer, stream via `HTTPUpdate` + `WiFiClientSecure`
  (insecure, consistent with rest of codebase) with redirect following enabled.
- **Display feedback:** `displaySetOtaStatus(msg, durationMs)` in `display.cpp` sets a retro
  green popup rendered on top of the radar. Permanent (durationMs=0) until replaced; timed
  messages auto-clear. Written from core 1, read from core 0 — intentionally unsynchronised
  (worst case: one frame of garbled text, imperceptible at 50 fps).
- **CI release job:** triggers on `v*.*.*` tags; builds with the tag as `FIRMWARE_VERSION`;
  creates a GitHub Release with `firmware.bin` attached via `softprops/action-gh-release`.
- **Web UI:** `/ota-check` POST route calls `otaTriggerCheck()`; `/config` JSON includes
  `firmware` field so the settings page can display the running version.

## Data sources
- **Aircraft:** `opendata.adsb.fi` — free, no API key. `GET /api/v3/lat/{lat}/lon/{lon}/dist/{nm}`
- **Wind:** `api.open-meteo.com` — free, no API key. `GET /v1/forecast?latitude=...&longitude=...&current=wind_speed_10m,wind_direction_10m&wind_speed_unit=kn&forecast_days=1`
- **Airports:** `overpass-api.de` (OpenStreetMap Overpass API) — free, no API key
- **Geocoding:** `nominatim.openstreetmap.org` — free, no API key
- **OTA update check:** `api.github.com/repos/Tom-bass/RempyRadar/releases/latest` — free, no API key, 60 req/hr unauthenticated (once/day is fine)
