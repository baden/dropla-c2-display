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
- Font: Arial Bold 110 px subset (`0-9`, `.`), generated with `lv_font_conv`
  -> `firmware/c2_display/font_voltage.c`.
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

A single line right under the voltage digits: 32 px, colour `#8695A5`,
centred at `+18` px from the screen centre. The text is whatever the link layer
puts into `telemetry.status` (max 47 characters); the firmware only renders it.

- messages up to **300 px** wide are shown statically, centred;
- anything wider automatically switches to `LV_LABEL_LONG_SCROLL_CIRCULAR`
  (circular marquee inside a 300 px window), so long messages are never clipped.

Demo rotation (4 s per message, all of them fit at 32 px):

1. `Starlink searching`
2. `Carrier connected`
3. `Charging...`
4. `VPN unavailable`

Measured widths at 32 px: 294 / 295 / 173 / 267 px.

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
- **the unit alternates every 5 s**: kilometres for 5 s, miles for 5 s, looping
  (`mi = round(km * 0.621371)`).

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
| 2700 ms | everything jumps to the real telemetry, the demo starts |

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

`telemetry.h` -> `telemetry_t`:

| Field | Type | Meaning |
|---|---|---|
| `voltage_v` | float | pack voltage, V (14S demo: 42.0 ... 58.8) |
| `batt_pct` | uint8 | state of charge, 0...100 |
| `rf_on` | bool | RC link |
| `wifi_on` | bool | Wi-Fi control link |
| `starlink_on` | bool | Starlink link |
| `lte_on` | bool | LTE link |
| `vpn_on` | bool | VPN tunnel |
| `sats` | uint8 | visible GNSS satellites |
| `odometer_km` | uint32 | total distance flown, km |
| `status` | char[48] | status line text under the voltage (47 chars max) |
| `uptime_ms` | uint32 | milliseconds since boot |

The UI reads this structure only; it never writes to it (`telemetry_update()`
is called from the main loop at 10 Hz).

## 4. Simulator (temporary)

`C2_SIMULATE 1` in `telemetry.cpp`:

- battery: 70 s per cycle, so the low-battery alarm is easy to check:
  - `0...20 s` 95 -> 45 % (green ring),
  - `20...40 s` 45 -> 15 % (green -> amber),
  - `40...60 s` 15 -> 5 % (amber -> red),
  - `60...70 s` held at 5 % (blinking red digits);
- voltage derived from the percentage: 42.0 V ... 58.8 V (14S);
- `CRSF` and `VPN` always on; `WIFI`, `STARLINK`, `LTE` blink with different periods;
- satellites: 12 +- 4;
- odometer: starts at 634 km, +1 km every 30 s; the UI itself flips the unit
  (`km` <-> `mi`) every 5 s;
- status line: cycles through four short demo messages, 4 s each
  (`Starlink searching`, `Carrier connected`, `Charging...`, `VPN unavailable`).

Set `C2_SIMULATE 0` when a real link fills `telemetry_t`.

## 5. Implementation notes

- Custom fonts are generated once:
  `npx lv_font_conv --font arialbd.ttf --range 0x30-0x39 --range 0x2E --size 110 --bpp 4 --format lvgl -o font_voltage.c`
  `npx lv_font_conv --font arialbd.ttf --range 0x56 --size 55 --bpp 4 --format lvgl -o font_unit.c`
  (only the glyphs that are actually used, so the bitmap tables stay small).
- **Never use `%f` in `lv_label_set_text_fmt()` on this core** - LVGL's formatter
  emits a bare `f` character, which is not present in the digits-only font and
  renders as a placeholder square. Format voltage as integers:
  `lv_label_set_text_fmt(lbl, "%d.%d", tenths / 10, tenths % 10);`
- No LVGL input device is registered at all (`lvgl_port.cpp`).
