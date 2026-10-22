# C2 status display — UI specification

Read-only status panel for drone C2 based on Waveshare ESP32-S3-Touch-LCD-1.85
(360 × 360 round IPS).

## 1. Fixed requirements

| # | Requirement |
|---|---|
| 1 | **Touch is not used.** The panel only displays state; no buttons, no swipe, no input device registered in LVGL. |
| 2 | **Microphone / speech recognition are not used** and stay disabled (`ENABLE_SPEECH_RECOGNITION 0`). |
| 3 | **All on-screen text is English.** |
| 4 | **Few elements, large sizes.** The voltage is the hero value; everything else is secondary. |

## 2. Screen composition

```
            ┌──────── circle 360 ────────┐
            │        BATTERY             │   ← 12 px caption
            │          82%               │   ← 28 px, colored by zone
            │                            │
            │         58.2               │   ← 110 px Arial Bold
            │         VOLTS              │   ← 12 px caption
            │                            │
            │   (RF)  (WIFI)  (STARLINK) │   ← row 1 of status chips
            │   (LTE)  (VPN)  (GPS 14)   │   ← row 2
            └── 36-segment battery ring ─┘
```

### 2.1 Center — voltage

- Large pack voltage, one decimal, e.g. `58.2`.
- Font: Arial Bold 110 px subset (`0–9`, `.`), generated with `lv_font_conv`
  → `firmware/c2_display/font_voltage.c`.
- Caption under the number: `VOLTS` (12 px, dim).

### 2.2 Ring — state of charge

- Full circle of **36 fragments**, 13 px thick.
- Fragment geometry: 8° drawn + 2° gap; fragment 0 starts at 12 o'clock.
- Fill goes clockwise from the top; the remaining fragments stay dim.
- Colour of the filled part depends on the current percentage:
  - `< 20 %` → red `#EF4444`
  - `20 … 39 %` → amber `#F59E0B`
  - `≥ 40 %` → green `#22C55E`
  - unlit → `#1A2430`

### 2.3 Top — percentage

- Caption `BATTERY` (12 px, dim) and value `82%` (28 px) in the zone colour.
- Kept because the ring itself has no numeric scale.

### 2.4 Status chips

Six chips, 2 rows × 3, each 76 × 30 px, radius 15:

| Row | Chip | Meaning |
|---|---|---|
| 1 | `RF` | radio control link is active |
| 1 | `WIFI` | control over Wi-Fi is active (drone side — not the display's own Wi-Fi) |
| 1 | `STARLINK` | Starlink link is up |
| 2 | `LTE` | LTE link is up |
| 2 | `VPN` | VPN tunnel is up |
| 2 | `GPS` | shows satellite count when the drone sees any (`GPS 14`), otherwise just `GPS` |

States:

| State | Border / text | Background |
|---|---|---|
| on | `#22C55E` | `#0A2A18` |
| off | `#46566A` | `#0B1118` |

## 3. Data model

`telemetry.h` → `telemetry_t`:

| Field | Type | Meaning |
|---|---|---|
| `voltage_v` | float | pack voltage, V (14S demo: 42.0 … 58.8) |
| `batt_pct` | uint8 | state of charge, 0…100 |
| `rf_on` | bool | RC link |
| `wifi_on` | bool | Wi-Fi control link |
| `starlink_on` | bool | Starlink link |
| `lte_on` | bool | LTE link |
| `vpn_on` | bool | VPN tunnel |
| `sats` | uint8 | visible GNSS satellites |
| `uptime_ms` | uint32 | milliseconds since boot |

The UI reads this structure only; it never writes to it (`telemetry_update()`
is called from the main loop at 10 Hz).

## 4. Simulator (temporary)

`C2_SIMULATE 1` in `telemetry.cpp`:

- battery: 95 % → 3 % over 240 s, then repeats;
- voltage derived from the percentage: 42.0 V … 58.8 V (14S);
- `RF` and `VPN` always on; `WIFI`, `STARLINK`, `LTE` blink with different periods;
- satellites: 12 ± 4.

Set `C2_SIMULATE 0` when a real link fills `telemetry_t`.

## 5. Implementation notes

- Custom font is generated once:
  `npx lv_font_conv --font arialbd.ttf --range 0x30-0x39 --range 0x2E --size 110 --bpp 4 --format lvgl -o font_voltage.c`
- **Never use `%f` in `lv_label_set_text_fmt()` on this core** — LVGL's formatter
  emits a bare `f` character, which is not present in the digits-only font and
  renders as a placeholder square. Format voltage as integers:
  `lv_label_set_text_fmt(lbl, "%d.%d", tenths / 10, tenths % 10);`
- No LVGL input device is registered at all (`lvgl_port.cpp`).
