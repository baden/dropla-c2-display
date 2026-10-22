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
        |                                  |
        |   (CRSF)  (WIFI)  (STARLINK)     |  <- row 1 of status chips
        |   (LTE)    (VPN)   (GPS 14)      |  <- row 2
        |            634 km                |  <- odometer
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

### 2.5 Low battery alarm

When the state of charge drops **below 10 %**:

- the big voltage number (and its `V` unit) and the percentage turn **red** `#EF4444`;
- both **blink** with a 1 s period (500 ms red / 500 ms hidden) - the "off"
  phase paints the text in the background colour, so nothing moves;
- above 10 % the voltage is white again and the percentage uses its zone colour.

### 2.6 Odometer

Under the second chip row, centred (`+128` px from the screen centre):

- value `634` in 24 px, colour `#93A3B5`;
- unit `km` in 14 px, dim `#5B6B7C`, baselines aligned with the value;
- the pair is laid out in a centred flex row, so it stays centred when the
  number grows (`999` -> `1000`).

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
- odometer: starts at 634 km, +1 km every 30 s.

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
