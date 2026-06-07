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
static float g_calMinX, g_calMaxX, g_calMinY, g_calMaxY, g_calMinZ, g_calMaxZ;
static float g_magOffsetX = 0.0f, g_magOffsetZ = 0.0f;
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
    storageLoadMagCal(g_calMinX, g_calMaxX, g_calMinY, g_calMaxY, g_calMinZ, g_calMaxZ);
    float sx = g_calMaxX - g_calMinX, sz = g_calMaxZ - g_calMinZ;
    if (sx > CAL_MIN_SPAN && sz > CAL_MIN_SPAN) {
        g_magOffsetX = (g_calMaxX + g_calMinX) / 2.0f;
        g_magOffsetZ = (g_calMaxZ + g_calMinZ) / 2.0f;
        g_calReady   = true;
    }

    g_magCorrection   = storageLoadMagCorrection();
    g_northCalibrated = storageLoadMagNorthSet();
    Serial.printf("Mag cal: offsetX=%.2f offsetZ=%.2f correction=%.1f ready=%d northSet=%d\n",
                  g_magOffsetX, g_magOffsetZ, g_magCorrection, g_calReady, g_northCalibrated);

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

    {
        float lat, lon;
        if (fetchGeocodeReady(&lat, &lon)) {
            config.homeLat = lat;
            config.homeLon = lon;
        }
    }

    float newCorr;
    if (portalMagCorrectionPending(newCorr)) {
        g_magCorrection = newCorr;
        storageSaveMagCorrection(newCorr);
    }

    bool compassRotateEnabled;
    if (portalCompassRotatePending(compassRotateEnabled)) {
        config.compassRotate = compassRotateEnabled;
        storageSave(config);
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
        float rz = event.magnetic.z;

        // Initialise running min/max from first reading if we have no saved data
        static bool firstReading = true;
        if (firstReading) {
            firstReading = false;
            if (!g_calReady) {
                g_calMinX = g_calMaxX = rx;
                g_calMinY = g_calMaxY = ry;
                g_calMinZ = g_calMaxZ = rz;
            }
        }

        // Accelerated spin calibration (user-triggered): resets range for a clean run
        if (portalMagCalPending()) {
            g_calSpinning  = true;
            g_calSpinStart = now;
            g_calMinX = g_calMaxX = rx;
            g_calMinY = g_calMaxY = ry;
            g_calMinZ = g_calMaxZ = rz;
            g_calReady = false;
            smoothedHeading = -1.0f;
            displaySetOtaStatus("Rotate 360\xb0 slowly...", 0);
            Serial.println("Mag calibration started");
        }

        // Update min/max only during spin calibration or before first good calibration.
        // Once calibrated, hard-iron bias is fixed to the device hardware and does not
        // change as the device moves around. Continuous updates would allow magnetic
        // anomalies in the environment (metal furniture, electronics) to corrupt the
        // offset and cause persistent heading drift.
        bool updateCal = g_calSpinning || !g_calReady;

        // During spin: reject obvious outliers (field strength > 2× expected radius
        // from current centre) so a momentary twitch near metal doesn't corrupt the range.
        if (updateCal && g_calSpinning && g_calReady) {
            float radX = (g_calMaxX - g_calMinX) * 0.5f;
            float radZ = (g_calMaxZ - g_calMinZ) * 0.5f;
            if (radX > 1.0f && fabsf(rx - g_magOffsetX) > radX * 2.5f) updateCal = false;
            if (radZ > 1.0f && fabsf(rz - g_magOffsetZ) > radZ * 2.5f) updateCal = false;
        }

        if (updateCal) {
            if (rx < g_calMinX) { g_calMinX = rx; calDirty = true; }
            if (rx > g_calMaxX) { g_calMaxX = rx; calDirty = true; }
            if (ry < g_calMinY) { g_calMinY = ry; calDirty = true; }
            if (ry > g_calMaxY) { g_calMaxY = ry; calDirty = true; }
            if (rz < g_calMinZ) { g_calMinZ = rz; calDirty = true; }
            if (rz > g_calMaxZ) { g_calMaxZ = rz; calDirty = true; }
        }

        float spanX = g_calMaxX - g_calMinX;
        float spanZ = g_calMaxZ - g_calMinZ;
        if (spanX > CAL_MIN_SPAN && spanZ > CAL_MIN_SPAN) {
            g_magOffsetX = (g_calMaxX + g_calMinX) / 2.0f;
            g_magOffsetZ = (g_calMaxZ + g_calMinZ) / 2.0f;
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
                storageSaveMagCal(g_calMinX, g_calMaxX, g_calMinY, g_calMaxY, g_calMinZ, g_calMaxZ);
                calDirty = false;
                lastCalSave = now;
                displaySetOtaStatus("Calibration saved!", 4000);
                Serial.printf("Spin cal done: offsetX=%.2f offsetZ=%.2f\n",
                              g_magOffsetX, g_magOffsetZ);
            }
        }

        // Persist calibration changes (only relevant when building up from uncalibrated state)
        if (calDirty && g_calReady && (now - lastCalSave > 60000)) {
            storageSaveMagCal(g_calMinX, g_calMaxX, g_calMinY, g_calMaxY, g_calMinZ, g_calMaxZ);
            calDirty    = false;
            lastCalSave = now;
        }

        // Apply hard-iron offsets then compute raw heading using X and Z axes
        // (magnetometer PCB is mounted on its side so Z is horizontal, Y is vertical)
        float cx = rx - g_magOffsetX;
        float cz = rz - g_magOffsetZ;
        float rawHeading = atan2f(cz, cx) * (180.0f / M_PI);
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
        displaySetNorthOffset((g_northCalibrated && config.compassRotate) ? smoothedHeading : 0.0f);
    }

    if (now - lastFrame < FRAME_MS) return;
    lastFrame = now;

    fetchConsumeStagingIfReady(planesLive, &planeCountLive);

    WindData wind;
    if (fetchConsumeWindIfReady(&wind)) displayUpdateWind(wind);

    displayRenderFrame(tft, planesLive, planeCountLive,
                       config.homeLat, config.homeLon, config.radiusKm);
}
