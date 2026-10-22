#include "ui.h"

#include <lvgl.h>
#include <Arduino.h>
#include <esp_heap_caps.h>

#include "telemetry.h"
#include "Gyro_QMI8658.h"
#include "RTC_PCF85063.h"

/* ---------- РїР°Р»РёС‚СЂР° ---------- */
#define C_BG        lv_color_hex(0x0A0E13)
#define C_SURFACE   lv_color_hex(0x131A23)
#define C_TEXT      lv_color_hex(0xE6EDF3)
#define C_DIM       lv_color_hex(0x64748B)
#define C_ACCENT    lv_color_hex(0x22D3EE)
#define C_OK        lv_color_hex(0x22C55E)
#define C_WARN      lv_color_hex(0xF59E0B)
#define C_ERR       lv_color_hex(0xEF4444)

static lv_obj_t *page_flight = NULL;
static lv_obj_t *page_system = NULL;

static lv_obj_t *arc_heading;
static lv_obj_t *lbl_time;
static lv_obj_t *lbl_link;
static lv_obj_t *lbl_hdg;
static lv_obj_t *lbl_alt;
static lv_obj_t *lbl_vario;
static lv_obj_t *lbl_spd;
static lv_obj_t *lbl_bat;
static lv_obj_t *lbl_dots;      /* РёРЅРґРёРєР°С‚РѕСЂ СЃС‚СЂР°РЅРёС† */
static lv_obj_t *btn_arm;
static lv_obj_t *lbl_arm;

static lv_obj_t *lbl_imu_roll;
static lv_obj_t *lbl_imu_pitch;
static lv_obj_t *lbl_imu_yaw;
static lv_obj_t *lbl_rtc;
static lv_obj_t *lbl_vbat;
static lv_obj_t *lbl_mem;
static lv_obj_t *lbl_uptime;

static bool armed_shown = false;

/* ---------- РІСЃРїРѕРјРѕРіР°С‚РµР»СЊРЅРѕРµ ---------- */

static lv_obj_t *make_label(lv_obj_t *parent, const lv_font_t *font, lv_color_t color, const char *text)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color, 0);
    lv_label_set_text(l, text);
    return l;
}

static void show_page(int idx)
{
    if (idx == 0) {
        lv_obj_clear_flag(page_flight, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(page_system, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(lbl_dots, "вЂў  В·");
    } else {
        lv_obj_add_flag(page_flight, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(page_system, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(lbl_dots, "В·  вЂў");
    }
}

static void page_btn_cb(lv_event_t *e)
{
    int idx = (int)(uintptr_t)lv_event_get_user_data(e);
    show_page(idx);
}

/* ---------- СЃС‚СЂР°РЅРёС†Р° РїРѕР»С‘С‚Р° ---------- */

static void build_flight_page(lv_obj_t *scr)
{
    page_flight = lv_obj_create(scr);
    lv_obj_remove_style_all(page_flight);
    lv_obj_set_size(page_flight, LV_PCT(100), LV_PCT(100));
    lv_obj_clear_flag(page_flight, LV_OBJ_FLAG_SCROLLABLE);

    /* РєРѕР»СЊС†Рѕ-РєРѕРјРїР°СЃ РїРѕ РєСЂР°СЋ СЌРєСЂР°РЅР° */
    arc_heading = lv_arc_create(page_flight);
    lv_obj_set_size(arc_heading, 344, 344);
    lv_obj_align(arc_heading, LV_ALIGN_CENTER, 0, 0);
    lv_arc_set_rotation(arc_heading, 0);
    lv_arc_set_bg_angles(arc_heading, 135, 405);   /* 270В° С€РєР°Р»С‹ */
    lv_arc_set_range(arc_heading, 0, 360);
    lv_arc_set_value(arc_heading, 0);
    lv_obj_remove_style(arc_heading, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(arc_heading, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(arc_heading, 10, LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc_heading, 10, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(arc_heading, C_SURFACE, LV_PART_MAIN);
    lv_obj_set_style_arc_color(arc_heading, C_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(arc_heading, true, LV_PART_INDICATOR);

    lbl_time = make_label(page_flight, &lv_font_montserrat_14, C_DIM, "--:--:--");
    lv_obj_align(lbl_time, LV_ALIGN_TOP_MID, 0, 22);

    lbl_link = make_label(page_flight, &lv_font_montserrat_14, C_OK, "LINK в—Џ  9 SAT");
    lv_obj_align(lbl_link, LV_ALIGN_TOP_MID, 0, 42);

    lbl_hdg = make_label(page_flight, &lv_font_montserrat_20, C_TEXT, "000В°");
    lv_obj_align(lbl_hdg, LV_ALIGN_CENTER, 0, -96);

    lbl_alt = make_label(page_flight, &lv_font_montserrat_48, C_TEXT, "0");
    lv_obj_align(lbl_alt, LV_ALIGN_CENTER, 0, -22);

    lv_obj_t *unit = make_label(page_flight, &lv_font_montserrat_14, C_DIM, "ALT, Рј");
    lv_obj_align(unit, LV_ALIGN_CENTER, 0, 14);

    lbl_vario = make_label(page_flight, &lv_font_montserrat_18, C_OK, "+0.0 Рј/СЃ");
    lv_obj_align(lbl_vario, LV_ALIGN_CENTER, 0, 44);

    lbl_spd = make_label(page_flight, &lv_font_montserrat_24, C_TEXT, "0.0");
    lv_obj_align(lbl_spd, LV_ALIGN_CENTER, -72, 84);

    lbl_bat = make_label(page_flight, &lv_font_montserrat_24, C_TEXT, "0%");
    lv_obj_align(lbl_bat, LV_ALIGN_CENTER, 72, 84);

    lv_obj_t *spd_cap = make_label(page_flight, &lv_font_montserrat_12, C_DIM, "Рј/СЃ");
    lv_obj_align(spd_cap, LV_ALIGN_CENTER, -72, 106);

    lv_obj_t *bat_cap = make_label(page_flight, &lv_font_montserrat_12, C_DIM, "BAT");
    lv_obj_align(bat_cap, LV_ALIGN_CENTER, 72, 106);

    /* ARM вЂ” Р±РѕР»СЊС€Р°СЏ РєРЅРѕРїРєР° РІРЅРёР·Сѓ */
    btn_arm = lv_btn_create(page_flight);
    lv_obj_set_size(btn_arm, 132, 40);
    lv_obj_align(btn_arm, LV_ALIGN_CENTER, 0, 130);
    lv_obj_set_style_radius(btn_arm, 20, 0);
    lv_obj_set_style_bg_color(btn_arm, C_SURFACE, 0);
    lv_obj_set_style_shadow_width(btn_arm, 0, 0);
    lbl_arm = make_label(btn_arm, &lv_font_montserrat_16, C_DIM, "ARM");
    lv_obj_center(lbl_arm);

    /* РїРµСЂРµС…РѕРґ РЅР° СЃРёСЃС‚РµРјРЅСѓСЋ СЃС‚СЂР°РЅРёС†Сѓ */
    lv_obj_t *btn_sys = lv_btn_create(page_flight);
    lv_obj_set_size(btn_sys, 46, 26);
    lv_obj_align(btn_sys, LV_ALIGN_CENTER, 96, -120);
    lv_obj_set_style_radius(btn_sys, 13, 0);
    lv_obj_set_style_bg_color(btn_sys, C_SURFACE, 0);
    lv_obj_set_style_shadow_width(btn_sys, 0, 0);
    lv_obj_t *l_sys = make_label(btn_sys, &lv_font_montserrat_12, C_DIM, "SYS");
    lv_obj_center(l_sys);
    lv_obj_add_event_cb(btn_sys, page_btn_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)1);

    lbl_dots = make_label(page_flight, &lv_font_montserrat_12, C_DIM, "вЂў  В·");
    lv_obj_align(lbl_dots, LV_ALIGN_BOTTOM_MID, 0, -20);
}

/* ---------- СЃРёСЃС‚РµРјРЅР°СЏ СЃС‚СЂР°РЅРёС†Р° ---------- */

static void build_system_page(lv_obj_t *scr)
{
    page_system = lv_obj_create(scr);
    lv_obj_remove_style_all(page_system);
    lv_obj_set_size(page_system, LV_PCT(100), LV_PCT(100));
    lv_obj_clear_flag(page_system, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = make_label(page_system, &lv_font_montserrat_20, C_ACCENT, "РЎРРЎРўР•РњРђ");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 40);

    struct row { const char *cap; lv_obj_t **val; lv_font_t const *font; };
    const struct row rows[] = {
        {"ACC X",  &lbl_imu_roll,  &lv_font_montserrat_18},
        {"ACC Y",  &lbl_imu_pitch, &lv_font_montserrat_18},
        {"GYRO Z", &lbl_imu_yaw,   &lv_font_montserrat_18},
    };

    for (int i = 0; i < 3; i++) {
        lv_obj_t *cap = make_label(page_system, &lv_font_montserrat_14, C_DIM, rows[i].cap);
        lv_obj_align(cap, LV_ALIGN_CENTER, -90, -54 + i * 30);
        *rows[i].val = make_label(page_system, rows[i].font, C_TEXT, "0.0В°");
        lv_obj_align(*rows[i].val, LV_ALIGN_CENTER, 40, -54 + i * 30);
    }

    lbl_rtc = make_label(page_system, &lv_font_montserrat_14, C_TEXT, "--.--.---- --:--:--");
    lv_obj_align(lbl_rtc, LV_ALIGN_CENTER, 0, 44);

    lbl_vbat = make_label(page_system, &lv_font_montserrat_14, C_TEXT, "BAT 0.00 Р’");
    lv_obj_align(lbl_vbat, LV_ALIGN_CENTER, 0, 70);

    lbl_mem = make_label(page_system, &lv_font_montserrat_14, C_TEXT, "RAM 0 / PSRAM 0 РљР‘");
    lv_obj_align(lbl_mem, LV_ALIGN_CENTER, 0, 96);

    lbl_uptime = make_label(page_system, &lv_font_montserrat_12, C_DIM, "UP 00:00:00");
    lv_obj_align(lbl_uptime, LV_ALIGN_CENTER, 0, 120);

    /* РЅР°Р·Р°Рґ */
    lv_obj_t *btn_back = lv_btn_create(page_system);
    lv_obj_set_size(btn_back, 46, 26);
    lv_obj_align(btn_back, LV_ALIGN_CENTER, 96, -120);
    lv_obj_set_style_radius(btn_back, 13, 0);
    lv_obj_set_style_bg_color(btn_back, C_SURFACE, 0);
    lv_obj_set_style_shadow_width(btn_back, 0, 0);
    lv_obj_t *l_back = make_label(btn_back, &lv_font_montserrat_12, C_DIM, "FLT");
    lv_obj_center(l_back);
    lv_obj_add_event_cb(btn_back, page_btn_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)0);
}

/* ---------- РѕР±РЅРѕРІР»РµРЅРёРµ ---------- */

static void update_arm_style(void)
{
    if (telem.armed) {
        lv_obj_set_style_bg_color(btn_arm, C_ERR, 0);
        lv_obj_set_style_text_color(lbl_arm, lv_color_white(), 0);
        lv_label_set_text(lbl_arm, "DISARM");
    } else {
        lv_obj_set_style_bg_color(btn_arm, C_SURFACE, 0);
        lv_obj_set_style_text_color(lbl_arm, C_DIM, 0);
        lv_label_set_text(lbl_arm, "ARM");
    }
}

static void arm_cb(lv_event_t *e)
{
    (void)e;
    telemetry_toggle_armed();
    update_arm_style();
}

void ui_init(void)
{
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, C_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    build_flight_page(scr);
    build_system_page(scr);

    lv_obj_add_event_cb(btn_arm, arm_cb, LV_EVENT_CLICKED, NULL);

    show_page(0);
    update_arm_style();
}

void ui_update(void)
{
    /* --- СЃС‚СЂР°РЅРёС†Р° РїРѕР»С‘С‚Р° --- */
    lv_arc_set_value(arc_heading, (int16_t)telem.heading_deg);
    lv_label_set_text_fmt(lbl_hdg, "%03dВ°", (int)telem.heading_deg);

    lv_label_set_text_fmt(lbl_alt, "%d", (int)lroundf(telem.alt_m));

    if (telem.vario_ms >= 0.05f) {
        lv_obj_set_style_text_color(lbl_vario, C_OK, 0);
    } else if (telem.vario_ms <= -0.05f) {
        lv_obj_set_style_text_color(lbl_vario, C_ERR, 0);
    } else {
        lv_obj_set_style_text_color(lbl_vario, C_DIM, 0);
    }
    /* Р±РµР· СЃРёРјРІРѕР»Р° % вЂ” LVGL snprintf РЅРµ РґСЂСѓР¶РёС‚ СЃ '+' С„Р»Р°РіРѕРј РґР»СЏ float */
    lv_label_set_text_fmt(lbl_vario, "%s%.1f Рј/СЃ",
                          telem.vario_ms >= 0 ? "+" : "", telem.vario_ms);

    lv_label_set_text_fmt(lbl_spd, "%.1f", telem.speed_ms);

    /* 10.5 Р’ (РїСѓСЃС‚Рѕ) .. 12.6 Р’ (РїРѕР»РЅС‹Р№) */
    int pct = (int)((telem.vbat_v - 10.5f) * 100.0f / 2.1f);
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    lv_label_set_text_fmt(lbl_bat, "%d%%", pct);
    lv_obj_set_style_text_color(lbl_bat, pct > 40 ? C_TEXT : (pct > 20 ? C_WARN : C_ERR), 0);

    lv_label_set_text_fmt(lbl_link, "LINK %s  %d SAT",
                          telem.link_ok ? "в—Џ" : "в—‹", telem.sats);
    lv_obj_set_style_text_color(lbl_link, telem.link_ok ? C_OK : C_ERR, 0);

    /* РІСЂРµРјСЏ: РёР· RTC, РµСЃР»Рё РѕРЅ РІС‹СЃС‚Р°РІР»РµРЅ, РёРЅР°С‡Рµ вЂ” Р°РїС‚Р°Р№Рј */
    datetime_t dt;
    PCF85063_Read_Time(&dt);
    bool rtc_ok = (dt.year >= 2020 && dt.year <= 2099 && dt.month >= 1 && dt.month <= 12);

    uint32_t up_s = telem.uptime_ms / 1000;
    if (rtc_ok) {
        lv_label_set_text_fmt(lbl_time, "%02d:%02d:%02d", dt.hour, dt.minute, dt.second);
    } else {
        lv_label_set_text_fmt(lbl_time, "UP %02u:%02u:%02u",
                              (unsigned)(up_s / 3600), (unsigned)((up_s / 60) % 60), (unsigned)(up_s % 60));
    }

    /* --- СЃРёСЃС‚РµРјРЅР°СЏ СЃС‚СЂР°РЅРёС†Р° --- */
    lv_label_set_text_fmt(lbl_imu_roll, "%+.2f g", Accel.x);
    lv_label_set_text_fmt(lbl_imu_pitch, "%+.2f g", Accel.y);
    lv_label_set_text_fmt(lbl_imu_yaw, "%+.1f /СЃ", Gyro.z);

    if (rtc_ok) {
        lv_label_set_text_fmt(lbl_rtc, "%02d.%02d.%04d  %02d:%02d:%02d",
                              dt.day, dt.month, dt.year, dt.hour, dt.minute, dt.second);
    } else {
        lv_label_set_text(lbl_rtc, "RTC РЅРµ РІС‹СЃС‚Р°РІР»РµРЅ");
    }

    lv_label_set_text_fmt(lbl_vbat, "BAT %.2f Р’", telem.vbat_v);

    lv_label_set_text_fmt(lbl_mem, "RAM %u / PSRAM %u РљР‘",
                          (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                          (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));

    lv_label_set_text_fmt(lbl_uptime, "UP %02u:%02u:%02u",
                          (unsigned)(up_s / 3600), (unsigned)((up_s / 60) % 60), (unsigned)(up_s % 60));
}
