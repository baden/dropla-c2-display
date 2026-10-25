#pragma once

#include <stdint.h>
#include <stdbool.h>

/* ---------------- link flags (telemetry.flags) ---------------- */
#define C2_LINK_CRSF     0x01
#define C2_LINK_WIFI     0x02
#define C2_LINK_STARLINK 0x04
#define C2_LINK_LTE      0x08
#define C2_LINK_VPN      0x10
#define C2_LINK_GPS_FIX  0x20   /* optional: chip is on even with 0 sats */

/* ---------------- notification flags ---------------- */
#define C2_NOTIFY_SEV_MASK 0x03 /* bits 0..1: 0 debug, 1 info, 2 warning, 3 error */
#define C2_NOTIFY_BLINK    0x04 /* blink while it is shown */
#define C2_NOTIFY_AUTOHIDE 0x08 /* hide 5 s after it arrived */

/* ---------------- display configuration ---------------- */
#define C2_ODO_KM 0
#define C2_ODO_MI 1

#define C2_NOTIFY_LEN 64

/**
 * Data model of the remote status display. Everything is filled by link.cpp
 * from frames received over USB CDC (and later UART / BLE); the UI only reads.
 */
typedef struct {
    /* --- telemetry (message type 0x1) --- */
    bool     valid;         /* at least one telemetry frame has been received */
    uint16_t voltage_cv;    /* pack voltage, centivolts (5820 = 58.20 V) */
    uint8_t  batt_pct;      /* state of charge 0..100, 255 = unknown (hide %) */
    uint8_t  flags;         /* C2_LINK_* */
    uint8_t  sats;          /* GNSS satellites */
    uint32_t odometer_dm;   /* total distance, decimetres (6 340 000 = 634 km) */
    uint32_t last_rx_ms;    /* millis() of the last valid telemetry frame */

    /* --- notification (message type 0x2) --- */
    char     notify[C2_NOTIFY_LEN];
    uint8_t  notify_sev;    /* 0..3 */
    uint8_t  notify_flags;  /* C2_NOTIFY_* */
    uint32_t notify_seq;    /* increments on every NOTIFY frame, 0 = none yet */
    uint32_t notify_ms;     /* millis() when the notification arrived */

    /* --- display configuration (message type 0x3) --- */
    bool     cfg_valid;     /* at least one CONFIG frame has been received */
    uint8_t  odometer_unit; /* C2_ODO_KM / C2_ODO_MI */

    /* --- housekeeping --- */
    uint32_t uptime_ms;
    uint32_t rx_frames;
    uint32_t rx_errors;
} telemetry_t;

extern telemetry_t telem;

/** Refresh housekeeping fields (call at ~10 Hz). */
void telemetry_update(void);

/** True when telemetry was received recently enough to be trusted. */
bool telemetry_fresh(void);
