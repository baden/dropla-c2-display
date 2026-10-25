# C2 status display - UI specification

Read-only status panel for drone C2 based on Waveshare ESP32-S3-Touch-LCD-1.85
(360 x 360 round IPS).

## 1. Fixed requirements

| # | Requirement |
|---|---|
| 1 | **Touch is not used.** The panel only displays state; no buttons, no swipe, no input device registered in LVGL. |
| 2 | **Microphone / speech recognition are not used** and stay disabled (`ENABLE_SPEECH_RECOGNITION 0`). |
| 3 | **All on-screen text is English.** |
| 4 | **Few elements, large sizes.** The voltage is the hero value; everything else is secondary. |

## 2. Screen composition

```
        +----------- circle 360 -----------+
        |              95%                 |  <- 36 px, colored by zone
        |                                  |
        |             58.2 V               |  <- 110 px Arial Bold + 55 px "V"
        |        Starlink searching        |  <- 32 px status line
        |   (CRSF)  (WIFI)  (STARLINK)     |  <- row 1 of status chips
        |   (LTE)    (VPN)   (GPS 14)      |  <- row 2
        |              641 km              |  <- odometer (km/mi)
        +--- 36-segment battery ring ------+
```

### 2.1 Center - voltage

- Large pack voltage, one decimal, e.g. `58.2`.
- Font: Arial Bold 110 px subset (`0-9`, `.` and `-` for the no-data placeholder),
  generated with `lv_font_conv` -> `firmware/c2_display/font_voltage.c`.
- Until the first telemetry frame the value is the placeholder `--.-` in the dim
  colour `#5B6B7C` (the `-` glyph exists only for this state).
- Unit: a barely visible grey `V` (`#2B343E`) in half the size of the digits
  (Arial Bold 55 px, `font_unit.c`), placed 1 px to the right of the number and
  aligned to the bottom edge of the digits.
  The digits themselves stay exactly where they are (centred, `-38` px from the
  screen centre); the unit is re-glued after every text change because the width
  of the number varies.

### 2.2 Ring - state of charge

- Full circle of **36 fragments**, 13 px thick.
- Fragment geometry: 8 deg drawn + 2 deg gap; fragment 0 starts at 12 o'clock.
- Every fragment has a **fixed zone colour** (the ring is a coloured scale):
  - fragments covering `0 ... 20 %` -> red `#EF4444` (7 fragments)
  - `20 ... 40 %` -> amber `#F59E0B` (7 fragments)
  - `40 ... 100 %` -> green `#22C55E` (22 fragments)
- Filled fragments (up to the current percentage) are drawn at full brightness;
  the remaining ones keep the same hue, dimmed to ~27 % (`lv_color_mix` with the
  background). So the gauge always shows the scale *and* the current level.

### 2.3 Top - percentage

- Value only, no caption: `82%` (36 px) in the zone colour of the current
  percentage, centered near the top of the inner circle.
- Kept because the ring itself has no numeric scale.

### 2.4 Status chips

Six chips, 2 rows x 3, radius 15, 16 px text:

| Row | Size | Chip | Meaning |
|---|---|---|---|
| 1 | 91 x 30 (20 % wider, 6 px gaps) | `CRSF` | Crossfire radio control link is active |
| 1 | | `WIFI` | control over Wi-Fi is active (drone side - not the display's own Wi-Fi) |
| 1 | | `STARLINK` | Starlink link is up |
| 2 | 76 x 30 (8 px gaps) | `LTE` | LTE link is up |
| 2 | | `VPN` | VPN tunnel is up |
| 2 | | `GPS` | shows satellite count when the drone sees any (`GPS 14`), otherwise just `GPS` |

States:

| State | Border / text | Background |
|---|---|---|
| on | `#22C55E` | `#0A2A18` |
| off | `#46566A` | `#0B1118` |

### 2.5 Status line

A single line right under the voltage digits: 32 px, centred at `+18` px from the
screen centre. The text comes from a **NOTIFY** frame of the link (see
`docs/link-protocol.md`); the firmware only renders it.

- messages up to **300 px** wide are shown statically, centred;
- anything wider automatically switches to `LV_LABEL_LONG_SCROLL_CIRCULAR`
  (circular marquee inside a 300 px window), so long messages are never clipped;
- the label buffer is 64 bytes (63 visible characters + `\0`).

Message flags (from the frame):

| Flag | Effect |
|---|---|
| severity `debug` | dim grey `#5B6B7C` |
| severity `info` | grey-blue `#8695A5` |
| severity `warning` | amber `#F59E0B` |
| severity `error` | red `#EF4444` |
| `flashing` | text blinks continuously, 1 s period (off phase = background colour, layout stays put) |
| `hide after 5 seconds` | the message disappears 5 s after it arrived |
| empty text | clears the line immediately |

A new message replaces the previous one and restarts its timers. If the link has
timed out and no message is active, the line shows `Link lost` in amber; a message
always wins over that fallback. Before the first frame the line is empty.

### 2.6 Low battery alarm

When the state of charge drops **below 10 %**:

- the big voltage number (and its `V` unit) and the percentage turn **red** `#EF4444`;
- both **blink** with a 1 s period (500 ms red / 500 ms hidden) - the "off"
  phase paints the text in the background colour, so nothing moves;
- above 10 % the voltage is white again and the percentage uses its zone colour.

### 2.7 Odometer

Under the second chip row, centred (`+126` px from the screen centre):

- value `641` in 32 px (same size as the status line), colour `#93A3B5`;
- unit `km` / `mi` in 18 px, dim `#5B6B7C`, baselines aligned with the value
  (`pad_bottom = 2`, because the 32 px font has `base_line` 6 and the 18 px
  font has `base_line` 4);
- the pair is laid out in a centred flex row, so it stays centred when the
  number grows (`999` -> `1000`);
- **the unit comes from the CONFIG frame** (`0` = km, `1` = mi). It is sent once
  at startup and again whenever the setting changes; kilometres are the default
  until the first CONFIG arrives;
- miles are derived from the stored decimetres: `mi = round(dm * 0.621371 / 10000)`;
- before the first telemetry frame the value is the placeholder `--` (dim), the
  unit stays visible.

### 2.8 Boot animation

On power-up the panel plays a ~2.7 s self-test, then switches to live data:

| Time | What happens |
|---|---|
| 0 … 1000 ms | ring sweeps 0 -> 100 % (all 36 fragments light up), stays full |
| 1000 ms | voltage `88.8` + `V` fade in (260 ms fade) |
| 1250 ms | charge `100%` fades in |
| 1450 ms | status line shows `System check` |
| 1550 ms + 90 ms each | six chips fade in one by one, all in the green "on" state |
| 2300 ms | odometer fades in (`0 km`) |
| 2700 ms | everything jumps to the real telemetry (or to the `--` placeholders if no frame has arrived yet) |

During the animation `ui_update()` is gated by `boot_active`, so nothing
overwrites the placeholders; the ring is driven by a separate `lv_anim`
(`boot_ring_cb` -> `ring_paint()`).

The fade is **not** an opacity animation: every element starts painted in the
background colour and its colours are mixed towards the final ones
(`lv_color_mix(target, C_BG, v)`). Semi-transparent objects would make LVGL
render into a temporary layer and leave white boxes around the text on this
panel. At the end a one-shot `lv_timer` finishes all fades, restores the exact
colours and calls `ui_update()` once. A `boot animation done` line is printed on
the serial console.

The backlight stays off (`Set_Backlight(0)`) until the first frame is flushed
(`lv_refr_now()`), otherwise the uninitialised panel shows red/blue noise for
the duration of the init.

## 3. Data model

`telemetry.h` -> `telemetry_t`, filled by `link.cpp` from received frames:

| Field | Type | Meaning |
|---|---|---|
| `valid` | bool | at least one TELEMETRY frame received |
| `voltage_cv` | uint16 | pack voltage, centivolts (`5820` = 58.20 V) |
| `batt_pct` | uint8 | state of charge 0...100, `255` = unknown (percentage hidden) |
| `flags` | uint8 | `C2_LINK_*` bitfield (CRSF, WIFI, STARLINK, LTE, VPN, GPS_FIX) |
| `sats` | uint8 | visible GNSS satellites |
| `odometer_dm` | uint32 | total distance flown, decimetres (`6 340 000` = 634 km) |
| `last_rx_ms` | uint32 | `millis()` of the last valid telemetry frame |
| `notify[64]` | char | current notification text, `""` = none |
| `notify_sev` | uint8 | 0 debug, 1 info, 2 warning, 3 error |
| `notify_flags` | uint8 | `C2_NOTIFY_BLINK`, `C2_NOTIFY_AUTOHIDE` |
| `notify_seq` | uint32 | increments on every NOTIFY frame (0 = none yet) |
| `notify_ms` | uint32 | when the notification arrived |
| `cfg_valid` | bool | at least one CONFIG frame received |
| `odometer_unit` | uint8 | `C2_ODO_KM` (0) or `C2_ODO_MI` (1) |
| `uptime_ms`, `rx_frames`, `rx_errors` | uint32 | housekeeping, also used by PONG |

Bit assignments live next to the struct (`C2_LINK_CRSF` = `0x01` ...
`C2_LINK_GPS_FIX` = `0x20`, severity mask `0x03`). The UI only reads this structure;
`telemetry_update()` refreshes `uptime_ms` at 10 Hz, `telemetry_fresh()` answers
"was the last telemetry frame recent enough" (10 s window).

## 4. Link and the "no data" state

`link.cpp` receives frames from USB CDC: a byte-wise state machine
(`A5 5A` -> header -> payload -> CRC16), a dispatcher for TELEMETRY / NOTIFY /
CONFIG / PING and a PONG sender. `link_poll()` is called from `loop()` on every
iteration; the frame format is documented in `docs/link-protocol.md`.

Until the first TELEMETRY frame the panel shows a neutral "no data" state:

- voltage `--.-` (dim `#5B6B7C`), no percentage at all (the label is hidden);
- ring empty, every chip grey, `GPS` without a count;
- odometer `--` with the configured unit (`km` until CONFIG says otherwise);
- empty status line.

If telemetry stops for **10 s** (`LINK_TIMEOUT_MS`), the chips go grey, the values
and the ring stay on screen but dim, and the status line shows `Link lost` in amber
(unless a notification is active). Frames coming back restore everything without a
reboot and without replaying the boot animation.

There is **no simulator** in the firmware any more. To exercise the panel from a PC
use the test client (Protocol is in `docs/link-protocol.md` §8):

```powershell
py tools/c2link.py --port COM4 demo
py tools/c2link.py --port COM4 notify "Carrier weak" --severity warning --blink
```

## 5. Implementation notes

- Custom fonts are generated once:
  `npx lv_font_conv --font arialbd.ttf --range 0x2D --range 0x2E --range 0x30-0x39 --size 110 --bpp 4 --format lvgl --no-compress -o font_voltage.c`
  `npx lv_font_conv --font arialbd.ttf --range 0x56 --size 55 --bpp 4 --format lvgl --no-compress -o font_unit.c`
  (`-` is only needed for the `--.-` placeholder; only the glyphs that are
  actually used, so the bitmap tables stay small).
- **Never use `%f` in `lv_label_set_text_fmt()` on this core** - LVGL's formatter
  emits a bare `f` character, which is not present in the digits-only font and
  renders as a placeholder square. Format voltage as integers:
  `lv_label_set_text_fmt(lbl, "%d.%d", tenths / 10, tenths % 10);`
- No LVGL input device is registered at all (`lvgl_port.cpp`).
