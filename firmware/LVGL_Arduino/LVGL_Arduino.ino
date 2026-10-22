/*Using LVGL with Arduino requires some extra steps:
 *Be sure to read the docs here: https://docs.lvgl.io/master/get-started/platforms/arduino.html  */

#include "Display_ST77916.h"
#include "Audio_PCM5101.h"
#include "RTC_PCF85063.h"
#include "Gyro_QMI8658.h"
#include "LVGL_Driver.h"
#include "MIC_MSM.h"
#include "PWR_Key.h"
#include "SD_Card.h"
#include "LVGL_Example.h"
#include "BAT_Driver.h"

void Psram_Inquiry() {
  char buffer[128];    /* Make sure buffer is enough for `sprintf` */
  sprintf(buffer, "   Biggest /     Free /    Total\n"
          "\t  SRAM : [%8d / %8d / %8d]\n"
          "\t PSRAM : [%8d / %8d / %8d]",
          heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
          heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
          heap_caps_get_total_size(MALLOC_CAP_INTERNAL),
          heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM),
          heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
          heap_caps_get_total_size(MALLOC_CAP_SPIRAM));
  printf("MEM : %s\r\n", buffer);
}

void DriverTask(void *parameter) {
  Wireless_Test2();
  while(1){
    PWR_Loop();
    BAT_Get_Volts();
    RTC_Loop();
    QMI8658_Loop(); 
    // Psram_Inquiry();
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}
void Driver_Loop() {
  xTaskCreatePinnedToCore(
    DriverTask,           
    "DriverTask",         
    4096,                 
    NULL,                 
    3,                    
    NULL,                 
    0                     
  );  
}
/* ESP-SR (распознавание речи) съедает почти всю внутреннюю RAM:
 * после его старта остаётся ~7 КБ heap и ~0 КБ DMA, из-за чего падают
 * DMA-буферы экрана и прерывания. Для UI-экспериментов по умолчанию выключено;
 * чтобы включить — собирать с -DENABLE_SPEECH_RECOGNITION=1. */
#ifndef ENABLE_SPEECH_RECOGNITION
#define ENABLE_SPEECH_RECOGNITION 0
#endif

void setup() {
  Flash_test();
  printf("PSRAM total: %u KB, free: %u KB; internal free: %u KB\r\n",
         (unsigned)(ESP.getPsramSize() / 1024), (unsigned)(ESP.getFreePsram() / 1024),
         (unsigned)(ESP.getFreeHeap() / 1024));
  PWR_Init();
  BAT_Init();
  I2C_Init();
  PCF85063_Init();
  QMI8658_Init();
  TCA9554PWR_Init(0x00);
  Backlight_Init();

  SD_Init();
  Audio_Init();
  LCD_Init();
  Lvgl_Init();
#if ENABLE_SPEECH_RECOGNITION
  MIC_Init();
#endif

  Lvgl_Example1();
  Driver_Loop();
  // lv_demo_widgets();
  // lv_demo_benchmark();
  // lv_demo_keypad_encoder();
  // lv_demo_music();
  // lv_demo_printer();
  // lv_demo_stress();
  

}
int Time_Loop=0;
void loop() {
  Lvgl_Loop();
  vTaskDelay(pdMS_TO_TICKS(5));

}
