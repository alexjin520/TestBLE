#!/usr/bin/env python3
from __future__ import annotations

import argparse
import asyncio
import datetime as dt

from bleak import BleakClient, BleakScanner

SVC_UUID = "78563412-3412-7856-1234-567812345678"
CHR_UUID = "79563412-3412-7856-1234-567812345678"


def norm_addr(addr: str) -> str:
    return addr.replace("-", ":").upper()


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(
        description="BLE scan + optional connect health check"
    )
    p.add_argument("-d", "--device", help="Target MAC address")
    p.add_argument("-n", "--name", help="Target name keyword")
    p.add_argument("--scan-timeout", type=float, default=8.0, help="Seconds per scan")
    p.add_argument("--loops", type=int, default=1, help="Number of scan loops")
    p.add_argument("--interval", type=float, default=1.0, help="Sleep between loops")
    p.add_argument("--connect-test", action="store_true", help="Try connect after found")
    p.add_argument("--connect-timeout", type=float, default=15.0, help="Connect timeout")
    return p.parse_args()


def pick_target(found: dict, addr: str | None, name: str | None):
    target_addr = norm_addr(addr) if addr else None
    candidates = []
    for dev, adv in found.values():
        names = [x for x in (dev.name, getattr(adv, "local_name", None)) if x]
        dev_addr = norm_addr(dev.address)
        hit_addr = target_addr and dev_addr == target_addr
        hit_name = name and any(name.casefold() in n.casefold() for n in names)
        if hit_addr or hit_name:
            rssi = adv.rssi if adv and adv.rssi is not None else -999
            candidates.append((rssi, dev, adv))

    if not candidates:
        return None
    candidates.sort(key=lambda x: x[0], reverse=True)
    return candidates[0][1], candidates[0][2]


async def connect_probe(target, timeout: float) -> tuple[bool, str]:
    try:
        async with BleakClient(target, timeout=timeout) as client:
            if not client.is_connected:
                return False, "connected=False"
            # Trigger service discovery for stronger health signal.
            if hasattr(client, "get_services"):
                await client.get_services()
            return True, "connected + services_ok"
    except Exception as e:  # pragma: no cover - diagnostics tool
        return False, f"{type(e).__name__}: {e}"


async def main() -> int:
    args = parse_args()
    if not args.device and not args.name:
        print("Tip: pass -d MAC or -n NAME to track one target.")

    for i in range(1, args.loops + 1):
        ts = dt.datetime.now().strftime("%H:%M:%S")
        print(f"\n[{ts}] scan loop {i}/{args.loops} ...")
        found = await BleakScanner.discover(timeout=args.scan_timeout, return_adv=True)
        if not found:
            print("  no devices")
        else:
            for dev, adv in found.values():
                print(
                    f"  {dev.address} | name={dev.name!r} "
                    f"| rssi={getattr(adv, 'rssi', None)} "
                    f"| uuids={getattr(adv, 'service_uuids', None)}"
                )

        target = pick_target(found, args.device, args.name)
        if not target:
            print("  target: NOT_FOUND")
        else:
            dev, adv = target
            print(
                f"  target: FOUND {dev.address} name={dev.name!r} "
                f"rssi={getattr(adv, 'rssi', None)}"
            )
            if args.connect_test:
                ok, msg = await connect_probe(dev, args.connect_timeout)
                print(f"  connect_test: {'PASS' if ok else 'FAIL'} ({msg})")

        if i < args.loops and args.interval > 0:
            await asyncio.sleep(args.interval)

    return 0


if __name__ == "__main__":
    raise SystemExit(asyncio.run(main()))