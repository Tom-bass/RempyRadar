#pragma once
#include "config.h"

constexpr int MAX_AIRPORTS = 30;

struct Airport {
    float lat;
    float lon;
    char  code[8]; // ICAO preferred, else short name
};

struct WindData {
    float speedKt;
    int   dirDeg;  // meteorological: direction wind comes FROM (0-359)
    bool  valid;
};

struct Plane {
    float lat;
    float lon;
    char  callsign[12]; // priority-resolved: flight → r → hex (dedup key)
    char  flight[12];   // raw "flight" field e.g. "JST655", empty if absent
    char  reg[12];      // raw "r" field e.g. "VH-XYZ", empty if absent
    char  acType[8];    // raw "t" field e.g. "B738", empty if absent
    float track;        // ground track degrees 0-359, -1 if unknown
    int   altFt;        // barometric altitude feet, -9999 if unknown
    char  category[4];  // ADS-B category e.g. "A3", empty if unknown
    int   baroRate;     // vertical rate fpm, 0 if unknown (positive = climbing)
    bool  isEmergency;  // squawking 7500 / 7600 / 7700
};


float toRad(float deg);
float distanceKm(float lat1, float lon1, float lat2, float lon2);
float bearingTo(float homeLat, float homeLon, float lat2, float lon2);
void  planeToScreen(float homeLat, float homeLon, float radiusKm,
                    float lat, float lon, int &px, int &py, float northOffset = 0.0f);
