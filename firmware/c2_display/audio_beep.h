#pragma once

/* Short "beep" signals through the onboard PCM5101 DAC and speaker amp.
 * The beeps are generated in software (sine wave over I2S), so no SD card
 * and no audio files are needed. Calls are non-blocking: they only post a
 * command to a small worker task. */
void beep_init(void);
void beep_single(void);   /* one short "peep"           */
void beep_double(void);   /* "peep - pause - peep"      */
