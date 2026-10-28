#include "link.h"

#include <Arduino.h>
#include <stdio.h>
#include <string.h>

#include "audio_beep.h"
#include "telemetry.h"
#include "Display_ST77916.h"
#include "settings.h"
#include "ui.h"

/* ---------------- frame format ----------------
 *   A5 5A | ver_type | seq | len(2, LE) | payload | crc16(2, LE)
 *   crc16 covers ver_type..payload (CRC-16/CCITT-FALSE)
 * ver_type: high nibble = protocol version, low nibble = message type.
 */
#define LINK_MAGIC1      0xA5
#define LINK_MAGIC2      0x5A
#define LINK_VERSION     1
#define LINK_MAX_PAYLOAD 64

#define MSG_TELEMETRY 0x1
#define MSG_NOTIFY    0x2
#define MSG_CONFIG    0x3
#define MSG_PING      0x4
#define MSG_SOUND     0x5
#define MSG_PONG      0x8

/* SOUND kinds (see docs/link-protocol.md §5.5) */
#define SND_SINGLE       0x00   /* one short peep                                  */
#define SND_DOUBLE       0x01   /* peep - pause - peep                             */
#define SND_TRIPLE       0x02   /* peep - peep - peep                              */
#define SND_LONG         0x03   /* one long tone                                   */
#define SND_SEQUENCE     0x10   /* custom tones: count, then count x (Hz, on, off) */
#define SND_CHARGE_START 0x20   /* charger connected: two rising tones             */
#define SND_CHARGE_DONE  0x21   /* charging finished: three rising tones           */

/* CONFIG tags (TLV) */
#define CFG_TAG_ODO_UNIT 0x01
#define CFG_TAG_ROTATION 0x02   /* 0, 90, 180 or 270 degrees */

/* ---------------- parser state ---------------- */
enum {
    ST_MAGIC1 = 0,
    ST_MAGIC2,
    ST_HEADER,
    ST_PAYLOAD,
    ST_CRC_LO,
    ST_CRC_HI,
};

static uint8_t  st = ST_MAGIC1;
static uint8_t  hdr[4];        /* ver_type, seq, len_lo, len_hi */
static uint8_t  hdr_n;
static uint8_t  pay[LINK_MAX_PAYLOAD];
static uint16_t pay_n;
static uint16_t pay_len;
static uint16_t crc_calc;
static uint16_t crc_rx;
static uint32_t last_err_log;
static bool     link_ok = true;   /* previous "telemetry fresh" state */

static uint16_t crc16_update(uint16_t crc, uint8_t b)
{
    crc ^= (uint16_t)b << 8;
    for (int i = 0; i < 8; i++) {
        crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
    }
    return crc;
}

/* ---------------- transmitter (PONG) ---------------- */
static uint8_t tx_seq;

static void send_frame(uint8_t type, const uint8_t *payload, uint16_t len)
{
    uint8_t buf[6 + LINK_MAX_PAYLOAD + 2];

    if (len > LINK_MAX_PAYLOAD) {
        return;
    }

    buf[0] = LINK_MAGIC1;
    buf[1] = LINK_MAGIC2;
    buf[2] = (uint8_t)((LINK_VERSION << 4) | (type & 0x0F));
    buf[3] = ++tx_seq;
    buf[4] = (uint8_t)(len & 0xFF);
    buf[5] = (uint8_t)(len >> 8);
    if (len) {
        memcpy(buf + 6, payload, len);
    }

    uint16_t crc = 0xFFFF;
    for (uint16_t i = 2; i < 6 + len; i++) {
        crc = crc16_update(crc, buf[i]);
    }
    buf[6 + len] = (uint8_t)(crc & 0xFF);
    buf[7 + len] = (uint8_t)(crc >> 8);

    Serial.write(buf, 8 + len);
}

static void send_pong(uint8_t echoed_seq)
{
    uint8_t p[10];
    uint32_t up = millis();

    p[0] = LINK_VERSION;
    p[1] = (uint8_t)(up & 0xFF);
    p[2] = (uint8_t)((up >> 8) & 0xFF);
    p[3] = (uint8_t)((up >> 16) & 0xFF);
    p[4] = (uint8_t)((up >> 24) & 0xFF);
    p[5] = (uint8_t)(telem.rx_frames & 0xFF);
    p[6] = (uint8_t)((telem.rx_frames >> 8) & 0xFF);
    p[7] = (uint8_t)((telem.rx_frames >> 16) & 0xFF);
    p[8] = (uint8_t)((telem.rx_frames >> 24) & 0xFF);
    p[9] = echoed_seq;

    send_frame(MSG_PONG, p, sizeof(p));
}

/* ---------------- message handlers ---------------- */
static void on_telemetry(const uint8_t *p, uint16_t len)
{
    if (len < 9) {
        telem.rx_errors++;
        return;
    }

    telem.voltage_cv = (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
    telem.batt_pct = p[2];
    telem.flags = p[3];
    telem.sats = p[4];
    telem.odometer_dm = (uint32_t)p[5] | ((uint32_t)p[6] << 8) |
                        ((uint32_t)p[7] << 16) | ((uint32_t)p[8] << 24);
    telem.last_rx_ms = millis();
    telem.rx_frames++;

    if (!telem.valid) {
        telem.valid = true;
        printf("link: first telemetry: %u.%02u V, %u %%, flags 0x%02X, %u sats, odo %lu dm\r\n",
               telem.voltage_cv / 100, telem.voltage_cv % 100,
               telem.batt_pct, telem.flags, telem.sats,
               (unsigned long)telem.odometer_dm);
    }
}

static void on_notify(const uint8_t *p, uint16_t len)
{
    if (len < 2) {
        telem.rx_errors++;
        return;
    }

    uint8_t flags = p[0];
    uint16_t n = p[1];
    if (n > len - 2) {
        n = len - 2;
    }
    if (n > C2_NOTIFY_LEN - 1) {
        n = C2_NOTIFY_LEN - 1;
    }

    memcpy(telem.notify, p + 2, n);
    telem.notify[n] = '\0';
    telem.notify_sev = flags & C2_NOTIFY_SEV_MASK;
    telem.notify_flags = flags & (C2_NOTIFY_BLINK | C2_NOTIFY_AUTOHIDE);
    telem.notify_seq++;
    telem.notify_ms = millis();

    printf("link: notify sev=%u flags=0x%02X \"%s\"\r\n",
           telem.notify_sev, telem.notify_flags, telem.notify);
}

static void on_config(const uint8_t *p, uint16_t len)
{
    if (len < 1) {
        telem.rx_errors++;
        return;
    }

    uint8_t count = p[0];
    uint16_t i = 1;

    for (uint8_t k = 0; k < count && i + 1 < len; k++) {
        uint8_t tag = p[i];
        uint8_t l = p[i + 1];
        i = (uint16_t)(i + 2);

        if (i + l > len) {
            break;      /* truncated TLV: ignore the rest of the frame */
        }

        switch (tag) {
        case CFG_TAG_ODO_UNIT:
            if (l >= 1) {
                telem.odometer_unit = (p[i] == C2_ODO_MI) ? C2_ODO_MI : C2_ODO_KM;
            }
            break;

        case CFG_TAG_ROTATION:
            if (l >= 2) {
                /* u16 LE, degrees: 0, 90, 180 or 270. */
                uint16_t deg = (uint16_t)(p[i] | ((uint16_t)p[i + 1] << 8));
                if (deg == 0 || deg == 90 || deg == 180 || deg == 270) {
                    if (deg != LCD_GetRotation()) {
                        LCD_SetRotation(deg);
                        settings_rotation_save(deg);
                        ui_redraw_all();
                    }
                } else {
                    printf("link: config rotation %u ignored (expected 0/90/180/270)\r\n", deg);
                }
            } else {
                printf("link: config rotation tag needs 2 bytes\r\n");
            }
            break;

        default:
            break;      /* unknown tags are skipped on purpose (forward compatible) */
        }

        i = (uint16_t)(i + l);
    }

    telem.cfg_valid = true;
    printf("link: config odo_unit=%u rotation=%u\r\n",
           telem.odometer_unit, LCD_GetRotation());
}

static void on_sound(const uint8_t *p, uint16_t len)
{
    if (len < 2) {
        telem.rx_errors++;
        return;
    }

    uint8_t kind = p[0];
    /* p[1] = flags: reserved for now (e.g. "replace what is playing"), must be 0 */

    beep_pattern_t pat = {};

    switch (kind) {
    case SND_SINGLE:
        pat.count = 1;
        pat.tone[0] = {1000, 120, 0};
        break;

    case SND_DOUBLE:
        pat.count = 2;
        pat.tone[0] = {1000, 120, 90};
        pat.tone[1] = {1000, 120, 0};
        break;

    case SND_TRIPLE:
        pat.count = 3;
        pat.tone[0] = {1000, 120, 90};
        pat.tone[1] = {1000, 120, 90};
        pat.tone[2] = {1000, 120, 0};
        break;

    case SND_LONG:
        pat.count = 1;
        pat.tone[0] = {1000, 600, 0};
        break;

    case SND_SEQUENCE: {
        if (len < 3) {
            telem.rx_errors++;
            return;
        }

        uint8_t n = p[2];
        if (n == 0) {
            return;
        }
        if (n > BEEP_MAX_TONES) {
            n = BEEP_MAX_TONES;         /* extra tones are ignored, the rest is played */
        }
        if ((uint16_t)(3 + n * 6) > len) {
            telem.rx_errors++;          /* truncated tone list */
            return;
        }

        pat.count = n;
        for (uint8_t i = 0; i < n; i++) {
            const uint8_t *t = p + 3 + i * 6;
            pat.tone[i].freq_hz = (uint16_t)(t[0] | ((uint16_t)t[1] << 8));
            pat.tone[i].on_ms = (uint16_t)(t[2] | ((uint16_t)t[3] << 8));
            pat.tone[i].off_ms = (uint16_t)(t[4] | ((uint16_t)t[5] << 8));
        }
        break;
    }

    case SND_CHARGE_START:      /* charger connected */
        pat.count = 2;
        pat.tone[0] = {800, 120, 60};
        pat.tone[1] = {1200, 160, 0};
        break;

    case SND_CHARGE_DONE:       /* charging finished */
        pat.count = 3;
        pat.tone[0] = {1000, 140, 70};
        pat.tone[1] = {1300, 140, 70};
        pat.tone[2] = {1600, 180, 0};
        break;

    default:
        printf("link: sound kind=0x%02X unknown, ignored\r\n", kind);
        return;
    }

    if (!beep_play(&pat)) {
        printf("link: sound kind=0x%02X dropped (queue full)\r\n", kind);
        return;
    }

    printf("link: sound kind=0x%02X tones=%u\r\n", kind, pat.count);
}

static void handle_frame(uint8_t ver_type, uint8_t seq, const uint8_t *p, uint16_t len)
{
    uint8_t ver = ver_type >> 4;
    uint8_t type = ver_type & 0x0F;

    if (ver != LINK_VERSION) {
        return;         /* other protocol versions are ignored silently */
    }

    switch (type) {
    case MSG_TELEMETRY: on_telemetry(p, len); break;
    case MSG_NOTIFY:    on_notify(p, len); break;
    case MSG_CONFIG:    on_config(p, len); break;
    case MSG_SOUND:     on_sound(p, len); break;
    case MSG_PING:      send_pong(seq); break;
    default:            telem.rx_errors++; break;
    }
}

static void feed(uint8_t b)
{
    switch (st) {
    case ST_MAGIC1:
        if (b == LINK_MAGIC1) {
            st = ST_MAGIC2;
        }
        break;

    case ST_MAGIC2:
        if (b == LINK_MAGIC2) {
            st = ST_HEADER;
            hdr_n = 0;
            crc_calc = 0xFFFF;
        } else if (b != LINK_MAGIC1) {
            st = ST_MAGIC1;
        }
        break;

    case ST_HEADER:
        hdr[hdr_n++] = b;
        crc_calc = crc16_update(crc_calc, b);
        if (hdr_n == 4) {
            pay_len = (uint16_t)hdr[2] | ((uint16_t)hdr[3] << 8);
            pay_n = 0;
            if (pay_len > LINK_MAX_PAYLOAD) {
                telem.rx_errors++;
                st = ST_MAGIC1;
            } else {
                st = pay_len ? ST_PAYLOAD : ST_CRC_LO;
            }
        }
        break;

    case ST_PAYLOAD:
        pay[pay_n++] = b;
        crc_calc = crc16_update(crc_calc, b);
        if (pay_n >= pay_len) {
            st = ST_CRC_LO;
        }
        break;

    case ST_CRC_LO:
        crc_rx = b;
        st = ST_CRC_HI;
        break;

    case ST_CRC_HI:
        crc_rx |= (uint16_t)b << 8;
        if (crc_rx == crc_calc) {
            handle_frame(hdr[0], hdr[1], pay, pay_len);
        } else {
            telem.rx_errors++;
            if (millis() - last_err_log > 1000) {
                last_err_log = millis();
                printf("link: crc error (calc 0x%04X, got 0x%04X)\r\n", crc_calc, crc_rx);
            }
        }
        st = ST_MAGIC1;
        break;

    default:
        st = ST_MAGIC1;
        break;
    }
}

/* ---------------- public ---------------- */
void link_init(void)
{
    st = ST_MAGIC1;
    tx_seq = 0;
    link_ok = telemetry_fresh();   /* no data yet, so it starts as "not ok" */
    printf("link: USB CDC ready (115200 8N1, baud is nominal for USB)\r\n");
}

void link_poll(void)
{
    /* Bounded work per call: the loop also has to run LVGL. */
    int budget = 512;
    while (budget-- > 0) {
        int c = Serial.read();
        if (c < 0) {
            break;
        }
        feed((uint8_t)c);
    }

    bool ok = telemetry_fresh();
    if (ok != link_ok) {
        link_ok = ok;
        printf(ok ? "link: telemetry ok\r\n" : "link: telemetry lost (timeout)\r\n");
    }
}
