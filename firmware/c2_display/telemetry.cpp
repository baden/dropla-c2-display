#include "telemetry.h"

#include <Arduino.h>

/* Nothing is simulated any more: the values come from link.cpp over USB CDC.
 * Until the first frame arrives the UI shows placeholders (dashes). */
#define LINK_TIMEOUT_MS 10000

telemetry_t telem = {
    .batt_pct = 255,
    .odometer_unit = C2_ODO_KM,
};

void telemetry_update(void)
{
    telem.uptime_ms = millis();
}

bool telemetry_fresh(void)
{
    return telem.valid && (millis() - telem.last_rx_ms) <= LINK_TIMEOUT_MS;
}
