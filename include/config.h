#pragma once

// --- Display pins (XIAO ESP32-S3 → Adafruit GC9A01A via EYESPI) ---
#define TFT_CS   D3
#define TFT_DC   D1
#define TFT_RST  D0

// --- Radar display geometry ---
#define CENTRE_X  120
#define CENTRE_Y  120
#define RADAR_R   115

// --- Timing ---
#define FRAME_MS  20

// --- Pool sizes ---
#define MAX_PLANES  40
#define MAX_PINGS   40

// --- Sweep ---
constexpr float SWEEP_SPEED = 1.0f;

// --- Fetch ---
constexpr unsigned long FETCH_INTERVAL_MS      = 20000UL;
constexpr unsigned long WATCHDOG_TIMEOUT_MS    = 120000UL;
constexpr int           REBOOT_AFTER_N_FETCHES = 4320;

// --- Wind fetch ---
constexpr int WIND_FETCH_EVERY_N_FETCHES = 30;  // ~10 min at 20s intervals

// --- OTA update source ---
#define OTA_GITHUB_OWNER "Tom-bass"
#define OTA_GITHUB_REPO  "RempyRadar"

// Injected by scripts/version.py at build time; falls back to "dev" for local builds.
#ifndef FIRMWARE_VERSION
#define FIRMWARE_VERSION "dev"
#endif

