# RempyRadar — Claude Context

## What this project is
A flight radar desk toy running on a Seeed Studio XIAO ESP32-S3 with a 1.28" round TFT display.
It fetches live ADS-B data and renders a sonar-style radar with a rotating sweep and aircraft pings.
Intended to be distributed to friends and made publicly available on GitHub.

## Hardware
- **MCU:** Seeed Studio XIAO ESP32-S3
- **Display:** Adafruit 1.28" 240x240 Round TFT GC9A01A (EYESPI connector)
- **IMU:** MPU-6050 (wired and initialised, not yet used in active code)
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
| `main.cpp` | `setup()` / `loop()` — minimal wiring |
| `radar.cpp` | Geometry: distance, bearing, screen projection |
| `display.cpp` | Canvas, sweep, pings, landmarks, waterways, grid, clock, wind widget, OTA popup |
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
- Large temporary buffers (airport body 128KB, waterway doc 128KB) use `malloc` → PSRAM automatically
- 24hr scheduled reboot (`esp_restart()`) mitigates long-term heap fragmentation

## FreeRTOS architecture
- `loop()` runs on core 0 — handles display only, never blocks
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
fit comfortably in a String (~10–20KB). Not used for airports or waterways.

## Display geometry (important constants)
- Canvas: 240×240, `GFXcanvas16`, allocated on PSRAM
- `CENTRE_X = 120`, `CENTRE_Y = 120`, `RADAR_R = 115`
- Three rings: outer r=115, middle r=77 (2/3), inner r=38 (1/3)
- Angle convention: 0°=north/up, clockwise positive
- Position formula: `x = cx + r*sin(θ)`, `y = cy - r*cos(θ)`
- Draw order in `displayRenderFrame()`: fillScreen → sweep trail → sweep line → radar grid (includes
  waterways + airport landmarks) → expire pings → check sweep hits → fade pings → trails →
  ping icons → ping labels → clock → wind widget → drawRGBBitmap

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
  (weathervane convention). No compass label shown.
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

## Waterway fetch (`fetchWaterways()` in fetch.cpp, `#ifdef FEATURE_WATERWAYS`)
- **Query:** `(way["waterway"="river"](bbox); way["natural"="coastline"](bbox);); out geom(bbox) qt 20;`
  `out geom(bbox)` clips geometry to the radar bbox — without this a coastal way can have thousands
  of nodes far outside the viewing area. `[maxsize:500000]` caps response to prevent server-side timeouts.
- **Flow:** direct TLS connect → HTTP/1.0 GET → skip headers → streaming byte-by-byte parser.
  No large buffer. Reads directly from `g_client` using lambdas (`rb`, `scan`, `scanLatOrEnd`, `readFloat`).
- **Chunked decoding:** detected by peeking first body byte. If not `{`, `isChunked=true` and the `rb`
  lambda handles chunk-size lines transparently.
- **Geometry parsing:** scans for `"geometry":` (then skips whitespace to `[`) rather than `"geometry":[`
  — Overpass uses pretty-printed JSON with a space after `:`.
- **`readFloat`** skips leading whitespace before the numeric value for the same reason.
- **Adaptive decimation:** when a way has more than `MAX_WATER_PTS_POLY` (40) nodes, the stored points
  are thinned in-place (keep every other) and `step` doubles. Runs until all points are read.
- **Not cached** — re-fetched every boot. `g_waterwaysFetched` flag + `g_waterwayRetries` (max 5) control retries.
- **Drawing:** `drawWaterways()` in display.cpp, called from `drawRadarGrid()`. Linear projection
  (accurate enough within 50km). Color: `color565(0, 55, 120)` (dark blue).

## Captive portal (`portalBegin()` in portal.cpp)
- Runs a blocking `for(;;)` loop — never returns to `main.cpp`'s `loop()`
- **Must check `g_restartPending` inline inside the loop** — `portalRestartPending()` is only polled
  from `main.cpp::loop()` which is unreachable during the captive portal
- After-setup settings server (`portalStartSettingsServer()`) uses the normal loop flow and
  `portalRestartPending()` / `portalConfigChanged()` correctly

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
- [x] Captive portal (`RempyRadar-Setup` AP → web form → NVS → reboot)
- [x] Post-setup settings web server (accessible at device IP)
- [x] Airport landmarks — auto-fetched via Overpass, cached in NVS, IATA labels preferred
- [x] Waterway overlay — rivers + coastline streamed from Overpass, drawn each boot; runtime toggle in settings
- [x] OTA firmware updates — nightly 3 am GitHub Releases check + manual "Check for updates" button; on-device status popup; CI publishes `.bin` on version tag push

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

## Planned / in progress

- [ ] **Orientation-aware north** — Add QMC5883L magnetometer (I2C, same D4/D5 bus). Use QMC5883L
  heading + MPU-6050 accelerometer for tilt compensation. Rotate radar so north always points to
  magnetic north.

## Data sources
- **Aircraft:** `opendata.adsb.fi` — free, no API key. `GET /api/v3/lat/{lat}/lon/{lon}/dist/{nm}`
- **Wind:** `api.open-meteo.com` — free, no API key. `GET /v1/forecast?latitude=...&longitude=...&current=wind_speed_10m,wind_direction_10m&wind_speed_unit=kn&forecast_days=1`
- **Airports + waterways:** `overpass-api.de` (OpenStreetMap Overpass API) — free, no API key
- **Geocoding:** `nominatim.openstreetmap.org` — free, no API key
- **OTA update check:** `api.github.com/repos/Tom-bass/RempyRadar/releases/latest` — free, no API key, 60 req/hr unauthenticated (once/day is fine)
