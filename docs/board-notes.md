# Waveshare ESP32-S3-Touch-LCD-1.85 — рабочая шпаргалка

MCU: ESP32-S3 (Xtensa LX7, 2 ядра @240 МГц), 16 МБ flash, 8 МБ OPI PSRAM @80 МГц.
Экран: 1.85" круглый, 360×360, ST77916 по QSPI. Тач: CST816S (I2C).
Данные взяты из `vendor/Arduino/examples/LVGL_Arduino/*.h`.

## Экран (ST77916, QSPI на SPI2_HOST, 80 МГц)

| Сигнал | GPIO |
|---|---|
| SCK | 40 |
| D0 | 46 |
| D1 | 45 |
| D2 | 42 |
| D3 | 41 |
| CS | 21 |
| TE | 18 |
| Подсветка (PWM, LEDC ch1, 20 кГц) | 5 |
| RST | через TCA9554 EXIO2 |
| Разрешение | 360×360, RGB565 |

## Тач (CST816S, отдельная I2C-шина @400 кГц)

| Сигнал | GPIO |
|---|---|
| SDA | 1 |
| SCL | 3 |
| INT | 4 |
| RST | через TCA9554 EXIO1 |
| Адрес | 0x15 |

## Внутренняя I2C-шина (@400 кГц): SDA 11, SCL 10

| Устройство | Адрес | Что даёт |
|---|---|---|
| TCA9554PWR (IO-расширитель) | 0x20 | EXIO1 = тач RST, EXIO2 = LCD RST, пищалка/прочее |
| QMI8658 (IMU 6 осей) | 0x6A/0x6B | акселерометр + гироскоп |
| PCF85063 (RTC) | 0x51 | часы + 1 RAM-байт |

## Аудио

| Что | Пины |
|---|---|
| PCM5101 (I2S выход, DAC) | DOUT 47, BCLK 48, LRCK 38 |
| MSM261S4030H0 (микрофон, I2S вход) | BCK 15, WS 2, DIN 39 |
| Пищалка | через TCA9554 |

## SD-карта (SDMMC 1-bit)

CLK 14, CMD 17, D0 16.

## Прочее

| Что | Пин |
|---|---|
| АКБ (ADC) | 8 |
| Кнопка PWR (ADC-тип) | 6 |
| Удержание питания (PWR_Control) | 7 |
| USB | нативный USB-Serial/JTAG (VID 303A:PID 1001) → сейчас COM4 |

## Важные нюансы

- Подсветка и LCD/touch reset висят на TCA9554 — без инициализации расширителя экран остаётся чёрным.
- `lv_conf.h` в архиве производителя **отсутствует**, его нужно создавать самому (см. `firmware/LVGL_Arduino/lv_conf.h`).
- Демо использует LVGL 8.3.10 (не 9.x!) — API `lv_disp_drv_t`, `lv_indev_drv_t`.
- Буферы LVGL в демо статические: 2 × (360×360/10) × 2 байта ≈ 52 КБ внутренней RAM.
- Для OPI PSRAM в Arduino IDE: `PSRAM=opi`, `FlashSize=16M`.
- `lv_label_set_text_fmt()` на этом ядре **не поддерживает `%f`**: вместо числа печатается
  одна буква `f` (в шрифте только цифры → на экране «квадратик»). Форматируйте целыми:
  `"%d.%d", tenths / 10, tenths % 10`.
- Тач CST816S инициализируется, но LVGL его не опрашивает — панель только показывает состояние.

## Что пришлось починить, чтобы демо собралось (arduino-esp32 3.3.12)

1. **`lv_conf.h`** → единый конфиг лежит в `firmware/lv_conf.h`. Важно: LVGL компилируется
   с файлом `<Documents>\Arduino\libraries\lvgl\src\lv_conf.h`, который перекрывает `lv_conf.h`
   из папки скетча (там он тоже нужен — его подключает `LVGL_Driver.h` демо).
   `tools/build.ps1` копирует конфиг в оба места.
   Демо производителя ссылается на `img_lvgl_logo`, поэтому нужен `LV_USE_DEMO_WIDGETS=1`.
2. **`sr_cmd_t` изменился** в ESP-SR → в `MIC_MSM.cpp` убрана третья (фонетическая) строка в инициализаторах:
   было `{0, "Turn on the backlight", "TkN nN jc BaKLiT"}`, стало `{0, "Turn on the backlight"}`.
3. **Не влезало в 3 МБ** (скетч 4.1 МБ из-за моделей ESP-SR и ассетов LVGL) → `PartitionScheme=custom`
   + `firmware/partitions-16mb.csv`: app 6 МБ, model 3.9 МБ, ffat 6 МБ.
4. **ESP-SR выжирал RAM** → после его старта оставалось ~7 КБ heap и 8 байт DMA,
   `test_draw_bitmap()` падал на `heap_caps_calloc` (Guru Meditation, StoreProhibited).
   Теперь в `.ino` есть `ENABLE_SPEECH_RECOGNITION` (по умолчанию 0), `MIC_Init()` вызывается после `Lvgl_Init()`.
   Для включения распознавания нужно ещё залить `srmodels.bin` в раздел `model`:
   `esptool.exe -p COM4 write-flash 0x610000 .build\LVGL_Arduino\srmodels.bin`
5. **SD-карты нет** — при загрузке печатается `SD card initialization failed` / `No SD card attached`, это нормально.

## Как собрать/залить

```powershell
# скетч LVGL_Arduino
.\tools\build.ps1 -Upload
# любой другой скетч из firmware\
.\tools\build.ps1 -Sketch firmware\00_display_test -Upload
```

Скрипт использует `arduino-cli`, встроенный в Arduino IDE 2.x, и сам подкладывает `partitions.csv`
в папку скетча (FQBN: `esp32:esp32:esp32s3`, `PSRAM=opi`, `FlashSize=16M`, `PartitionScheme=custom`,
`USBMode=hwcdc`, `CDCOnBoot=cdc`).

## Скетчи в этом репозитории

| Папка | Что делает |
|---|---|
| `firmware/c2_display` | свой UI: компас-кольцо, высота, варио, скорость, АКБ, связь, ARM + страница «система» |
| `firmware/LVGL_Arduino` | демо производителя (LVGL 8.3, часы/виджеты/IMU/батарея) |
| `firmware/00_display_test` | проверка панели без LVGL: заливка красный→зелёный→синий→белый→чёрный |

