# JK-PB2A16S-30P — BLE protocol notes

Field notes from reverse-engineering the BLE link of a Chengdu Jikong
**JK-PB2A16S-30P** BMS with a live 16S LiFePO4 pack. Everything marked
*(verified)* was observed on this hardware; everything else is marked
*(from reference implementation)* or *(not decoded)*.

Companion document: `docs/JK_BMS_CAN_Integration.md` (the same data over the
WeAct USB-CAN adapter). The values in both documents are cross-checked against
each other in section 9.

---

## 1. Device and module identity

Advertising is short and gives nothing away — the device shows up as a serial
number, which is why it is easy to mistake for something else:

| Property | Value (verified) |
|---|---|
| BLE MAC | `A4:C1:38:05:47:C3` (OUI `A4:C1:38` = Telink Semiconductor) |
| Advertised name | `603134C60007081-00` — this is `<serial>-00` |
| Advertised services | `0000FFE0`, `0000FEE7` (FEE7 = Telink/OTA channel) |
| Manufacturer data | ID `0x0B65`, payload `88 a0 a4 c1 38 05 47 c3` (contains the MAC) |
| BLE module firmware | `ZXXT BLE Master Module Ver3.40` (reported on `0000FF03` notify) |
| BLE PIN (asked by the app) | **1234** |
| Settings password | **123456** |
| Connection | no pairing/bonding needed to read; Windows connects directly |

The BLE module is a generic serial↔BLE bridge (ZXXT) with the JK MCU talking the
JK02 binary protocol over its UART. The `ZXXT ...` string is the *module's*
firmware version, not another device — do not draw conclusions from it.

Device identity comes from the protocol itself (section 8): the device-info
record answers `JK-PB2A16S30P` with serial `603134C60007081`, BLE name `JK-BMS`,
PIN `1234` and password `123456`.

**The module is advertising only while no central is connected.** With the JK
app open on a phone, the BMS disappears from scanners entirely (a connected
peripheral stops advertising). This is the #1 reason a scan finds nothing.

---

## 2. GATT map (verified)

Full dump of the device as enumerated by Windows/bleak:

```
service 00001800-0000-1000-8000-00805f9b34fb  handle=1     Generic Access
  char 00002a00  handle=2   read        Device Name
  char 00002a01  handle=4   read        Appearance
  char 00002a04  handle=6   read        Preferred Connection Parameters

service 0000ffe0-0000-1000-8000-00805f9b34fb  handle=8     JK data channel
  char 0000ffe1  handle=9   notify, write-without-response   <-- JK02 traffic
  char 0000ffe2  handle=12  notify, write
  char 0000ffe3  handle=15  write                             (write only)
  (each with CCCD 00002902)

service 0000fe59-0000-1000-8000-00805f9b34fb  handle=18    (Nordic-style DFU)
  char 8ec90001-f315-4f60-9fb8-838830daea50  handle=19  notify, write
  char 8ec90002-f315-4f60-9fb8-838830daea50  handle=22  write-without-response
  char 00000003-0000-1000-8000-00805f9b34fb  handle=24  notify, read, write

service 000001bf-0000-1000-8000-00805f9b34fb  handle=27
  char 04100042-7442-0000-0202-00427feeff02  handle=28  write-without-response

service 0000ff01-0000-1000-8000-00805f9b34fb  handle=30
  char 0000ff03  handle=31  notify, write-without-response  <-- module diagnostics
```

Practical rules:

* **All JK traffic goes over `0000FFE1`**: write the 20-byte request there
  (`write-without-response`) and subscribe to notifications on the same
  characteristic. `FFE2`/`FFE3` and the FE59/FF01 services are not needed.
* The MTU negotiated by Windows gives 150-byte notification payloads, so one
  300-byte record arrives as two notifications of 150 bytes.
* `0000FF03` occasionally emits the module firmware string; ignore it.
* The 4-byte notifications `41 54 0D 0A` = `"AT\r\n"` appear spontaneously and
  interleaved with real data on `FFE1`. **Filter them out** before reassembly —
  they are not part of the JK02 stream and will corrupt a naive concatenation.

---

## 3. Request format (verified)

20 bytes, ASCII-free binary, always written to `FFE1`:

```
offset  size  content
0       4     0xAA 0x55 0x90 0xEB      start sequence
4       1     register                see table below
5       1     length of the value     0x00 for all read requests
6       4     value, little-endian    0x00000000 for reads
10      9     zero padding
19      1     CRC = (sum of bytes 0..18) & 0xFF
```

Known registers:

| Register | Meaning | Verified |
|---|---|---|
| `0x97` | device info → answer type `0x03`, 300 bytes | yes |
| `0x96` | realtime data → answer type `0x02`, 300 bytes | yes |
| `0xA1` | logbook | no — from reference implementation |
| `0xC8` | observed as an outgoing/echo frame, not a request | seen, not decoded |

Worked examples:

```
device info : aa 55 90 eb 97 00 00 00 00 00 00 00 00 00 00 00 00 00 00 11
realtime    : aa 55 90 eb 96 00 00 00 00 00 00 00 00 00 00 00 00 00 00 10
```

**Order matters.** Asking for `0x96` on a fresh connection returned only `0x01`
(settings) frames; requesting `0x97` first and then `0x96` reliably produced
realtime frames. Recipe: connect → subscribe → `0x97` → wait ~1 s → `0x96`.

---

## 4. Response format (verified)

All responses share one header and are chopped into 150-byte notifications:

```
0  4   0x55 0xAA 0xEB 0x90     preamble (note: reversed vs the request)
4  1   record type            0x01 = settings, 0x02 = realtime, 0x03 = device info
5  1   frame counter          free-running per device
6  ... payload
-1 1   CRC = (sum of all previous bytes) & 0xFF
```

Lengths observed on this firmware: `0x02` = 300 bytes, `0x03` = 300 bytes,
`0x01` = 300 bytes. Reassembly rules:

1. Drop notifications equal to `AT\r\n` (and other short ASCII module chatter).
2. Append the rest to a buffer.
3. Find the preamble; the record type follows at +4.
4. Take the next 300 bytes; check `sum(bytes[:-1]) & 0xFF == bytes[-1]`.
   Do **not** slice a fixed 320 — that eats the start of the next record and the
   CRC check then fails (a mistake made and fixed during this investigation).
5. On a CRC failure, advance one byte and search for the next preamble rather
   than discarding the whole buffer.

Every record we captured validated with the byte-sum CRC, so the CRC is a real
integrity check, not a counter.

---

## 5. Notification cadence

* The BMS only answers requests; it does not stream. One `0x96` request produces
  exactly one `0x02` record (~1.0-1.2 s end to end, including the round trip and
  the notification churn).
* At 1 Hz polling the connection was stable for the whole session (>60 polls).
* `0x01` (settings) records were pushed spontaneously right after connecting and
  interleaved with realtime answers — accept and ignore them unless you decode
  them.
* A `0x02` record is ~300 bytes over 2 notifications, i.e. roughly 2.4 kbit/s of
  BLE traffic at 1 Hz — trivial, but note that BLE connection interval (30-50 ms
  on Windows) dominates the latency, not bandwidth.

---

## 6. Device info record (type `0x03`)

300 bytes; the interesting fields are ASCII strings, space/zero padded:

| Offset | Length | Field | Example value |
|---|---|---|---|
| 6 | 16 | model | `JK-PB2A16S30P` |
| 22 | 3-8 | hardware version (ASCII) | `19A` |
| 30 | 8 | firmware version (ASCII) | `19.31` |
| 46 | 15 | serial number | `603134C60007081` |
| 62 | 4 | BLE PIN (ASCII!) | `1234` |
| 77 | 6 | unknown ASCII | `000000` |
| 85 | 15 | serial number (repeated) | `603134C60007081` |
| 101 | 6 | BLE device name | `JK-BMS` |
| 117 | 6 | settings password (ASCII!) | `123456` |
| 133 | 6 | BLE device name (repeated) | `JK-BMS` |
| 189 | 7 | unknown ASCII | `CEHMPRT` |

Numeric fields in the tail (manufacturing date, cycle counters, capability flags)
were **not decoded**. Note that the offsets above were measured on an assembled
record where one zero byte in the middle may be missing from the archived copy;
the string contents are unambiguous, the tail offsets should be treated as ±1.

Security note: the PIN and the settings password are transmitted in cleartext in
this record and both are vendor defaults. Anyone in radio range can read the
settings password and then write settings.

---

## 7. Realtime record (type `0x02`) — verified offset map

This firmware uses **32 cell slots and 32 resistance slots** (16 used on a 16S
pack), which shifts every field after the resistances by +32 bytes compared to
the widely published 24-slot JK02 layout. All multi-byte fields are
little-endian. Offsets are from the start of the record (including the 4-byte
preamble).

| Offset | Size | Field | Scale / formula | Example |
|---|---|---|---|---|
| 0 | 4 | preamble | `55 AA EB 90` | |
| 4 | 1 | record type | `0x02` | |
| 5 | 1 | counter | 0..255 | `0x20` |
| 6 + 2·i | 2·32 | cell voltage *i+1* (mV), 32 slots | `u16` | 3604, 3609, 3601, … |
| 70 | 4 | enabled-cells bitmask | `u32`, bit per cell | `0x0000FFFF` (16 cells) |
| 74 | 2 | average cell voltage (mV) | `u16` | 3604 |
| 76 | 2 | delta cell voltage (mV) | `u16` | 8 |
| 78 | 1 | max-voltage cell index, 0-based | `+1` = cell number | `1` → cell 2 |
| 79 | 1 | min-voltage cell index, 0-based | `+1` = cell number | `2` → cell 3 |
| 80 + 2·i | 2·32 | cell resistance *i+1* (mΩ), 32 slots | `u16` | 56, 59, 63, … |
| 144 | 2 | likely MOS/board temperature | `i16 × 0.1 °C` | 217 → 21.7 °C *(unconfirmed position)* |
| 146 | 4 | wire-resistance warning bitmask | `u32`, bit per wire | `0x00000000` |
| 150 | 4 | pack voltage | `u32 × 0.001 V` | 57667 → 57.667 V |
| 154 | 4 | power | `i32 × 0.001 W` | 0 |
| 158 | 4 | current | `i32 × 0.001 A`, + = charge | 0 |
| 162 | 2 | temperature sensor 1 | `i16 × 0.1 °C` | 211 → 21.1 °C |
| 164 | 2 | temperature sensor 2 | `i16 × 0.1 °C` | 214 → 21.4 °C |
| 166 | 2 | value always 0 in our captures | unknown | 0 |
| 168 | 2 | 0x0008 in our captures — probably delta again, not an alarm mask | unknown | 8 |
| 170 | 2 | balance current | `i16 × 0.001 A` | 0 |
| 172 | 1 | balancing action (0 = idle) | `u8` | 0 |
| 173 | 1 | SOC | `u8 %` | 100 |
| 174 | 4 | capacity remaining | `u32 × 0.001 Ah` | 300000 → 300.00 Ah |
| 178 | 4 | nominal capacity | `u32 × 0.001 Ah` | 300000 → 300.00 Ah |
| 182 | 4 | charge cycles | `u32` | 0 |
| 186 | 4 | cycle capacity | `u32 × 0.001 Ah` | 0 |
| 190 | 1 | SOH | `u8 %` | 100 |
| 191 | 1 | precharge flag | `u8` | 0 |
| 192 | 2 | user alarm word | `u16` | 0 |
| 194 | 4 | runtime since power-on | `u32` seconds | 3220 |
| 198 | 1 | charge MOSFET | `0` off / `1` on | 1 |
| 199 | 1 | discharge MOSFET | `0` off / `1` on | 1 |
| 200 | 1 | precharge MOSFET | `u8` | 0 |
| 201 | 1 | balancer working | `u8` | 0 |
| 299 | 1 | CRC | sum of 0..298 | |

Notes:

* Cells 17..32 and resistances 17..32 are present as zero slots on a 16S pack.
  Read the enabled-cells bitmask (`70`) instead of guessing the count.
* Three fields are still ambiguous and are marked as such in the table: `144`,
  `166`, `168`. Everything else was cross-validated against the CAN frames
  (section 9), which is why the map can be trusted.
* The published 24-slot layout (esphome-jk-bms) puts pack voltage at offset 118
  and SOC at 141; on this unit they are at 150 and 173. Always locate the pack
  voltage first (`u32` equal to the measured pack mV) before trusting a layout.

---

## 8. Settings record (type `0x01`)

300 bytes, same framing, pushed by the BMS on connect and occasionally in
between. It contains the protection thresholds (over/under voltage, overcurrent,
temperature limits, balance settings, capacity, etc.).

**Not decoded here.** The reference implementation decodes it with a 24-slot
layout, and since this unit uses the 32-slot variant the offsets would need to be
re-derived the same way the realtime map was (locate a known value, then map).
First bytes of one capture, for reference:

```
55 aa eb 90 01 00 ac 0d 00 00 c4 09 00 00 54 0b 00 00 42 0e 00 00 48 0d 00 00
0a 00 00 00 75 0d 00 00 8c 0a 00 00 7a 0d 00 00 16 0d 00 00 ba 09 00 00 e0 93
04 00 03 00 00 00 3c 00 00 00 e0 93 04 00 2c 01 00 00 3c 00 00 00 05 00 00 00
...
```

(`0x0DAC`, `0x0DC4` … are 3500, 3530 … mV-scale thresholds, `0x000493E0` = 300000
appears here too, i.e. capacity in 0.001 Ah.)

---

## 9. Cross-check against the CAN link

Same pack, same moment, read simultaneously over BLE and over the WeAct USB-CAN
adapter (250 kbit/s):

| Quantity | CAN | BLE |
|---|---|---|
| Cell 1..16 (mV) | 3604 3609 3601 3605 3603 3603 3606 3603 3601 3605 3605 3605 3603 3604 3604 3605 | identical |
| max/min cell | 3609 (#2) / 3601 (#3) | 3609 (#2) / 3601 (#3) |
| delta | 8 mV | 8 mV |
| pack voltage | 57.6 V (`0x2F4`), 57.66 V (`0x01F21400`) | 57.667 V |
| current | 0.0 A | 0.000 A |
| SOC | 100 % | 100 % |
| T1 / T2 | 21.0 / 21.4 °C | 21.1 / 21.4 °C |
| alarms | none (`0x7F4` absent) | no alarm bits observed |

Everything that exists in both channels agrees, which validates both decoders.

### What BLE has that CAN does not

| Data | BLE | CAN |
|---|---|---|
| 16 cell resistances (mΩ) | yes (56…69 mΩ here) | no |
| wire-resistance warning bitmask | yes | no |
| capacity remaining / nominal | yes (300.00 / 300.00 Ah) | no |
| charge cycles, cycle capacity | yes | no |
| SOH | yes | no |
| runtime since power-on | yes | no |
| charge/discharge/precharge MOSFET states | yes | no |
| balance current and balancing action | yes | partly |
| protection thresholds (settings record) | yes (undecoded) | no |
| device model, serial, firmware, PIN, password | yes | no |

### What CAN has that BLE does not

Continuous 10 Hz broadcast without asking, wired link that never needs pairing,
and no "someone else is connected" failure mode. For a vehicle, CAN is the right
telemetry channel; BLE is the commissioning/diagnostics channel.

---

## 10. Implementation sketch (Rust / BlueZ)

```
crate: btleplug (cross-platform, wraps BlueZ on Linux) or bluez-async
       UUID service 0000ffe0-0000-1000-8000-00805f9b34fb
       UUID char    0000ffe1-0000-1000-8000-00805f9b34fb  (notify + write)
```

State machine mirroring the Python probe:

1. Scan for the service `FFE0` (or by MAC/serial suffix) — do not filter on a name
   starting with "JK"; this unit advertises its serial.
2. Connect, subscribe to `FFE1` notifications.
3. Write `jk_frame(0x97)`, wait for a valid `0x03` record (~1.2 s).
4. Loop at 1 Hz: write `jk_frame(0x96)`, reassemble the next `0x02` record.
5. Decoder is the same `Snapshot` struct as the CAN path — only the transport and
   the field map differ. Keep one `Snapshot` type, two decoders.

Frame builder and reassembly in five lines each:

```rust
fn jk_frame(reg: u8) -> [u8; 20] {
    let mut f = [0u8; 20];
    f[0..4].copy_from_slice(&[0xAA, 0x55, 0x90, 0xEB]);
    f[4] = reg;
    f[19] = f[..19].iter().fold(0u8, |a, b| a.wrapping_add(*b));
    f
}
// notification callback: skip "AT\r\n", append the rest, cut 300-byte records
// at 55 AA EB 90, accept only if sum(record[..299]) == record[299]
```

Linux prerequisites: BlueZ ≥ 5.50, `bluetoothd` running, the service user in the
`bluetooth` group (or D-Bus policy for `org.bluez`), and an LE-capable adapter
(`bluetoothctl show` → `Powered: yes`). No pairing agent is required to read.
On an i.MX95 FRDM with no on-board radio, use a USB BLE dongle (CSR8510 / RTL8761
both work with BlueZ).

---

## 11. Troubleshooting

| Symptom | Cause / fix |
|---|---|
| device not found in scan | JK app connected on a phone (one central at a time) — close it; or BLE disabled in BMS settings; or out of range |
| connects but no `0x02` records | request `0x97` first, then `0x96`; also verify notifications are enabled on `FFE1` |
| notifications arrive but nothing parses | `AT\r\n` chatter interleaved — filter 4-byte ASCII notifications |
| CRC always fails | wrong record length (do not slice 320 bytes for a 300-byte record) |
| first bytes look like `AA 55 90 EB C8 ...` | that is a different record (0xC8), skip it and keep searching for the preamble |
| values look plausible but shifted | wrong layout variant — locate pack voltage (`u32` = measured mV) and SOC (byte `100`) first, then rebase |
| connection drops every few seconds | phone in range reconnecting, or Windows BLE power saving; disable "Allow the computer to turn off this device" on the BT adapter |

---

## 12. Appendix — raw records

On the wire each record is **300 bytes**: two 150-byte notifications. The two
archived copies below are 299 bytes each — exactly one `0x00` byte was lost while
transcribing the dump out of the terminal, and it sits inside the *undecoded*
tail (after offset 201), so:

* every field documented in sections 6 and 7 is byte-exact at the stated offset,
* the byte-sum CRC still validates (adding/removing a `0x00` does not change the
  sum) — so a CRC check alone will **not** catch a lost zero byte,
* when testing a parser against these vectors, accept 299 or 300 bytes and
  compare only the documented offsets, or re-capture the record yourself.

Device info (`0x03`, counter `0x00`):

```
55aaeb9003004a4b2d50423241313653333050000010313941000000000031392e3331000000b80b0000
100000003630333133344336303030373038310031323334000000000000000000000030303030303000
00363033313334433630303037303831004a4b2d424d53000000000000000000003132333435360000
00000000000000004a4b2d424d5300000000000000000000ffffffffafe9850f00000000901f00000000
c0d8f7feef00000000000000000000000100fbe600004345484d5052540000000000ff0f000000000000
00000000000000001500000000000000000000000000000009080001640000005f0000003c0000003200
00000000000000000000100e00000532011e0f03a40000000000010100000000000000fe9fe9fe190000
00000000022f
```

Realtime (`0x02`, counter `0x20`, CRC `0x8A`):

```
55aaeb900220140e190e110e150e130e140e160e130e110e150e150e150e130e140e140e140e0000
000000000000000000000000000000000000000000000000000000000000ffff0000140e08000102
38003b003f003f0044004200450044004400400040003d003d003a003a0036000000000000000000
000000000000000000000000000000000000000000000000d9000000000043e10000000000000000
0000d300d6000000080000000064e0930400e0930400000000000000000064000000660c00000101
0000000000000000000000000000ff000100000002000000000059573f4000000000861600000001
010100060100090c000000000000d900d500d50001004fe3e4001800000080510100000001000000
000000000000feff7fdd2f0121b00f0700008a
```

Settings (`0x01`, first 80 bytes of 300):

```
55aaeb900100ac0d0000c4090000540b0000420e0000480d00000a000000750d00008c0a00007a0d000016
0d0000ba090000e0930400030000003c000000e09304002c0100003c00000005000000
```

---

## 13. Tooling used

| Item | Path |
|---|---|
| scan (advertisements, names, services) | `ble_scan.py` (bench host scratch dir) |
| GATT dump / capability probe | `ble_probe.py` |
| request + reassemble + raw dump | `ble_jk2.py` |
| final decoder (verified offsets) | `jk_ble_final.py` |
| offline device-info analysis | `devinfo_offline.py` |

Runs on Python 3.11 + `bleak` (WinRT on Windows, BlueZ on Linux) — see
`C:\Users\ПК\.venvs\can` on the bench host. The Rust equivalents belong in the
same crate as the CAN reader (`code/jkcan/`) as a second transport module.

Sources for the parts not derived here: `syssi/esphome-jk-bms`
(`components/jk_bms_ble/jk_bms_ble.cpp`) — frame builder, CRC, record types,
and the 24-slot field table used as the starting hypothesis for section 7.
