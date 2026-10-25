#pragma once

#include <stdint.h>

/* "Beep" signals through the onboard PCM5101 DAC and speaker amp.
 * Tones are generated in software (sine over I2S), so no SD card and no audio
 * files are needed. The worker task plays one pattern after another. */

#define BEEP_MAX_TONES 8

typedef struct {
    uint16_t freq_hz;   /* tone frequency, clamped to 100..8000 Hz */
    uint16_t on_ms;     /* tone length, clamped to 20..2000 ms */
    uint16_t off_ms;    /* pause after this tone, 0..2000 ms */
} beep_tone_t;

typedef struct {
    uint8_t     count;                  /* 1..BEEP_MAX_TONES */
    beep_tone_t tone[BEEP_MAX_TONES];
} beep_pattern_t;

void beep_init(void);

/* Non-blocking: copy the pattern into a small queue (deep enough for a couple
 * of signals). Returns false when the queue is full. */
bool beep_play(const beep_pattern_t *pattern);

/* Ready-made patterns; each returns the result of beep_play(). */
bool beep_single(void);
bool beep_double(void);
bool beep_triple(void);
bool beep_long(void);
