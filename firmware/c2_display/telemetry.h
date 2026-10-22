#pragma once

#include <stdint.h>
#include <stdbool.h>

/**
 * Data model of the remote status display.
 *
 * The screen is read-only: it shows the state of the drone link, it does not
 * accept any input. Right now the values are produced by the simulator in
 * telemetry.cpp; later they will be filled from the real link (Wi-Fi/MAVLink).
 */
typedef struct {
    float    voltage_v;    /* pack voltage, V (14S demo: 42.0 .. 58.8) */
    uint8_t  batt_pct;     /* state of charge, 0..100 % */
    bool     rf_on;        /* RC link (radio control) is active */
    bool     wifi_on;      /* control over Wi-Fi is active (drone side) */
    bool     starlink_on;  /* Starlink link is up */
    bool     lte_on;       /* LTE link is up */
    bool     vpn_on;       /* VPN tunnel is up */
    uint8_t  sats;         /* GNSS satellites the drone sees */
    uint32_t odometer_km;  /* total distance flown, km */
    uint32_t uptime_ms;
} telemetry_t;

extern telemetry_t telem;

/** Refresh the model (call at ~10 Hz). */
void telemetry_update(void);
