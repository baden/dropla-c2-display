/*
 * c2_display — remote C2 status display for Waveshare ESP32-S3-Touch-LCD-1.85.
 *
 * Read-only panel (no touch input): pack voltage in the center, a segmented
 * ring for the state of charge and six status chips (RF / WIFI / STARLINK /
 * LTE / VPN / GPS). All values come from link.cpp (USB CDC); until the first
 * frame arrives the panel shows dashes.
 */
#include <Arduino.h>
#include <lvgl.h>

#include "I2C_Driver.h"
#include "TCA9554PWR.h"
#include "Display_ST77916.h"
#include "PWR_Key.h"

#include "lvgl_port.h"
#include "telemetry.h"
#include "ui.h"
#include "audio_beep.h"
#include "link.h"

void setup()
{
    Serial.begin(115200);
    delay(200);
    printf("\r\n=== c2_display: remote status panel ===\r\n");

    PWR_Init();
    I2C_Init();
    TCA9554PWR_Init(0x00);      /* display/touch reset lines live on the expander */
    Backlight_Init();
    Set_Backlight(0);           /* keep the panel dark until a valid frame is drawn */

    LCD_Init();
    lvgl_port_init();
    ui_init();
    beep_init();                /* PCM5101 + speaker: short beeps, no SD card needed */

    /* First frame = black screen with the ring at 0 %, no placeholders yet.
     * Only then do we switch the backlight on, so the uninitialised panel
     * (red/blue noise) is never visible. */
    lv_refr_now(NULL);
    delay(50);
    Set_Backlight(LCD_Backlight);

    telemetry_update();
    ui_update();

    link_init();

    printf("ready: internal free %u KB, psram free %u KB\r\n",
           (unsigned)(ESP.getFreeHeap() / 1024),
           (unsigned)(ESP.getFreePsram() / 1024));
}

void loop()
{
    static uint32_t t_telem = 0;
    static uint32_t t_ui = 0;

    lv_timer_handler();
    link_poll();                /* parse USB CDC frames as they arrive */

    uint32_t now = millis();

    if (now - t_telem >= 100) {
        t_telem = now;
        telemetry_update();
    }

    if (now - t_ui >= 100) {
        t_ui = now;
        ui_update();
    }

    PWR_Loop();
    delay(5);
}
