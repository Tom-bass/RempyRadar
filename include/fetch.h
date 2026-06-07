#pragma once

#include "radar.h"
#include "storage.h"

// Connect to WiFi and start background fetch + watchdog tasks.
void fetchInit(const DeviceConfig &cfg);

// Copy staging plane data to dest if a new fetch has completed.
// Returns true if data was consumed.
bool fetchConsumeStagingIfReady(Plane *dest, int *count);

// Copy latest wind data to dest if a new fetch has completed.
// Returns true if data was consumed.
bool fetchConsumeWindIfReady(WindData *dest);

// Returns true after 3 consecutive geocoding failures (bad address).
bool fetchGeocodeFailed();

// Returns true (once) after an address is resolved to coordinates.
// Copies the resolved lat/lon into the provided pointers.
bool fetchGeocodeReady(float *lat, float *lon);

// Apply updated location/radius/airport settings to the running fetch task.
// Does not reconnect WiFi — call only when WiFi credentials are unchanged.
void fetchUpdateConfig(const DeviceConfig &cfg);
