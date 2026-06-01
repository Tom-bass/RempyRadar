#include "fetch.h"
#include "config.h"
#include "radar.h"
#include "storage.h"
#include "display.h"
#include "ota.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <math.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Module-level state
// ---------------------------------------------------------------------------

static DeviceConfig g_cfg;

// Staging buffer — written by fetchTask, consumed by main via fetchConsumeStagingIfReady()
static Plane             g_staging[MAX_PLANES];
static int               g_stagingCount  = 0;
static volatile bool     g_stagingReady  = false;
static SemaphoreHandle_t g_mutex         = nullptr;

// Wind staging — updated every WIND_FETCH_EVERY_N_FETCHES iterations
static WindData          g_windStaging   = { 0.0f, 0, false };
static volatile bool     g_windReady     = false;

// Task state
static TaskHandle_t      g_fetchHandle      = nullptr;
static volatile unsigned long g_lastFetch   = 0;
static int               g_fetchCount       = 0;
static int               g_geocodeFailCount = 0;
static volatile bool     g_geocodeFailed    = false;
static bool              g_airportsFetched  = false;

// Network objects — kept global to avoid heap fragmentation
static WiFiClientSecure      g_client;
static StaticJsonDocument<16384> g_json;

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

static bool wifiEnsureConnected() {
    if (WiFi.status() == WL_CONNECTED) return true;
    Serial.println("LOG: WiFi reconnecting...");
    WiFi.begin(g_cfg.wifiSSID, g_cfg.wifiPass);
    for (int i = 0; i < 20 && WiFi.status() != WL_CONNECTED; i++) {
        vTaskDelay(500 / portTICK_PERIOD_MS);
    }
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("LOG: WiFi reconnect failed");
        return false;
    }
    Serial.println("LOG: WiFi reconnected");
    return true;
}

// ---------------------------------------------------------------------------
// HTTP helper — shared by aircraft and wind fetches
// ---------------------------------------------------------------------------

static bool httpGet(const char *host, const String &path, String &outPayload,
                    int firstByteWaitSecs = 8) {
    g_client.stop();
    g_client.setInsecure();

    unsigned long t0 = millis();
    bool connected = g_client.connect(host, 443, 10000);
    Serial.printf("LOG: TLS %s to %s | %dms | Heap: %d\n",
        connected ? "OK" : "FAIL", host, (int)(millis() - t0), ESP.getFreeHeap());
    if (!connected) return false;

    g_client.println("GET " + path + " HTTP/1.1");
    g_client.println(String("Host: ") + host);
    g_client.println("User-Agent: RempyRadar/1.0");
    g_client.println("Accept-Encoding: identity");
    g_client.println("Connection: close");
    g_client.println();
    g_client.flush();

    int waited = 0;
    int maxWait = firstByteWaitSecs * 100;  // 100 iterations per second (10ms each)
    while (!g_client.available() && waited < maxWait) {
        vTaskDelay(10 / portTICK_PERIOD_MS);
        waited++;
    }
    if (waited >= maxWait) {
        Serial.println("LOG: Response timeout");
        g_client.stop();
        return false;
    }

    String response = "";
    unsigned long deadline = millis() + 10000;
    while (millis() < deadline) {
        while (g_client.available()) {
            response += (char)g_client.read();
            deadline  = millis() + 3000;
        }
        if (!g_client.connected() && response.length() > 0) break;
        vTaskDelay(1);
    }
    g_client.stop();

    int headerEnd = response.indexOf("\r\n\r\n");
    if (headerEnd == -1) {
        Serial.printf("LOG: httpGet %s: no header end (len=%d)\n", host, response.length());
        if (response.length() > 0)
            Serial.println("LOG: Response head: " + response.substring(0, min((int)response.length(), 120)));
        response = "";
        return false;
    }
    outPayload = response.substring(headerEnd + 4);
    response   = "";

    // Strip chunked encoding prefix (hex size line before JSON body)
    int jsonStart = outPayload.indexOf('{');
    if (jsonStart < 0) {
        Serial.printf("LOG: httpGet %s: no JSON in body\n", host);
        outPayload = "";
        return false;
    }
    if (jsonStart > 0)  outPayload = outPayload.substring(jsonStart);

    return true;
}

// Decode HTTP chunked transfer encoding in-place. Returns new length.
// Only call when body[0] is a hex digit (i.e. body starts with a chunk-size line).
static int decodeChunked(char *buf, int len) {
    int r = 0, w = 0;
    while (r < len) {
        while (r < len && (buf[r] == '\r' || buf[r] == '\n')) r++;
        if (r >= len) break;
        int sz = 0; bool ok = false;
        while (r < len && buf[r] != '\r' && buf[r] != '\n' && buf[r] != ';') {
            char c = buf[r++];
            int d = (c>='0'&&c<='9') ? c-'0' : (c>='a'&&c<='f') ? c-'a'+10 : (c>='A'&&c<='F') ? c-'A'+10 : -1;
            if (d >= 0) { sz = sz*16 + d; ok = true; }
        }
        while (r < len && buf[r] != '\n') r++;
        if (r < len) r++;
        if (!ok || sz == 0) break;
        int n = (sz < len - r) ? sz : len - r;
        if (w != r) memmove(buf + w, buf + r, n);
        w += n; r += sz;
    }
    if (w < len) buf[w] = '\0';
    return w;
}

// ---------------------------------------------------------------------------
// Wind fetch helper — Open-Meteo free API, no key required
// ---------------------------------------------------------------------------

static void fetchWindData() {
    Serial.println("LOG: Wind fetch starting");

    String path = "/v1/forecast?latitude=" + String(g_cfg.homeLat, 4) +
                  "&longitude=" + String(g_cfg.homeLon, 4) +
                  "&current=wind_speed_10m,wind_direction_10m" +
                  "&wind_speed_unit=kn&forecast_days=1";

    String payload;
    if (!httpGet("api.open-meteo.com", path, payload)) {
        Serial.println("LOG: Wind fetch failed");
        return;
    }

    static StaticJsonDocument<256> windFilter;
    windFilter.clear();
    JsonObject filterCur = windFilter.createNestedObject("current");
    filterCur["wind_speed_10m"]     = true;
    filterCur["wind_direction_10m"] = true;

    static StaticJsonDocument<512> windJson;
    windJson.clear();
    DeserializationError err = deserializeJson(windJson, payload,
                                               DeserializationOption::Filter(windFilter));
    payload = "";

    if (err) {
        Serial.printf("LOG: Wind JSON error: %s\n", err.c_str());
        return;
    }

    float speed = windJson["current"]["wind_speed_10m"]     | -1.0f;
    int   dir   = windJson["current"]["wind_direction_10m"] | -1;

    if (speed < 0.0f || dir < 0) {
        Serial.println("LOG: Wind fetch: missing fields");
        return;
    }

    if (xSemaphoreTake(g_mutex, portMAX_DELAY)) {
        g_windStaging.speedKt = speed;
        g_windStaging.dirDeg  = dir;
        g_windStaging.valid   = true;
        g_windReady           = true;
        xSemaphoreGive(g_mutex);
    }
    Serial.printf("LOG: Wind: %.1fkt from %d°\n", speed, dir);
}

// ---------------------------------------------------------------------------
// Geocoding — Nominatim (OpenStreetMap), HTTPS, no key required
// ---------------------------------------------------------------------------

static bool fetchGeocode() {
    Serial.printf("LOG: Geocoding: %s\n", g_cfg.homeAddress);

    // URL-encode the address
    String enc;
    for (int i = 0; g_cfg.homeAddress[i]; i++) {
        char c = g_cfg.homeAddress[i];
        if      (c == ' ')  enc += '+';
        else if (c == ',')  enc += "%2C";
        else if (c == '\'') enc += "%27";
        else if (c == '&')  enc += "%26";
        else if (c == '#')  enc += "%23";
        else                enc += c;
    }

    String path = "/search?q=" + enc + "&format=json&limit=1";
    enc = "";

    // httpGet strips the leading [ from the array and starts at the first {
    // so we get a JSON object directly — ArduinoJson ignores the trailing ]
    String payload;
    if (!httpGet("nominatim.openstreetmap.org", path, payload)) {
        Serial.println("LOG: Geocode: request failed");
        return false;
    }

    static StaticJsonDocument<64> filter;
    filter.clear();
    filter["lat"] = true;
    filter["lon"] = true;

    static StaticJsonDocument<128> doc;
    doc.clear();
    DeserializationError err = deserializeJson(doc, payload,
                                               DeserializationOption::Filter(filter));
    payload = "";

    if (err || doc["lat"].isNull() || doc["lon"].isNull()) {
        Serial.println("LOG: Geocode: no result");
        return false;
    }

    g_cfg.homeLat = atof(doc["lat"].as<const char *>());
    g_cfg.homeLon = atof(doc["lon"].as<const char *>());
    storageSave(g_cfg);  // persist resolved coordinates

    Serial.printf("LOG: Geocode: resolved %.4f, %.4f\n", g_cfg.homeLat, g_cfg.homeLon);
    return true;
}

// ---------------------------------------------------------------------------
// Airport fetch — Overpass API via HTTPS (reuses g_client TLS path)
// ---------------------------------------------------------------------------

static bool fetchAirports() {
    Serial.println("LOG: Airport fetch starting");

    float lat = g_cfg.homeLat, lon = g_cfg.homeLon, km = g_cfg.radiusKm;
    float dLat = km / 111.0f;
    float dLon = km / (111.0f * cosf(lat * 3.14159265f / 180.0f));
    float S = lat - dLat, N = lat + dLat;
    float W = lon - dLon, E = lon + dLon;

    // nwr = nodes+ways+relations; out center gives a calculated centre for polygons.
    String q = "[out:json][timeout:15];nwr[\"aeroway\"=\"aerodrome\"](" +
               String(S, 4) + "," + String(W, 4) + "," +
               String(N, 4) + "," + String(E, 4) + ");out center;";

    String enc;
    for (int i = 0; i < (int)q.length(); i++) {
        char c = q[i];
        if      (c == '[')  enc += "%5B";
        else if (c == ']')  enc += "%5D";
        else if (c == '"')  enc += "%22";
        else if (c == '=')  enc += "%3D";
        else if (c == ';')  enc += "%3B";
        else if (c == '(')  enc += "%28";
        else if (c == ')')  enc += "%29";
        else if (c == ' ')  enc += "%20";
        else                enc += c;
    }
    q = "";

    // Connect directly (not via httpGet) — avoids String buffer issues and gives
    // streaming JSON parse, which also fixes the filter-too-small silent-drop problem.
    g_client.stop();
    g_client.setInsecure();

    unsigned long t0 = millis();
    bool connected = g_client.connect("overpass-api.de", 443, 15000);
    Serial.printf("LOG: TLS %s to overpass-api.de | %dms | Heap: %d\n",
        connected ? "OK" : "FAIL", (int)(millis() - t0), ESP.getFreeHeap());
    if (!connected) {
        Serial.println("LOG: Airport fetch: TLS failed");
        return false;
    }

    // HTTP/1.0 prevents chunked transfer encoding so the body is plain JSON
    g_client.println("GET /api/interpreter?data=" + enc + " HTTP/1.0");
    g_client.println("Host: overpass-api.de");
    g_client.println("User-Agent: RempyRadar/1.0");
    g_client.println("Accept-Encoding: identity");
    g_client.println("Connection: close");
    g_client.println();
    g_client.flush();
    enc = "";

    // Wait up to 15s for first byte
    int waited = 0;
    while (!g_client.available() && waited < 1500) {
        vTaskDelay(10 / portTICK_PERIOD_MS);
        waited++;
    }
    if (waited >= 1500) {
        Serial.println("LOG: Airport fetch: response timeout");
        g_client.stop();
        return false;
    }

    // Skip HTTP headers (read until \r\n\r\n)
    int matchLen = 0;
    unsigned long deadline = millis() + 10000;
    bool headersEnd = false;
    while (millis() < deadline && !headersEnd) {
        while (g_client.available() && !headersEnd) {
            char c = g_client.read();
            deadline = millis() + 3000;
            switch (matchLen) {
                case 0: matchLen = (c == '\r') ? 1 : 0; break;
                case 1: matchLen = (c == '\n') ? 2 : (c == '\r' ? 1 : 0); break;
                case 2: matchLen = (c == '\r') ? 3 : 0; break;
                case 3:
                    if (c == '\n') headersEnd = true;
                    else matchLen = (c == '\r') ? 1 : 0;
                    break;
            }
        }
        if (!g_client.connected()) break;
        vTaskDelay(1);
    }
    if (!headersEnd) {
        Serial.println("LOG: Airport fetch: no header end");
        g_client.stop();
        return false;
    }

    // 512 bytes is needed for 8 nested filter fields — 256 silently drops center.lat/lon,
    // causing way/relation airports (YMML etc.) to be skipped entirely.
    StaticJsonDocument<512> filter;
    filter["elements"][0]["lat"]           = true;
    filter["elements"][0]["lon"]           = true;
    filter["elements"][0]["center"]["lat"] = true;
    filter["elements"][0]["center"]["lon"] = true;
    filter["elements"][0]["tags"]["icao"]  = true;
    filter["elements"][0]["tags"]["iata"]  = true;
    filter["elements"][0]["tags"]["ref"]   = true;
    filter["elements"][0]["tags"]["name"]  = true;

    // Buffer body in PSRAM then parse — streaming from WiFiClientSecure is unreliable
    // (read() returns -1 between TCP segments) and overpass-api.de sends chunked
    // transfer encoding even for HTTP/1.0 requests, which corrupts streaming JSON parse.
    static const int AP_BUF = 131072;  // 128KB in PSRAM; 32KB overflowed at 100km radius
    char *body = (char*)malloc(AP_BUF);
    if (!body) { g_client.stop(); Serial.println("LOG: Airport fetch: malloc failed"); return false; }
    int bodyLen = 0;
    unsigned long rdl = millis() + 10000;
    while (millis() < rdl) {
        while (g_client.available() && bodyLen < AP_BUF - 1) { body[bodyLen++] = (char)g_client.read(); rdl = millis() + 3000; }
        if (!g_client.connected() && bodyLen > 0) break;
        vTaskDelay(1);
    }
    g_client.stop();
    body[bodyLen] = '\0';
    Serial.printf("LOG: Airport fetch: body %d bytes\n", bodyLen);

    // Decode chunked encoding if present (body starts with a hex size line, not '{')
    char *data = body;
    int   dataLen = bodyLen;
    while (dataLen > 0 && (data[0] == '\r' || data[0] == '\n')) { data++; dataLen--; }
    if (dataLen > 0 && data[0] != '{') dataLen = decodeChunked(data, dataLen);

    // Log a preview so we can see exactly what the server returned
    int prevLen = dataLen < 120 ? dataLen : 120;
    char preview[121]; memcpy(preview, data, prevLen); preview[prevLen] = '\0';
    Serial.printf("LOG: Airport fetch: body preview: %s\n", preview);

    char *jsonPtr = (char*)memchr(data, '{', dataLen);
    if (!jsonPtr) { free(body); Serial.println("LOG: Airport fetch: no JSON in body"); return false; }

    DynamicJsonDocument doc(16384);  // 16KB in PSRAM; static 4KB was too small at 100km
    DeserializationError err = deserializeJson(doc, jsonPtr,
                                                data + dataLen - jsonPtr,
                                                DeserializationOption::Filter(filter));
    free(body);

    if (err) {
        Serial.printf("LOG: Airport fetch: JSON error: %s\n", err.c_str());
        return false;
    }

    JsonArray elements = doc["elements"];
    Serial.printf("LOG: Airport fetch: %d elements in JSON\n", elements.isNull() ? 0 : (int)elements.size());

    Airport airports[MAX_AIRPORTS];
    int count = 0;

    if (!elements.isNull()) {
        for (JsonObject el : elements) {
            if (count >= MAX_AIRPORTS) break;

            // Nodes have lat/lon directly; ways/relations have a center object
            float aLat, aLon;
            if (!el["lat"].isNull()) {
                aLat = el["lat"].as<float>();
                aLon = el["lon"].as<float>();
            } else if (!el["center"].isNull()) {
                aLat = el["center"]["lat"].as<float>();
                aLon = el["center"]["lon"].as<float>();
            } else {
                continue;
            }
            airports[count].lat = aLat;
            airports[count].lon = aLon;

            JsonObject tags = el["tags"];
            const char *iata = tags["iata"] | "";
            const char *icao = tags["icao"] | "";
            const char *ref  = tags["ref"]  | "";
            const char *name = tags["name"] | "";

            if      (iata[0]) strncpy(airports[count].code, iata, 7);
            else if (icao[0]) strncpy(airports[count].code, icao, 7);
            else if (ref[0])  strncpy(airports[count].code, ref,  7);
            else if (name[0]) strncpy(airports[count].code, name, 7);
            else continue;
            airports[count].code[7] = '\0';
            count++;
        }
    }

    storageSaveAirports(airports, count, g_cfg.homeLat, g_cfg.homeLon);
    displaySetAirports(airports, count);
    Serial.printf("LOG: Airport fetch: done — %d airports sent to display\n", count);
    return true;
}

// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// Aircraft fetch — connects, parses, and writes to staging buffer
// ---------------------------------------------------------------------------

static void fetchAircraft() {
    if (ESP.getFreeHeap() < 60000) {
        Serial.printf("LOG: Low heap (%d), skipping\n", ESP.getFreeHeap());
        return;
    }

    Serial.printf("LOG: Radar centre: %.4f, %.4f | radius: %.0fkm\n",
                  g_cfg.homeLat, g_cfg.homeLon, g_cfg.radiusKm);

    float  radiusNm = g_cfg.radiusKm * 0.539957f;
    String path     = "/api/v3/lat/" + String(g_cfg.homeLat, 4) +
                      "/lon/"        + String(g_cfg.homeLon, 4) +
                      "/dist/"       + String((int)radiusNm);

    String payload;
    if (!httpGet("opendata.adsb.fi", path, payload)) return;

    if (payload.length() == 0) {
        Serial.println("LOG: Empty payload");
        return;
    }
    Serial.printf("LOG: Payload %d bytes | Heap: %d\n", payload.length(), ESP.getFreeHeap());

    StaticJsonDocument<512> filter;
    JsonObject filterAcObj = filter.createNestedArray("ac").createNestedObject();
    filterAcObj["lat"]      = true;
    filterAcObj["lon"]      = true;
    filterAcObj["flight"]   = true;
    filterAcObj["hex"]      = true;
    filterAcObj["r"]        = true;
    filterAcObj["track"]    = true;
    filterAcObj["alt_baro"] = true;
    filterAcObj["category"] = true;
    filterAcObj["baro_rate"]= true;
    filterAcObj["squawk"]   = true;
    filterAcObj["t"]        = true;

    g_json.clear();
    DeserializationError err = deserializeJson(g_json, payload, DeserializationOption::Filter(filter));
    payload = "";
    Serial.printf("LOG: Heap after JSON parse: %d\n", ESP.getFreeHeap());

    if (err) {
        Serial.printf("LOG: JSON error: %s\n", err.c_str());
        return;
    }

    int   count = 0;
    Plane staging[MAX_PLANES];

    JsonArray ac = g_json["ac"];
    if (!ac.isNull()) {
        Serial.printf("LOG: %d aircraft in response\n", (int)ac.size());
        for (JsonObject aircraft : ac) {
            if (count >= MAX_PLANES) break;
            if (!aircraft.containsKey("lat") || !aircraft.containsKey("lon")) continue;

            // Skip ground service vehicles (category C*)
            const char *catCheck = aircraft["category"] | "";
            if (catCheck[0] == 'C') continue;

            // Skip grounded / no-altitude aircraft.
            // alt_baro is an integer or float (feet) for airborne, or the string "ground".
            if (!aircraft.containsKey("alt_baro")) continue;
            JsonVariant altVar = aircraft["alt_baro"];
            if (altVar.is<const char *>()) continue;  // "ground"
            if (altVar.as<int>() <= 0) continue;

            float pLat = aircraft["lat"].as<float>();
            float pLon = aircraft["lon"].as<float>();
            if (distanceKm(g_cfg.homeLat, g_cfg.homeLon, pLat, pLon) > g_cfg.radiusKm) continue;

            staging[count].lat = pLat;
            staging[count].lon = pLon;

            const char *cs = "???";
            if (aircraft.containsKey("flight"))   cs = aircraft["flight"];
            else if (aircraft.containsKey("r"))   cs = aircraft["r"];
            else if (aircraft.containsKey("hex")) cs = aircraft["hex"];
            strncpy(staging[count].callsign, cs, 11);
            staging[count].callsign[11] = '\0';
            for (int j = strlen(staging[count].callsign) - 1;
                 j >= 0 && staging[count].callsign[j] == ' '; j--) {
                staging[count].callsign[j] = '\0';
            }

            const char *fl = aircraft["flight"] | "";
            strncpy(staging[count].flight, fl, 11);
            staging[count].flight[11] = '\0';
            for (int j = strlen(staging[count].flight) - 1;
                 j >= 0 && staging[count].flight[j] == ' '; j--) {
                staging[count].flight[j] = '\0';
            }

            const char *rg = aircraft["r"] | "";
            strncpy(staging[count].reg, rg, 11);
            staging[count].reg[11] = '\0';

            const char *tp = aircraft["t"] | "";
            strncpy(staging[count].acType, tp, 7);
            staging[count].acType[7] = '\0';

            staging[count].track = aircraft.containsKey("track")
                ? aircraft["track"].as<float>() : -1.0f;

            if (aircraft.containsKey("alt_baro")) {
                JsonVariant av = aircraft["alt_baro"];
                staging[count].altFt = av.is<int>() ? av.as<int>() : 0;
            } else {
                staging[count].altFt = -9999;
            }

            const char *cat = aircraft["category"] | "";
            strncpy(staging[count].category, cat, 3);
            staging[count].category[3] = '\0';

            staging[count].baroRate = aircraft.containsKey("baro_rate")
                ? aircraft["baro_rate"].as<int>() : 0;

            const char *sq = aircraft["squawk"] | "";
            staging[count].isEmergency = (strcmp(sq, "7500") == 0 ||
                                          strcmp(sq, "7600") == 0 ||
                                          strcmp(sq, "7700") == 0);

            Serial.printf("  %s\n", staging[count].callsign);
            count++;
        }
    }

    if (xSemaphoreTake(g_mutex, portMAX_DELAY)) {
        for (int i = 0; i < count; i++) g_staging[i] = staging[i];
        g_stagingCount = count;
        g_stagingReady = true;
        xSemaphoreGive(g_mutex);
    }

    g_lastFetch = millis();
    Serial.printf("LOG: Fetch complete. %d planes | Heap: %d\n", count, ESP.getFreeHeap());
}

// ---------------------------------------------------------------------------
// Fetch task (core 1)
// ---------------------------------------------------------------------------

static void fetchTask(void *) {
    for (;;) {
        g_fetchCount++;

        if (g_fetchCount % REBOOT_AFTER_N_FETCHES == 0) {
            Serial.println("LOG: 24hr reboot");
            esp_restart();
        }

        Serial.printf("\n=== Fetch #%d | Stack: %d | Heap: %d ===\n",
            g_fetchCount, uxTaskGetStackHighWaterMark(nullptr), ESP.getFreeHeap());

        if (!wifiEnsureConnected()) {
            vTaskDelay(FETCH_INTERVAL_MS / portTICK_PERIOD_MS);
            continue;
        }

        // Manual OTA check runs first so the display popup appears immediately.
        // The scheduled 3am check runs at the end of the cycle instead.
        if (otaIsForced()) {
            otaCheckIfDue();
            vTaskDelay(FETCH_INTERVAL_MS / portTICK_PERIOD_MS);
            continue;
        }

        // Resolve address to coordinates on first fetch if needed
        if (strlen(g_cfg.homeAddress) > 0 &&
            g_cfg.homeLat == 0.0f && g_cfg.homeLon == 0.0f) {
            if (!fetchGeocode()) {
                if (++g_geocodeFailCount >= 3) g_geocodeFailed = true;
                vTaskDelay(FETCH_INTERVAL_MS / portTICK_PERIOD_MS);
                continue;
            }
        }

        fetchAircraft();

        if (!g_airportsFetched && g_cfg.showAirports)
            g_airportsFetched = fetchAirports();


        if (!g_windStaging.valid || g_fetchCount % WIND_FETCH_EVERY_N_FETCHES == 1) {
            fetchWindData();
        }

        otaCheckIfDue();

        vTaskDelay(FETCH_INTERVAL_MS / portTICK_PERIOD_MS);
    }
}

// ---------------------------------------------------------------------------
// Watchdog task (core 1)
// ---------------------------------------------------------------------------

static void watchdogTask(void *) {
    for (;;) {
        vTaskDelay(10000 / portTICK_PERIOD_MS);
        if (g_lastFetch > 0 && millis() - g_lastFetch > WATCHDOG_TIMEOUT_MS) {
            Serial.println("LOG: Watchdog: fetch task hung, restarting");
            if (g_fetchHandle) {
                vTaskDelete(g_fetchHandle);
                g_fetchHandle = nullptr;
            }
            g_client.stop();
            vTaskDelay(2000 / portTICK_PERIOD_MS);
            xTaskCreatePinnedToCore(fetchTask, "fetchTask", 65536, nullptr, 1, &g_fetchHandle, 1);
            g_lastFetch = millis();
            Serial.println("LOG: Fetch task restarted");
        }
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void fetchInit(const DeviceConfig &cfg) {
    g_cfg   = cfg;
    g_mutex = xSemaphoreCreateMutex();

    g_client.setInsecure();
    WiFi.begin(g_cfg.wifiSSID, g_cfg.wifiPass);
    Serial.print("Connecting to WiFi");
    for (int i = 0; i < 40 && WiFi.status() != WL_CONNECTED; i++) {
        delay(500);
        Serial.print(".");
    }
    Serial.println(WiFi.status() == WL_CONNECTED ? "\nWiFi connected" : "\nWiFi failed");

    g_lastFetch = millis();
    xTaskCreatePinnedToCore(fetchTask,    "fetchTask",    65536, nullptr, 1, &g_fetchHandle, 1);
    xTaskCreatePinnedToCore(watchdogTask, "watchdogTask", 4096,  nullptr, 1, nullptr,        1);
}

bool fetchConsumeStagingIfReady(Plane *dest, int *count) {
    if (!g_stagingReady) return false;
    if (!xSemaphoreTake(g_mutex, 0)) return false;
    for (int i = 0; i < g_stagingCount; i++) dest[i] = g_staging[i];
    *count         = g_stagingCount;
    g_stagingReady = false;
    xSemaphoreGive(g_mutex);
    return true;
}

bool fetchGeocodeFailed() { return g_geocodeFailed; }

void fetchUpdateConfig(const DeviceConfig &cfg) {
    g_cfg.homeLat        = cfg.homeLat;
    g_cfg.homeLon        = cfg.homeLon;
    g_cfg.radiusKm       = cfg.radiusKm;
    g_cfg.showAirports   = cfg.showAirports;
    g_airportsFetched    = false;
}

bool fetchConsumeWindIfReady(WindData *dest) {
    if (!g_windReady) return false;
    if (!xSemaphoreTake(g_mutex, 0)) return false;
    *dest       = g_windStaging;
    g_windReady = false;
    xSemaphoreGive(g_mutex);
    return true;
}
