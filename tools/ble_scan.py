#!/usr/bin/env python3
"""Scan BLE advertisements and report whether a given device is in the air.

Presence check only: no connection, no pairing, no writes.

    py tools/ble_scan.py --mac A4:C1:38:05:47:C3
    py tools/ble_scan.py --mac A4:C1:38:05:47:C3 --timeout 30
    py tools/ble_scan.py --all            # show every device, including unnamed

Requires: pip install bleak
Exit code: 0 if the target MAC was seen, 1 otherwise.
"""

from __future__ import annotations

import argparse
import asyncio
import sys

try:
    from bleak import BleakScanner
except ImportError:  # pragma: no cover
    sys.exit("bleak is not installed: py -m pip install bleak")


def norm(mac: str) -> str:
    return mac.upper().replace("-", ":").strip()


def hexs(data: bytes) -> str:
    return data.hex()


def describe(adv) -> str:
    parts = []
    if adv.service_uuids:
        parts.append("svc=" + ",".join(u.lstrip("0") or u for u in adv.service_uuids))
    for cid, data in (adv.manufacturer_data or {}).items():
        parts.append(f"mfg=0x{cid:04X}:{hexs(data)}")
    for uuid, data in (adv.service_data or {}).items():
        parts.append(f"sd={uuid}:{hexs(data)}")
    if adv.tx_power is not None:
        parts.append(f"tx={adv.tx_power}")
    return " ".join(parts)


async def scan(timeout: float) -> dict:
    return await BleakScanner.discover(timeout=timeout, return_adv=True)


def main() -> int:
    ap = argparse.ArgumentParser(description="BLE presence scan")
    ap.add_argument("--mac", help="target address, e.g. A4:C1:38:05:47:C3")
    ap.add_argument("--timeout", type=float, default=20.0, help="scan time, s (default 20)")
    ap.add_argument("--all", action="store_true", help="include devices without a name")
    args = ap.parse_args()

    target = norm(args.mac) if args.mac else None
    print(f"scanning for {args.timeout:.0f} s ...", flush=True)

    try:
        found = asyncio.run(scan(args.timeout))
    except Exception as e:  # noqa: BLE001
        print(f"scan failed: {e}", file=sys.stderr)
        return 2

    rows = []
    for addr, (dev, adv) in found.items():
        name = adv.local_name or dev.name or ""
        if not args.all and not name and addr.upper() != target:
            continue
        rows.append((adv.rssi, addr.upper(), name, describe(adv)))

    rows.sort(key=lambda r: r[0], reverse=True)

    print()
    print(f"{'RSSI':>5}  {'ADDRESS':<19}  {'NAME':<24}  DETAILS")
    print("-" * 100)
    hit = False
    for rssi, addr, name, details in rows:
        mark = ""
        if target and addr == target:
            hit = True
            mark = "  <== TARGET"
        print(f"{rssi:>5}  {addr:<19}  {name:<24}  {details}{mark}")

    print()
    if target:
        if hit:
            print(f"FOUND: {target} is advertising")
            return 0

        print(f"NOT FOUND: {target} was not seen in {args.timeout:.0f} s")
        print("hints:")
        print("  - battery BMS may be off, asleep, or out of range")
        print("  - if a phone/app is connected to the BMS, it may stop advertising")
        print("  - JK BMS usually advertises under a name like 'JK-...' (see --all)")
        return 1

    print(f"total: {len(rows)} device(s)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
