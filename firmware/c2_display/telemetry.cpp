#include "telemetry.h"

#include <math.h>
#include <Arduino.h>

#include "BAT_Driver.h"

/* 1 — данные рисуются симулятором, 0 — только реальные значения (АКБ, позже канал связи) */
#ifndef C2_SIMULATE
#define C2_SIMULATE 1
#endif

telemetry_t telem = {
    .alt_m = 120.0f,
    .vario_ms = 0.0f,
    .speed_ms = 12.5f,
    .heading_deg = 0.0f,
    .vbat_v = 12.6f,
    .rssi_dbm = -62,
    .sats = 9,
    .armed = false,
    .link_ok = true,
    .uptime_ms = 0,
};

void telemetry_update(void)
{
    telem.uptime_ms = millis();
    float t = telem.uptime_ms / 1000.0f;

#if C2_SIMULATE
    telem.heading_deg = fmodf(t * 9.0f, 360.0f);
    telem.alt_m = 120.0f + 18.0f * sinf(t * 0.35f);
    telem.vario_ms = 18.0f * 0.35f * cosf(t * 0.35f);
    telem.speed_ms = 12.5f + 1.8f * fabsf(sinf(t * 0.2f));
    telem.rssi_dbm = (int8_t)(-58.0f + 7.0f * sinf(t * 0.5f));
    telem.sats = (uint8_t)(9 + (int)(2.0f * sinf(t * 0.15f)));
    telem.link_ok = true;
#else
    (void)t;
#endif

    /* Реальное напряжение АКБ: если делитель не подключён, остаётся значение модели */
    float v = BAT_Get_Volts();
    if (v > 3.0f) {
        telem.vbat_v = v;
    }
}

void telemetry_toggle_armed(void)
{
    telem.armed = !telem.armed;
}
