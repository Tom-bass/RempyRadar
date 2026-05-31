#include "radar.h"
#include "config.h"
#include <math.h>

float toRad(float deg) {
    return deg * M_PI / 180.0f;
}

float distanceKm(float lat1, float lon1, float lat2, float lon2) {
    float dLat = toRad(lat2 - lat1);
    float dLon = toRad(lon2 - lon1);
    float a = sinf(dLat / 2) * sinf(dLat / 2) +
              cosf(toRad(lat1)) * cosf(toRad(lat2)) *
              sinf(dLon / 2) * sinf(dLon / 2);
    return 6371.0f * 2.0f * atan2f(sqrtf(a), sqrtf(1.0f - a));
}

float bearingTo(float homeLat, float homeLon, float lat2, float lon2) {
    float dLon = toRad(lon2 - homeLon);
    float y    = sinf(dLon) * cosf(toRad(lat2));
    float x    = cosf(toRad(homeLat)) * sinf(toRad(lat2)) -
                 sinf(toRad(homeLat)) * cosf(toRad(lat2)) * cosf(dLon);
    return fmodf(atan2f(y, x) * 180.0f / M_PI + 360.0f, 360.0f);
}

void planeToScreen(float homeLat, float homeLon, float radiusKm,
                   float lat, float lon, int &px, int &py) {
    float bearing = bearingTo(homeLat, homeLon, lat, lon);
    float dist    = distanceKm(homeLat, homeLon, lat, lon);
    float angle   = toRad(bearing - 90.0f);
    float r       = (dist / radiusKm) * RADAR_R;
    px = CENTRE_X + (int)(r * cosf(angle));
    py = CENTRE_Y + (int)(r * sinf(angle));
}
