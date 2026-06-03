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

// Set device compass heading (degrees, 0-360) so north stays at top of the display.
void displaySetNorthOffset(float deg);

// Show a retro status popup over the radar. durationMs=0 keeps it until replaced.
void displaySetOtaStatus(const char *msg, unsigned long durationMs = 0);


// Render a complete frame and push it to the display.
void displayRenderFrame(Adafruit_GC9A01A &tft,
                        const Plane *planes, int planeCount,
                        float homeLat, float homeLon, float radiusKm);
