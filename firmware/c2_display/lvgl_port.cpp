#include "lvgl_port.h"

#include <lvgl.h>
#include <esp_timer.h>

#include "Display_ST77916.h"
#include "Touch_CST816.h"

/* 40 строк на буфер: 360 * 40 * 2 байта = 28.8 КБ, два буфера ~57 КБ internal RAM.
 * PSRAM для flush не подходит — SPI DMA на S3 требует internal RAM. */
#define LV_BUF_LINES 40

static lv_disp_draw_buf_t draw_buf;
static lv_color_t buf_a[EXAMPLE_LCD_WIDTH * LV_BUF_LINES];
static lv_color_t buf_b[EXAMPLE_LCD_WIDTH * LV_BUF_LINES];

static void disp_flush(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *px_map)
{
    /* LCD_addWindow сам делает байт-свап под RGB565 панели */
    LCD_addWindow(area->x1, area->y1, area->x2, area->y2, (uint16_t *)&px_map->full);
    lv_disp_flush_ready(drv);
}

static void touch_read(lv_indev_drv_t *drv, lv_indev_data_t *data)
{
    static uint16_t last_x = 0;
    static uint16_t last_y = 0;

    Touch_Read_Data();

    if (touch_data.points) {
        last_x = touch_data.x;
        last_y = touch_data.y;
        data->state = LV_INDEV_STATE_PR;
    } else {
        data->state = LV_INDEV_STATE_REL;
    }

    data->point.x = last_x;
    data->point.y = last_y;

    /* сброс, как в демо производителя */
    touch_data.x = 0;
    touch_data.y = 0;
    touch_data.points = 0;
    touch_data.gesture = NONE;
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
    disp_drv.full_refresh = 0;  /* обновляем только грязные области */
    lv_disp_drv_register(&disp_drv);

    static lv_indev_drv_t indev_drv;
    lv_indev_drv_init(&indev_drv);
    indev_drv.type = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = touch_read;
    lv_indev_drv_register(&indev_drv);

    const esp_timer_create_args_t tick_args = {
        .callback = &tick_cb,
        .name = "lvgl_tick",
    };
    esp_timer_handle_t tick_timer = NULL;
    esp_timer_create(&tick_args, &tick_timer);
    esp_timer_start_periodic(tick_timer, 2000);  /* 2 мс */
}
