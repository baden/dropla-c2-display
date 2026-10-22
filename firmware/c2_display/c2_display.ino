/*
 * c2_display — свой экран C2 для Waveshare ESP32-S3-Touch-LCD-1.85.
 *
 * Страница «полёт»: компас-кольцо, высота, варио, скорость, заряд, связь, ARM.
 * Страница «система»: IMU, RTC, напряжение АКБ, свободная память, аптайм.
 *
 * Данные пока имитируются в telemetry.cpp (C2_SIMULATE=1).
 */
#include <Arduino.h>
#include <lvgl.h>

#include "I2C_Driver.h"
#include "TCA9554PWR.h"
#include "Display_ST77916.h"
#include "Gyro_QMI8658.h"
#include "RTC_PCF85063.h"
#include "BAT_Driver.h"
#include "PWR_Key.h"

#include "lvgl_port.h"
#include "telemetry.h"
#include "ui.h"

void setup()
{
    Serial.begin(115200);
    delay(200);
    printf("\r\n=== c2_display ===\r\n");

    PWR_Init();
    I2C_Init();
    TCA9554PWR_Init(0x00);
    BAT_Init();
    PCF85063_Init();
    QMI8658_Init();
    Backlight_Init();

    LCD_Init();
    lvgl_port_init();
    ui_init();

    telemetry_update();
    ui_update();

    printf("ready: internal free %u KB, psram free %u KB\r\n",
           (unsigned)(ESP.getFreeHeap() / 1024),
           (unsigned)(ESP.getFreePsram() / 1024));
}

void loop()
{
    static uint32_t t_telem = 0;
    static uint32_t t_ui = 0;

    lv_timer_handler();

    uint32_t now = millis();

    if (now - t_telem >= 100) {
        t_telem = now;
        QMI8658_Loop();
        telemetry_update();
    }

    if (now - t_ui >= 150) {
        t_ui = now;
        ui_update();
    }

    PWR_Loop();
    delay(5);
}
