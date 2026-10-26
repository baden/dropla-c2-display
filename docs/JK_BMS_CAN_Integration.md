# JK BMS over a WeAct USB2CAN adapter — integration guide

Target: read a Chengdu Jikong **JK-PB2A16S-30P** BMS over CAN from a Linux
(aarch64) device, using a **WeAct USB2CAN(FD) V1** USB adapter, application
software in Rust.

Everything in the "verified" sections below was measured on real hardware:
the adapter on a Windows host, the BMS on a live 16S LiFePO4 pack, and the Rust
reference implementation compiled and run against that adapter. Items that are
documented but **not** executed here are marked _(not verified)_.

---

## 1. Summary of the two devices

### 1.1 Adapter

| Property | Value |
|---|---|
| Board | WeAct Studio USB2CANFD V1 (USB-CAN + CAN-FD, 5 Mbit/s) |
| MCU | STM32G0B1CBT6 |
| CAN front-end | SIT1051A transceiver, CA-IS2062A digital isolator |
| USB | VID:PID **0483:5740** (ST generic CDC-ACM — *not* unique to this board) |
| USB product string | `USB2CANV1 ComPort` |
| USB serial | `AAA2208E3281` (stable — use this to find the port) |
| Driver | CDC-ACM → `cdc_acm` / `usbser.sys`, appears as a plain serial port |
| Firmware | `WeAct Studio V1.0.0.3_bb264e71` (answer to the `V` command) |
| Protocol | ASCII **SLCAN** over the virtual serial port |
| Connector | 3.81 mm 1x3P screw terminal (CAN_H, CAN_L, GND) |
| Limits | ~500 frames/s, no hardware timestamps, re-enumerates on its own |

Because `0483:5740` is the stock STM32 VCP ID, **never** match the port on
VID/PID alone unless this adapter is the only such device on the box. Prefer the
USB serial number or the product string.

### 1.2 BMS

| Property | Value |
|---|---|
| Model | JK-PB2A16S-30P (JK Energy Storage Series, active balance 2 A) |
| Pack | 16S LiFePO4 in this installation (configurable 8–16S LiFePO4) |
| Current | 300 A continuous, 600 A instantaneous |
| Temperature sensing | **1 built-in + 4 external NTC** (NTC1..NTC4 on P6/P7/P8/P9) |
| CAN | 250 kbit/s, classic CAN 2.0A + a set of 29-bit extended frames |
| Direction | **BMS → bus only.** It never answers and ignores everything it receives |

Documented CAN protocol: "JK BMS CAN Protocol (250K) V2.0", protocol index `000`
in the JK app. Other indices (`001` Deye, `002` Pylontech, `004` Victron, ...) change
the frame set and sometimes the bitrate (500 k) — check the app setting before
deploying, and do not assume `000`.

---

## 2. Wiring

```
BMS CAN port (P5 / interface board)          Adapter terminal
    CAN_H  ─────────── twisted pair ───────────  CAN_H
    CAN_L  ─────────── twisted pair ───────────  CAN_L
    GND    ────────────────────────────────────  GND
```

Rules that cost money if ignored:

1. **Connect GND first**, then CAN_L, then CAN_H. The SIT1051A front-end has no
   galvanic isolation towards the bus (the "1500 V" rating is the digital
   isolator between USB and CAN); ±70 V on the pins kills it. CAN does **not**
   support hot-plug.
2. Use twisted pair, keep the stub short.
3. **120 Ω at both physical ends** of the bus. A single adapter + single BMS on a
   bench cable works without them (verified), but on the vehicle put them in.
4. Check the silkscreen on the adapter's 3-pin terminal before powering: pin
   order is not guaranteed across revisions.

Bus load with this BMS is ~40 frames/s ≈ 5 kbit/s ≈ **2 % of 250 kbit/s**, so the
adapter's ~500 frames/s ceiling is not a constraint for one BMS.

---

## 3. Linux bring-up

### 3.1 Device node

```sh
dmesg | tail -5                 # expect: cdc_acm 1-1.2:1.0: ttyACM0: USB ACM device
ls -l /dev/ttyACM* /dev/serial/by-id/
```

The by-id link encodes the USB product string and serial, so it survives
re-enumeration and COM/ttyACM renumbering:

```
/dev/serial/by-id/usb-USB2CANV1_ComPort_AAA2208E3281-if00   (verify the exact name)
```

Recommended udev rule — `/etc/udev/rules.d/99-jkcan.rules`:

```
SUBSYSTEM=="tty", ATTRS{idVendor}=="0483", ATTRS{idProduct}=="5740", \
  ATTRS{serial}=="AAA2208E3281", SYMLINK+="jkcan", MODE="0660", GROUP="dialout"
```

```sh
udevadm control --reload && udevadm trigger
usermod -aG dialout <user>        # or run the service as root
ls -l /dev/jkcan
```

Then the application opens `/dev/jkcan` and nothing else in the system may hold
that tty open.

### 3.2 Two ways to talk to the device

**A. Raw tty, SLCAN in the application (recommended for an embedded controller).**

The adapter is a CDC-ACM serial port speaking ASCII SLCAN. The application opens
the tty, sends `C`, `M0`, `S5`, `O`, then reads `\r`-terminated lines. This is
exactly what the reference Rust code in section 6 does, and it is what was
validated here. No extra packages, no kernel module, no `ip link` state, full
control over the mode bytes and the status byte of every command.

**B. SocketCAN via `slcand` (idiomatic if the rest of the system is CAN-aware).**

```sh
apt install can-utils                 # or the Yocto package can-utils
slcand -o -s5 -t sw /dev/jkcan can0   # -o open now, -s5 = 250 kbit/s, host timestamps
ip link set can0 up
candump -t d can0
```

Then the Rust side uses the `socketcan` crate and sees a normal `can0`. Notes:

* `CONFIG_CAN_SLCAN` must exist in the kernel (it is a module in most distros:
  `modprobe slcan`).
* `slcand` takes exclusive ownership of the tty — do not also open it yourself.
* `-s5` is the SLCAN **bitrate code**, not a number in bit/s (see table 5.1).
* Use `-t sw`: this firmware has no hardware timestamps, so asking for `hw`
  timestamps gets you nothing.
* `slcand` opens the channel in normal mode. Do **not** put the interface in
  listen-only mode (see 8.1) — the BMS then never gets its ACK.
* Timestamps therefore come from the kernel receive path and are only good to ~ms.
* _(not verified in this session — the direct-tty path was the one exercised)_

### 3.3 Build dependencies (Rust)

* `serialport = "4"` — on Linux the default feature set links **libudev**
  (`libudev-dev` / `systemd-dev` on the build host). Ship it or use
  `default-features = false` and match the port by `/dev/serial/by-id/...`
  (with libudev disabled the crate cannot report VID/PID/serial).
* Rust ≥ 1.75 (2021 edition). Cross-compiled to `aarch64-unknown-linux-gnu`
  without change; there is no `unsafe` and no platform-specific code in the
  parser/decoder.

---

## 4. What the BMS actually puts on the bus

Measured over 10 s windows on an idle 100 % pack. Total ≈ 40 frames/s.

### 4.1 Standard 11-bit frames (`t...` lines)

| ID | Period | Payload |
|---|---|---|
| `0x2F4` | 100 ms | `BATT_ST` |
| `0x4F4` | 100 ms | `CELL_VOLT` (max/min only) |
| `0x5F4` | 1000 ms | `CELL_TEMP` (max/min/avg of cell temps) |
| `0x7F4` | event | `ALM_INFO` — **absent while there is no alarm** |

Decode (all multi-byte fields **little-endian**):

```
0x2F4  pack_mV    = u16(d[0..2]) * 100                    # 0.1 V
       current_A  = u16(d[2..4]) * 0.1 - 400.0            # + = discharge
       soc_pct    = d[4]
       dischg_h   = u16(d[6..8])                          # hours

0x4F4  max_cell_mV = u16(d[0..2]),  max_cell_no = d[2]
       min_cell_mV = u16(d[3..5]),  min_cell_no = d[5]    # NOTE: d[3..5], not d[4..6]

0x5F4  t_max_c = d[0] - 50,  t_max_probe_no = d[1]
       t_min_c = d[2] - 50,  t_min_probe_no = d[3]
       t_avg_c = d[4] - 50
```

> **Verified deviation from the vendor PDF.** The official JK protocol document
> places the minimum cell voltage at `d[4..6]` with `d[3]` reserved. On this
> firmware `d[3]` is the low byte of the minimum voltage: reading the PDF layout
> gives 782 mV instead of 3601 mV. Cross-checked against the extended cell frames
> (section 4.4) — the corrected layout is the right one.

> `0x2F4` reports pack voltage with 0.1 V resolution, so it reads 57.6 V while the
> cell sum is 57.667 V. Use the cell sum (or `0x01F21400`) when you need precision.

### 4.2 Extended 29-bit frames (`T...` lines)

The firmware (this version, protocol index `000`) also emits a J1939-flavoured set
that is **not** in the vendor PDF. Empirically:

| ID | Period | Content |
|---|---|---|
| `0x18E028F4` `0x18E128F4` `0x18E228F4` `0x18E328F4` | ~1.5 s, rotating | 16 cell voltages, 4 per frame, `u16_le` mV |
| `0x18F228F4` | 1 s | `d[0]` = cell count (0x10 = 16); `d[1..6]-50` = **all 5 NTC channels** in °C |
| `0x18F128F4` | 200 ms | two `u16_le` words = 3000/3000 on a 300 A unit → charge/discharge current limits, 0.1 A |
| `0x18F428F4` | 1 s | undecoded; `d[6]` = 100 (looks like SOC), `d[0..2]` slowly counts up |
| `0x18F528F4` | 500 ms | undecoded; constant `0x23` |
| `0x1806E5F4` | 500 ms | undecoded; constant `02 28 0B 22` |
| `0x01F21400` | 200 ms | pack voltage `u16_le(d[0..2]) * 0.01` + `u16_le(d[4..6]) * 0.1` = the same NTC channel that runs warm; source address `0x00`, not `0xF4` |

ID structure of the cell frames: priority 6, PF `0xE0..0xE3`, PS `0x28`, SA `0xF4`.
Group index = `PF - 0xE0`, cells = group*4 + k + 1. Decoder guard:

```rust
fn cell_group(id: u32) -> Option<usize> {
    if (id >> 24) == 0x18 && (id & 0xFF) == 0xF4 && ((id >> 8) & 0xFF) == 0x28 {
        let pf = ((id >> 16) & 0xFF) as usize;
        if (0xE0..=0xE3).contains(&pf) { return Some(pf - 0xE0); }
    }
    None
}
```

### 4.3 Temperature channels

The PB has **1 built-in + 4 external NTC**, and `0x18F228F4` reports exactly five
channels, offset `-50 °C`. Which byte is which probe is **not** encoded in the
traffic — to map indices to physical NTC1..NTC4, unplug one probe and watch the
index change. Observed on the bench:

```
d[1..6] = 47 47 46 50 47  ->  21, 21, 20, 30, 21 °C
```

The channel reading 30 °C is the only one that is excluded from the `0x5F4`
cell-temperature statistics, so it is the built-in (power-tube/board) sensor:
after the BMS was powered down and later re-powered, that channel read 21 °C, i.e.
it follows real board temperature. Do not treat it as a cell probe.

### 4.4 Worked example (real capture, one full cycle)

```
t2F484002A00F64000000              V=57.6 V  I=0.0 A  SOC=100 %
t4F48190E02110E030000              max 3609 mV (#2)  min 3601 mV (#3)
t5F484701470047000000              cell temp 21 / 21 / 21 °C
T18F128F48B80BB80B00000000         limits 300.0 A / 300.0 A
T18F228F481047474650470000         cell count 16, NTC 21 21 20 30 21 °C
T18E028F48140E190E110E150E         cells 1-4   3604 3609 3601 3605 mV
T18E128F48130E130E160E130E         cells 5-8   3603 3603 3606 3603 mV
T18E228F48110E150E150E140E         cells 9-12  3601 3605 3605 3604 mV
T18E328F48130E150E140E150E         cells 13-16 3603 3605 3604 3605 mV
T01F2140088616000036010000         57.66 V, NTC channel = 21.4 °C
```

Cell sum 57 666 mV matches `0x01F21400` (57.66 V), and the cell frames agree with
`0x4F4` (highest 3609 = cell 2, lowest 3601 = cell 3). Those two cross-checks are
the acceptance test for any new decoder implementation.

Reported accuracy is ±3–5 mV per cell and the values jitter ±1 mV between frames;
do not chase single-millivolt changes.

---

## 5. SLCAN protocol reference (only what this adapter implements)

Every command is ASCII terminated by `\r` (0x0D). Reply: a single `\r` = OK,
`0x07` (BEL) = rejected/failed. There is **no echo**.

### 5.1 Commands used

| Command | Meaning |
|---|---|
| `O` | open CAN channel |
| `C` | close CAN channel |
| `M0` | normal mode (adapter ACKs) — **use this** |
| `M1` | silent/listen-only mode (adapter never ACKs) |
| `S0`..`SD` | bitrate, in this order: 10k, 20k, 50k, 100k, 125k(default), 250k, 500k, 800k, 1M, 83.3k, 75k, 62.5k, 33.3k, 5k |
| `tIIILD…` / `TIIIIIIIILD…` | transmit data frame, std(3 hex)/ext(8 hex) id |
| `rIIIL` / `RIIIIIIIIL` | transmit remote frame |
| `d`/`D`, `b`/`B` | transmit CAN-FD frame, no BRS / with BRS |
| `f…` / `F…` | set std / ext acceptance filter (channel closed) |
| `V` | firmware version string |
| `E` | error register (`CANable Error Register: N`) |
| `X` | enter firmware-upgrade mode — **never send by accident** |

Not implemented by this firmware: hardware timestamps (`Z`/`z` → BEL), `Sn`.

Bitrate for this BMS is **250 kbit/s = `S5`**.

### 5.2 Receive line formats

| Prefix | Frame |
|---|---|
| `t` / `T` | classic data frame, std / ext id |
| `r` / `R` | remote frame, std / ext id |
| `d` / `D` | CAN-FD, no BRS, std / ext |
| `b` / `B` | CAN-FD with BRS, std / ext |

Classic: `t` + 3 hex id + 1 hex len + payload. Extended: `T` + 8 hex id + 1 hex len
+ payload. CAN-FD length codes: `0-8` as usual, then `9`=12, `A`=16, `B`=20,
`C`=24, `D`=32, `E`=48, `F`=64.

The first read after `O\r` can already contain frame data in the same buffer —
treat any non-frame text as noise instead of assuming a clean status reply.

---

## 6. Rust reference implementation

The working crate is part of this repo: **`code/jkcan/`** (`cargo test`, then
`cargo run -- 8`). Copy of the two sources is inline below so the document stands
alone; the repo copy is the one that was compiled and run against the hardware.

Cargo.toml:

```toml
[package]
name = "jkcan"
version = "0.1.0"
edition = "2021"
rust-version = "1.75"

[dependencies]
serialport = "4"           # on Linux: needs libudev-dev, or default-features = false
```

`src/lib.rs` — parser + decoder (unit-tested against the capture in 4.4):

```rust
//! SLCAN line parser + JK BMS frame decoder.

pub const BEL: u8 = 0x07;

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Frame { pub id: u32, pub ext: bool, pub data: Vec<u8> }

pub fn parse_line(line: &str) -> Option<Frame> {
    let b = line.trim().as_bytes();
    let t = *b.first()?;
    let (idlen, ext, remote, fd) = match t {
        b't' => (3, false, false, false),
        b'T' => (8, true, false, false),
        b'r' => (3, false, true, false),
        b'R' => (8, true, true, false),
        b'd' => (3, false, false, true),
        b'D' => (8, true, false, true),
        b'b' => (3, false, false, true),
        b'B' => (8, true, false, true),
        _ => return None,
    };
    let id = u32::from_str_radix(std::str::from_utf8(b.get(1..1 + idlen)?).ok()?, 16).ok()?;
    let len_char = *b.get(1 + idlen)?;
    let n = if fd && !len_char.is_ascii_digit() {
        match len_char.to_ascii_uppercase() {
            b'9' => 12, b'A' => 16, b'B' => 20, b'C' => 24,
            b'D' => 32, b'E' => 48, b'F' => 64, _ => return None,
        }
    } else {
        (len_char as char).to_digit(16)? as usize
    };
    if remote { return Some(Frame { id, ext, data: Vec::new() }); }
    let hex = std::str::from_utf8(b.get(2 + idlen..2 + idlen + 2 * n)?).ok()?;
    let data = (0..n)
        .map(|i| u8::from_str_radix(&hex[2 * i..2 * i + 2], 16).ok())
        .collect::<Option<Vec<u8>>>()?;
    Some(Frame { id, ext, data })
}

fn u16_le(d: &[u8], i: usize) -> u16 { u16::from_le_bytes([d[i], d[i + 1]]) }

fn cell_group(id: u32) -> Option<usize> {
    if (id >> 24) == 0x18 && (id & 0xFF) == 0xF4 && ((id >> 8) & 0xFF) == 0x28 {
        let pf = ((id >> 16) & 0xFF) as usize;
        if (0xE0..=0xE3).contains(&pf) { return Some(pf - 0xE0); }
    }
    None
}

#[derive(Debug, Default, Clone)]
pub struct Snapshot {
    pub pack_mv: Option<u32>,
    pub current_a: Option<f32>,
    pub soc: Option<u8>,
    pub discharge_h: Option<u16>,
    pub max_cell: Option<(u16, u8)>,
    pub min_cell: Option<(u16, u8)>,
    pub cell_temp_c: Option<(i16, i16, i16)>,
    pub cells_mv: Vec<u16>,       // index 0 = cell 1
    pub probes_c: Vec<i16>,       // 1 built-in + 4 external NTC
    pub limits_a: Option<(f32, f32)>,
    pub alarm_raw: Option<Vec<u8>>,
    pub unknown_ids: Vec<u32>,
}

impl Snapshot {
    pub fn decode(&mut self, f: &Frame) {
        let d = &f.data;
        if d.len() < 8 { return; }
        match (f.ext, f.id) {
            (false, 0x2F4) => {
                self.pack_mv = Some(u16_le(d, 0) as u32 * 100);
                self.current_a = Some(u16_le(d, 2) as f32 * 0.1 - 400.0);
                self.soc = Some(d[4]);
                self.discharge_h = Some(u16_le(d, 6));
            }
            (false, 0x4F4) => {
                self.max_cell = Some((u16_le(d, 0), d[2]));
                self.min_cell = Some((u16_le(d, 3), d[5])); // d[3..5], not d[4..6]
            }
            (false, 0x5F4) => {
                let t = |x: u8| x as i16 - 50;
                self.cell_temp_c = Some((t(d[0]), t(d[2]), t(d[4])));
            }
            (false, 0x7F4) => self.alarm_raw = Some(d.clone()),
            _ => {
                if let Some(group) = cell_group(f.id) {
                    if self.cells_mv.len() < 16 { self.cells_mv.resize(16, 0); }
                    for k in 0..4 { self.cells_mv[group * 4 + k] = u16_le(d, 2 * k); }
                } else if f.id == 0x18F2_28F4 {
                    self.probes_c = d[1..6].iter().map(|b| *b as i16 - 50).collect();
                } else if f.id == 0x18F1_28F4 {
                    self.limits_a =
                        Some((u16_le(d, 0) as f32 * 0.1, u16_le(d, 2) as f32 * 0.1));
                } else if !self.unknown_ids.contains(&f.id) {
                    self.unknown_ids.push(f.id);
                }
            }
        }
    }

    pub fn cell_sum_mv(&self) -> Option<u32> {
        if self.cells_mv.len() == 16 && self.cells_mv.iter().all(|v| *v > 0) {
            Some(self.cells_mv.iter().map(|v| *v as u32).sum())
        } else { None }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const CAPTURE: &[&str] = &[
        "t2F484002A00F64000000",
        "t4F48190E02110E030000",
        "t5F484701470047000000",
        "T18F128F48B80BB80B00000000",
        "T18F228F481047474650470000",
        "T18E028F48140E190E110E150E",
        "T18E128F48130E130E160E130E",
        "T18E228F48110E150E150E140E",
        "T18E328F48130E150E140E150E",
        "T01F2140088616000036010000",
    ];

    #[test]
    fn decodes_pack_cells_and_temps() {
        let mut s = Snapshot::default();
        for line in CAPTURE {
            let f = parse_line(line).unwrap_or_else(|| panic!("unparsed: {line}"));
            s.decode(&f);
        }
        assert_eq!(s.pack_mv, Some(57_600));
        assert_eq!(s.current_a, Some(0.0));
        assert_eq!(s.soc, Some(100));
        assert_eq!(s.max_cell, Some((3609, 2)));
        assert_eq!(s.min_cell, Some((3601, 3)));
        assert_eq!(s.cell_temp_c, Some((21, 21, 21)));
        assert_eq!(s.probes_c, vec![21, 21, 20, 30, 21]);
        assert_eq!(s.limits_a, Some((300.0, 300.0)));
        assert_eq!(s.cells_mv[1], 3609);
        assert_eq!(s.cells_mv[2], 3601);
        assert_eq!(s.cell_sum_mv(), Some(57_666));
        assert!(s.alarm_raw.is_none());
    }
}
```

`src/main.rs` — the transport half (channel setup + reader loop):

```rust
use std::io::Read;
use std::time::{Duration, Instant};
use jkcan::{parse_line, Snapshot, BEL};

const VID: u16 = 0x0483;
const PID: u16 = 0x5740;

fn find_port() -> Option<String> {
    serialport::available_ports().ok()?.into_iter().find_map(|p| match p.port_type {
        serialport::SerialPortType::UsbPort(info)
            if info.vid == VID && info.pid == PID => Some(p.port_name),
        _ => None,
    })
}

fn main() -> Result<(), Box<dyn std::error::Error>> {
    let secs: f64 = std::env::args().nth(1).and_then(|a| a.parse().ok()).unwrap_or(4.0);
    let port = find_port().ok_or("USB2CAN adapter (0483:5740) not found")?;

    let mut sp = serialport::new(&port, 115200)      // VCP baud is irrelevant
        .timeout(Duration::from_millis(50))
        .open()?;

    for cmd in ["C", "M0", "S5", "O"] {
        sp.write_all(format!("{cmd}\r").as_bytes())?;
        sp.flush()?;
        std::thread::sleep(Duration::from_millis(150));
        let mut buf = [0u8; 64];
        let n = sp.read(&mut buf).unwrap_or(0);
        if &buf[..n] == [BEL] && cmd != "C" {
            return Err(format!("adapter rejected {cmd}").into());
        }
    }

    let mut snap = Snapshot::default();
    let mut buf: Vec<u8> = Vec::new();
    let mut chunk = [0u8; 1024];
    let mut lines = 0u32;
    let deadline = Instant::now() + Duration::from_secs_f64(secs);

    while Instant::now() < deadline {
        let n = sp.read(&mut chunk).unwrap_or(0);
        buf.extend_from_slice(&chunk[..n]);
        while let Some(pos) = buf.iter().position(|b| *b == b'\r') {
            let line: Vec<u8> = buf.drain(..=pos).collect();
            if let Some(frame) = parse_line(&String::from_utf8_lossy(&line)) {
                snap.decode(&frame);
                lines += 1;
            }
        }
    }
    // ... print snap: pack_mv, current_a, soc, cells, probes_c, limits_a, alarm_raw
    Ok(())
}
```

Status of this code: `cargo test` — 4 tests pass, including the full decode of the
real capture (57.6 V, 3609/3601 mV, NTC `[21,21,20,30,21]`, cell sum 57 666 mV).
`cargo run` against the physical adapter on `/dev/ttyACM*/COM15` printed 131 frames
in 3 s and the values above, so the handshake, line framing and decoder all work
end to end.

The same code is the basis for a Linux build; the only platform-sensitive part is
`find_port()` (libudev-backed). Under `slcand`, drop that and open `can0` with the
`socketcan` crate instead — the parser/decoder above is transport agnostic.

---

## 7. Recommended application structure

```
┌──────────────┐   tty lines    ┌────────────┐   Snapshot    ┌────────────┐
│ transport    │ ─────────────► │ parser +   │ ────────────► │ consumer   │
│ (tty/slcan)  │                │ decoder    │               │ (telemetry,│
│              │ ◄───────────── │ (lib.rs)   │               │  watchdog) │
└──────────────┘  C/M0/S5/O    └────────────┘               └────────────┘
```

* One task owns the tty. It reconnects with backoff, re-sends `C/M0/S5/O` after
  every reopen, and never blocks the reader on the consumer.
* Publish a full `Snapshot` at 1 Hz. A complete picture (all 16 cells) needs
  ~1.5 s because the four cell frames rotate; if you publish faster, cells 5-16
  will be stale, not wrong.
* Freshness per frame: `0x2F4`/`0x4F4` 100 ms, `0x5F4`/`0x18F2…` 1 s,
  cell groups ~1.5 s. Keep a per-frame timestamp and mark values stale after
  3× their period.
* The adapter's timestamps do not exist — stamp on receive.

---

## 8. Behaviour you must handle (learned the hard way)

### 8.1 Never listen in silent mode

`M1` (listen-only) means the adapter sends no ACK. The BMS then retransmits its
first frame forever: a 15 s listen-only capture produced ~300 identical `0x2F4`
frames per second and never advanced to another ID. In `M0` the same bus is a
clean 100 ms cycle. Symptom to recognise: one repeated ID with unchanging data at
an impossible rate → you are in the wrong mode, or the bus has no second node.

Corollary: transmitting while you are the only node on the bus always returns
`0x07`, because nothing ACKs. That is not a fault.

### 8.2 The adapter re-enumerates

Observed: the device drops off USB for a fraction of a second after heavy
open/close activity, and the tty disappears (on Windows: `FileNotFoundError`;
on Linux: `ENODEV`/`EIO` from read). Handle `open` with retries for a few seconds,
re-scan for the port, and never cache the tty path across reconnects — resolve
`/dev/serial/by-id/...` or `/dev/jkcan` every time. Rapid bitrate sweeping
(open/close for 14 rates in a row) was enough to trigger it once.

### 8.3 Throughput ceiling

~500 frames/s max, and the firmware stops answering the host (`ERROR_GEN_FAILURE`)
after roughly 12k frames at that rate — only a USB replug recovers it. This BMS
sends ~40 frames/s, so the ceiling is irrelevant unless you also use the adapter
as a bus monitor on a busy vehicle bus.

### 8.4 Bus silence means the BMS is off

Verified: pressing the BMS power button stops **all** traffic immediately — not
just `0x2F4`, but the extended frames too, including the ones with source address
`0x00`. So silence = the BMS controller is unpowered, not "MOS switches open"
(a MOS-only shutdown would keep telemetry running). Use this as a cheap presence
signal: `> 2 s` of silence ⇒ BMS off, CAN wiring broken, or termination/power
problem. After the pack was re-powered, traffic returned on its own, with no
intervention on the adapter side.

### 8.5 Values move

Cells jitter ±1 mV between frames, `0x18F128F4` limits read 2999-3000 (i.e.
299.9-300.0 A). Rate-limit or low-pass anything you display or control.

---

## 9. Acceptance test on the new Linux device

```sh
# 1. adapter visible?
dmesg | grep -i cdc_acm
ls -l /dev/ttyACM*
ls -l /dev/serial/by-id/          # look for ...USB2CANV1_ComPort_AAA2208E3281...if00

# 2. permissions
ls -l /dev/jkcan            # after installing the udev rule

# 3. transport + protocol handshake (adapter firmware string)
exec 3<>/dev/jkcan
printf 'V\r' >&3
timeout 1 cat <&3
exec 3<&-
#   expect: WeAct Studio V1.0.0.3_bb264e71

# 4. application
./jkcan 8
#   expect, on a 100 % 16S LiFePO4 pack:
#     frames: ~300 in 8 s
#     pack 57.6 V, current 0.0 A, SOC 100 %
#     cell max 3609 mV (#2), cell min 3601 mV (#3), delta ~8 mV
#     cell sum ~57.6-57.7 V
#     NTC 5 channels in the 20-31 °C range
#     alarms: none

# 5. cross-check against SocketCAN, if installed
candump -t d can0     # must show 2F4/4F4/5F4 at 100/100/1000 ms
```

Pass criteria: all four standard IDs present, cell sum within 0.15 V of `0x2F4`'s
pack voltage (`0x2F4` only carries 0.1 V resolution, so the sum legitimately reads
~70 mV higher), highest/lowest cell numbers from `0x4F4` equal to the argmax/argmin
of the 16 decoded cells, and 5 NTC channels present.

---

## 10. Troubleshooting

| Symptom | Cause / fix |
|---|---|
| `0 frames`, all bitrates | BMS off, wrong/missing GND, CAN_H/CAN_L swapped, no termination on a long cable |
| `0x07` on every transmit | normal with only one node on the bus (no ACK); ignore unless a second node exists |
| one ID repeated at ~300/s | adapter in `M1`; switch to `M0` |
| values decode as nonsense (782 mV cells) | `0x4F4` decoded with the vendor PDF layout — use `d[3..5]` |
| `open failed`, `ENODEV`/`EIO` mid-run | adapter re-enumerated; retry open, re-resolve the by-id symlink |
| tty busy | `slcand` or another process holds it; the adapter allows one reader |
| adapter wedged, no reply to `V` | unplug/replug USB (firmware hangs under sustained high rate) |
| cell 17+ or 15 cells | wrong cell-count assumption; this pack is 16S, `0x18F228F4` `d[0]` = 16 |

---

## 11. Limits: this link is read-only

CAN on this BMS is **telemetry only**. It answers no requests and ignores inbound
frames (including inverter heartbeats), in every protocol index. You can read
status; you cannot switch anything from CAN.

Controlling the battery (cutting the discharge/charge path) is a **different port
and protocol**: JK "Communication protocol between monitoring platform and BMS"
over RS485-2 or the UART display connector, 115200, frames delimited `0x4E 0x57`,
command word `0x02` = write, `0x05` = pair code (password):

| Register | Meaning | Values |
|---|---|---|
| `0xAC` | Discharge MOS switch | 0 = off, 1 = on |
| `0xAB` | Charge MOS switch | 0 = off, 1 = on |
| `0x8C` | Battery status (read) | bit0 charge MOS, bit1 discharge MOS |
| `0xBB` | Restart system | 1 = restart |

If you ever add this path:

* Do not connect the RS485 connector's VCC line — on these boards it carries the
  pack voltage, not 5 V.
* Do not open the discharge MOS under load (300 A). Remove the load first.
* Read `0x8C` back after a write and log it; do not assume the write took.

---

## 12. Appendix

### 12.1 Files and tooling from this investigation

| Item | Path |
|---|---|
| Rust crate (lib + bin + tests) | `code/jkcan/` in this repo |
| SLCAN CLI, reference implementation (Python) | bench host: `%LOCALAPPDATA%\hermes\skills\hardware\weact-usb2can-slcan\scripts\canbus.py` |
| JK BMS poller, reference implementation (Python) | bench host: `...\scripts\jk_poll.py` |

The two Python tools are the ones used to discover everything in sections 4 and 8;
they run against the same adapter on Windows and are useful for a quick sanity
check from a laptop before deploying the Rust service to the target.

### 12.2 Raw capture, 10 s, idle pack (reference data)

```
0x2F4  101 frames  40 02 A0 0F 64 00 00 00
0x4F4  101 frames  19 0E 02 11 0E 03 00 00
0x18F128F4  51 frames  B8 0B B8 0B 00 00 00 00
0x01F21400  51 frames  86 16 00 00 36 01 00 00   (and 87 16 ... once)
0x18F528F4  20 frames  23 00 00 00 00 00 00 00
0x1806E5F4  20 frames  02 28 0B 22 00 00 00 00
0x5F4       10 frames  47 01 47 00 47 00 00 00
0x18F228F4  10 frames  10 47 47 46 51 47 00 00
0x18F428F4  10 frames  5C 02 00 00 00 00 64 00 ... 63 02 ... (counts up)
0x18E028F4   7 frames  14 0E 19 0E 11 0E 15 0E
0x18E128F4   7 frames  13 0E 13 0E 16 0E 13 0E
0x18E228F4   7 frames  11 0E 15 0E 15 0E 14 0E
0x18E328F4   7 frames  13 0E 15 0E 14 0E 15 0E
```

### 12.3 Sources

* `WeActStudio/WeActStudio.USB2CANFDV1` — board, SLCAN command table, firmware.
* JK "Jikong-BMS-CAN Communication protocol" (CAN 2.0A, 250k, SA 0xF4) — the
  standard frame set, with the `0x4F4` layout deviation noted in 4.1.
* JK "Communication protocol between monitoring platform and BMS" — RS485/UART
  register map used in section 11.
* JK-PB2A16S-30P product specification V19.0.3 — 300 A, NTC count (1 built-in +
  4 external), connector pinout.
