#pragma once

#include <stdint.h>

/* Display settings that must survive a panel reboot. Kept in NVS under the
 * "c2disp" namespace; the protocol CONFIG message can change them at runtime. */

#define C2_SETTINGS_ROTATION_DEFAULT 0

/** Saved panel rotation in degrees: 0, 90, 180 or 270. */
uint16_t settings_rotation_load(void);

/** Persist the panel rotation; invalid values are ignored. */
void settings_rotation_save(uint16_t deg);
