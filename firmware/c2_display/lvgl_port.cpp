#include "lvgl_port.h"

#include <lvgl.h>
#include <esp_timer.h>

#include "Display_ST77916.h"

/* 40 lines per buffer: 360 * 40 * 2 bytes = 28.8 KB, two buffers ≈ 57 KB of
 * internal RAM. PSRAM cannot be used here — SPI DMA on the S3 needs internal RAM. */
#define LV_BUF_LINES 40

static lv_disp_draw_buf_t draw_buf;
static lv_color_t buf_a[EXAMPLE_LCD_WIDTH * LV_BUF_LINES];
static lv_color_t buf_b[EXAMPLE_LCD_WIDTH * LV_BUF_LINES];

static void disp_flush(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *px_map)
{
    /* LCD_addWindow swaps bytes for the RGB565 panel itself */
    LCD_addWindow(area->x1, area->y1, area->x2, area->y2, (uint16_t *)&px_map->full);
    lv_disp_flush_ready(drv);
}

static void tick_cb(void *arg)
{
    (void)arg;
    lv_tick_inc(2);
}

void lvgl_port_init(void)
{
    lv_init();

    lv_disp_draw_buf_init(&draw_buf, buf_a, buf_b, EXAMPLE_LCD_WIDTH * LV_BUF_LINES);

    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = EXAMPLE_LCD_WIDTH;
    disp_drv.ver_res = EXAMPLE_LCD_HEIGHT;
    disp_drv.flush_cb = disp_flush;
    disp_drv.draw_buf = &draw_buf;
    disp_drv.full_refresh = 0;   /* redraw only the dirty areas */
    lv_disp_drv_register(&disp_drv);

    /* No input device: the screen is a read-only status display. */

    const esp_timer_create_args_t tick_args = {
        .callback = &tick_cb,
        .name = "lvgl_tick",
    };
    esp_timer_handle_t tick_timer = NULL;
    esp_timer_create(&tick_args, &tick_timer);
    esp_timer_start_periodic(tick_timer, 2000);   /* 2 ms */
}
