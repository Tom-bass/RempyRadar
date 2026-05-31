#pragma once

#include <Adafruit_GC9A01A.h>
#include <Adafruit_GFX.h>
#include "radar.h"
#include "storage.h"

// Initialise display hardware and allocate canvas.
void displayInit(Adafruit_GC9A01A &tft);

// Apply user config (palette, show/hide flags). Call after storageLoad().
void displayApplyConfig(const DeviceConfig &cfg);

// Update the airport landmarks drawn on the radar grid.
void displaySetAirports(const Airport *airports, int count);

// Splash screens shown before the radar starts.
void displayShowConnecting(Adafruit_GC9A01A &tft);
void displayShowSetupScreen(Adafruit_GC9A01A &tft);
void displayShowGeocodeError(Adafruit_GC9A01A &tft);
void displayShowConnected(Adafruit_GC9A01A &tft, const String &ip);

// Update the cached wind reading shown in the corner widget.
void displayUpdateWind(const WindData &wind);

#ifdef FEATURE_WATERWAYS
// Store waterway polylines for rendering. Called once after fetch.
void displaySetWaterways(const WaterPolyline *polys, int count);
#endif

// Render a complete frame and push it to the display.
void displayRenderFrame(Adafruit_GC9A01A &tft,
                        const Plane *planes, int planeCount,
                        float homeLat, float homeLon, float radiusKm);
