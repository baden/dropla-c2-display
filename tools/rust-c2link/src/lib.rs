//! Host-side driver for the **c2_display** status panel (link protocol v1).
//!
//! The panel is a read-only display: the host streams [`Telemetry`] frames,
//! occasionally sends [`Notify`] banners, [`Config`] changes and sound
//! requests, and may [`Display::ping`] to check that the panel is alive
//! (it answers with a [`Pong`]).
//!
//! The link is a USB CDC-ACM port (native USB of the ESP32-S3), so the baud
//! rate is irrelevant. Keep the port open for the whole session: the panel
//! starts a ~2.7 s boot animation when it is reset, and on ESP32-S3 changing
//! DTR/RTS is interpreted as a reset command — never call
//! `write_data_terminal_ready()` / `write_request_to_send()`.
//!
//! ```no_run
//! use c2link::{channel, Display, Notify, Severity, Telemetry};
//! use std::time::Duration;
//!
//! # fn main() -> Result<(), Box<dyn std::error::Error>> {
//! let mut dev = Display::open("/dev/ttyACM0")?;
//! dev.telemetry(&Telemetry {
//!     voltage_cv: 5820,                 // 58.20 V
//!     batt_pct: Some(87),
//!     flags: channel::CRSF | channel::WIFI | channel::GPS_FIX,
//!     sats: 14,
//!     odometer_dm: 6_341_200,           // 634.12 km
//! })?;
//! dev.notify(&Notify::new("Starlink searching", Severity::Info))?;
//! std::thread::sleep(Duration::from_secs(1));
//! # Ok(())
//! # }
//! ```

use std::io::{self, Read, Write};
use std::time::Duration;

/// Frame delimiter.
pub const MAGIC: [u8; 2] = [0xA5, 0x5A];
/// Protocol version (high nibble of the type byte).
pub const VERSION: u8 = 1;
/// Largest payload the panel accepts; longer frames are dropped.
pub const MAX_PAYLOAD: usize = 64;

/// Message types.
pub mod msg {
    /// Host -> panel: 9-byte telemetry snapshot.
    pub const TELEMETRY: u8 = 0x1;
    /// Host -> panel: notification banner.
    pub const NOTIFY: u8 = 0x2;
    /// Host -> panel: configuration TLV.
    pub const CONFIG: u8 = 0x3;
    /// Host -> panel: liveness probe.
    pub const PING: u8 = 0x4;
    /// Host -> panel: play a sound.
    pub const SOUND: u8 = 0x5;
    /// Panel -> host: answer to [`super::Display::ping`].
    pub const PONG: u8 = 0x8;
}

/// Bits of [`Telemetry::flags`].
pub mod channel {
    pub const CRSF: u8 = 0x01;
    pub const WIFI: u8 = 0x02;
    pub const STARLINK: u8 = 0x04;
    pub const LTE: u8 = 0x08;
    pub const VPN: u8 = 0x10;
    /// GPS has a fix; `sats` is still displayed either way.
    pub const GPS_FIX: u8 = 0x20;
}

/// Sound kinds for [`Display::sound`] / [`sound_payload`].
pub mod sound {
    /// One short beep (1 kHz, 120 ms).
    pub const SINGLE: u8 = 0x00;
    /// Two short beeps.
    pub const DOUBLE: u8 = 0x01;
    /// Three short beeps.
    pub const TRIPLE: u8 = 0x02;
    /// One long beep (1 kHz, 600 ms).
    pub const LONG: u8 = 0x03;
    /// Arbitrary tone sequence, see [`super::sound_sequence_payload`].
    pub const SEQUENCE: u8 = 0x10;
    /// Charger connected (rising two-tone).
    pub const CHARGER_CONNECTED: u8 = 0x20;
    /// Charging finished (rising three-tone).
    pub const CHARGING_DONE: u8 = 0x21;
}

/// Maximum number of tones in one sequence frame.
pub const MAX_TONES: usize = 8;

/// CRC-16/CCITT-FALSE (poly `0x1021`, init `0xFFFF`, no reflection).
pub fn crc16(data: &[u8]) -> u16 {
    let mut crc: u16 = 0xFFFF;
    for &b in data {
        crc = crc16_update(crc, b);
    }
    crc
}

fn crc16_update(mut crc: u16, byte: u8) -> u16 {
    crc ^= (byte as u16) << 8;
    for _ in 0..8 {
        crc = if crc & 0x8000 != 0 {
            (crc << 1) ^ 0x1021
        } else {
            crc << 1
        };
    }
    crc
}

/// Wrap a payload into a wire frame: magic, version/type, seq, length, payload,
/// CRC. Panics if the payload is longer than [`MAX_PAYLOAD`].
pub fn encode(msg_type: u8, seq: u8, payload: &[u8]) -> Vec<u8> {
    assert!(payload.len() <= MAX_PAYLOAD, "payload too long");
    let mut out = Vec::with_capacity(8 + payload.len());
    out.extend_from_slice(&MAGIC);
    out.push((VERSION << 4) | (msg_type & 0x0F));
    out.push(seq);
    out.extend_from_slice(&(payload.len() as u16).to_le_bytes());
    out.extend_from_slice(payload);
    let crc = crc16(&out[2..]);
    out.extend_from_slice(&crc.to_le_bytes());
    out
}

/// Periodic status snapshot. 9 bytes on the wire.
///
/// Send it every 1-3 s. The panel marks the data as stale ("Link lost") when
/// nothing arrived for 10 s; values are shown dimmed, not cleared.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct Telemetry {
    /// Pack voltage in centivolts: `5820` = 58.20 V.
    pub voltage_cv: u16,
    /// Battery charge 0..=100 %, or `None` for "unknown" (255 on the wire).
    pub batt_pct: Option<u8>,
    /// Combination of [`channel`] bits.
    pub flags: u8,
    /// GNSS satellites in view, 0-99.
    pub sats: u8,
    /// Odometer in decimetres: `6_341_200` = 634.12 km.
    pub odometer_dm: u32,
}

impl Telemetry {
    /// Encode the payload (9 bytes, little endian).
    pub fn encode(&self) -> Vec<u8> {
        let mut p = Vec::with_capacity(9);
        p.extend_from_slice(&self.voltage_cv.to_le_bytes());
        p.push(self.batt_pct.unwrap_or(255));
        p.push(self.flags);
        p.push(self.sats);
        p.extend_from_slice(&self.odometer_dm.to_le_bytes());
        p
    }

    /// Decode a payload produced by [`Telemetry::encode`] or by the panel.
    /// Trailing bytes (from a newer host) are ignored.
    pub fn decode(p: &[u8]) -> Option<Self> {
        if p.len() < 9 {
            return None;
        }
        let pct = p[2];
        Some(Self {
            voltage_cv: u16::from_le_bytes([p[0], p[1]]),
            batt_pct: if pct <= 100 { Some(pct) } else { None },
            flags: p[3],
            sats: p[4],
            odometer_dm: u32::from_le_bytes([p[5], p[6], p[7], p[8]]),
        })
    }
}

/// Severity of a notification banner; also picks the colour.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(u8)]
pub enum Severity {
    /// Dark grey-blue `#5B6B7C`.
    Debug = 0,
    /// Grey `#8695A5`.
    Info = 1,
    /// Amber `#F59E0B`; the panel also plays no sound by itself.
    Warning = 2,
    /// Red `#EF4444`.
    Error = 3,
}

/// A banner shown in the status line under the voltage.
///
/// An empty text clears the banner. The panel keeps at most 63 bytes of text
/// (UTF-8 is truncated on a character boundary by [`Notify::encode`]).
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Notify {
    pub severity: Severity,
    /// Toggle between the severity colour and the background every 500 ms.
    pub blink: bool,
    /// Hide after 5 s. Without it the banner stays until replaced/cleared.
    pub autohide: bool,
    pub text: String,
}

impl Notify {
    pub fn new(text: impl Into<String>, severity: Severity) -> Self {
        Self {
            severity,
            blink: false,
            autohide: false,
            text: text.into(),
        }
    }

    /// Clear the current banner.
    pub fn clear() -> Self {
        Self::new(String::new(), Severity::Info)
    }

    pub fn with_blink(mut self) -> Self {
        self.blink = true;
        self
    }

    pub fn with_autohide(mut self) -> Self {
        self.autohide = true;
        self
    }

    pub fn encode(&self) -> Vec<u8> {
        let mut flags = self.severity as u8;
        if self.blink {
            flags |= 0x04;
        }
        if self.autohide {
            flags |= 0x08;
        }

        let raw = self.text.as_bytes();
        let mut len = raw.len().min(80);
        while len > 0 && !self.text.is_char_boundary(len) {
            len -= 1;
        }

        let mut p = Vec::with_capacity(2 + len);
        p.push(flags);
        p.push(len as u8);
        p.extend_from_slice(&raw[..len]);
        p
    }

    pub fn decode(p: &[u8]) -> Option<Self> {
        if p.len() < 2 {
            return None;
        }
        let flags = p[0];
        let len = (p[1] as usize).min(p.len() - 2);
        Some(Self {
            severity: match flags & 0x03 {
                0 => Severity::Debug,
                1 => Severity::Info,
                2 => Severity::Warning,
                _ => Severity::Error,
            },
            blink: flags & 0x04 != 0,
            autohide: flags & 0x08 != 0,
            text: String::from_utf8_lossy(&p[2..2 + len]).into_owned(),
        })
    }
}

/// Odometer unit shown by the panel (tag `0x01`).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(u8)]
pub enum OdometerUnit {
    Km = 0,
    Miles = 1,
}

/// Configuration TLV. The panel applies defaults (km) on boot, so send the
/// configuration once after connecting (and after every panel reboot).
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct Config {
    pub odometer_unit: Option<OdometerUnit>,
}

impl Config {
    pub fn odometer_unit(mut self, unit: OdometerUnit) -> Self {
        self.odometer_unit = Some(unit);
        self
    }

    pub fn encode(&self) -> Vec<u8> {
        let mut items: Vec<(u8, Vec<u8>)> = Vec::new();
        if let Some(unit) = self.odometer_unit {
            items.push((0x01, vec![unit as u8]));
        }

        let mut p = vec![items.len() as u8];
        for (tag, value) in items {
            p.push(tag);
            p.push(value.len() as u8);
            p.extend_from_slice(&value);
        }
        p
    }

    pub fn decode(p: &[u8]) -> Option<Self> {
        let count = *p.first()? as usize;
        let mut pos = 1;
        let mut cfg = Config::default();
        for _ in 0..count {
            if pos + 2 > p.len() {
                return None;
            }
            let tag = p[pos];
            let len = p[pos + 1] as usize;
            pos += 2;
            if pos + len > p.len() {
                return None;
            }
            let value = &p[pos..pos + len];
            pos += len;
            if tag == 0x01 && len >= 1 {
                cfg.odometer_unit = Some(if value[0] == 1 {
                    OdometerUnit::Miles
                } else {
                    OdometerUnit::Km
                });
            }
            // Unknown tags are skipped on purpose (forward compatibility).
        }
        Some(cfg)
    }
}

/// One tone of a sequence sound.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Tone {
    /// 100-8000 Hz.
    pub freq_hz: u16,
    /// 20-2000 ms.
    pub on_ms: u16,
    /// 0-2000 ms pause after the tone (0 = none).
    pub off_ms: u16,
}

/// Payload for a preset sound: `kind` + reserved flags byte.
pub fn sound_payload(kind: u8) -> Vec<u8> {
    vec![kind, 0]
}

/// Payload for [`sound::SEQUENCE`]; `None` if `tones` is empty or longer than
/// [`MAX_TONES`].
pub fn sound_sequence_payload(tones: &[Tone]) -> Option<Vec<u8>> {
    if tones.is_empty() || tones.len() > MAX_TONES {
        return None;
    }
    let mut p = Vec::with_capacity(3 + tones.len() * 6);
    p.push(sound::SEQUENCE);
    p.push(0);
    p.push(tones.len() as u8);
    for t in tones {
        p.extend_from_slice(&t.freq_hz.to_le_bytes());
        p.extend_from_slice(&t.on_ms.to_le_bytes());
        p.extend_from_slice(&t.off_ms.to_le_bytes());
    }
    Some(p)
}

/// Answer to [`Display::ping`]. 10 bytes on the wire.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Pong {
    pub version: u8,
    /// Milliseconds since the panel booted (resets on every reboot).
    pub uptime_ms: u32,
    /// Telemetry frames received since boot.
    pub rx_frames: u32,
    /// Sequence byte of the ping that triggered this answer.
    pub echoed_seq: u8,
}

impl Pong {
    pub fn decode(p: &[u8]) -> Option<Self> {
        if p.len() < 10 {
            return None;
        }
        Some(Self {
            version: p[0],
            uptime_ms: u32::from_le_bytes([p[1], p[2], p[3], p[4]]),
            rx_frames: u32::from_le_bytes([p[5], p[6], p[7], p[8]]),
            echoed_seq: p[9],
        })
    }

    pub fn encode(&self) -> Vec<u8> {
        let mut p = Vec::with_capacity(10);
        p.push(self.version);
        p.extend_from_slice(&self.uptime_ms.to_le_bytes());
        p.extend_from_slice(&self.rx_frames.to_le_bytes());
        p.push(self.echoed_seq);
        p
    }
}

/// A frame received from the panel.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Frame {
    pub version: u8,
    pub msg_type: u8,
    pub seq: u8,
    pub payload: Vec<u8>,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum State {
    Magic1,
    Magic2,
    Header,
    Payload,
    CrcLo,
    CrcHi,
}

/// Byte-wise frame parser: feed every received byte, get complete frames back.
///
/// It resynchronises on the magic bytes after garbage or a bad CRC, so it can
/// be fed a raw stream (including ESP-IDF log lines) without extra framing.
#[derive(Debug)]
pub struct Parser {
    state: State,
    header: [u8; 4],
    header_len: usize,
    payload: Vec<u8>,
    payload_len: usize,
    crc_calc: u16,
    crc_rx: u16,
    errors: u32,
}

impl Default for Parser {
    fn default() -> Self {
        Self::new()
    }
}

impl Parser {
    pub fn new() -> Self {
        Self {
            state: State::Magic1,
            header: [0; 4],
            header_len: 0,
            payload: Vec::with_capacity(MAX_PAYLOAD),
            payload_len: 0,
            crc_calc: 0xFFFF,
            crc_rx: 0,
            errors: 0,
        }
    }

    /// Number of frames dropped so far (bad CRC, oversized payload).
    pub fn errors(&self) -> u32 {
        self.errors
    }

    pub fn feed(&mut self, byte: u8) -> Option<Frame> {
        match self.state {
            State::Magic1 => {
                if byte == MAGIC[0] {
                    self.state = State::Magic2;
                }
                None
            }
            State::Magic2 => {
                if byte == MAGIC[1] {
                    self.state = State::Header;
                    self.header_len = 0;
                    self.crc_calc = 0xFFFF;
                } else if byte != MAGIC[0] {
                    // Not a repeated first magic byte: restart.
                    self.state = State::Magic1;
                }
                None
            }
            State::Header => {
                self.header[self.header_len] = byte;
                self.header_len += 1;
                self.crc_calc = crc16_update(self.crc_calc, byte);
                if self.header_len == 4 {
                    self.payload_len = u16::from_le_bytes([self.header[2], self.header[3]]) as usize;
                    self.payload.clear();
                    if self.payload_len > MAX_PAYLOAD {
                        self.errors += 1;
                        self.state = State::Magic1;
                    } else {
                        self.state = if self.payload_len == 0 {
                            State::CrcLo
                        } else {
                            State::Payload
                        };
                    }
                }
                None
            }
            State::Payload => {
                self.payload.push(byte);
                self.crc_calc = crc16_update(self.crc_calc, byte);
                if self.payload.len() >= self.payload_len {
                    self.state = State::CrcLo;
                }
                None
            }
            State::CrcLo => {
                self.crc_rx = byte as u16;
                self.state = State::CrcHi;
                None
            }
            State::CrcHi => {
                self.crc_rx |= (byte as u16) << 8;
                self.state = State::Magic1;
                if self.crc_rx == self.crc_calc {
                    let ver_type = self.header[0];
                    Some(Frame {
                        version: ver_type >> 4,
                        msg_type: ver_type & 0x0F,
                        seq: self.header[1],
                        payload: std::mem::take(&mut self.payload),
                    })
                } else {
                    self.errors += 1;
                    None
                }
            }
        }
    }
}

/// An open connection to the panel.
///
/// The write methods are cheap (`write` + `flush`); call [`Display::poll`]
/// regularly (every 10-100 ms) to read PONG answers and to notice a broken
/// link early. `Display` is `Send`, so it can live in a dedicated thread that
/// owns the port (recommended when the rest of the software is async).
pub struct Display {
    port: Box<dyn serialport::SerialPort>,
    seq: u8,
    parser: Parser,
}

impl Display {
    /// Open the panel. The baud rate is ignored by USB CDC but must be a valid
    /// number for the serial layer; 115200 is a safe choice.
    ///
    /// On Linux this is usually `/dev/ttyACM0` (member of `dialout`); on
    /// Windows `COM4` or whatever the panel enumerated as.
    pub fn open(path: &str) -> serialport::Result<Self> {
        let port = serialport::new(path, 115_200)
            .timeout(Duration::from_millis(10))
            .open()?;
        Ok(Self {
            port,
            seq: 0,
            parser: Parser::new(),
        })
    }

    /// Send one frame with the next sequence number.
    pub fn send(&mut self, msg_type: u8, payload: &[u8]) -> io::Result<()> {
        if payload.len() > MAX_PAYLOAD {
            return Err(io::Error::new(
                io::ErrorKind::InvalidInput,
                "payload longer than 64 bytes",
            ));
        }
        self.seq = self.seq.wrapping_add(1);
        let frame = encode(msg_type, self.seq, payload);
        self.port.write_all(&frame)?;
        self.port.flush()
    }

    pub fn telemetry(&mut self, t: &Telemetry) -> io::Result<()> {
        self.send(msg::TELEMETRY, &t.encode())
    }

    pub fn notify(&mut self, n: &Notify) -> io::Result<()> {
        self.send(msg::NOTIFY, &n.encode())
    }

    pub fn config(&mut self, c: &Config) -> io::Result<()> {
        self.send(msg::CONFIG, &c.encode())
    }

    /// Play a preset sound, e.g. [`sound::DOUBLE`].
    pub fn sound(&mut self, kind: u8) -> io::Result<()> {
        self.send(msg::SOUND, &sound_payload(kind))
    }

    /// Play a custom tone sequence (1-8 tones).
    pub fn sound_sequence(&mut self, tones: &[Tone]) -> io::Result<()> {
        let payload = sound_sequence_payload(tones).ok_or_else(|| {
            io::Error::new(io::ErrorKind::InvalidInput, "expected 1..=8 tones")
        })?;
        self.send(msg::SOUND, &payload)
    }

    pub fn ping(&mut self) -> io::Result<()> {
        self.send(msg::PING, &[])
    }

    /// Read everything that is available now and return the complete frames.
    ///
    /// Blocks for at most the port timeout (10 ms) when the link is idle.
    /// Frames with a bad CRC or an oversized payload are dropped and counted
    /// in [`Parser::errors`] (access it through [`Display::parse_errors`]).
    pub fn poll(&mut self) -> io::Result<Vec<Frame>> {
        let mut buf = [0u8; 256];
        let mut frames = Vec::new();
        loop {
            match self.port.read(&mut buf) {
                Ok(0) => break,
                Ok(n) => {
                    for &b in &buf[..n] {
                        if let Some(frame) = self.parser.feed(b) {
                            frames.push(frame);
                        }
                    }
                }
                Err(e) if e.kind() == io::ErrorKind::TimedOut => break,
                Err(e) if e.kind() == io::ErrorKind::WouldBlock => break,
                Err(e) => return Err(e),
            }
        }
        Ok(frames)
    }

    /// Number of malformed frames received so far.
    pub fn parse_errors(&self) -> u32 {
        self.parser.errors()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn hex(bytes: &[u8]) -> String {
        bytes
            .iter()
            .map(|b| format!("{b:02X}"))
            .collect::<Vec<_>>()
            .join(" ")
    }

    fn unhex(s: &str) -> Vec<u8> {
        s.split_whitespace()
            .map(|w| u8::from_str_radix(w, 16).unwrap())
            .collect()
    }

    #[test]
    fn crc_check_vector() {
        assert_eq!(crc16(b"123456789"), 0x29B1);
    }

    #[test]
    fn golden_telemetry() {
        let t = Telemetry {
            voltage_cv: 5820,
            batt_pct: Some(95),
            flags: 0x3F,
            sats: 14,
            odometer_dm: 6_340_000,
        };
        let frame = encode(msg::TELEMETRY, 1, &t.encode());
        assert_eq!(
            hex(&frame),
            "A5 5A 11 01 09 00 BC 16 5F 3F 0E A0 BD 60 00 1D 59"
        );
        assert_eq!(Telemetry::decode(&t.encode()), Some(t));
    }

    #[test]
    fn golden_notify_clear() {
        let frame = encode(msg::NOTIFY, 3, &Notify::clear().encode());
        assert_eq!(hex(&frame), "A5 5A 12 03 02 00 01 00 5F AF");
    }

    #[test]
    fn golden_config_miles() {
        let cfg = Config::default().odometer_unit(OdometerUnit::Miles);
        let frame = encode(msg::CONFIG, 4, &cfg.encode());
        assert_eq!(hex(&frame), "A5 5A 13 04 04 00 01 01 01 01 0C 61");
        assert_eq!(Config::decode(&cfg.encode()), Some(cfg));
    }

    #[test]
    fn golden_ping() {
        let frame = encode(msg::PING, 5, &[]);
        assert_eq!(hex(&frame), "A5 5A 14 05 00 00 66 BE");
    }

    #[test]
    fn golden_sound_double() {
        let frame = encode(msg::SOUND, 2, &sound_payload(sound::DOUBLE));
        assert_eq!(hex(&frame), "A5 5A 15 02 02 00 01 00 4F CD");
    }

    #[test]
    fn golden_sound_sequence() {
        let tones = [
            Tone {
                freq_hz: 800,
                on_ms: 120,
                off_ms: 60,
            },
            Tone {
                freq_hz: 1200,
                on_ms: 160,
                off_ms: 0,
            },
        ];
        let payload = sound_sequence_payload(&tones).unwrap();
        let frame = encode(msg::SOUND, 7, &payload);
        assert_eq!(
            hex(&frame),
            "A5 5A 15 07 0F 00 10 00 02 20 03 78 00 3C 00 B0 04 A0 00 00 00 3C 98"
        );
    }

    #[test]
    fn parser_reads_golden_frames() {
        let stream = unhex(
            "A5 5A 11 01 09 00 BC 16 5F 3F 0E A0 BD 60 00 1D 59 \
             A5 5A 12 03 02 00 01 00 5F AF",
        );
        let mut parser = Parser::new();
        let frames: Vec<Frame> = stream.iter().filter_map(|&b| parser.feed(b)).collect();

        assert_eq!(frames.len(), 2);
        assert_eq!(frames[0].msg_type, msg::TELEMETRY);
        assert_eq!(frames[0].version, VERSION);
        assert_eq!(frames[0].seq, 1);
        assert_eq!(
            Telemetry::decode(&frames[0].payload),
            Some(Telemetry {
                voltage_cv: 5820,
                batt_pct: Some(95),
                flags: 0x3F,
                sats: 14,
                odometer_dm: 6_340_000,
            })
        );
        assert_eq!(frames[1].msg_type, msg::NOTIFY);
        assert_eq!(Notify::decode(&frames[1].payload), Some(Notify::clear()));
        assert_eq!(parser.errors(), 0);
    }

    #[test]
    fn parser_resyncs_after_garbage_and_bad_crc() {
        let golden = unhex("A5 5A 14 05 00 00 66 BE");
        let mut stream = vec![0x00, 0xFF, 0xA5, 0xA5, 0x5A, 0x00, 0xAA]; // junk
        let mut broken = golden.clone();
        let last = broken.len() - 1;
        broken[last] ^= 0xFF;
        stream.extend_from_slice(&broken); // bad CRC: dropped
        stream.extend_from_slice(&golden); // good frame: must be found

        let mut parser = Parser::new();
        let frames: Vec<Frame> = stream.iter().filter_map(|&b| parser.feed(b)).collect();
        assert_eq!(frames.len(), 1);
        assert_eq!(frames[0].msg_type, msg::PING);
        assert_eq!(frames[0].seq, 5);
        assert!(parser.errors() >= 1);
    }

    #[test]
    fn parser_rejects_oversized_payload() {
        // Magic + ver/type + seq + length 0x0100 (256 bytes) + CRC.
        let stream = unhex("A5 5A 11 01 00 01 00 00");
        let mut parser = Parser::new();
        for b in stream {
            assert!(parser.feed(b).is_none());
        }
        assert_eq!(parser.errors(), 1);
    }

    #[test]
    fn pong_roundtrip() {
        let pong = Pong {
            version: 1,
            uptime_ms: 38_731,
            rx_frames: 7,
            echoed_seq: 42,
        };
        assert_eq!(Pong::decode(&pong.encode()), Some(pong));
    }

    #[test]
    fn notify_truncates_on_char_boundary() {
        // 80 bytes of ASCII + a 2-byte character that must not be split.
        let text = "x".repeat(79) + "ї";
        let payload = Notify::new(text, Severity::Info).encode();
        assert_eq!(payload[1], 79);
    }

    #[test]
    fn config_ignores_unknown_tags() {
        // count 2: tag 0x01 unit=mi, tag 0x42 len 2 value 0xAA 0xBB
        let payload = unhex("02 01 01 01 42 02 AA BB");
        assert_eq!(
            Config::decode(&payload),
            Some(Config::default().odometer_unit(OdometerUnit::Miles))
        );
    }
}
