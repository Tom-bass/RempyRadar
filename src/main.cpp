#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <Adafruit_GC9A01A.h>
#include <Adafruit_MMC56x3.h>
#include <esp_heap_caps.h>

#include "config.h"
#include "storage.h"
#include "display.h"
#include "fetch.h"
#include "radar.h"
#include "portal.h"
#include "ota.h"

static Adafruit_GC9A01A tft(TFT_CS, TFT_DC, TFT_RST);
static Adafruit_MMC5603 mag(12345);

static DeviceConfig config;
static Plane        planesLive[MAX_PLANES];
static int          planeCountLive = 0;

// Hard-iron calibration: running min/max of raw X/Y field, persisted in NVS.
// Offsets are recomputed as midpoints whenever the observed range is large enough.
static float g_calMinX, g_calMaxX, g_calMinY, g_calMaxY;
static float g_magOffsetX = 0.0f, g_magOffsetY = 0.0f;
static bool  g_calReady   = false;  // true once range is large enough to trust

// Mounting-angle correction: the single offset that maps sensor axes to display north.
// Saved by "Set North" — user points a reference edge north, taps once.
static float g_magCorrection  = 0.0f;
static bool  g_northCalibrated = false;  // false = lock north to top until Set North is done

// Accelerated calibration spin (user-triggered 360° rotation)
static bool          g_calSpinning = false;
static unsigned long g_calSpinStart = 0;
static const unsigned long CAL_DURATION_MS = 20000;

// Minimum field range (µT) needed before we trust hard-iron offsets.
// Earth's horizontal field in Australia is ~25 µT; a full 360° gives ~50 µT span.
static const float CAL_MIN_SPAN = 15.0f;

void setup() {
    Serial.begin(115200);
    delay(500);

    heap_caps_malloc_extmem_enable(4096);

    otaInit();
    displayInit(tft);

    if (!storageLoad(config)) {
        displayShowSetupScreen(tft);
        portalBegin();
    }

    displayApplyConfig(config);

    if (config.showAirports) {
        Airport airports[MAX_AIRPORTS];
        int count = 0;
        storageLoadAirports(airports, &count, config.homeLat, config.homeLon);
        if (count > 0) displaySetAirports(airports, count);
    }

    // Load persisted calibration range
    storageLoadMagCal(g_calMinX, g_calMaxX, g_calMinY, g_calMaxY);
    float sx = g_calMaxX - g_calMinX, sy = g_calMaxY - g_calMinY;
    if (sx > CAL_MIN_SPAN && sy > CAL_MIN_SPAN) {
        g_magOffsetX = (g_calMaxX + g_calMinX) / 2.0f;
        g_magOffsetY = (g_calMaxY + g_calMinY) / 2.0f;
        g_calReady   = true;
    }

    g_magCorrection   = storageLoadMagCorrection();
    g_northCalibrated = storageLoadMagNorthSet();
    Serial.printf("Mag cal: offsetX=%.2f offsetY=%.2f correction=%.1f ready=%d northSet=%d\n",
                  g_magOffsetX, g_magOffsetY, g_magCorrection, g_calReady, g_northCalibrated);

    Wire.begin();
    if (mag.begin(0x30, &Wire)) {
        mag.setDataRate(100);
        Serial.println("MMC5603 ready");
    } else {
        Serial.println("MMC5603 failed — check wiring");
    }

    displayShowConnecting(tft);
    fetchInit(config);
    configTzTime(config.timezone, "pool.ntp.org");

    if (WiFi.status() == WL_CONNECTED) {
        portalStartSettingsServer();
        displayShowConnected(tft, WiFi.localIP().toString());
        delay(3000);
    }
}

void loop() {
    portalHandleClient();

    if (portalConfigChanged()) {
        storageLoad(config);
        displayApplyConfig(config);
        fetchUpdateConfig(config);
        configTzTime(config.timezone, "pool.ntp.org");
    }

    if (portalRestartPending()) ESP.restart();

    if (fetchGeocodeFailed()) {
        displayShowGeocodeError(tft);
        delay(4000);
        portalBegin();
    }

    float newCorr;
    if (portalMagCorrectionPending(newCorr)) {
        g_magCorrection = newCorr;
        storageSaveMagCorrection(newCorr);
    }

    static float         smoothedHeading = -1.0f;
    static unsigned long lastFrame       = 0;
    static unsigned long lastCalSave     = 0;
    static bool          calDirty        = false;
    unsigned long now = millis();

    // ---- Magnetometer: read, calibrate, compute heading ----
    {
        sensors_event_t event;
        mag.getEvent(&event);
        float rx = event.magnetic.x;
        float ry = event.magnetic.y;

        // Initialise running min/max from first reading if we have no saved data
        static bool firstReading = true;
        if (firstReading) {
            firstReading = false;
            if (!g_calReady) {
                g_calMinX = g_calMaxX = rx;
                g_calMinY = g_calMaxY = ry;
            }
        }

        // Accelerated spin calibration (user-triggered): resets range for a clean run
        if (portalMagCalPending()) {
            g_calSpinning  = true;
            g_calSpinStart = now;
            g_calMinX = g_calMaxX = rx;
            g_calMinY = g_calMaxY = ry;
            g_calReady = false;
            smoothedHeading = -1.0f;
            displaySetOtaStatus("Rotate 360\xb0 slowly...", 0);
            Serial.println("Mag calibration started");
        }

        // Continuous background update of min/max
        if (rx < g_calMinX) { g_calMinX = rx; calDirty = true; }
        if (rx > g_calMaxX) { g_calMaxX = rx; calDirty = true; }
        if (ry < g_calMinY) { g_calMinY = ry; calDirty = true; }
        if (ry > g_calMaxY) { g_calMaxY = ry; calDirty = true; }

        float spanX = g_calMaxX - g_calMinX;
        float spanY = g_calMaxY - g_calMinY;
        if (spanX > CAL_MIN_SPAN && spanY > CAL_MIN_SPAN) {
            g_magOffsetX = (g_calMaxX + g_calMinX) / 2.0f;
            g_magOffsetY = (g_calMaxY + g_calMinY) / 2.0f;
            g_calReady   = true;
        }

        // Spin calibration: countdown popup and completion
        if (g_calSpinning) {
            int secsLeft = (int)((CAL_DURATION_MS - (now - g_calSpinStart)) / 1000) + 1;
            char msg[32];
            snprintf(msg, sizeof(msg), "Rotate... %ds", secsLeft);
            displaySetOtaStatus(msg, 0);
            if (now - g_calSpinStart >= CAL_DURATION_MS) {
                g_calSpinning = false;
                storageSaveMagCal(g_calMinX, g_calMaxX, g_calMinY, g_calMaxY);
                calDirty = false;
                lastCalSave = now;
                displaySetOtaStatus("Calibration saved!", 4000);
                Serial.printf("Spin cal done: offsetX=%.2f offsetY=%.2f\n",
                              g_magOffsetX, g_magOffsetY);
            }
        }

        // Periodically persist background calibration (every 60 s, only if changed)
        if (calDirty && g_calReady && (now - lastCalSave > 60000)) {
            storageSaveMagCal(g_calMinX, g_calMaxX, g_calMinY, g_calMaxY);
            calDirty    = false;
            lastCalSave = now;
        }

        // Apply hard-iron offsets then compute raw heading
        float cx = rx - g_magOffsetX;
        float cy = ry - g_magOffsetY;
        float rawHeading = atan2f(cy, cx) * (180.0f / M_PI);
        if (rawHeading < 0.0f)    rawHeading += 360.0f;
        if (rawHeading >= 360.0f) rawHeading -= 360.0f;

        // "Set North": capture current orientation as the north reference
        if (portalSetNorthPending()) {
            g_magCorrection   = -rawHeading;
            if (g_magCorrection < -180.0f) g_magCorrection += 360.0f;
            g_northCalibrated = true;
            storageSaveMagCorrection(g_magCorrection);
            storageSaveMagNorthSet(true);
            displaySetOtaStatus("North set!", 3000);
            Serial.printf("North set: correction=%.1f\n", g_magCorrection);
            smoothedHeading = -1.0f;
        }

        // Apply mounting-angle correction
        float heading = rawHeading + g_magCorrection;
        if (heading < 0.0f)    heading += 360.0f;
        if (heading >= 360.0f) heading -= 360.0f;

        // Exponential smoothing with circular interpolation
        if (smoothedHeading < 0.0f) {
            smoothedHeading = heading;
        } else {
            float diff = heading - smoothedHeading;
            if (diff >  180.0f) diff -= 360.0f;
            if (diff < -180.0f) diff += 360.0f;
            smoothedHeading += 0.05f * diff;
            if (smoothedHeading < 0.0f)    smoothedHeading += 360.0f;
            if (smoothedHeading >= 360.0f) smoothedHeading -= 360.0f;
        }
        displaySetNorthOffset(g_northCalibrated ? smoothedHeading : 0.0f);
    }

    if (now - lastFrame < FRAME_MS) return;
    lastFrame = now;

    fetchConsumeStagingIfReady(planesLive, &planeCountLive);

    WindData wind;
    if (fetchConsumeWindIfReady(&wind)) displayUpdateWind(wind);

    displayRenderFrame(tft, planesLive, planeCountLive,
                       config.homeLat, config.homeLon, config.radiusKm);
}
