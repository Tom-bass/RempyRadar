#include "display.h"
#include "config.h"
#include "radar.h"
#include <math.h>
#include <string.h>
#include <time.h>

// ---------------------------------------------------------------------------
// Module-level state
// ---------------------------------------------------------------------------

static const int TRAIL_LEN = 8;

struct TrailPoint { int x, y; bool valid; };

struct Ping {
    int           x;
    int           y;
    char          callsign[12];
    char          flight[12];
    char          reg[12];
    char          acType[8];
    float         bearing;
    uint8_t       brightness;
    bool          active;
    unsigned long holdUntil;
    uint16_t      altColor;
    float         track;
    char          category[4];
    bool          groundLevel;
    int           baroRate;
    bool          isEmergency;
    TrailPoint    trail[TRAIL_LEN];
    int           trailHead;
    float         lastTriggerSweep;
};

static GFXcanvas16 *g_canvas        = nullptr;
static float        g_sweep         = 0.0f;
static Ping         g_pings[MAX_PINGS];
static WindData     g_wind          = { 0.0f, 0, false };
static Airport      g_airports[MAX_AIRPORTS];
static int          g_airportCount  = 0;
static bool         g_showWind      = true;
static bool         g_showClock         = true;
static uint8_t      g_clockPosition     = 0;
static bool         g_showNorth         = true;
static uint16_t     g_northColor565     = 0xF800; // red
static uint16_t     g_ringColor565      = 0x01E0; // dark green
static uint16_t     g_sweepColor565     = 0x07E0; // bright green
static bool         g_showTrail     = true;
static bool         g_showAltColors = true;
static float        g_radiusKm          = 0.0f;
static bool         g_showAirports      = true;
static bool         g_showAirportNames  = true;
static uint16_t     g_airportColor565   = 0x7800; // yellowish default
static bool         g_showClimbDescent      = true;
static bool         g_showFlightNumber      = true;
static bool         g_showFlightReg         = false;
static bool         g_showFlightType        = false;
static uint16_t     g_flightNumColor565        = 0x07E0;
static uint16_t     g_flightRegColor565        = 0x07E0;
static uint16_t     g_flightTypeColor565       = 0x07E0;
static uint8_t      g_altPalette    = ALT_PALETTE_CLASSIC;
static RGBColor     g_customAltColors[10];

#ifdef FEATURE_WATERWAYS
static WaterPolyline g_waterPolys[MAX_WATER_POLYS];
static int           g_waterPolyCount = 0;
#endif

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static uint16_t color565(uint8_t r, uint8_t g, uint8_t b) {
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

// Fixed altitude tiers shared by all palettes (feet)
static const int k_altTiers[10] = {0,1000,2000,4000,6000,8000,10000,20000,30000,40000};

// Built-in palette RGB values at each tier
static const RGBColor k_palettes[4][10] = {
    // Classic Rainbow
    {{200,80,0},{230,130,0},{240,170,0},{240,220,0},{200,230,0},
     {100,220,0},{0,220,0},{0,180,140},{0,50,200},{150,0,200}},
    // Fire
    {{0,0,51},{17,0,0},{51,0,0},{153,0,0},{204,51,0},
     {255,102,0},{255,153,0},{255,204,0},{255,255,102},{255,255,255}},
    // Ocean
    {{0,51,102},{0,68,136},{0,85,170},{0,119,204},{0,153,221},
     {0,170,238},{0,187,255},{102,221,255},{170,238,255},{204,255,255}},
    // Monochrome
    {{0,51,0},{0,85,0},{0,119,0},{0,153,0},{0,187,0},
     {0,204,0},{0,221,0},{68,238,0},{136,255,68},{204,255,170}},
};

static uint16_t altitudeColor(int altFt) {
    if (altFt < -999) return color565(0, 220, 0);  // unknown → primary green
    if (altFt <= 0)   return color565(70, 70, 70); // grounded → dim grey
    if (!g_showAltColors) return color565(0, 220, 0); // palette disabled → plain green

    const RGBColor *stops = (g_altPalette < 4)
        ? k_palettes[g_altPalette]
        : g_customAltColors;

    if (altFt >= k_altTiers[9])
        return color565(stops[9].r, stops[9].g, stops[9].b);

    for (int i = 0; i < 9; i++) {
        if (altFt <= k_altTiers[i + 1]) {
            float t = (float)(altFt - k_altTiers[i]) /
                      (float)(k_altTiers[i + 1] - k_altTiers[i]);
            uint8_t r = (uint8_t)((int)stops[i].r + (int)(t * ((int)stops[i+1].r - (int)stops[i].r)));
            uint8_t g = (uint8_t)((int)stops[i].g + (int)(t * ((int)stops[i+1].g - (int)stops[i].g)));
            uint8_t b = (uint8_t)((int)stops[i].b + (int)(t * ((int)stops[i+1].b - (int)stops[i].b)));
            return color565(r, g, b);
        }
    }
    return color565(150, 0, 200);
}

static uint16_t scaleColor565(uint16_t col, uint8_t b) {
    uint8_t r  = ((col >> 11) & 0x1F) * b / 255;
    uint8_t g  = ((col >> 5)  & 0x3F) * b / 255;
    uint8_t bv = (col & 0x1F) * b / 255;
    return (r << 11) | (g << 5) | bv;
}

// ---------------------------------------------------------------------------
// Config application
// ---------------------------------------------------------------------------

#ifdef FEATURE_WATERWAYS
void displaySetWaterways(const WaterPolyline *polys, int count) {
    g_waterPolyCount = count < MAX_WATER_POLYS ? count : MAX_WATER_POLYS;
    for (int i = 0; i < g_waterPolyCount; i++) g_waterPolys[i] = polys[i];
}

static void drawWaterways(float homeLat, float homeLon, float radiusKm) {
    if (g_waterPolyCount == 0) return;
    // Linear projection — accurate enough within 50km, avoids per-point trig
    float cosLat = cosf(toRad(homeLat));
    float scaleX = (RADAR_R / radiusKm) * 111.0f * cosLat;
    float scaleY = (RADAR_R / radiusKm) * 111.0f;
    uint16_t col = color565(0, 55, 120);
    for (int p = 0; p < g_waterPolyCount; p++) {
        const WaterPolyline &poly = g_waterPolys[p];
        for (int i = 0; i < poly.count - 1; i++) {
            int x0 = CENTRE_X + (int)roundf((poly.lons[i]     - homeLon) * scaleX);
            int y0 = CENTRE_Y - (int)roundf((poly.lats[i]     - homeLat) * scaleY);
            int x1 = CENTRE_X + (int)roundf((poly.lons[i + 1] - homeLon) * scaleX);
            int y1 = CENTRE_Y - (int)roundf((poly.lats[i + 1] - homeLat) * scaleY);
            if ((x0 < -20 && x1 < -20) || (x0 > 260 && x1 > 260) ||
                (y0 < -20 && y1 < -20) || (y0 > 260 && y1 > 260)) continue;
            g_canvas->drawLine(x0, y0, x1, y1, col);
        }
    }
}
#endif

void displaySetAirports(const Airport *airports, int count) {
    g_airportCount = (count > MAX_AIRPORTS) ? MAX_AIRPORTS : count;
    for (int i = 0; i < g_airportCount; i++) g_airports[i] = airports[i];
}

void displayApplyConfig(const DeviceConfig &cfg) {
    if (cfg.radiusKm != g_radiusKm) {
        // Screen coordinates are baked at a specific radius — clear stale pings and trails
        for (int i = 0; i < MAX_PINGS; i++) {
            g_pings[i].active    = false;
            g_pings[i].trailHead = 0;
            for (int t = 0; t < TRAIL_LEN; t++) g_pings[i].trail[t].valid = false;
        }
        g_radiusKm = cfg.radiusKm;
    }
    g_showWind          = cfg.showWindWidget;
    g_showClock         = cfg.showClock;
    g_clockPosition     = cfg.clockPosition < 4 ? cfg.clockPosition : 0;
    g_showNorth         = cfg.showNorth;
    g_northColor565     = color565(cfg.northColor.r, cfg.northColor.g, cfg.northColor.b);
    g_ringColor565      = color565(cfg.ringColor.r,  cfg.ringColor.g,  cfg.ringColor.b);
    g_sweepColor565     = color565(cfg.sweepColor.r, cfg.sweepColor.g, cfg.sweepColor.b);
    g_showTrail         = cfg.showTrail;
    g_showAltColors     = cfg.showAltColors;
    g_showAirports      = cfg.showAirports;
    g_showAirportNames  = cfg.showAirportNames;
    g_airportColor565   = color565(cfg.airportColor.r, cfg.airportColor.g, cfg.airportColor.b);
    g_showClimbDescent    = cfg.showClimbDescent;
    g_showFlightNumber    = cfg.showFlightNumber;
    g_showFlightReg       = cfg.showFlightReg;
    g_showFlightType      = cfg.showFlightType;
    g_flightNumColor565  = color565(cfg.flightNumColor.r,  cfg.flightNumColor.g,  cfg.flightNumColor.b);
    g_flightRegColor565  = color565(cfg.flightRegColor.r,  cfg.flightRegColor.g,  cfg.flightRegColor.b);
    g_flightTypeColor565 = color565(cfg.flightTypeColor.r, cfg.flightTypeColor.g, cfg.flightTypeColor.b);
    g_altPalette        = cfg.altPalette;
    memcpy(g_customAltColors, cfg.customAltColors, sizeof(g_customAltColors));
}

// ---------------------------------------------------------------------------
// Init / splash screens
// ---------------------------------------------------------------------------

void displayInit(Adafruit_GC9A01A &tft) {
    tft.begin();
    tft.setRotation(0);
    tft.fillScreen(0x0000);

    for (int i = 0; i < MAX_PINGS; i++) {
        g_pings[i].active      = false;
        g_pings[i].holdUntil   = 0;
        g_pings[i].track       = -1.0f;
        g_pings[i].altColor    = color565(0, 200, 0);
        g_pings[i].category[0] = '\0';
        g_pings[i].groundLevel  = false;
        g_pings[i].baroRate     = 0;
        g_pings[i].isEmergency  = false;
        g_pings[i].trailHead        = 0;
        g_pings[i].lastTriggerSweep = 0.0f;
        for (int t = 0; t < TRAIL_LEN; t++) g_pings[i].trail[t].valid = false;
    }

    g_canvas = new GFXcanvas16(240, 240);
    if (!g_canvas) {
        Serial.println("Canvas allocation failed!");
        while (true);
    }
    g_canvas->setTextWrap(false);
}

void displayShowConnecting(Adafruit_GC9A01A &tft) {
    tft.setTextSize(2);
    tft.setTextColor(color565(0, 255, 0));
    tft.setCursor(60, 104);
    tft.print("Connecting");
    tft.setCursor(102, 124);
    tft.print("...");
}

void displayShowConnected(Adafruit_GC9A01A &tft, const String &ip) {
    tft.fillScreen(0x0000);
    tft.setTextColor(color565(0, 200, 0));
    tft.setTextSize(1);
    tft.setCursor(93, 86);   // "Settings:" 9 chars × 6px = 54px → x=(240-54)/2
    tft.print("Settings:");
    tft.setTextSize(2);
    tft.setCursor((240 - (int)ip.length() * 12) / 2, 104);
    tft.print(ip);
    tft.setTextSize(1);
    tft.setCursor(75, 130);  // "in your browser" 15 chars × 6px = 90px → x=(240-90)/2
    tft.print("in your browser");
}

void displayShowGeocodeError(Adafruit_GC9A01A &tft) {
    tft.fillScreen(0x0000);
    tft.setTextSize(2);
    tft.setTextColor(color565(255, 80, 0));
    tft.setCursor(78, 76);   // "Address"   7 chars × 12px = 84px  → x=(240-84)/2
    tft.print("Address");
    tft.setCursor(66, 96);   // "not found" 9 chars × 12px = 108px → x=(240-108)/2
    tft.print("not found");
    tft.setTextSize(1);
    tft.setTextColor(color565(0, 200, 0));
    tft.setCursor(72, 128);  // 16 chars × 6px = 96px → x=(240-96)/2
    tft.print("Connect to WiFi:");
    tft.setCursor(72, 142);
    tft.print("RempyRadar-Setup");
    tft.setCursor(81, 156);  // "to try again." 13 chars × 6px = 78px → x=(240-78)/2
    tft.print("to try again.");
}

void displayShowSetupScreen(Adafruit_GC9A01A &tft) {
    tft.fillScreen(0x0000);
    tft.setTextColor(color565(0, 255, 0));
    tft.setTextSize(1);
    tft.setCursor(46, 96);
    tft.print("Connect to WiFi:");
    tft.setCursor(46, 112);
    tft.print("RempyRadar-Setup");
    tft.setCursor(46, 136);
    tft.print("to get started.");
}

// ---------------------------------------------------------------------------
// Internal drawing — landmarks & grid
// ---------------------------------------------------------------------------

static void drawLandmark(float homeLat, float homeLon, float radiusKm,
                          float lat, float lon, const char *label, uint16_t colour, bool showLabel) {
    int px, py;
    planeToScreen(homeLat, homeLon, radiusKm, lat, lon, px, py);
    if (px < 0 || px >= 240 || py < 0 || py >= 240) return;
    g_canvas->drawCircle(px, py, 4, colour);
    g_canvas->fillCircle(px, py, 2, colour);
    if (showLabel) {
        g_canvas->setTextColor(colour);
        g_canvas->setTextSize(1);
        g_canvas->setCursor(px + 6, py - 3);
        g_canvas->print(label);
    }
}

static void drawRadarGrid(float homeLat, float homeLon, float radiusKm) {
#ifdef FEATURE_WATERWAYS
    drawWaterways(homeLat, homeLon, radiusKm);
#endif
    g_canvas->drawCircle(CENTRE_X, CENTRE_Y, RADAR_R,          g_ringColor565);
    g_canvas->drawCircle(CENTRE_X, CENTRE_Y, RADAR_R * 2 / 3, g_ringColor565);
    g_canvas->drawCircle(CENTRE_X, CENTRE_Y, RADAR_R * 1 / 3, g_ringColor565);
    g_canvas->drawLine(CENTRE_X - RADAR_R, CENTRE_Y, CENTRE_X + RADAR_R, CENTRE_Y, g_ringColor565);
    g_canvas->drawLine(CENTRE_X, CENTRE_Y - RADAR_R, CENTRE_X, CENTRE_Y + RADAR_R, g_ringColor565);
    g_canvas->fillCircle(CENTRE_X, CENTRE_Y, 2, g_sweepColor565);

    if (g_showNorth) {
        g_canvas->setTextColor(g_northColor565);
        g_canvas->setTextSize(1);
        g_canvas->setCursor(117, CENTRE_Y - RADAR_R + 6);
        g_canvas->print("N");
    }

    // Range labels at 45° (NE), inside each ring
    char buf[8];
    g_canvas->setTextColor(g_ringColor565);
    const float k = 0.707f;
    int rings[3] = { RADAR_R, RADAR_R * 2 / 3, RADAR_R / 3 };
    float dists[3] = { radiusKm, radiusKm * 2.0f / 3.0f, radiusKm / 3.0f };
    for (int i = 0; i < 3; i++) {
        snprintf(buf, sizeof(buf), "%.0fkm", dists[i]);
        int r = rings[i] - 28;
        g_canvas->setCursor(CENTRE_X + (int)(r * k), CENTRE_Y - (int)(r * k) - 4);
        g_canvas->print(buf);
    }

    if (g_showAirports) {
        static bool loggedOnce = false;
        if (!loggedOnce) {
            Serial.printf("LOG: drawRadarGrid: showAirports=1 count=%d\n", g_airportCount);
            loggedOnce = true;
        }
        uint16_t airportColour = g_airportColor565;
        for (int i = 0; i < g_airportCount; i++) {
            drawLandmark(homeLat, homeLon, radiusKm,
                         g_airports[i].lat, g_airports[i].lon,
                         g_airports[i].code, airportColour, g_showAirportNames);
        }
    }
}

// ---------------------------------------------------------------------------
// Internal drawing — sweep
// ---------------------------------------------------------------------------

static void drawSweepTrail() {
    const int   trailSteps = 40;
    const float stepSize   = 1.0f;
    for (float i = (float)trailSteps; i >= 0; i -= stepSize) {
        float a1 = g_sweep - i;
        float a2 = g_sweep - i + stepSize;
        if (a1 < 0) a1 += 360.0f;
        if (a2 < 0) a2 += 360.0f;
        uint8_t  intensity = (uint8_t)map((long)(i * 10), 0, trailSteps * 10, 200, 0);
        uint8_t  scaledB   = (uint8_t)((int)intensity * 255 / 200);
        uint16_t colour    = scaleColor565(g_sweepColor565, scaledB);
        float r1 = toRad(a1 - 90.0f);
        float r2 = toRad(a2 - 90.0f);
        int x1 = CENTRE_X + (int)(RADAR_R * cosf(r1));
        int y1 = CENTRE_Y + (int)(RADAR_R * sinf(r1));
        int x2 = CENTRE_X + (int)(RADAR_R * cosf(r2));
        int y2 = CENTRE_Y + (int)(RADAR_R * sinf(r2));
        g_canvas->fillTriangle(CENTRE_X, CENTRE_Y, x1, y1, x2, y2, colour);
    }
}

static void drawSweepLine() {
    float rad = toRad(g_sweep - 90.0f);
    int ex = CENTRE_X + (int)(RADAR_R * cosf(rad));
    int ey = CENTRE_Y + (int)(RADAR_R * sinf(rad));
    g_canvas->drawLine(CENTRE_X, CENTRE_Y, ex, ey, g_sweepColor565);
}

// ---------------------------------------------------------------------------
// Internal drawing — pings
// ---------------------------------------------------------------------------

static void pushTrailPoint(int slot, int newX, int newY) {
    if (newX != g_pings[slot].x || newY != g_pings[slot].y) {
        g_pings[slot].trail[g_pings[slot].trailHead] = { g_pings[slot].x, g_pings[slot].y, true };
        g_pings[slot].trailHead = (g_pings[slot].trailHead + 1) % TRAIL_LEN;
    }
}

static void populatePing(int slot, int px, int py, const char *callsign,
                         const char *flight, const char *reg, const char *acType,
                         float bearing, uint16_t altColor, float track,
                         const char *category, bool groundLevel, int baroRate,
                         bool isEmergency) {
    g_pings[slot].x                = px;
    g_pings[slot].y                = py;
    strncpy(g_pings[slot].callsign, callsign, 11);
    g_pings[slot].callsign[11]     = '\0';
    strncpy(g_pings[slot].flight,   flight,   11);
    g_pings[slot].flight[11]       = '\0';
    strncpy(g_pings[slot].reg,      reg,      11);
    g_pings[slot].reg[11]          = '\0';
    strncpy(g_pings[slot].acType,   acType,   7);
    g_pings[slot].acType[7]        = '\0';
    g_pings[slot].bearing          = bearing;
    g_pings[slot].brightness       = 255;
    g_pings[slot].holdUntil        = millis() + 4000;
    g_pings[slot].active           = true;
    g_pings[slot].lastTriggerSweep = g_sweep;
    g_pings[slot].altColor         = altColor;
    g_pings[slot].track            = track;
    g_pings[slot].groundLevel      = groundLevel;
    g_pings[slot].baroRate         = baroRate;
    g_pings[slot].isEmergency      = isEmergency;
    strncpy(g_pings[slot].category, category, 3);
    g_pings[slot].category[3]      = '\0';
}

static void triggerPing(int px, int py, const char *callsign,
                        const char *flight, const char *reg, const char *acType,
                        float bearing, uint16_t altColor, float track,
                        const char *category, bool groundLevel, int baroRate,
                        bool isEmergency) {
    // Update existing active ping
    for (int i = 0; i < MAX_PINGS; i++) {
        if (g_pings[i].active && strncmp(g_pings[i].callsign, callsign, 11) == 0) {
            pushTrailPoint(i, px, py);
            populatePing(i, px, py, callsign, flight, reg, acType, bearing,
                         altColor, track, category, groundLevel, baroRate, isEmergency);
            return;
        }
    }

    // Reactivate a recently-expired ping, inheriting its trail
    for (int i = 0; i < MAX_PINGS; i++) {
        if (!g_pings[i].active && strncmp(g_pings[i].callsign, callsign, 11) == 0) {
            pushTrailPoint(i, px, py);
            populatePing(i, px, py, callsign, flight, reg, acType, bearing,
                         altColor, track, category, groundLevel, baroRate, isEmergency);
            return;
        }
    }

    // Allocate a new slot, or evict the dimmest
    int slot = -1;
    for (int i = 0; i < MAX_PINGS; i++) {
        if (!g_pings[i].active) { slot = i; break; }
    }
    if (slot == -1) {
        uint8_t minB = 255;
        for (int i = 0; i < MAX_PINGS; i++) {
            if (g_pings[i].brightness < minB) { minB = g_pings[i].brightness; slot = i; }
        }
    }
    if (slot >= 0) {
        g_pings[slot].trailHead = 0;
        for (int t = 0; t < TRAIL_LEN; t++) g_pings[slot].trail[t].valid = false;
        populatePing(slot, px, py, callsign, flight, reg, acType, bearing,
                     altColor, track, category, groundLevel, baroRate, isEmergency);
    }
}

static void expirePings() {
    for (int i = 0; i < MAX_PINGS; i++) {
        if (!g_pings[i].active) continue;
        float diff = fmodf(g_sweep - g_pings[i].bearing + 360.0f, 360.0f);
        if (diff > 350.0f) g_pings[i].active = false;
    }
}

static void checkSweepHitsPlanes(const Plane *planes, int planeCount,
                                  float homeLat, float homeLon, float radiusKm) {
    for (int i = 0; i < planeCount; i++) {
        int px, py;
        planeToScreen(homeLat, homeLon, radiusKm, planes[i].lat, planes[i].lon, px, py);
        if (px < 0 || px >= 240 || py < 0 || py >= 240) continue;
        float bearing = bearingTo(homeLat, homeLon, planes[i].lat, planes[i].lon);
        float diff    = fmodf(g_sweep - bearing + 360.0f, 360.0f);
        if (diff < SWEEP_SPEED * 1.5f) {
            triggerPing(px, py, planes[i].callsign,
                        planes[i].flight, planes[i].reg, planes[i].acType,
                        bearing, altitudeColor(planes[i].altFt),
                        planes[i].track, planes[i].category,
                        planes[i].altFt <= 0, planes[i].baroRate,
                        planes[i].isEmergency);
        }
    }
}

static void fadePings() {
    static unsigned long lastFade = 0;
    if (millis() - lastFade < 28) return;
    lastFade = millis();
    for (int i = 0; i < MAX_PINGS; i++) {
        if (!g_pings[i].active) continue;
        if (millis() < g_pings[i].holdUntil) continue;
        if (g_pings[i].brightness > 8) {
            g_pings[i].brightness -= 1;
        } else {
            g_pings[i].active = false;
        }
    }
}

static void drawPingIcon(int px, int py, const char *cat, float track, uint16_t col) {
    char c0 = cat[0], c1 = cat[1];

    // Helicopter: cross — two clean lines, no heading needed
    if (c0 == 'A' && c1 == '7') {
        g_canvas->drawLine(px - 3, py,     px + 3, py,     col);
        g_canvas->drawLine(px,     py - 3, px,     py + 3, col);
        return;
    }

    // No heading data: small dot
    if (track < 0.0f) {
        g_canvas->fillCircle(px, py, 2, col);
        return;
    }

    float rad   = toRad(track);
    float sin_t = sinf(rad);
    float cos_t = cosf(rad);

    // Same geometry for all fixed-wing — fill distinguishes heavy from light
    int tx = (int)roundf(px + 7.0f * sin_t);
    int ty = (int)roundf(py - 7.0f * cos_t);

    float rcx = px - 2.0f * sin_t;
    float rcy = py + 2.0f * cos_t;

    int b1x = (int)roundf(rcx - 2.0f * cos_t);
    int b1y = (int)roundf(rcy - 2.0f * sin_t);
    int b2x = (int)roundf(rcx + 2.0f * cos_t);
    int b2y = (int)roundf(rcy + 2.0f * sin_t);

    bool isLight = (c0 == 'A' && (c1 == '1' || c1 == '2'));
    if (isLight) {
        g_canvas->drawTriangle(tx, ty, b1x, b1y, b2x, b2y, col);
    } else {
        g_canvas->fillTriangle(tx, ty, b1x, b1y, b2x, b2y, col);
    }
}

// Returns true if this screen point should be visible given when the ping was triggered.
// Points behind triggerSweep (already swept when ping appeared) reveal instantly.
// Points ahead of triggerSweep reveal progressively as the sweep reaches them.
static bool trailPointRevealed(int x, int y, float triggerSweep) {
    float bearing = atan2f((float)(x - CENTRE_X), -(float)(y - CENTRE_Y)) * (180.0f / 3.14159265f);
    if (bearing < 0.0f) bearing += 360.0f;
    float toPoint = fmodf(bearing - triggerSweep + 360.0f, 360.0f);
    if (toPoint >= 180.0f) return true;  // behind trigger — reveal instantly
    return fmodf(g_sweep - triggerSweep + 360.0f, 360.0f) >= toPoint;
}

static void drawTrails() {
    static const uint8_t trailBrightness[TRAIL_LEN] = { 130, 110, 90, 72, 56, 42, 30, 20 };
    for (int i = 0; i < MAX_PINGS; i++) {
        if (!g_pings[i].active) continue;

        int ptx[TRAIL_LEN], pty[TRAIL_LEN];
        int count = 0;
        for (int t = 0; t < TRAIL_LEN; t++) {
            int idx = (g_pings[i].trailHead - 1 - t + TRAIL_LEN * 2) % TRAIL_LEN;
            if (!g_pings[i].trail[idx].valid) break;
            ptx[count] = g_pings[i].trail[idx].x;
            pty[count] = g_pings[i].trail[idx].y;
            count++;
        }

        if (count == 0) continue;

        uint8_t pingB = g_pings[i].brightness;

        // First segment: current ping → newest trail point.
        // If newest point isn't revealed yet, suppress the whole trail.
        float triggerSweep = g_pings[i].lastTriggerSweep;
        if (!trailPointRevealed(ptx[0], pty[0], triggerSweep)) continue;
        uint16_t col = scaleColor565(g_pings[i].altColor,
                           (uint8_t)((uint16_t)trailBrightness[0] * pingB / 255));
        g_canvas->drawLine(g_pings[i].x, g_pings[i].y, ptx[0], pty[0], col);

        // Older segments — stop at the first point the sweep hasn't reached yet.
        for (int t = 0; t < count - 1; t++) {
            if (!trailPointRevealed(ptx[t + 1], pty[t + 1], triggerSweep)) break;
            col = scaleColor565(g_pings[i].altColor,
                      (uint8_t)((uint16_t)trailBrightness[t + 1] * pingB / 255));
            g_canvas->drawLine(ptx[t], pty[t], ptx[t + 1], pty[t + 1], col);
        }
    }
}

// Icons drawn before the grid so grid lines are never overdrawn by fading icons
static void drawPingIcons() {
    for (int i = 0; i < MAX_PINGS; i++) {
        if (!g_pings[i].active) continue;
        uint16_t col = g_pings[i].isEmergency
            ? color565(255, 0, 0)
            : scaleColor565(g_pings[i].altColor, g_pings[i].brightness);
        drawPingIcon(g_pings[i].x, g_pings[i].y, g_pings[i].category,
                     g_pings[i].track, col);
    }
}

// Labels drawn after the grid; suppressed below threshold so text never becomes a dark smudge
static void drawPingLabels() {
    if (!g_showFlightNumber && !g_showFlightReg && !g_showFlightType) return;

    for (int i = 0; i < MAX_PINGS; i++) {
        if (!g_pings[i].active) continue;
        if (g_pings[i].groundLevel) continue;
        if (!g_pings[i].isEmergency && g_pings[i].brightness < 35) continue;

        bool emerg = g_pings[i].isEmergency;
        uint8_t br = g_pings[i].brightness;

        auto labelCol = [&](uint16_t col565) -> uint16_t {
            if (emerg) return color565(255, 0, 0);
            return scaleColor565(col565, br);
        };

        g_canvas->setTextSize(1);

        char indicator = 0;
        if (g_showClimbDescent) {
            if      (g_pings[i].baroRate >  200) indicator = '+';
            else if (g_pings[i].baroRate < -200) indicator = '-';
        }

        int lineY = g_pings[i].y + 6;

        if (g_showFlightNumber) {
            g_canvas->setTextColor(labelCol(g_flightNumColor565));
            int labelLen = strlen(g_pings[i].callsign) + (indicator ? 1 : 0);
            g_canvas->setCursor(g_pings[i].x - (labelLen * 3), lineY);
            g_canvas->print(g_pings[i].callsign);
            if (indicator) g_canvas->print(indicator);
            lineY += 9;
        }

        if (g_showFlightReg && g_pings[i].reg[0] != '\0' &&
            strcmp(g_pings[i].reg, g_pings[i].callsign) != 0) {
            g_canvas->setTextColor(labelCol(g_flightRegColor565));
            int len = strlen(g_pings[i].reg);
            g_canvas->setCursor(g_pings[i].x - (len * 3), lineY);
            g_canvas->print(g_pings[i].reg);
            lineY += 9;
        }

        if (g_showFlightType && g_pings[i].acType[0] != '\0') {
            g_canvas->setTextColor(labelCol(g_flightTypeColor565));
            int len = strlen(g_pings[i].acType);
            g_canvas->setCursor(g_pings[i].x - (len * 3), lineY);
            g_canvas->print(g_pings[i].acType);
        }
    }
}

// ---------------------------------------------------------------------------
// Clock (bottom-left, AEDT)
// ---------------------------------------------------------------------------

static void drawClock() {
    struct tm timeinfo;
    if (!getLocalTime(&timeinfo)) return;  // NTP not yet synced

    char buf[6];
    snprintf(buf, sizeof(buf), "%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min);

    uint16_t col = color565(0, 60, 0);
    g_canvas->setTextColor(col);
    g_canvas->setTextSize(2);

    // Centre angle and arc direction for each clock position:
    //   0=Bottom-Left(SW,225°)  1=Bottom-Right(SE,135°)
    //   2=Top-Left(NW,315°)     3=Top-Right(NE,45°)
    // Bottom positions arc counterclockwise (dir=-1) so text reads left→right.
    // Top positions arc clockwise (dir=+1) for the same visual result.
    static const float k_centre[4] = { 225.0f, 135.0f, 315.0f,  45.0f };
    static const float k_dir[4]    = {  -1.0f,  -1.0f,  +1.0f, +1.0f };

    const int   len     = strlen(buf);
    const float spacing = 10.0f;
    const float arcR    = 96.0f;
    float centre    = k_centre[g_clockPosition];
    float dir       = k_dir[g_clockPosition];
    float halfSpan  = ((len - 1) / 2.0f) * spacing;
    float startAngle = centre - dir * halfSpan;

    for (int i = 0; i < len; i++) {
        float angle = startAngle + dir * i * spacing;
        float rad   = toRad(angle);
        int   px    = (int)roundf(CENTRE_X + arcR * sinf(rad));
        int   py    = (int)roundf(CENTRE_Y - arcR * cosf(rad));
        g_canvas->setCursor(px - 6, py - 8);
        g_canvas->print(buf[i]);
    }
}

// ---------------------------------------------------------------------------
// Wind widget
// ---------------------------------------------------------------------------

void displayUpdateWind(const WindData &wind) {
    g_wind = wind;
}

static void drawWindWidget() {
    if (!g_wind.valid) return;

    uint16_t col = color565(0, 60, 0);  // match clock / range labels

    // Arrow and text share centre angle 188° — sits just clockwise of the clock's last char (207°)
    float arrowDeg = g_wind.dirDeg;
    float arrowRad = toRad(arrowDeg);
    float sinA = sinf(arrowRad), cosA = cosf(arrowRad);

    float acRad = toRad(192.0f);
    int acx = (int)roundf(CENTRE_X + 90.0f * sinf(acRad));
    int acy = (int)roundf(CENTRE_Y - 90.0f * cosf(acRad));

    int tailX = (int)roundf(acx - 6.0f * sinA);
    int tailY = (int)roundf(acy + 6.0f * cosA);
    int tipX  = (int)roundf(acx + 6.0f * sinA);
    int tipY  = (int)roundf(acy - 6.0f * cosA);
    g_canvas->drawLine(tailX, tailY, tipX, tipY, col);

    float h1Rad = toRad(arrowDeg + 150.0f);
    float h2Rad = toRad(arrowDeg - 150.0f);
    g_canvas->drawLine(tipX, tipY,
        (int)roundf(tipX + 4.0f * sinf(h1Rad)),
        (int)roundf(tipY - 4.0f * cosf(h1Rad)), col);
    g_canvas->drawLine(tipX, tipY,
        (int)roundf(tipX + 4.0f * sinf(h2Rad)),
        (int)roundf(tipY - 4.0f * cosf(h2Rad)), col);

    int speedKt = (int)roundf(g_wind.speedKt);

    char buf[6];
    snprintf(buf, sizeof(buf), "%dkt", speedKt);

    g_canvas->setTextColor(col);
    g_canvas->setTextSize(1);

    int         len       = strlen(buf);
    const float spacing  = 7.0f;
    const float ktGap    = 4.0f;  // tighter gap between 'k' and 't'
    float totalSpan = (len > 2) ? ((len - 2) * spacing + ktGap) : ((len - 1) * spacing);
    float angle     = 192.0f + totalSpan / 2.0f;

    for (int i = 0; i < len; i++) {
        float rad = toRad(angle);
        int   px  = (int)roundf(CENTRE_X + 104.0f * sinf(rad));
        int   py  = (int)roundf(CENTRE_Y - 104.0f * cosf(rad));
        g_canvas->setCursor(px - 3, py - 4);
        g_canvas->print(buf[i]);
        angle -= (i == len - 2) ? ktGap : spacing;
    }
}

// ---------------------------------------------------------------------------
// Public render entry point
// ---------------------------------------------------------------------------

void displayRenderFrame(Adafruit_GC9A01A &tft,
                        const Plane *planes, int planeCount,
                        float homeLat, float homeLon, float radiusKm) {
    g_canvas->fillScreen(0x0000);

    drawSweepTrail();
    drawSweepLine();
    drawRadarGrid(homeLat, homeLon, radiusKm);
    expirePings();
    checkSweepHitsPlanes(planes, planeCount, homeLat, homeLon, radiusKm);
    fadePings();
    if (g_showTrail) drawTrails();
    drawPingIcons();
    drawPingLabels();
    if (g_showClock) drawClock();
    if (g_showWind)  drawWindWidget();

    tft.drawRGBBitmap(0, 0, g_canvas->getBuffer(), 240, 240);

    g_sweep += SWEEP_SPEED;
    if (g_sweep >= 360.0f) g_sweep -= 360.0f;
}
