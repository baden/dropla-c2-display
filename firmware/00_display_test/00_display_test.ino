/*
 * 00_display_test — минимальная проверка ЖК-панели ST77916 без LVGL.
 *
 * Заливает весь экран по очереди: красный -> зелёный -> синий -> белый -> чёрный.
 * В serial (115200) печатает, какой цвет залит.
 *
 * Пины/драйверы взяты из демо Waveshare.
 */
#include "Display_ST77916.h"
#include "TCA9554PWR.h"
#include "I2C_Driver.h"

static const uint16_t BAND_H = 20;                       // строк за один вызов
static uint16_t band_buf[EXAMPLE_LCD_WIDTH * 20];        // 14.4 КБ

static void fill_screen(uint16_t color565)
{
  for (size_t i = 0; i < sizeof(band_buf) / sizeof(band_buf[0]); i++) {
    band_buf[i] = color565;
  }
  for (uint16_t y = 0; y < EXAMPLE_LCD_HEIGHT; y += BAND_H) {
    uint16_t y2 = y + BAND_H - 1;
    if (y2 >= EXAMPLE_LCD_HEIGHT) y2 = EXAMPLE_LCD_HEIGHT - 1;
    LCD_addWindow(0, y, EXAMPLE_LCD_WIDTH - 1, y2, band_buf);
  }
}

void setup()
{
  Serial.begin(115200);
  delay(300);
  printf("\r\n=== display test start ===\r\n");

  I2C_Init();                 // шина SDA11/SCL10 для TCA9554
  TCA9554PWR_Init(0x00);      // EXIO1 = touch reset, EXIO2 = LCD reset
  Backlight_Init();           // подсветка GPIO5
  LCD_Init();                 // ST77916 + QSPI + CST816S

  printf("LCD init done, backlight=%d\r\n", LCD_Backlight);
}

void loop()
{
  static const struct { uint16_t color; const char *name; } seq[] = {
    {0xF800, "RED"},
    {0x07E0, "GREEN"},
    {0x001F, "BLUE"},
    {0xFFFF, "WHITE"},
    {0x0000, "BLACK"},
  };
  static size_t idx = 0;

  fill_screen(seq[idx].color);
  printf("fill: %s (0x%04X)\r\n", seq[idx].name, seq[idx].color);
  idx = (idx + 1) % (sizeof(seq) / sizeof(seq[0]));

  delay(2000);
}
