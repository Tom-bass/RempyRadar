#include "portal.h"
#include "storage.h"
#include "config.h"
#include "ota.h"
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <LittleFS.h>
#include <ArduinoJson.h>

static WebServer      g_server(80);
static DNSServer      g_dns;
static bool           g_configChanged  = false;
static bool           g_restartPending = false;
static unsigned long  g_restartAt      = 0;
static bool           g_magCalPending   = false;
static bool           g_setNorthPending = false;
static bool           g_magCorrPending        = false;
static float          g_magCorrValue          = 0.0f;
static bool           g_compassRotatePending  = false;
static bool           g_compassRotateValue    = true;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static uint8_t hexByte(const String &s, int offset) {
    char buf[3] = { s[offset], s[offset + 1], '\0' };
    return (uint8_t)strtol(buf, nullptr, 16);
}

static void sendError(const char *msg) {
    String page;
    page += F("<!DOCTYPE html><html><head>"
              "<meta charset='utf-8'>"
              "<meta name='viewport' content='width=device-width,initial-scale=1'>"
              "<style>body{background:#111;color:#fff;font-family:sans-serif;"
              "text-align:center;padding:60px 20px}h2{color:#ff4444}a{color:#00dc00}</style>"
              "</head><body><h2>Error</h2><p>");
    page += msg;
    page += F("</p><p><a href='/'>&#8592; Back to settings</a></p></body></html>");
    g_server.send(400, "text/html", page);
}

// ---------------------------------------------------------------------------
// Request handlers
// ---------------------------------------------------------------------------

static void handleRoot() {
    if (!LittleFS.exists("/index.html")) {
        g_server.send(503, "text/plain",
            "Setup page missing. Run: pio run --target uploadfs");
        return;
    }
    File f = LittleFS.open("/index.html", "r");
    g_server.streamFile(f, "text/html");
    f.close();
}

static void handleSave() {
    // Load existing config early — used for restart detection and address fallback
    DeviceConfig oldCfg;
    storageLoad(oldCfg);

    if (!g_server.hasArg("ssid") || g_server.arg("ssid").length() == 0) {
        sendError("WiFi network name is required.");
        return;
    }

    DeviceConfig cfg;
    memset(&cfg, 0, sizeof(cfg));

    // WiFi
    strncpy(cfg.wifiSSID, g_server.arg("ssid").c_str(), sizeof(cfg.wifiSSID) - 1);
    String submittedPass = g_server.arg("pass");
    if (submittedPass == "****") {
        // Sentinel — user didn't change the password; preserve existing value from NVS
        strncpy(cfg.wifiPass, oldCfg.wifiPass, sizeof(cfg.wifiPass) - 1);
    } else {
        strncpy(cfg.wifiPass, submittedPass.c_str(), sizeof(cfg.wifiPass) - 1);
    }

    // Location — four cases:
    //   1. New address typed            → geocode on next connect
    //   2. Coords mode, non-zero        → use submitted lat/lon
    //   3. Coords mode, both zero/blank → preserve existing coords from NVS
    //   4. Address mode, blank field    → preserve existing address/coords from NVS
    bool wantsAddress = g_server.arg("locMode") == "address";
    bool newAddress   = wantsAddress && g_server.arg("address").length() > 0;

    if (newAddress) {
        strncpy(cfg.homeAddress, g_server.arg("address").c_str(), sizeof(cfg.homeAddress) - 1);
        cfg.homeLat = 0.0f;
        cfg.homeLon = 0.0f;
    } else if (!wantsAddress) {
        cfg.homeLat = g_server.arg("lat").toFloat();
        cfg.homeLon = g_server.arg("lon").toFloat();
        if (cfg.homeLat == 0.0f && cfg.homeLon == 0.0f) {
            if (oldCfg.homeLat != 0.0f || oldCfg.homeLon != 0.0f) {
                // Blank fields submitted — keep existing coordinates
                cfg.homeLat = oldCfg.homeLat;
                cfg.homeLon = oldCfg.homeLon;
            } else if (oldCfg.homeAddress[0] == '\0') {
                sendError("Please enter your home coordinates or an address.");
                return;
            }
        }
    } else {
        // Address tab selected but field left blank — keep existing location
        strncpy(cfg.homeAddress, oldCfg.homeAddress, sizeof(cfg.homeAddress));
        cfg.homeLat = oldCfg.homeLat;
        cfg.homeLon = oldCfg.homeLon;
        if (cfg.homeAddress[0] == '\0' && cfg.homeLat == 0.0f && cfg.homeLon == 0.0f) {
            sendError("Please enter a home address.");
            return;
        }
    }
    cfg.radiusKm = g_server.arg("radius").toFloat();
    if (cfg.radiusKm < 1.0f || cfg.radiusKm > 50.0f) cfg.radiusKm = 25.0f;

    // Display toggles (checkboxes absent = unchecked)
    cfg.showWindWidget = g_server.hasArg("showWind");
    cfg.showClock      = g_server.hasArg("showClock");
    cfg.showTrail      = g_server.hasArg("showTrail");
    cfg.showAltColors  = g_server.hasArg("showAltColors");
    cfg.showAirports      = g_server.hasArg("showAirports");
    cfg.showAirportNames  = g_server.hasArg("showAirportNames");
    cfg.showClimbDescent  = g_server.hasArg("showClimbDescent");
    cfg.showFlightNumber  = g_server.hasArg("showFlightNumber");
    cfg.showFlightReg          = g_server.hasArg("showFlightReg");
    cfg.showFlightType         = g_server.hasArg("showFlightType");
    auto parseColor = [&](const char *arg, RGBColor &out) {
        String hex = g_server.arg(arg);
        if (hex.length() == 7 && hex[0] == '#') {
            out.r = hexByte(hex, 1);
            out.g = hexByte(hex, 3);
            out.b = hexByte(hex, 5);
        }
    };
    cfg.airportColor    = { 120, 120, 0 }; parseColor("airportColor",    cfg.airportColor);
    cfg.flightNumColor  = { 0, 220, 0 };  parseColor("flightNumColor",  cfg.flightNumColor);
    cfg.flightRegColor  = { 0, 220, 0 };  parseColor("flightRegColor",  cfg.flightRegColor);
    cfg.flightTypeColor = { 0, 220, 0 };  parseColor("flightTypeColor", cfg.flightTypeColor);

    // Timezone
    strncpy(cfg.timezone, g_server.arg("tz").c_str(), sizeof(cfg.timezone) - 1);
    if (cfg.timezone[0] == '\0') strncpy(cfg.timezone, "UTC0", sizeof(cfg.timezone));
    cfg.clockPosition = (uint8_t)constrain(g_server.arg("clockPos").toInt(), 0, 3);
    cfg.compassRotate = oldCfg.compassRotate;  // managed by /set-compass-rotate, not the main form
    cfg.showNorth     = g_server.hasArg("showNorth");
    cfg.northColor    = { 255, 0, 0 };   parseColor("northColor",  cfg.northColor);
    cfg.ringColor     = { 0, 60, 0 };    parseColor("ringColor",   cfg.ringColor);
    cfg.sweepColor    = { 0, 255, 0 };   parseColor("sweepColor",  cfg.sweepColor);

    // Icon scale (100–200 %, clamped; 100 is default)
    cfg.iconScale = (uint8_t)constrain(g_server.arg("iconScale").toInt(), 100, 200);

    // Altitude palette
    cfg.altPalette = (uint8_t)constrain(g_server.arg("palette").toInt(), 0, 4);

    // Custom colours (only saved when palette == ALT_PALETTE_CUSTOM)
    if (cfg.altPalette == ALT_PALETTE_CUSTOM) {
        for (int i = 0; i < 10; i++) {
            String hex = g_server.arg("altColor" + String(i));
            if (hex.length() == 7 && hex[0] == '#') {
                cfg.customAltColors[i].r = hexByte(hex, 1);
                cfg.customAltColors[i].g = hexByte(hex, 3);
                cfg.customAltColors[i].b = hexByte(hex, 5);
            }
        }
    }

    // Check what changed so we know whether a restart is required
    bool needsRestart = strcmp(oldCfg.wifiSSID,    cfg.wifiSSID)    != 0 ||
                        strcmp(oldCfg.wifiPass,    cfg.wifiPass)    != 0 ||
                        strcmp(oldCfg.homeAddress, cfg.homeAddress) != 0;

    storageSave(cfg);
    Serial.println("Portal: config saved");
    g_configChanged = true;

    if (needsRestart) {
        // Clear cached airports so the next boot doesn't briefly show stale landmarks
        // from the old location. The first fetch will repopulate with correct data.
        storageSaveAirports(nullptr, 0, 0.0f, 0.0f);
    }

    if (needsRestart) {
        g_restartPending = true;
        g_restartAt      = millis() + 2000;
        g_server.send(200, "text/html",
            "<!DOCTYPE html><html><head>"
            "<meta charset='utf-8'>"
            "<meta name='viewport' content='width=device-width,initial-scale=1'>"
            "<style>body{background:#111;color:#fff;font-family:sans-serif;"
            "text-align:center;padding:60px 20px}h2{color:#00dc00}</style>"
            "</head><body>"
            "<h2>Saved!</h2>"
            "<p>Restarting to apply WiFi or location changes.</p>"
            "<p>Reconnect to your WiFi and visit this device&rsquo;s IP to configure again.</p>"
            "</body></html>");
    } else {
        g_server.send(200, "text/html",
            "<!DOCTYPE html><html><head>"
            "<meta charset='utf-8'>"
            "<meta name='viewport' content='width=device-width,initial-scale=1'>"
            "<style>body{background:#111;color:#fff;font-family:sans-serif;"
            "text-align:center;padding:60px 20px}h2{color:#00dc00}"
            ".btn{display:inline-block;margin-top:16px;padding:12px 28px;"
            "background:transparent;border:1px solid #00dc00;color:#00dc00;"
            "border-radius:8px;font-size:1rem;font-weight:600;cursor:pointer;"
            "text-decoration:none;letter-spacing:0.3px}"
            ".btn:hover{background:#00dc0018}</style>"
            "</head><body>"
            "<h2>Saved!</h2>"
            "<p>Settings applied to the running radar.</p>"
            "<a class='btn' href='/'>Return to settings</a>"
            "</body></html>");
    }
}

static void handleOtaCheck() {
    otaTriggerCheck();
    g_server.send(200, "text/plain", "ok");
}

static void handleMagCal() {
    g_magCalPending = true;
    g_server.send(200, "text/plain", "ok");
}

static void handleSetNorth() {
    g_setNorthPending = true;
    g_server.send(200, "text/plain", "ok");
}

static void handleMagCorrection() {
    g_magCorrValue   = g_server.arg("deg").toFloat();
    g_magCorrPending = true;
    g_server.send(200, "text/plain", "ok");
}

static void handleSetCompassRotate() {
    g_compassRotateValue   = g_server.arg("enabled") == "1";
    g_compassRotatePending = true;
    g_server.send(200, "text/plain", "ok");
}

static void handleConfig() {
    DeviceConfig cfg;
    storageLoad(cfg);

    static StaticJsonDocument<1536> doc;
    doc.clear();
    doc["firmware"]      = FIRMWARE_VERSION;
    doc["ssid"]          = cfg.wifiSSID;
    doc["pass"]          = cfg.wifiPass[0] ? "****" : "";
    doc["lat"]           = cfg.homeLat;
    doc["lon"]           = cfg.homeLon;
    doc["hasAddress"]    = (cfg.homeAddress[0] != '\0');
    doc["radius"]        = (int)cfg.radiusKm;
    doc["timezone"]      = cfg.timezone;
    doc["clockPos"]      = cfg.clockPosition;
    doc["northCalibrated"] = storageLoadMagNorthSet();
    doc["compassRotate"] = cfg.compassRotate;
    doc["showNorth"]     = cfg.showNorth;
    {
        char h[8];
        snprintf(h, sizeof(h), "#%02X%02X%02X", cfg.northColor.r, cfg.northColor.g, cfg.northColor.b);
        doc["northColor"] = h;
        snprintf(h, sizeof(h), "#%02X%02X%02X", cfg.ringColor.r,  cfg.ringColor.g,  cfg.ringColor.b);
        doc["ringColor"]  = h;
        snprintf(h, sizeof(h), "#%02X%02X%02X", cfg.sweepColor.r, cfg.sweepColor.g, cfg.sweepColor.b);
        doc["sweepColor"] = h;
    }
    doc["showWind"]      = cfg.showWindWidget;
    doc["showClock"]     = cfg.showClock;
    doc["showTrail"]     = cfg.showTrail;
    doc["showAltColors"] = cfg.showAltColors;
    doc["showAirports"]      = cfg.showAirports;
    doc["showAirportNames"]  = cfg.showAirportNames;
    doc["showClimbDescent"]  = cfg.showClimbDescent;
    doc["showFlightNumber"]  = cfg.showFlightNumber;
    doc["showFlightReg"]        = cfg.showFlightReg;
    doc["showFlightType"]       = cfg.showFlightType;
    {
        char h[8];
        snprintf(h, sizeof(h), "#%02X%02X%02X", cfg.airportColor.r,    cfg.airportColor.g,    cfg.airportColor.b);
        doc["airportColor"]    = h;
        snprintf(h, sizeof(h), "#%02X%02X%02X", cfg.flightNumColor.r,  cfg.flightNumColor.g,  cfg.flightNumColor.b);
        doc["flightNumColor"]  = h;
        snprintf(h, sizeof(h), "#%02X%02X%02X", cfg.flightRegColor.r,  cfg.flightRegColor.g,  cfg.flightRegColor.b);
        doc["flightRegColor"]  = h;
        snprintf(h, sizeof(h), "#%02X%02X%02X", cfg.flightTypeColor.r, cfg.flightTypeColor.g, cfg.flightTypeColor.b);
        doc["flightTypeColor"] = h;
    }
    doc["altPalette"]    = cfg.altPalette;
    doc["iconScale"]     = cfg.iconScale ? cfg.iconScale : 100;

    if (cfg.altPalette == ALT_PALETTE_CUSTOM) {
        JsonArray colors = doc.createNestedArray("customColors");
        for (int i = 0; i < 10; i++) {
            char hex[8];
            snprintf(hex, sizeof(hex), "#%02X%02X%02X",
                     cfg.customAltColors[i].r, cfg.customAltColors[i].g, cfg.customAltColors[i].b);
            colors.add(hex);
        }
    }

    String json;
    serializeJson(doc, json);
    g_server.send(200, "application/json", json);
}

// ---------------------------------------------------------------------------
// Public entry points
// ---------------------------------------------------------------------------

void portalStartSettingsServer() {
    if (!LittleFS.begin(true)) {
        Serial.println("Settings: LittleFS mount failed");
    }
    g_server.on("/",                 handleRoot);
    g_server.on("/index.html",       handleRoot);
    g_server.on("/config",           handleConfig);
    g_server.on("/save",        HTTP_POST, handleSave);
    g_server.on("/ota-check",   HTTP_POST, handleOtaCheck);
    g_server.on("/mag-calibrate",   HTTP_POST, handleMagCal);
    g_server.on("/set-north",       HTTP_POST, handleSetNorth);
    g_server.on("/mag-correction",      HTTP_POST, handleMagCorrection);
    g_server.on("/set-compass-rotate",  HTTP_POST, handleSetCompassRotate);
    g_server.onNotFound(handleRoot);
    g_server.begin();
    Serial.println("Settings: server started at http://" + WiFi.localIP().toString());
}

void portalHandleClient() {
    g_server.handleClient();
}

bool portalConfigChanged() {
    if (!g_configChanged) return false;
    g_configChanged = false;
    return true;
}

bool portalRestartPending() {
    return g_restartPending && millis() >= g_restartAt;
}

bool portalMagCalPending() {
    if (!g_magCalPending) return false;
    g_magCalPending = false;
    return true;
}

bool portalSetNorthPending() {
    if (!g_setNorthPending) return false;
    g_setNorthPending = false;
    return true;
}

bool portalCompassRotatePending(bool &enabled) {
    if (!g_compassRotatePending) return false;
    g_compassRotatePending = false;
    enabled = g_compassRotateValue;
    return true;
}

bool portalMagCorrectionPending(float &outDeg) {
    if (!g_magCorrPending) return false;
    g_magCorrPending = false;
    outDeg = g_magCorrValue;
    return true;
}

void portalBegin() {
    Serial.println("Portal: mounting LittleFS");
    if (!LittleFS.begin(true)) {
        Serial.println("Portal: LittleFS mount failed");
    }

    WiFi.softAP("RempyRadar");
    delay(100);
    IPAddress ip = WiFi.softAPIP();
    Serial.println("Portal: AP IP " + ip.toString());

    // Redirect all DNS queries to this device so captive portal fires on all platforms
    g_dns.start(53, "*", ip);

    // Serve setup page for all common captive-portal detection URLs
    g_server.on("/",                         handleRoot);
    g_server.on("/index.html",               handleRoot);
    g_server.on("/generate_204",             handleRoot);  // Android
    g_server.on("/hotspot-detect.html",      handleRoot);  // iOS/macOS
    g_server.on("/library/test/success.html",handleRoot);  // Safari
    g_server.on("/ncsi.txt",                 handleRoot);  // Windows
    g_server.on("/fwlink",                   handleRoot);  // Windows
    g_server.on("/save", HTTP_POST,          handleSave);
    g_server.onNotFound(handleRoot);
    g_server.begin();

    Serial.println("Portal: ready — connect to 'RempyRadar'");

    for (;;) {
        g_dns.processNextRequest();
        g_server.handleClient();
        if (g_restartPending && millis() >= g_restartAt) esp_restart();
    }
}
