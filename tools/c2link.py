#!/usr/bin/env python3
"""Test client for the c2_display USB CDC link.

The protocol is described in docs/link-protocol.md:

    A5 5A | ver_type | seq | len (LE16) | payload | crc16 (LE16)

Examples (Windows: `py tools/c2link.py ...`, Linux: `python3 ...`):

    py tools/c2link.py --port COM4 telemetry --voltage 58.2 --pct 95 --sats 14 --odo 634
    py tools/c2link.py --port COM4 notify "Starlink searching" --severity info --hide
    py tools/c2link.py --port COM4 notify                    # clear the line
    py tools/c2link.py --port COM4 config --odo mi
    py tools/c2link.py --port COM4 ping
    py tools/c2link.py --port COM4 sound double
    py tools/c2link.py --port COM4 sound sequence --tones "800:120:60,1200:160:0"
    py tools/c2link.py --port COM4 demo                      # live demo stream

Requires pyserial (`py -m pip install pyserial`).
"""

import argparse
import struct
import sys
import time

try:
    import serial
except ImportError:  # pragma: no cover
    sys.exit("pyserial is required: py -m pip install pyserial")

MAGIC = b"\xA5\x5A"
VERSION = 1

MSG_TELEMETRY = 0x1
MSG_NOTIFY = 0x2
MSG_CONFIG = 0x3
MSG_PING = 0x4
MSG_SOUND = 0x5
MSG_PONG = 0x8

CHANNELS = {
    "crsf": 0x01,
    "wifi": 0x02,
    "starlink": 0x04,
    "lte": 0x08,
    "vpn": 0x10,
    "gps": 0x20,
}

SEVERITY = {"debug": 0, "info": 1, "warning": 2, "error": 3}
NOTIFY_BLINK = 0x04
NOTIFY_AUTOHIDE = 0x08

# Sound kinds; 0x10 is a custom tone sequence, the rest are display presets.
SOUND_KINDS = {
    "single": 0x00,        # one short peep
    "double": 0x01,        # peep - pause - peep
    "triple": 0x02,        # peep - peep - peep
    "long": 0x03,          # one long tone
    "charge-start": 0x20,  # charger connected: two rising tones
    "charge-done": 0x21,   # charging finished: three rising tones
}
SOUND_SEQUENCE = 0x10
SOUND_MAX_TONES = 8

_seq = 0


def crc16(data: bytes) -> int:
    """CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF)."""
    crc = 0xFFFF
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def frame(msg_type: int, payload: bytes) -> bytes:
    global _seq
    _seq = (_seq + 1) & 0xFF
    body = bytes([(VERSION << 4) | msg_type, _seq]) + struct.pack("<H", len(payload)) + payload
    return MAGIC + body + struct.pack("<H", crc16(body))


def telemetry_frame(voltage: float, pct: int, flags: int, sats: int, odo_km: float) -> bytes:
    payload = struct.pack(
        "<HBBBI",
        int(round(voltage * 100)),
        max(0, min(255, pct)),
        flags & 0xFF,
        max(0, min(255, sats)),
        int(round(odo_km * 10000)),  # km -> decimetres
    )
    return frame(MSG_TELEMETRY, payload)


def notify_frame(text: str, severity: str = "info", blink: bool = False, hide: bool = False) -> bytes:
    flags = SEVERITY[severity]
    if blink:
        flags |= NOTIFY_BLINK
    if hide:
        flags |= NOTIFY_AUTOHIDE
    raw = text.encode("utf-8")[:80]
    return frame(MSG_NOTIFY, bytes([flags, len(raw)]) + raw)


def config_frame(odo_unit: str = "km", extra: bytes = b"") -> bytes:
    """TLV list; tag 0x01 = odometer unit (0 km, 1 mi)."""
    items = bytes([0x01, 0x01, 1 if odo_unit == "mi" else 0]) + extra
    count = 1 + (len(extra) > 0)
    return frame(MSG_CONFIG, bytes([count]) + items)


def ping_frame() -> bytes:
    return frame(MSG_PING, b"")


def sound_frame(kind: int, tones=None) -> bytes:
    """kind 0x10 = tone sequence: list of (freq_hz, on_ms, off_ms)."""
    if kind == SOUND_SEQUENCE:
        if not tones:
            raise ValueError("a tone sequence needs at least one tone")
        payload = bytes([kind, 0x00, len(tones)])
        for freq, on_ms, off_ms in tones:
            payload += struct.pack("<HHH", freq, on_ms, off_ms)
        return frame(MSG_SOUND, payload)
    return frame(MSG_SOUND, bytes([kind, 0x00]))


def open_port(name: str, baud: int) -> "serial.Serial":
    ser = serial.Serial()
    ser.port = name
    ser.baudrate = baud          # nominal for USB CDC, ignored by the device
    ser.timeout = 0
    ser.write_timeout = 2
    ser.dsrdtr = False
    ser.rtscts = False
    # Set both lines *before* opening: pyserial applies the stored state while
    # configuring the port, so DTR/RTS are never asserted. Asserting them makes
    # the ESP32-S3 USB-Serial/JTAG reboot (and can drop it into download mode).
    ser.dtr = False
    ser.rts = False
    ser.open()
    return ser


def drain_logs(ser: "serial.Serial") -> None:
    """Print whatever the display sends back (its serial log)."""
    data = ser.read(getattr(ser, "in_waiting", 0) or 0)
    if data:
        for line in data.decode("utf-8", "replace").splitlines():
            if line.strip():
                print(f"[dev] {line}")


def parse_channels(value: str) -> int:
    flags = 0
    for name in (value or "").replace(" ", "").split(","):
        if not name:
            continue
        if name not in CHANNELS:
            sys.exit(f"unknown channel {name!r}; use {', '.join(CHANNELS)} or 'all'/'none'")
        flags |= CHANNELS[name]
    return flags


def cmd_telemetry(ser, args):
    ser.write(
        telemetry_frame(
            args.voltage,
            args.pct,
            parse_channels(args.channels),
            args.sats,
            args.odo,
        )
    )
    print("sent telemetry")


def cmd_notify(ser, args):
    ser.write(notify_frame(args.text, args.severity, args.blink, args.hide))
    print(f"sent notification {args.text!r} severity={args.severity} blink={args.blink} hide={args.hide}")


def cmd_config(ser, args):
    ser.write(config_frame(args.odo))
    print(f"sent config odo_unit={args.odo}")


def parse_tones(spec: str):
    tones = []
    for item in spec.replace(" ", "").split(","):
        if not item:
            continue
        parts = item.split(":")
        if len(parts) != 3:
            sys.exit(f"bad tone {item!r}, expected freq:on_ms:off_ms")
        tones.append(tuple(int(x) for x in parts))
    return tones


def cmd_sound(ser, args):
    if args.name == "sequence":
        tones = parse_tones(args.tones)
        if not tones:
            sys.exit("--tones is required for 'sequence', e.g. 800:120:60,1200:160:0")
        if len(tones) > SOUND_MAX_TONES:
            sys.exit(f"at most {SOUND_MAX_TONES} tones")
        ser.write(sound_frame(SOUND_SEQUENCE, tones))
        print(f"sent tone sequence: {tones}")
        return

    ser.write(sound_frame(SOUND_KINDS[args.name]))
    print(f"sent sound {args.name} (kind 0x{SOUND_KINDS[args.name]:02X})")


def cmd_ping(ser, args):
    ser.write(ping_frame())
    time.sleep(0.3)
    data = ser.read(ser.in_waiting or 0)
    print(f"received {len(data)} bytes")
    idx = data.find(MAGIC)
    if idx >= 0 and len(data) >= idx + 10:
        frame_bytes = data[idx:]
        msg_type = frame_bytes[2] & 0x0F
        length = struct.unpack("<H", frame_bytes[4:6])[0]
        payload = frame_bytes[6:6 + length]
        if msg_type == MSG_PONG and len(payload) >= 10:
            up = struct.unpack("<I", payload[1:5])[0]
            frames = struct.unpack("<I", payload[5:9])[0]
            print(f"pong: uptime {up} ms, {frames} telemetry frames, echoed seq {payload[9]}")
            return
    print("no valid pong in the reply")


def cmd_demo(ser, args):
    """Continuously stream a plausible flight: telemetry + rare notifications."""
    print(f"demo stream on {args.port}, {args.period:.1f} s per telemetry frame, Ctrl+C to stop")
    ser.write(config_frame(args.odo))

    notifications = [
        ("Starlink searching", "info", False, True),
        ("Carrier signal weak", "warning", True, True),
        ("VPN tunnel down", "error", True, False),
        ("", "info", False, False),   # clear
    ]

    t0 = time.time()
    last_notify = 0.0
    notify_idx = 0
    last_log = 0.0

    try:
        while True:
            t = time.time() - t0

            if args.duration and t >= args.duration:
                print("duration elapsed, stopping")
                break

            cycle = t % 40.0
            if cycle < 20.0:
                pct = 95 - (95 - 45) * (cycle / 20.0)
            elif cycle < 35.0:
                pct = 45 - (45 - 10) * ((cycle - 20.0) / 15.0)
            else:
                pct = 10.0
            voltage = 42.0 + (pct / 100.0) * (58.8 - 42.0)

            flags = CHANNELS["crsf"] | CHANNELS["vpn"]
            if (t // 12) % 2 == 0:
                flags |= CHANNELS["wifi"]
            if (t // 9) % 3 != 2:
                flags |= CHANNELS["starlink"]
            if (t // 15) % 2 == 0:
                flags |= CHANNELS["lte"]
            sats = 14 if pct > 20 else 0

            ser.write(telemetry_frame(voltage, int(pct + 0.5), flags, sats, 634 + t / 30.0))

            if t - last_notify >= 9.0:
                last_notify = t
                text, sev, blink, hide = notifications[notify_idx % len(notifications)]
                notify_idx += 1
                ser.write(notify_frame(text, sev, blink, hide))
                if text and not args.no_sound:
                    # info -> single, warning -> double, error -> triple
                    ser.write(sound_frame(SOUND_KINDS[{"info": "single",
                                                       "warning": "double",
                                                       "error": "triple"}[sev]]))

            if time.time() - last_log > 0.2:
                last_log = time.time()
                drain_logs(ser)

            time.sleep(args.period)
    except KeyboardInterrupt:
        print("\nstopped")


def main():
    ap = argparse.ArgumentParser(description="c2_display USB CDC test client")
    ap.add_argument("--port", required=True, help="serial port, e.g. COM4 or /dev/ttyACM0")
    ap.add_argument("--baud", type=int, default=115200, help="nominal only, USB CDC ignores it")
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("telemetry", help="send one telemetry frame")
    p.add_argument("--voltage", type=float, default=58.2)
    p.add_argument("--pct", type=int, default=95)
    p.add_argument("--sats", type=int, default=14)
    p.add_argument("--odo", type=float, default=634.0, help="odometer in km")
    p.add_argument("--channels", default="crsf,wifi,starlink,vpn", help="comma list or all/none")
    p.set_defaults(func=cmd_telemetry)

    p = sub.add_parser("notify", help="send a notification (omitted/empty text clears the line)")
    p.add_argument("text", nargs="?", default="", help="text; leave empty to clear the line")
    p.add_argument("--severity", choices=list(SEVERITY), default="info")
    p.add_argument("--blink", action="store_true", help="flash the text")
    p.add_argument("--hide", action="store_true", help="hide after 5 s")
    p.set_defaults(func=cmd_notify)

    p = sub.add_parser("config", help="send display configuration")
    p.add_argument("--odo", choices=["km", "mi"], default="km")
    p.set_defaults(func=cmd_config)

    p = sub.add_parser("ping", help="ping the display and print its pong")
    p.set_defaults(func=cmd_ping)

    p = sub.add_parser("sound", help="play a sound signal on the display")
    p.add_argument("name", choices=list(SOUND_KINDS) + ["sequence"])
    p.add_argument("--tones", default="",
                   help='custom sequence "freq:on_ms:off_ms,..." (name=sequence, max 8)')
    p.set_defaults(func=cmd_sound)

    p = sub.add_parser("demo", help="stream a continuous demo")
    p.add_argument("--period", type=float, default=2.0, help="seconds between telemetry frames")
    p.add_argument("--odo", choices=["km", "mi"], default="km")
    p.add_argument("--duration", type=float, default=0.0, help="stop after N seconds (0 = forever)")
    p.add_argument("--no-sound", action="store_true", help="do not beep on notifications")
    p.set_defaults(func=cmd_demo)

    args = ap.parse_args()

    ser = open_port(args.port, args.baud)
    try:
        if getattr(args, "channels", None) == "all":
            args.channels = ",".join(CHANNELS)
        elif getattr(args, "channels", None) == "none":
            args.channels = ""
        args.func(ser, args)
        time.sleep(0.3)
        drain_logs(ser)
    finally:
        ser.close()


if __name__ == "__main__":
    main()
