#pragma once

// Call once in setup() — marks the running firmware as valid, preventing bootloader rollback.
void otaInit();

// Call from fetchTask each cycle. Checks GitHub for a new release at 3am local time (once per day).
// If a newer .bin is found, downloads and flashes it, then reboots. Returns immediately otherwise.
void otaCheckIfDue();

// Trigger an immediate check on the next fetchTask cycle, bypassing the time/day guards.
void otaTriggerCheck();

// Returns true if a manual check is pending (used by fetchTask to jump the queue).
bool otaIsForced();
