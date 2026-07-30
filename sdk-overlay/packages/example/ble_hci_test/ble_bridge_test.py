#!/usr/bin/env python3
"""
Bridge verification script for my-server custom BLE characteristic.

Tests fanout behavior:
  1) BLE input  -> BLE notify + UART output
  2) UART input -> UART echo + BLE notify

Dependencies:
  pip install bleak pyserial
"""

from __future__ import annotations

import argparse
import asyncio
import contextlib
import inspect
import os
import sys
import threading
import time
from typing import Optional, Any

SVC_UUID = "78563412-3412-7856-1234-567812345678"
CHR_UUID = "79563412-3412-7856-1234-567812345678"
DEFAULT_BLE_NAME = "TestBLE"
OP_BRIDGE_PASSTHROUGH = 0x20


def norm_uuid(u: str) -> str:
    return u.replace("-", "").lower()


def find_transfer_char(svc_list) -> object:
    want_chr = norm_uuid(CHR_UUID)
    want_svc = norm_uuid(SVC_UUID)
    for svc in svc_list:
        for ch in svc.characteristics:
            if norm_uuid(ch.uuid) == want_chr:
                return ch
    for svc in svc_list:
        if norm_uuid(svc.uuid) != want_svc:
            continue
        for ch in svc.characteristics:
            props = set(ch.properties)
            if "write-without-response" in props or "write" in props:
                return ch
    raise RuntimeError("Transfer characteristic not found")


async def get_ble_services(client) -> list[object]:
    """Return GATT services on both old and new Bleak versions.

    Bleak <= 0.21 exposed await client.get_services(). Newer Bleak versions
    expose the discovered services through client.services after connection.
    """
    get_services = getattr(client, "get_services", None)
    if callable(get_services):
        services = get_services()
        if inspect.isawaitable(services):
            services = await services
        return list(services)

    services = getattr(client, "services", None)
    if services is None:
        raise RuntimeError("BLE services are not available after connecting")
    return list(services)


async def resolve_target(address: Optional[str], name: str, scan_timeout: float) -> Any:
    """Scan and return the matching BLEDevice object.

    On Windows/Bleak, connecting with a BLEDevice object is more reliable than
    connecting with a plain address string because it avoids an implicit discover
    inside BleakClient.connect().
    """
    from bleak import BleakScanner

    if address:
        print(f"Scanning {scan_timeout:.0f}s for address {address} ...")
    else:
        print(f"Scanning {scan_timeout:.0f}s for name {name!r} ...")

    found = await BleakScanner.discover(timeout=scan_timeout, return_adv=True)
    matches = []
    target_addr = address.replace("-", ":").casefold() if address else None

    for d, ad in found.values():
        names = []
        if d.name:
            names.append(d.name)
        if ad and ad.local_name and ad.local_name not in names:
            names.append(ad.local_name)

        addr_matches = target_addr is not None and d.address.replace("-", ":").casefold() == target_addr
        name_matches = target_addr is None and any(name.casefold() in n.casefold() for n in names)
        if addr_matches or name_matches:
            rssi = ad.rssi if ad and ad.rssi is not None else -999
            matches.append((rssi, d, names[0] if names else "(no name)"))

    if not matches:
        if address:
            raise RuntimeError(f"Device with address {address} was not found")
        raise RuntimeError(f"Device not found: {name!r}")

    matches.sort(key=lambda x: x[0], reverse=True)
    rssi, dev, display_name = matches[0]
    print(f"Using {dev.address} ({display_name}, RSSI={rssi})")
    return dev


class ByteMonitor:
    def __init__(self) -> None:
        self._buf = bytearray()
        self._lock = threading.Lock()

    def feed(self, data: bytes) -> None:
        if not data:
            return
        with self._lock:
            self._buf.extend(data)
            if len(self._buf) > 65536:
                del self._buf[:-32768]

    def wait_contains(self, token: bytes, timeout: float) -> bool:
        deadline = time.time() + timeout
        while time.time() < deadline:
            with self._lock:
                if token in self._buf:
                    return True
            time.sleep(0.03)
        return False

    async def async_wait_contains(self, token: bytes, timeout: float) -> bool:
        """Wait without blocking the asyncio event loop.

        BLE notification callbacks on Windows/Bleak may need the event loop to
        keep running. Using the blocking wait_contains() inside async code can
        starve notifications and produce false BLE_echo=MISS results.
        """
        deadline = time.time() + timeout
        while time.time() < deadline:
            with self._lock:
                if token in self._buf:
                    return True
            await asyncio.sleep(0.03)
        return False


class SerialReader:
    def __init__(self, port: str, baud: int) -> None:
        import serial

        self.ser = serial.Serial(port, baudrate=baud, timeout=0.05)
        self.mon = ByteMonitor()
        self._stop = threading.Event()
        self._th = threading.Thread(target=self._run, daemon=True)
        self._th.start()

    def _run(self) -> None:
        while not self._stop.is_set():
            chunk = self.ser.read(256)
            if chunk:
                self.mon.feed(chunk)

    def write(self, data: bytes) -> None:
        self.ser.write(data)
        self.ser.flush()

    def close(self) -> None:
        self._stop.set()
        self._th.join(timeout=0.5)
        self.ser.close()


async def run(args: argparse.Namespace) -> int:
    from bleak import BleakClient
    from bleak.exc import BleakError

    target = await resolve_target(args.device, args.name, args.scan_timeout)
    ble_mon = ByteMonitor()
    serial_reader = None

    if args.serial:
        serial_reader = SerialReader(args.serial, args.baud)
        print(f"UART opened: {args.serial} @ {args.baud}")
    else:
        print("UART not configured: UART checks will be skipped")

    def on_notify(_ch, data: bytearray) -> None:
        payload = bytes(data)
        if args.debug_notify:
            text = payload.decode(errors="replace")
            print(f"    NOTIFY len={len(payload)} hex={payload.hex()} text={text!r}")
        ble_mon.feed(payload)

    client_kwargs: dict[str, Any] = {"timeout": args.connect_timeout}
    if sys.platform != "win32":
        client_kwargs["services"] = [SVC_UUID]

    client: Any = None
    ch: Any = None

    async def connect_session(label: str) -> tuple[Any, Any]:
        nonlocal client, ch
        last_err: Exception | None = None
        for attempt in range(1, args.connect_retries + 1):
            try:
                if client is not None:
                    with contextlib.suppress(Exception):
                        await client.disconnect()
                client = BleakClient(target, **client_kwargs)
                await client.connect()
                if not client.is_connected:
                    raise BleakError("Not connected")

                svcs = await get_ble_services(client)
                ch = find_transfer_char(svcs)
                props = list(getattr(ch, "properties", []))
                if "notify" not in props and "indicate" not in props:
                    print("WARNING: chosen characteristic does not advertise notify/indicate")

                await client.start_notify(ch, on_notify)
                await asyncio.sleep(0.2)

                print(
                    f"{label} connected ({attempt}/{args.connect_retries}): "
                    f"char={ch.uuid} props={props}"
                )
                return client, ch
            except Exception as e:
                last_err = e
                print(
                    f"{label} failed ({attempt}/{args.connect_retries}): "
                    f"{type(e).__name__}: {e}"
                )
                await asyncio.sleep(args.reconnect_delay)
        raise RuntimeError(f"{label} failed after {args.connect_retries} attempts: {last_err}")

    failed = 0
    link_lost = False
    client, ch = await connect_session("initial connect")
    print(f"Write mode: {'write-with-response' if args.write_response else 'write-without-response'}")

    print("\n[1/2] BLE -> UART + BLE")
    i = 0
    while i < args.count:
        token = f"B2U_{i:03d}_{int(time.time() * 1000)}".encode()
        pkt = bytes([OP_BRIDGE_PASSTHROUGH]) + token + b"\n"

        try:
            await client.write_gatt_char(ch, pkt, response=args.write_response)
        except BleakError as e:
            print(f"  #{i + 1:02d} LINK_LOST during write: {e} (trying reconnect)")
            try:
                client, ch = await connect_session("reconnect")
                continue
            except Exception as re:
                remain = args.count - i
                failed += remain
                link_lost = True
                print(
                    f"  reconnect failed: {type(re).__name__}: {re} "
                    f"(mark remaining {remain} frames as FAIL)"
                )
                break

        if serial_reader:
            ok_ble, ok_uart = await asyncio.gather(
                ble_mon.async_wait_contains(token, args.timeout),
                serial_reader.mon.async_wait_contains(token, args.timeout),
            )
        else:
            ok_ble = await ble_mon.async_wait_contains(token, args.timeout)
            ok_uart = None

        uart_rx_text = "SKIP" if ok_uart is None else ("OK" if ok_uart else "MISS")
        print(
            f"  #{i + 1:02d} BLE_echo={'OK' if ok_ble else 'MISS'} "
            f"UART_rx={uart_rx_text}"
        )
        if not ok_ble or (ok_uart is False):
            failed += 1
        i += 1
        await asyncio.sleep(args.gap)

    if link_lost:
        print("\n[2/2] UART -> UART + BLE (skipped: BLE link lost)")
    elif serial_reader:
        print("\n[2/2] UART -> UART + BLE")
        for i in range(args.count):
            if not client.is_connected:
                try:
                    client, ch = await connect_session("reconnect")
                except Exception as re:
                    remain = args.count - i
                    failed += remain
                    print(
                        f"  #{i + 1:02d} LINK_LOST before UART frame: {type(re).__name__}: {re} "
                        f"(mark remaining {remain} frames as FAIL)"
                    )
                    break

            token = f"U2B_{i:03d}_{int(time.time() * 1000)}".encode()
            serial_reader.write(token + b"\n")

            ok_uart, ok_ble = await asyncio.gather(
                serial_reader.mon.async_wait_contains(token, args.timeout),
                ble_mon.async_wait_contains(token, args.timeout),
            )
            print(
                f"  #{i + 1:02d} UART_echo={'OK' if ok_uart else 'MISS'} "
                f"BLE_rx={'OK' if ok_ble else 'MISS'}"
            )
            if not (ok_ble and ok_uart):
                failed += 1
            await asyncio.sleep(args.gap)
    else:
        print("\n[2/2] UART -> UART + BLE (skipped: --serial not set)")

    if client is not None:
        with contextlib.suppress(Exception):
            await client.stop_notify(ch)
        with contextlib.suppress(Exception):
            await client.disconnect()

    if serial_reader:
        serial_reader.close()

    total = args.count * (2 if args.serial else 1)
    passed = total - failed
    print(f"\nResult: PASS={passed} FAIL={failed} TOTAL={total}")
    return 0 if failed == 0 else 2


def build_argparser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        description="Verify my-server UART<->BLE bridge fanout behavior."
    )
    p.add_argument("-d", "--device", help="Board BLE MAC (optional)")
    p.add_argument("-n", "--name", default=DEFAULT_BLE_NAME, help="Board BLE name")
    p.add_argument("--scan-timeout", type=float, default=8.0, help="BLE scan seconds")
    p.add_argument("--connect-timeout", type=float, default=20.0, help="BLE connect timeout")
    p.add_argument("--serial", help="UART device path, e.g. /dev/ttyUSB0 or COM5")
    p.add_argument("--baud", type=int, default=115200, help="UART baudrate")
    p.add_argument("--write-response", action="store_true", help="Use BLE write with response instead of write without response")
    p.add_argument("--count", type=int, default=10, help="Frames per direction")
    p.add_argument("--timeout", type=float, default=2.5, help="Per-frame verify timeout")
    p.add_argument("--gap", type=float, default=0.08, help="Gap between frames")
    p.add_argument("--connect-retries", type=int, default=3, help="BLE connect/reconnect attempts")
    p.add_argument("--reconnect-delay", type=float, default=1.0, help="Seconds between reconnect attempts")
    p.add_argument("--debug-notify", action="store_true", help="Print raw BLE notifications")
    return p


def main() -> int:
    parser = build_argparser()
    args = parser.parse_args()
    if sys.platform == "win32" and args.write_response:
        print("Windows WinRT: forcing write-without-response ( --write-response disabled )")
        args.write_response = False
    try:
        return asyncio.run(run(args))
    except KeyboardInterrupt:
        print("\nInterrupted.")
        return 130
    except Exception as e:
        import traceback

        print(f"ERROR TYPE: {type(e).__name__}")
        print(f"ERROR REPR: {repr(e)}")
        traceback.print_exc()
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
