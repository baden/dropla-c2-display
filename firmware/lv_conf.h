/**
 * lv_conf.h — единый конфиг LVGL 8.3 для всех скетчей этого репозитория.
 *
 * Важно: LVGL (как Arduino-библиотека) компилируется с файлом
 * <Documents>/Arduino/libraries/lvgl/src/lv_conf.h — он лежит внутри библиотеки
 * и перекрывает lv_conf.h из папки скетча. Поэтому tools/build.ps1 копирует
 * этот файл и туда, и в папку скетча перед сборкой.
 */
#ifndef LV_CONF_H
#define LV_CONF_H

/*====================
   COLOR / MEMORY
 *====================*/
#define LV_COLOR_DEPTH 16
#define LV_COLOR_16_SWAP 0

#define LV_MEM_CUSTOM 0
#define LV_MEM_SIZE (64U * 1024U)

/*====================
   HAL
 *====================*/
#define LV_TICK_CUSTOM 0
#define LV_USE_LOG 0
#define LV_USE_PERF_MONITOR 0

/*=====================
   FONTS
 *=====================*/
#define LV_FONT_MONTSERRAT_12 1
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_16 1
#define LV_FONT_MONTSERRAT_18 1
#define LV_FONT_MONTSERRAT_20 1
#define LV_FONT_MONTSERRAT_24 1
#define LV_FONT_MONTSERRAT_28 1
#define LV_FONT_MONTSERRAT_32 1
#define LV_FONT_MONTSERRAT_36 1
#define LV_FONT_MONTSERRAT_40 1
#define LV_FONT_MONTSERRAT_48 1

#define LV_FONT_DEFAULT &lv_font_montserrat_14

/*==================
   DEMOS
 *==================*/
/* Демо производителя (firmware/LVGL_Arduino) использует картинки lv_demo_music,
 * поэтому ассеты должны компилироваться. Для своего UI это ни на что не влияет. */
#define LV_USE_DEMO_MUSIC 1
#define LV_DEMO_MUSIC_LANDSCAPE 0
#define LV_DEMO_MUSIC_SQUARE 0
#define LV_DEMO_MUSIC_ROUND 1
#define LV_DEMO_MUSIC_LARGE 0
#define LV_DEMO_MUSIC_AUTO_PLAY 0

#define LV_USE_DEMO_WIDGETS 1
#define LV_USE_DEMO_BENCHMARK 0
#define LV_USE_DEMO_STRESS 0
#define LV_USE_DEMO_KEYPAD_AND_ENCODER 0

#endif /* LV_CONF_H */
