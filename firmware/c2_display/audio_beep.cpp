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
#define BEEP_AMP     14000    /* ~43 % of full scale, the knob does the rest */
#define BEEP_LEN_MS  120      /* default length of one "peep" */
#define BEEP_GAP_MS  90       /* default pause inside a double/triple signal */

#define BEEP_FREQ_MIN 100
#define BEEP_FREQ_MAX 8000
#define BEEP_ON_MIN   20
#define BEEP_ON_MAX   2000
#define BEEP_OFF_MAX  2000

static i2s_chan_handle_t tx_chan;
static QueueHandle_t beep_q;

static void write_frames(const int16_t *frames, int cnt)
{
    size_t written = 0;
    i2s_channel_write(tx_chan, frames, (size_t)cnt * 2 * sizeof(int16_t),
                      &written, portMAX_DELAY);
}

static void tone(uint16_t freq_hz, uint16_t ms)
{
    int16_t buf[256 * 2];

    if (freq_hz < BEEP_FREQ_MIN) freq_hz = BEEP_FREQ_MIN;
    if (freq_hz > BEEP_FREQ_MAX) freq_hz = BEEP_FREQ_MAX;
    if (ms < BEEP_ON_MIN) ms = BEEP_ON_MIN;
    if (ms > BEEP_ON_MAX) ms = BEEP_ON_MAX;

    const int total = (int)((uint64_t)BEEP_RATE * ms / 1000u);
    const int ramp = BEEP_RATE / 200;   /* 5 ms fade in/out: no click at the edges */
    const float w = 2.0f * (float)M_PI * (float)freq_hz / (float)BEEP_RATE;

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
    beep_pattern_t p;

    for (;;) {
        if (xQueueReceive(beep_q, &p, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        if (p.count > BEEP_MAX_TONES) {
            p.count = BEEP_MAX_TONES;
        }

        for (uint8_t i = 0; i < p.count; i++) {
            tone(p.tone[i].freq_hz, p.tone[i].on_ms);

            uint16_t off = p.tone[i].off_ms;
            if (off > BEEP_OFF_MAX) off = BEEP_OFF_MAX;
            if (off) {
                silence(off);
            }
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

    beep_q = xQueueCreate(4, sizeof(beep_pattern_t));
    xTaskCreate(beep_task, "beep", 3072, NULL, 3, NULL);
}

bool beep_play(const beep_pattern_t *pattern)
{
    if (!beep_q || !pattern || pattern->count == 0) {
        return false;
    }
    return xQueueSend(beep_q, pattern, 0) == pdTRUE;
}

/* ---------------- ready-made patterns ---------------- */

static beep_pattern_t pattern_single(void)
{
    beep_pattern_t p = {};
    p.count = 1;
    p.tone[0] = {1000, BEEP_LEN_MS, 0};
    return p;
}

static beep_pattern_t pattern_double(void)
{
    beep_pattern_t p = {};
    p.count = 2;
    p.tone[0] = {1000, BEEP_LEN_MS, BEEP_GAP_MS};
    p.tone[1] = {1000, BEEP_LEN_MS, 0};
    return p;
}

static beep_pattern_t pattern_triple(void)
{
    beep_pattern_t p = {};
    p.count = 3;
    p.tone[0] = {1000, BEEP_LEN_MS, BEEP_GAP_MS};
    p.tone[1] = {1000, BEEP_LEN_MS, BEEP_GAP_MS};
    p.tone[2] = {1000, BEEP_LEN_MS, 0};
    return p;
}

static beep_pattern_t pattern_long(void)
{
    beep_pattern_t p = {};
    p.count = 1;
    p.tone[0] = {1000, 600, 0};
    return p;
}

bool beep_single(void)
{
    beep_pattern_t p = pattern_single();
    return beep_play(&p);
}

bool beep_double(void)
{
    beep_pattern_t p = pattern_double();
    return beep_play(&p);
}

bool beep_triple(void)
{
    beep_pattern_t p = pattern_triple();
    return beep_play(&p);
}

bool beep_long(void)
{
    beep_pattern_t p = pattern_long();
    return beep_play(&p);
}
