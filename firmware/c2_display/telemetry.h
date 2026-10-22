#pragma once

#include <stdint.h>
#include <stdbool.h>

/** Телеметрия, которую показывает экран.
 *  Пока данные имитируются (C2_SIMULATE), позже сюда придут пакеты по Wi-Fi/MAVLink. */
typedef struct {
    float alt_m;        /* высота, м */
    float vario_ms;     /* вертикальная скорость, м/с */
    float speed_ms;     /* горизонтальная скорость, м/с */
    float heading_deg;  /* курс, 0..360 */
    float vbat_v;       /* напряжение АКБ, В */
    int8_t rssi_dbm;    /* уровень связи */
    uint8_t sats;       /* спутники */
    bool armed;         /* моторы разрешены */
    bool link_ok;       /* есть связь с бортом */
    uint32_t uptime_ms;
} telemetry_t;

extern telemetry_t telem;

/** Обновить модель данных (вызывать 10 Гц). */
void telemetry_update(void);

/** Переключить armed (по кнопке на экране). */
void telemetry_toggle_armed(void);
