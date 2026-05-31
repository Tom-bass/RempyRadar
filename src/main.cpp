#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <Adafruit_GC9A01A.h>
#include <Adafruit_MPU6050.h>
#include <esp_heap_caps.h>

#include "config.h"
#include "storage.h"
#include "display.h"
#include "fetch.h"
#include "radar.h"
#include "portal.h"

static Adafruit_GC9A01A tft(TFT_CS, TFT_DC, TFT_RST);
static Adafruit_MPU6050 mpu;

static DeviceConfig config;
static Plane        planesLive[MAX_PLANES];
static int          planeCountLive = 0;

void setup() {
    Serial.begin(115200);
    delay(500);

    // Push allocations >4KB to PSRAM, keeping internal RAM free for TLS
    heap_caps_malloc_extmem_enable(4096);

    displayInit(tft);

    if (!storageLoad(config)) {
        // No config in NVS — start captive portal so user can set up the device.
        // portalBegin() never returns; the device reboots once config is saved.
        displayShowSetupScreen(tft);
        portalBegin();
    }

    displayApplyConfig(config);

    // Load cached airports so landmarks appear from the first frame
    if (config.showAirports) {
        Airport airports[MAX_AIRPORTS];
        int count = 0;
        storageLoadAirports(airports, &count, config.homeLat, config.homeLon);
        if (count > 0) displaySetAirports(airports, count);
    }

    Wire.begin();
    if (mpu.begin()) Serial.println("MPU6050 ready");
    else             Serial.println("MPU6050 failed");

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

    if (portalRestartPending()) {
        ESP.restart();
    }

    if (fetchGeocodeFailed()) {
        displayShowGeocodeError(tft);
        delay(4000);
        portalBegin();
    }

    static unsigned long lastFrame = 0;
    unsigned long now = millis();
    if (now - lastFrame < FRAME_MS) return;
    lastFrame = now;

    fetchConsumeStagingIfReady(planesLive, &planeCountLive);

    WindData wind;
    if (fetchConsumeWindIfReady(&wind)) displayUpdateWind(wind);

    displayRenderFrame(tft, planesLive, planeCountLive,
                       config.homeLat, config.homeLon, config.radiusKm);
}
