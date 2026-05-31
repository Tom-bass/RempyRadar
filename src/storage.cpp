#include "storage.h"
#include <Preferences.h>
#include <string.h>
#include <math.h>

static const char *NVS_NAMESPACE = "radar";

bool storageLoad(DeviceConfig &cfg) {
    memset(&cfg, 0, sizeof(cfg));
    cfg.radiusKm      = 25.0f;
    cfg.showWindWidget = true;
    cfg.showClock      = true;
    cfg.showTrail      = true;
    cfg.showAltColors  = true;
    cfg.showAirports      = true;
    cfg.showAirportNames  = true;
    cfg.showWaterways     = true;
    cfg.airportColor             = { 120, 120, 0 };
    cfg.showClimbDescent  = true;
    cfg.showFlightNumber  = true;
    cfg.showFlightReg          = false;
    cfg.showFlightType         = false;
    cfg.flightNumColor           = { 0, 220, 0 };
    cfg.flightRegColor           = { 0, 220, 0 };
    cfg.flightTypeColor          = { 0, 220, 0 };
    cfg.altPalette     = ALT_PALETTE_CLASSIC;
    strncpy(cfg.timezone, "UTC0", sizeof(cfg.timezone));
    cfg.clockPosition  = 0;
    cfg.showNorth      = true;
    cfg.northColor     = { 255,   0,   0 };
    cfg.ringColor      = {   0,  60,   0 };
    cfg.sweepColor     = {   0, 255,   0 };

    Preferences prefs;
    prefs.begin(NVS_NAMESPACE, true);
    cfg.configured = prefs.getBool("configured", false);

    if (cfg.configured) {
        prefs.getString("ssid",      cfg.wifiSSID, sizeof(cfg.wifiSSID));
        prefs.getString("pass",      cfg.wifiPass, sizeof(cfg.wifiPass));
        cfg.homeLat        = prefs.getFloat("lat",        0.0f);
        cfg.homeLon        = prefs.getFloat("lon",        0.0f);
        cfg.radiusKm       = prefs.getFloat("radius",     25.0f);
        cfg.showWindWidget = prefs.getBool("showWind",     true);
        cfg.showClock      = prefs.getBool("showClock",   true);
        cfg.showTrail      = prefs.getBool("showTrail",   true);
        cfg.showAltColors  = prefs.getBool("showAltClr",  true);
        cfg.showAirports      = prefs.getBool("showAirports",     true);
        cfg.showAirportNames  = prefs.getBool("showAirportNames", true);
        cfg.showWaterways     = prefs.getBool("showWaterways",    true);
        cfg.airportColor             = { 120, 120, 0 };
        prefs.getBytes("apColor",     &cfg.airportColor,   sizeof(RGBColor));
        cfg.showClimbDescent  = prefs.getBool("showClimbDesc",    true);
        cfg.showFlightNumber  = prefs.getBool("showFlightNum",    true);
        cfg.showFlightReg          = prefs.getBool("showFlightReg",      false);
        cfg.showFlightType         = prefs.getBool("showFlightType",     false);
        cfg.flightNumColor           = { 0, 220, 0 };
        prefs.getBytes("flNumColor",  &cfg.flightNumColor,  sizeof(RGBColor));
        cfg.flightRegColor           = { 0, 220, 0 };
        prefs.getBytes("flRegColor",  &cfg.flightRegColor,  sizeof(RGBColor));
        cfg.flightTypeColor          = { 0, 220, 0 };
        prefs.getBytes("flTypColor",  &cfg.flightTypeColor, sizeof(RGBColor));
        cfg.altPalette     = (uint8_t)prefs.getUChar("altPalette", ALT_PALETTE_CLASSIC);
        prefs.getString("address",  cfg.homeAddress, sizeof(cfg.homeAddress));
        prefs.getString("timezone", cfg.timezone,    sizeof(cfg.timezone));
        cfg.clockPosition  = (uint8_t)prefs.getUChar("clockPos", 0);
        cfg.showNorth      = prefs.getBool("showNorth", true);
        cfg.northColor     = { 255, 0, 0 };
        prefs.getBytes("northColor",  &cfg.northColor,  sizeof(RGBColor));
        cfg.ringColor      = { 0, 60, 0 };
        prefs.getBytes("ringColor",   &cfg.ringColor,   sizeof(RGBColor));
        cfg.sweepColor     = { 0, 255, 0 };
        prefs.getBytes("sweepColor",  &cfg.sweepColor,  sizeof(RGBColor));
        prefs.getBytes("customAlt", cfg.customAltColors, sizeof(cfg.customAltColors));
        if (cfg.timezone[0] == '\0') strncpy(cfg.timezone, "UTC0", sizeof(cfg.timezone));
    }

    prefs.end();
    return cfg.configured;
}

void storageSave(const DeviceConfig &cfg) {
    Preferences prefs;
    prefs.begin(NVS_NAMESPACE, false);
    prefs.putString("ssid",      cfg.wifiSSID);
    prefs.putString("pass",      cfg.wifiPass);
    prefs.putFloat("lat",        cfg.homeLat);
    prefs.putFloat("lon",        cfg.homeLon);
    prefs.putFloat("radius",     cfg.radiusKm);
    prefs.putBool("showWind",     cfg.showWindWidget);
    prefs.putBool("showClock",   cfg.showClock);
    prefs.putBool("showTrail",   cfg.showTrail);
    prefs.putBool("showAltClr",  cfg.showAltColors);
    prefs.putBool("showAirports",    cfg.showAirports);
    prefs.putBool("showAirportNames",cfg.showAirportNames);
    prefs.putBool("showWaterways",   cfg.showWaterways);
    prefs.putBytes("apColor",        &cfg.airportColor,   sizeof(RGBColor));
    prefs.putBool("showClimbDesc",   cfg.showClimbDescent);
    prefs.putBool("showFlightNum",   cfg.showFlightNumber);
    prefs.putBool("showFlightReg",   cfg.showFlightReg);
    prefs.putBool("showFlightType",  cfg.showFlightType);
    prefs.putBytes("flNumColor",   &cfg.flightNumColor,  sizeof(RGBColor));
    prefs.putBytes("flRegColor",   &cfg.flightRegColor,  sizeof(RGBColor));
    prefs.putBytes("flTypColor",   &cfg.flightTypeColor, sizeof(RGBColor));
    prefs.putString("address",   cfg.homeAddress);
    prefs.putString("timezone",  cfg.timezone);
    prefs.putUChar("clockPos",   cfg.clockPosition);
    prefs.putBool("showNorth",   cfg.showNorth);
    prefs.putBytes("northColor", &cfg.northColor,  sizeof(RGBColor));
    prefs.putBytes("ringColor",  &cfg.ringColor,   sizeof(RGBColor));
    prefs.putBytes("sweepColor", &cfg.sweepColor,  sizeof(RGBColor));
    prefs.putUChar("altPalette", cfg.altPalette);
    prefs.putBytes("customAlt",  cfg.customAltColors, sizeof(cfg.customAltColors));
    prefs.putBool("configured",  true);
    prefs.end();
}

void storageErase() {
    Preferences prefs;
    prefs.begin(NVS_NAMESPACE, false);
    prefs.clear();
    prefs.end();
}

void storageLoadAirports(Airport *airports, int *count, float currentLat, float currentLon) {
    Preferences prefs;
    prefs.begin(NVS_NAMESPACE, true);
    *count = (int)prefs.getInt("apCount", 0);
    if (*count > 0) {
        float storedLat = prefs.getFloat("apLat", currentLat);
        float storedLon = prefs.getFloat("apLon", currentLon);
        float dlat = currentLat - storedLat;
        float dlon = (currentLon - storedLon) * cosf(storedLat * 3.14159265f / 180.0f);
        float distKm = sqrtf(dlat * dlat + dlon * dlon) * 111.0f;
        if (distKm > 10.0f) {
            *count = 0;  // stale — discard; fetchAirports() will repopulate
        } else {
            prefs.getBytes("apData", airports, sizeof(Airport) * (*count));
        }
    }
    prefs.end();
}

void storageSaveAirports(const Airport *airports, int count, float lat, float lon) {
    Preferences prefs;
    prefs.begin(NVS_NAMESPACE, false);
    prefs.putInt("apCount", count);
    if (count > 0) {
        prefs.putBytes("apData", airports, sizeof(Airport) * count);
        prefs.putFloat("apLat", lat);
        prefs.putFloat("apLon", lon);
    }
    prefs.end();
}
