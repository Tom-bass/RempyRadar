#pragma once
#include <stdint.h>

struct RGBColor { uint8_t r, g, b; };

// Altitude palette indices
enum AltPalette : uint8_t {
    ALT_PALETTE_CLASSIC  = 0,
    ALT_PALETTE_FIRE     = 1,
    ALT_PALETTE_OCEAN    = 2,
    ALT_PALETTE_MONO     = 3,
    ALT_PALETTE_CUSTOM   = 4,
};

struct DeviceConfig {
    char     wifiSSID[64];
    char     wifiPass[64];
    float    homeLat;
    float    homeLon;
    float    radiusKm;
    bool     configured;
    bool     showWindWidget;
    bool     showClock;
    bool     showTrail;
    bool     showAltColors;        // false = all aircraft render as plain green
    bool     showAirports;         // false = skip airport fetch and landmarks
    bool     showAirportNames;     // false = dot only, no IATA/ICAO label
    bool     showClimbDescent;    // show +/- climb/descent indicator after callsign
    bool     showFlightNumber;    // show callsign / flight number label on pings
    bool     showFlightReg;       // show aircraft registration below flight number
    bool     showFlightType;      // show ICAO type code (e.g. B738) below registration
    RGBColor airportColor;     // colour of airport dots and labels
    RGBColor flightNumColor;   // label colour for flight number
    RGBColor flightRegColor;   // label colour for registration
    RGBColor flightTypeColor;  // label colour for aircraft type
    uint8_t  altPalette;           // AltPalette enum value
    RGBColor customAltColors[10];  // used when altPalette == ALT_PALETTE_CUSTOM
    char     homeAddress[128];     // if non-empty and lat/lon==0, geocode on first WiFi connect
    char     timezone[64];         // POSIX TZ string e.g. "AEST-10AEDT,M10.1.0,M4.1.0/3"
    uint8_t  clockPosition;        // 0=BL 1=BR 2=TL 3=TR
    bool     showNorth;            // show "N" north indicator
    RGBColor northColor;           // colour of the N
    RGBColor ringColor;            // radar rings, crosshair lines, range labels
    RGBColor sweepColor;           // sweep line and trail
};

// Load config from NVS. Returns true if a saved config exists.
bool storageLoad(DeviceConfig &cfg);

// Persist config to NVS.
void storageSave(const DeviceConfig &cfg);

// Wipe all stored config (factory reset).
void storageErase();

// Magnetometer hard-iron calibration — stores the observed field range so
// continuous background calibration survives reboots.
void storageSaveMagCal(float minX, float maxX, float minY, float maxY);
void storageLoadMagCal(float &minX, float &maxX, float &minY, float &maxY);

// Magnetometer mounting-angle correction (degrees, added to raw heading).
void  storageSaveMagCorrection(float deg);
float storageLoadMagCorrection();

// True once the user has performed the "Set North" one-time alignment.
void storageSaveMagNorthSet(bool v);
bool storageLoadMagNorthSet();

// Airport cache — auto-fetched, stored separately from user config.
// storageLoadAirports discards the cache if the stored location differs from
// currentLat/currentLon by more than 10 km, preventing stale airports showing
// after a location change.
#include "radar.h"
void storageLoadAirports(Airport *airports, int *count, float currentLat, float currentLon);
void storageSaveAirports(const Airport *airports, int count, float lat, float lon);
