#pragma once

/** Создать экран и обновлять его данными из telemetry.h (вызывать 5–10 Гц). */
void ui_init(void);
void ui_update(void);

/** true once the boot animation has finished and live data is shown. */
bool ui_boot_done(void);
