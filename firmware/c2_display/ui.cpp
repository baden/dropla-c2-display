#include "ui.h"

#include <lvgl.h>
#include <math.h>
#include <stdio.h>

#include "telemetry.h"

/* Big digits (Arial Bold 110 px, subset 0-9 and '.') generated with lv_font_conv */
LV_FONT_DECLARE(font_voltage);

/* ---------------- palette ---------------- */
#define C_BG         lv_color_hex(0x05070A)
#define C_VOLT       lv_color_hex(0xF2F6FA)
#define C_DIM        lv_color_hex(0x5B6B7C)
#define C_RED        lv_color_hex(0xEF4444)
#define C_AMBER      lv_color_hex(0xF59E0B)
#define C_GREEN      lv_color_hex(0x22C55E)
#define C_SEG_OFF    lv_color_hex(0x1A2430)

#define C_CHIP_BG    lv_color_hex(0x0B1118)
#define C_CHIP_OFF   lv_color_hex(0x46566A)
#define C_CHIP_ON_BG lv_color_hex(0x0A2A18)

/* ---------------- ring ---------------- */
#define RING_SEGMENTS 36
#define RING_SIZE     344
#define RING_WIDTH    13

static lv_obj_t *segs[RING_SEGMENTS];

static lv_obj_t *lbl_volt;
static lv_obj_t *lbl_pct;

#define CHIP_COUNT 6
static lv_obj_t *chips[CHIP_COUNT];
static lv_obj_t *chip_text[CHIP_COUNT];

static const char *const CHIP_NAMES[CHIP_COUNT] = {
    "RF", "WIFI", "STARLINK", "LTE", "VPN", "GPS"
};

static lv_color_t zone_color(uint8_t pct)
{
    if (pct < 20) return C_RED;
    if (pct < 40) return C_AMBER;
    return C_GREEN;
}

/* Fixed colour of fragment i: the ring itself is a coloured scale,
 * red 0-20 %, amber 20-40 %, green 40-100 %. */
static lv_color_t seg_zone(int i)
{
    int p = (i * 100) / RING_SEGMENTS + 100 / (RING_SEGMENTS * 2);
    if (p < 20) return C_RED;
    if (p < 40) return C_AMBER;
    return C_GREEN;
}

/* Same hue, dimmed towards the background — used for the not-yet-filled part */
static lv_color_t dim_of(lv_color_t c)
{
    return lv_color_mix(c, C_BG, 70);
}

/* ---------------- construction ---------------- */

static void build_ring(lv_obj_t *parent)
{
    /* Full circle split into 36 fragments; 2 degrees of gap between them.
     * Segment 0 starts at the top (270 deg in LVGL angle system). */
    for (int i = 0; i < RING_SEGMENTS; i++) {
        int start = (270 + i * 10 + 1) % 360;
        int end = start + 8;

        lv_obj_t *a = lv_arc_create(parent);
        lv_obj_set_size(a, RING_SIZE, RING_SIZE);
        lv_obj_align(a, LV_ALIGN_CENTER, 0, 0);
        lv_arc_set_rotation(a, 0);
        lv_arc_set_bg_angles(a, start, end);
        lv_arc_set_range(a, 0, 1);
        lv_arc_set_value(a, 1);              /* fill the whole fragment */
        lv_obj_remove_style(a, NULL, LV_PART_KNOB);
        lv_obj_clear_flag(a, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(a, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_bg_opa(a, LV_OPA_TRANSP, 0);
        lv_obj_set_style_arc_width(a, RING_WIDTH, LV_PART_MAIN);
        lv_obj_set_style_arc_width(a, RING_WIDTH, LV_PART_INDICATOR);
        lv_obj_set_style_arc_rounded(a, false, LV_PART_MAIN);
        lv_obj_set_style_arc_rounded(a, false, LV_PART_INDICATOR);
        lv_obj_set_style_arc_color(a, C_SEG_OFF, LV_PART_MAIN);
        lv_obj_set_style_arc_color(a, C_SEG_OFF, LV_PART_INDICATOR);

        segs[i] = a;
    }
}

static void build_center(lv_obj_t *parent)
{
    lv_obj_t *cap = lv_label_create(parent);
    lv_obj_set_style_text_font(cap, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(cap, C_DIM, 0);
    lv_label_set_text(cap, "BATTERY");
    lv_obj_align(cap, LV_ALIGN_CENTER, 0, -146);

    lbl_pct = lv_label_create(parent);
    lv_obj_set_style_text_font(lbl_pct, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(lbl_pct, C_GREEN, 0);
    lv_label_set_text(lbl_pct, "0%");
    lv_obj_align(lbl_pct, LV_ALIGN_CENTER, 0, -116);

    lbl_volt = lv_label_create(parent);
    lv_obj_set_style_text_font(lbl_volt, &font_voltage, 0);
    lv_obj_set_style_text_color(lbl_volt, C_VOLT, 0);
    lv_label_set_text(lbl_volt, "0.0");
    lv_obj_align(lbl_volt, LV_ALIGN_CENTER, 0, -38);

    lv_obj_t *unit = lv_label_create(parent);
    lv_obj_set_style_text_font(unit, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(unit, C_DIM, 0);
    lv_label_set_text(unit, "VOLTS");
    lv_obj_align(unit, LV_ALIGN_CENTER, 0, 18);
}

static void build_chip(lv_obj_t *parent, int idx, int dx, int dy)
{
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, 76, 30);
    lv_obj_align(box, LV_ALIGN_CENTER, dx, dy);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_radius(box, 15, 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(box, C_CHIP_BG, 0);
    lv_obj_set_style_border_width(box, 1, 0);
    lv_obj_set_style_border_color(box, C_CHIP_OFF, 0);

    lv_obj_t *l = lv_label_create(box);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(l, C_CHIP_OFF, 0);
    lv_label_set_text(l, CHIP_NAMES[idx]);
    lv_obj_center(l);

    chips[idx] = box;
    chip_text[idx] = l;
}

static void build_chips(lv_obj_t *parent)
{
    /* row 1: RF / WIFI / STARLINK, row 2: LTE / VPN / GPS */
    const int dx = 84;
    build_chip(parent, 0, -dx, 64);
    build_chip(parent, 1, 0, 64);
    build_chip(parent, 2, dx, 64);

    build_chip(parent, 3, -dx, 100);
    build_chip(parent, 4, 0, 100);
    build_chip(parent, 5, dx, 100);
}

/* ---------------- update helpers ---------------- */

static void set_chip(int idx, bool on)
{
    lv_obj_set_style_bg_color(chips[idx], on ? C_CHIP_ON_BG : C_CHIP_BG, 0);
    lv_obj_set_style_border_color(chips[idx], on ? C_GREEN : C_CHIP_OFF, 0);
    lv_obj_set_style_text_color(chip_text[idx], on ? C_GREEN : C_CHIP_OFF, 0);
}

/* ---------------- public ---------------- */

void ui_init(void)
{
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, C_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    build_ring(scr);
    build_center(scr);
    build_chips(scr);

    ui_update();
}

void ui_update(void)
{
    uint8_t pct = telem.batt_pct;
    if (pct > 100) pct = 100;

    lv_color_t zc = zone_color(pct);

    /* ring: every fragment keeps its zone colour; filled ones are bright,
     * the rest is the same hue dimmed. */
    int lit = (pct * RING_SEGMENTS + 50) / 100;
    for (int i = 0; i < RING_SEGMENTS; i++) {
        lv_color_t c = seg_zone(i);
        lv_obj_set_style_arc_color(segs[i], (i < lit) ? c : dim_of(c), LV_PART_INDICATOR);
    }

    lv_label_set_text_fmt(lbl_pct, "%d%%", pct);
    lv_obj_set_style_text_color(lbl_pct, zc, 0);

    /* lroundf + integer formatting: LVGL's formatter on this core does not
     * handle "%f" (produces a bare 'f'), so avoid float specifiers entirely. */
    int tenths = (int)lroundf(telem.voltage_v * 10.0f);
    lv_label_set_text_fmt(lbl_volt, "%d.%d", tenths / 10, tenths % 10);

    set_chip(0, telem.rf_on);
    set_chip(1, telem.wifi_on);
    set_chip(2, telem.starlink_on);
    set_chip(3, telem.lte_on);
    set_chip(4, telem.vpn_on);

    bool gps = telem.sats > 0;
    set_chip(5, gps);
    lv_label_set_text_fmt(chip_text[5], gps ? "GPS %d" : "GPS", telem.sats);
}
