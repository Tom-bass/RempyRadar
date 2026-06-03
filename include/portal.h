#pragma once

// Start the captive-portal access point and serve the setup web UI.
// Blocks forever — the device reboots once the user saves their config.
void portalBegin();

// Start a non-blocking settings server on the station IP (call after WiFi connects).
void portalStartSettingsServer();

// Process any pending HTTP requests — call from loop() every frame.
void portalHandleClient();

// Returns true once after settings have been saved (auto-clears).
bool portalConfigChanged();

// Returns true when a restart has been requested and enough time has elapsed
// for the HTTP response to be delivered to the client.
bool portalRestartPending();

// Returns true once when the user has requested an accelerated calibration spin.
bool portalMagCalPending();

// Returns true once when the user has tapped "Set North" (device pointing north).
bool portalSetNorthPending();

// Returns true once when a new mounting-angle correction has been submitted.
bool portalMagCorrectionPending(float &outDeg);
