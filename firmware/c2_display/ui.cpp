#include "ui.h"

#include <Arduino.h>
#include <lvgl.h>
#include <math.h>
#include <stdio.h>

#include "telemetry.h"

/* Big digits (Arial Bold 110 px) and the small "V" unit (Arial Bold 55 px),
 * both generated with lv_font_conv. */
LV_FONT_DECLARE(font_voltage);
LV_FONT_DECLARE(font_unit);

/* ---------------- palette ---------------- */
#define C_BG         lv_color_hex(0x05070A)
#define C_VOLT       lv_color_hex(0xF2F6FA)
#define C_DIM        lv_color_hex(0x5B6B7C)
#define C_UNIT       lv_color_hex(0x2B343E)  /* barely visible unit "V" */
#define C_ODO        lv_color_hex(0x93A3B5)  /* odometer value */
#define C_STATUS     lv_color_hex(0x8695A5)  /* status line under the voltage */
#define C_RED        lv_color_hex(0xEF4444)
#define C_AMBER      lv_color_hex(0xF59E0B)
#define C_GREEN      lv_color_hex(0x22C55E)
#define C_SEG_OFF    lv_color_hex(0x1A2430)

#define C_CHIP_BG    lv_color_hex(0x0B1118)
#define C_CHIP_OFF   lv_color_hex(0x46566A)
#define C_CHIP_ON_BG lv_color_hex(0x0A2A18)

/* ---------------- low battery alarm ---------------- */
#define LOW_BATTERY_PCT 10   /* % at which the readout starts blinking red */
#define BLINK_HALF_MS   500  /* half period: 500 ms on, 500 ms off */

/* ---------------- ring ---------------- */
#define RING_SEGMENTS 36
#define RING_SIZE     344
#define RING_WIDTH    13

static lv_obj_t *segs[RING_SEGMENTS];

static lv_obj_t *lbl_volt;
static lv_obj_t *lbl_unit;
static lv_obj_t *lbl_pct;
static lv_obj_t *lbl_status;
static lv_obj_t *lbl_odo;
static lv_obj_t *lbl_odo_unit;

#define CHIP_COUNT 6
static lv_obj_t *chips[CHIP_COUNT];
static lv_obj_t *chip_text[CHIP_COUNT];

static const char *const CHIP_NAMES[CHIP_COUNT] = {
    "CRSF", "WIFI", "STARLINK", "LTE", "VPN", "GPS"
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

/* Largest width the status line may occupy without touching the ring. */
#define STATUS_MAX_W 300

/* Static text is centred; anything wider than the free space above the chips
 * scrolls around, so no message ever gets clipped. */
static void set_status_text(const char *txt)
{
    lv_point_t sz;
    lv_txt_get_size(&sz, txt, &lv_font_montserrat_32, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);

    if (sz.x > STATUS_MAX_W) {
        lv_obj_set_width(lbl_status, STATUS_MAX_W);
        lv_label_set_long_mode(lbl_status, LV_LABEL_LONG_SCROLL_CIRCULAR);
    } else {
        lv_obj_set_width(lbl_status, LV_SIZE_CONTENT);
        lv_label_set_long_mode(lbl_status, LV_LABEL_LONG_CLIP);
    }

    lv_label_set_text(lbl_status, txt);
    lv_obj_align(lbl_status, LV_ALIGN_CENTER, 0, 18);
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
    lbl_pct = lv_label_create(parent);
    lv_obj_set_style_text_font(lbl_pct, &lv_font_montserrat_36, 0);
    lv_obj_set_style_text_color(lbl_pct, C_GREEN, 0);
    lv_label_set_text(lbl_pct, "0%");
    lv_obj_align(lbl_pct, LV_ALIGN_CENTER, 0, -116);

    lbl_volt = lv_label_create(parent);
    lv_obj_set_style_text_font(lbl_volt, &font_voltage, 0);
    lv_obj_set_style_text_color(lbl_volt, C_VOLT, 0);
    lv_label_set_text(lbl_volt, "0.0");
    lv_obj_align(lbl_volt, LV_ALIGN_CENTER, 0, -38);

    /* Unit: half-height grey "V" sitting to the right of the digits with its
     * baseline on the digits' baseline. The digits themselves are untouched. */
    lv_obj_t *unit = lv_label_create(parent);
    lv_obj_set_style_text_font(unit, &font_unit, 0);
    lv_obj_set_style_text_color(unit, C_UNIT, 0);
    lv_label_set_text(unit, "V");
    lv_obj_align_to(unit, lbl_volt, LV_ALIGN_OUT_RIGHT_BOTTOM, 1, -1);
    lbl_unit = unit;

    /* Status line right under the digits: text is pushed by the link layer.
     * Long messages scroll (LV_LABEL_LONG_SCROLL_CIRCULAR), short ones are
     * simply centred - see set_status_text(). */
    lbl_status = lv_label_create(parent);
    lv_obj_set_style_text_font(lbl_status, &lv_font_montserrat_32, 0);
    lv_obj_set_style_text_color(lbl_status, C_STATUS, 0);
    lv_label_set_long_mode(lbl_status, LV_LABEL_LONG_CLIP);
    lv_label_set_text(lbl_status, "");
    lv_obj_align(lbl_status, LV_ALIGN_CENTER, 0, 18);
}

static void build_chip(lv_obj_t *parent, int idx, int dx, int dy, int w)
{
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, w, 30);
    lv_obj_align(box, LV_ALIGN_CENTER, dx, dy);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_radius(box, 15, 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(box, C_CHIP_BG, 0);
    lv_obj_set_style_border_width(box, 1, 0);
    lv_obj_set_style_border_color(box, C_CHIP_OFF, 0);

    lv_obj_t *l = lv_label_create(box);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(l, C_CHIP_OFF, 0);
    lv_label_set_text(l, CHIP_NAMES[idx]);
    lv_obj_center(l);

    chips[idx] = box;
    chip_text[idx] = l;
}

static void build_chips(lv_obj_t *parent)
{
    /* Row 1 (CRSF / WIFI / STARLINK) is ~20 % wider than row 2 so the longer
     * words keep comfortable margins; the row stays inside the ring:
     * sqrt((dx + w/2 - 15)^2 + dy^2) + 15 < inner radius (159 px). */
    const int w1 = 91;          /* 76 * 1.2 */
    const int gap1 = 6;
    const int dx1 = w1 + gap1;  /* distance between neighbouring centres */
    build_chip(parent, 0, -dx1, 54, w1);
    build_chip(parent, 1, 0, 54, w1);
    build_chip(parent, 2, dx1, 54, w1);

    const int w2 = 76;
    const int dx2 = 84;
    build_chip(parent, 3, -dx2, 90, w2);
    build_chip(parent, 4, 0, 90, w2);
    build_chip(parent, 5, dx2, 90, w2);
}

/* Odometer under the second chip row: value 24 px, unit 14 px, baselines aligned. */
static void build_odometer(lv_obj_t *parent)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, 220, 30);
    lv_obj_align(row, LV_ALIGN_CENTER, 0, 128);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 4, 0);

    lbl_odo = lv_label_create(row);
    lv_obj_set_style_text_font(lbl_odo, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(lbl_odo, C_ODO, 0);
    lv_label_set_text(lbl_odo, "0");

    lbl_odo_unit = lv_label_create(row);
    lv_obj_set_style_text_font(lbl_odo_unit, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_odo_unit, C_DIM, 0);
    /* 24 px font has base_line 5, 14 px font has 3: +2 px padding lines the
     * baselines up when both boxes are aligned to the bottom. */
    lv_obj_set_style_pad_bottom(lbl_odo_unit, 2, 0);
    lv_label_set_text(lbl_odo_unit, "km");
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
    build_odometer(scr);

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

    /* lroundf + integer formatting: LVGL's formatter on this core does not
     * handle "%f" (produces a bare 'f'), so avoid float specifiers entirely. */
    int tenths = (int)lroundf(telem.voltage_v * 10.0f);
    lv_label_set_text_fmt(lbl_volt, "%d.%d", tenths / 10, tenths % 10);

    /* The digits are centred and their width depends on the value, so the unit
     * is re-glued to their bottom-right corner after every text change. */
    lv_obj_align_to(lbl_unit, lbl_volt, LV_ALIGN_OUT_RIGHT_BOTTOM, 1, -1);

    /* Low battery alarm: below 10 % the voltage and the percentage blink red.
     * The "off" phase paints the text in the background colour, so nothing
     * moves and the layout stays stable. */
    lv_color_t volt_c = C_VOLT;
    lv_color_t unit_c = C_UNIT;
    lv_color_t pct_c = zc;
    if (pct < LOW_BATTERY_PCT) {
        bool on = ((millis() / BLINK_HALF_MS) & 1) == 0;
        volt_c = on ? C_RED : C_BG;
        unit_c = on ? C_RED : C_BG;
        pct_c = on ? C_RED : C_BG;
    }
    lv_obj_set_style_text_color(lbl_pct, pct_c, 0);
    lv_obj_set_style_text_color(lbl_volt, volt_c, 0);
    lv_obj_set_style_text_color(lbl_unit, unit_c, 0);

    set_status_text(telem.status);
    set_chip(0, telem.rf_on);
    set_chip(1, telem.wifi_on);
    set_chip(2, telem.starlink_on);
    set_chip(3, telem.lte_on);
    set_chip(4, telem.vpn_on);

    bool gps = telem.sats > 0;
    set_chip(5, gps);
    lv_label_set_text_fmt(chip_text[5], gps ? "GPS %d" : "GPS", telem.sats);

    lv_label_set_text_fmt(lbl_odo, "%u", (unsigned)telem.odometer_km);
}
