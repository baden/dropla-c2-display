#include "audio_beep.h"

#include <Arduino.h>
#include <math.h>

#include "driver/i2s_std.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

/* PCM5101 I2S DAC (same pins the vendor demo uses for MP3 playback).
 * The speaker amplifier is always on; its volume is set by the knob on the
 * board, so the firmware just needs a reasonable digital level. */
#define BEEP_I2S_PORT  I2S_NUM_0
#define BEEP_PIN_BCLK  48
#define BEEP_PIN_LRC   38
#define BEEP_PIN_DOUT  47

#define BEEP_RATE    44100
#define BEEP_FREQ_HZ 1000
#define BEEP_AMP     14000    /* ~43 % of full scale, the knob does the rest */
#define BEEP_LEN_MS  120
#define BEEP_GAP_MS  90

enum {
    BEEP_CMD_SINGLE = 1,
    BEEP_CMD_DOUBLE = 2,
};

static i2s_chan_handle_t tx_chan;
static QueueHandle_t beep_q;

/* Write `cnt` stereo frames of a 1 kHz sine (or silence) to the DAC. */
static void write_frames(const int16_t *frames, int cnt)
{
    size_t written = 0;
    i2s_channel_write(tx_chan, frames, (size_t)cnt * 2 * sizeof(int16_t),
                      &written, portMAX_DELAY);
}

static void tone(uint32_t ms)
{
    int16_t buf[256 * 2];

    const int total = (int)((uint64_t)BEEP_RATE * ms / 1000u);
    const int ramp = BEEP_RATE / 200;   /* 5 ms fade in/out: no click at the edges */
    const float w = 2.0f * (float)M_PI * (float)BEEP_FREQ_HZ / (float)BEEP_RATE;

    for (int n = 0; n < total; n += 256) {
        int cnt = (total - n < 256) ? (total - n) : 256;

        for (int i = 0; i < cnt; i++) {
            int k = n + i;
            float env = 1.0f;
            if (k < ramp) {
                env = (float)k / (float)ramp;
            } else if (k > total - ramp) {
                env = (float)(total - k) / (float)ramp;
            }
            int16_t v = (int16_t)(sinf(w * (float)k) * (float)BEEP_AMP * env);
            buf[i * 2] = v;
            buf[i * 2 + 1] = v;
        }

        write_frames(buf, cnt);
    }
}

static void silence(uint32_t ms)
{
    int16_t buf[256 * 2] = {0};

    int total = (int)((uint64_t)BEEP_RATE * ms / 1000u);
    while (total > 0) {
        int cnt = (total < 256) ? total : 256;
        write_frames(buf, cnt);
        total -= cnt;
    }
}

static void beep_task(void *)
{
    uint8_t cmd;

    for (;;) {
        if (xQueueReceive(beep_q, &cmd, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        if (cmd == BEEP_CMD_DOUBLE) {
            tone(BEEP_LEN_MS);
            silence(BEEP_GAP_MS);
            tone(BEEP_LEN_MS);
        } else {
            tone(BEEP_LEN_MS);
        }
        silence(40);   /* let the tail play out, then leave the DAC quiet */
    }
}

void beep_init(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(BEEP_I2S_PORT, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;    /* idle channel sends zeros instead of the last sample */
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &tx_chan, NULL));

    i2s_std_config_t std_cfg = {};
    std_cfg.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(BEEP_RATE);
    std_cfg.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                          I2S_SLOT_MODE_STEREO);
    std_cfg.gpio_cfg.mclk = I2S_GPIO_UNUSED;   /* PCM5101 runs from BCLK alone */
    std_cfg.gpio_cfg.bclk = (gpio_num_t)BEEP_PIN_BCLK;
    std_cfg.gpio_cfg.ws = (gpio_num_t)BEEP_PIN_LRC;
    std_cfg.gpio_cfg.dout = (gpio_num_t)BEEP_PIN_DOUT;
    std_cfg.gpio_cfg.din = I2S_GPIO_UNUSED;

    ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_chan, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(tx_chan));

    beep_q = xQueueCreate(4, sizeof(uint8_t));
    xTaskCreate(beep_task, "beep", 3072, NULL, 3, NULL);
}

void beep_single(void)
{
    uint8_t cmd = BEEP_CMD_SINGLE;
    if (beep_q) {
        xQueueSend(beep_q, &cmd, 0);
    }
}

void beep_double(void)
{
    uint8_t cmd = BEEP_CMD_DOUBLE;
    if (beep_q) {
        xQueueSend(beep_q, &cmd, 0);
    }
}
