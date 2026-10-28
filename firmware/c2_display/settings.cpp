#include "settings.h"

#include <Arduino.h>
#include <Preferences.h>

#define SETTINGS_NAMESPACE "c2disp"
#define SETTINGS_KEY_ROT   "rot"

static bool rotation_valid(uint16_t deg)
{
    return deg == 0 || deg == 90 || deg == 180 || deg == 270;
}

uint16_t settings_rotation_load(void)
{
    Preferences prefs;
    if (!prefs.begin(SETTINGS_NAMESPACE, true)) {
        printf("settings: NVS open failed, rotation default\r\n");
        return C2_SETTINGS_ROTATION_DEFAULT;
    }

    uint16_t deg = prefs.getUShort(SETTINGS_KEY_ROT, C2_SETTINGS_ROTATION_DEFAULT);
    prefs.end();

    if (!rotation_valid(deg)) {
        printf("settings: stored rotation %u invalid, using default\r\n", deg);
        return C2_SETTINGS_ROTATION_DEFAULT;
    }
    return deg;
}

void settings_rotation_save(uint16_t deg)
{
    if (!rotation_valid(deg)) {
        return;
    }

    Preferences prefs;
    if (!prefs.begin(SETTINGS_NAMESPACE, false)) {
        printf("settings: NVS open for write failed\r\n");
        return;
    }

    prefs.putUShort(SETTINGS_KEY_ROT, deg);
    prefs.end();
}
