#!/usr/bin/env python3
"""
BLE file sender for my-server (packages/example/ble_hci_test/mybtgatt-server.c).

Board runs /opt/golgi/my-server, phone/PC connects, then this script writes:
  START -> DATA (chunked) -> END
Files appear on board under /app_data/ble_rx/<filename> (persistent; /tmp is cleared on reboot).

Requirements (dev PC with Bluetooth, not WSL-without-BT):
  pip install bleak

Examples:
  python3 ble_file_tx.py scan
  python3 ble_file_tx.py send photo.jpg
  python3 ble_file_tx.py status
  python3 ble_file_tx.py abort
  python3 ble_file_tx.py list-rx

Legacy (still works):
  python3 ble_file_tx.py -n TestBLE photo.jpg

Flash my-server build 20250703-rx12-fast-flow (file RX + status Notify flow control).
Python rev 20250703-v7-fc-fix: board-buffer cap, first-status wait, pacing fallback.

Custom GATT (same as mybtgatt-server.c):
  Service: 78563412-3412-7856-1234-567812345678
  Char:    79563412-3412-7856-1234-567812345678

DATA frame: [op][seq:4][len:2][payload][crc16:2]  crc16 over seq+len+payload
"""

from __future__ import annotations

import argparse
import asyncio
import os
import struct
import sys
import time
import zlib
from collections.abc import Callable

SCRIPT_REV = "20250703-v7-fc-fix"

SVC_UUID = "78563412-3412-7856-1234-567812345678"
CHR_UUID = "79563412-3412-7856-1234-567812345678"
BLE_RX_DIR = "/app_data/ble_rx"

OP_START = 0x01
OP_DATA = 0x02
OP_END = 0x03
OP_ABORT = 0x11

DEFAULT_BLE_NAME = "TestBLE"
DEVICE_FILE_NAME = ".ble_device"

DATA_HDR_LEN = 7  # opcode(1) + seq(4) + chunk_len(2)
DATA_CRC_LEN = 2
DATA_FRAME_OVERHEAD = DATA_HDR_LEN + DATA_CRC_LEN  # excluding opcode in hdr calc below
ATT_WRITE_OVERHEAD = 3
RESYNC_INTERVAL_PKTS = 32
STARTUP_EXTRA_DELAY = 0.50 if sys.platform == "win32" else 0.20
WARMUP_PACKETS = 12 if sys.platform == "win32" else 6
WARMUP_DELAY = 0.008 if sys.platform == "win32" else 0.004
MAX_TRANSFER_ATTEMPTS = 5
# WinRT: GATT read during write-without-response TX drops the link (proven on Windows)
WIN32_DISABLE_RESYNC = True

RX_ERR_FRAME_CRC = 0xE1
RX_ERR_SEQ = 0xE2
RX_ERR_FRAME_LEN = 0xE3
RX_ERR_NAMES = {
    RX_ERR_FRAME_CRC: "FRAME_CRC",
    RX_ERR_SEQ: "SEQ",
    RX_ERR_FRAME_LEN: "FRAME_LEN",
}
DEFAULT_MTU_REQUEST = 247
# WinRT write-without-response is unreliable above ~200-byte ATT payloads (non-fast)
SAFE_CHUNK_CAP = 200
# --fast: use MTU-sized payloads (247 MTU -> 235 bytes with frame CRC)
FAST_CHUNK_DEFAULT = 235
LARGE_FILE_THRESHOLD = 512 * 1024
LARGE_FILE_CHUNK = FAST_CHUNK_DEFAULT
DEFAULT_TX_DELAY = 0.002
LEGACY_FALLBACK_TX_DELAY = 0.006
# Notify sliding window: max in-flight DATA packets before sender blocks
# Must stay under mybtgatt-server FILE_RX_IO_BUF_SIZE (64KB) + HCI queue headroom
BOARD_RX_BUF_BYTES = 56 * 1024
DEFAULT_FC_WINDOW_PKTS = 64
FAST_FC_WINDOW_PKTS = 96
# Check flow control every N packets once the window is full
DEFAULT_FC_WAIT_STEP_PKTS = 1
FAST_FC_WAIT_STEP_PKTS = 4
FLOW_CONTROL_POLL_SEC = float(os.environ.get("BLE_FC_POLL_SEC", "0.05"))
FIRST_STATUS_TIMEOUT = float(os.environ.get("BLE_FIRST_STATUS_TIMEOUT", "8.0"))
FLOW_CONTROL_TIMEOUT = float(os.environ.get("BLE_FC_TIMEOUT", "20.0"))
DISABLE_NOTIFY_FC = os.environ.get("BLE_DISABLE_NOTIFY_FC", "0") not in ("", "0", "false", "False", "no", "No")
PROFILE_TX = os.environ.get("BLE_PROFILE", "0") not in ("", "0", "false", "False", "no", "No")
# Some Bleak backends report a temporary 20-byte WWR size even when the link can
# carry a larger negotiated ATT payload.  Treat very small reported values as
# suspicious and fall back to MTU-derived sizing; real oversize writes still fail
# immediately and are retried by the normal error path.
MIN_TRUSTED_WWR_PAYLOAD = 64
PROGRESS_INTERVAL_PKTS = 32
POST_TX_DRAIN_BPS = 10000
POST_TX_DRAIN_MIN = 5.0
# --safe: smaller chunks, longer pacing
SAFE_CHUNK = 128
SAFE_DELAY = 0.006

RX_STATE_IDLE = 0x00
RX_STATE_ACTIVE = 0x01
RX_STATE_DONE = 0x02
RX_STATE_ERROR = 0x03

RX_STATE_NAMES = {
    RX_STATE_IDLE: "IDLE",
    RX_STATE_ACTIVE: "ACTIVE",
    RX_STATE_DONE: "DONE",
    RX_STATE_ERROR: "ERROR",
}


def get_fc_params(fast: bool) -> tuple[int, int]:
    """Return (window_pkts, wait_step_pkts) for Notify flow control."""
    env_window = os.environ.get("BLE_FC_WINDOW_PKTS", "").strip()
    env_step = os.environ.get("BLE_FC_WAIT_STEP_PKTS", "").strip()
    window = int(env_window) if env_window else (FAST_FC_WINDOW_PKTS if fast else DEFAULT_FC_WINDOW_PKTS)
    step = int(env_step) if env_step else (FAST_FC_WAIT_STEP_PKTS if fast else DEFAULT_FC_WAIT_STEP_PKTS)
    return max(1, window), max(1, step)


def clamp_fc_window(window_pkts: int, chunk_bytes: int) -> int:
    """Cap in-flight packets so bytes in flight fit board 64KB RX buffer."""
    if chunk_bytes <= 0:
        return window_pkts
    max_pkts = max(8, BOARD_RX_BUF_BYTES // chunk_bytes)
    return min(window_pkts, max_pkts)


def should_fc_wait(seq: int, fc_window: int, fc_wait_step: int) -> bool:
    """True when sender should block until board next_seq catches up."""
    if seq < fc_window:
        return False
    return seq == fc_window or (seq % fc_wait_step == 0)


def startup_delay_for(fast: bool) -> float:
    if fast and sys.platform == "win32":
        return 0.20
    return STARTUP_EXTRA_DELAY


def crc32_ieee(data: bytes) -> int:
    return zlib.crc32(data) & 0xFFFFFFFF


_CRC16_TABLE = (
    0x0000, 0x1021, 0x2042, 0x3063, 0x4084, 0x50A5, 0x60C6, 0x70E7,
    0x8108, 0x9129, 0xA14A, 0xB16B, 0xC18C, 0xD1AD, 0xE1CE, 0xF1EF,
    0x1231, 0x0210, 0x3273, 0x2252, 0x52B5, 0x4294, 0x72F7, 0x62D6,
    0x9339, 0x8318, 0xB37B, 0xA35A, 0xD3BD, 0xC39C, 0xF3FF, 0xE3DE,
    0x2462, 0x3443, 0x0420, 0x1401, 0x64E6, 0x74C7, 0x44A4, 0x5485,
    0xA56A, 0xB54B, 0x8528, 0x9509, 0xE5EE, 0xF5CF, 0xC5AC, 0xD58D,
    0x3653, 0x2672, 0x1611, 0x0630, 0x76D7, 0x66F6, 0x5695, 0x46B4,
    0xB75B, 0xA77A, 0x9719, 0x8738, 0xF7DF, 0xE7FE, 0xD79D, 0xC7BC,
    0x48C4, 0x58E5, 0x6886, 0x78A7, 0x0840, 0x1861, 0x2802, 0x3823,
    0xC9CC, 0xD9ED, 0xE98E, 0xF9AF, 0x8948, 0x9969, 0xA90A, 0xB92B,
    0x5AF5, 0x4AD4, 0x7AB7, 0x6A96, 0x1A71, 0x0A50, 0x3A33, 0x2A12,
    0xDBFD, 0xCBDC, 0xFBBF, 0xEB9E, 0x9B79, 0x8B58, 0xBB3B, 0xAB1A,
    0x6CA6, 0x7C87, 0x4CE4, 0x5CC5, 0x2C22, 0x3C03, 0x0C60, 0x1C41,
    0xEDAE, 0xFD8F, 0xCDEC, 0xDDCD, 0xAD2A, 0xBD0B, 0x8D68, 0x9D49,
    0x7E97, 0x6EB6, 0x5ED5, 0x4EF4, 0x3E13, 0x2E32, 0x1E51, 0x0E70,
    0xFF9F, 0xEFBE, 0xDFDD, 0xCFFC, 0xBF1B, 0xAF3A, 0x9F59, 0x8F78,
    0x9188, 0x81A9, 0xB1CA, 0xA1EB, 0xD10C, 0xC12D, 0xF14E, 0xE16F,
    0x1080, 0x00A1, 0x30C2, 0x20E3, 0x5004, 0x4025, 0x7046, 0x6067,
    0x83B9, 0x9398, 0xA3FB, 0xB3DA, 0xC33D, 0xD31C, 0xE37F, 0xF35E,
    0x02B1, 0x1290, 0x22F3, 0x32D2, 0x4235, 0x5214, 0x6277, 0x7256,
    0xB5EA, 0xA5CB, 0x95A8, 0x8589, 0xF56E, 0xE54F, 0xD52C, 0xC50D,
    0x34E2, 0x24C3, 0x14A0, 0x0481, 0x7466, 0x6447, 0x5424, 0x4405,
    0xA7DB, 0xB7FA, 0x8799, 0x97B8, 0xE75F, 0xF77E, 0xC71D, 0xD73C,
    0x26D3, 0x36F2, 0x0691, 0x16B0, 0x6657, 0x7676, 0x4615, 0x5634,
    0xD94C, 0xC96D, 0xF90E, 0xE92F, 0x99C8, 0x89E9, 0xB98A, 0xA9AB,
    0x5844, 0x4865, 0x7806, 0x6827, 0x18C0, 0x08E1, 0x3882, 0x28A3,
    0xCB7D, 0xDB5C, 0xEB3F, 0xFB1E, 0x8BF9, 0x9BD8, 0xABBB, 0xBB9A,
    0x4A75, 0x5A54, 0x6A37, 0x7A16, 0x0AF1, 0x1AD0, 0x2AB3, 0x3A92,
    0xFD2E, 0xED0F, 0xDD6C, 0xCD4D, 0xBDAA, 0xAD8B, 0x9DE8, 0x8DC9,
    0x7C26, 0x6C07, 0x5C64, 0x4C45, 0x3CA2, 0x2C83, 0x1CE0, 0x0CC1,
    0xEF1F, 0xFF3E, 0xCF5D, 0xDF7C, 0xAF9B, 0xBFBA, 0x8FD9, 0x9FF8,
    0x6E17, 0x7E36, 0x4E55, 0x5E74, 0x2E93, 0x3EB2, 0x0ED1, 0x1EF0,
)


def crc16_fnirs(data: bytes, init: int = 0xFFFF) -> int:
    """CRC16 x^16+x^12+x^5+1, same as my-fnirs my_crc16()."""
    crc = init & 0xFFFF
    for b in data:
        crc = ((crc << 8) ^ _CRC16_TABLE[((crc >> 8) ^ b) & 0xFF]) & 0xFFFF
    return crc


def build_start(name: bytes, size: int, crc: int) -> bytes:
    if len(name) > 127:
        raise ValueError("filename too long (max 127 bytes)")
    return bytes([OP_START, len(name)]) + name + struct.pack("<II", size, crc)


def build_data(seq: int, chunk: bytes, use_frame_crc: bool = False) -> bytes:
    body = struct.pack("<IH", seq, len(chunk)) + chunk
    if use_frame_crc:
        return bytes([OP_DATA]) + body + struct.pack("<H", crc16_fnirs(body))
    return bytes([OP_DATA]) + body


def build_end() -> bytes:
    return bytes([OP_END])


def build_abort() -> bytes:
    return bytes([OP_ABORT])


def build_data_bad_crc(seq: int, chunk: bytes) -> bytes:
    """DATA with wrong trailing CRC16 (for frame-loss detection test)."""
    pkt = build_data(seq, chunk, use_frame_crc=True)
    return pkt[:-2] + b"\xde\xad"


def max_chunk_for_mtu(mtu: int, use_frame_crc: bool = False) -> int:
    """Max payload bytes per DATA packet for the given ATT MTU."""
    extra = DATA_CRC_LEN if use_frame_crc else 0
    return max(1, mtu - ATT_WRITE_OVERHEAD - DATA_HDR_LEN - extra)


def device_label(d, ad) -> str:
    names = names_for(d, ad)
    name = names[0] if names else "(no name)"
    return f"{name}  {d.address}"


def names_for(d, ad) -> list[str]:
    out: list[str] = []
    if d.name:
        out.append(d.name)
    if ad and ad.local_name and ad.local_name not in out:
        out.append(ad.local_name)
    return out


def name_matches(needle: str, d, ad) -> bool:
    n = needle.casefold()
    return any(n in x.casefold() for x in names_for(d, ad))


async def scan_devices(timeout: float) -> list:
    from bleak import BleakScanner

    print(f"Scanning {timeout:.0f}s ...")
    devices = await BleakScanner.discover(timeout=timeout, return_adv=True)
    rows = []
    for d, ad in devices.values():
        rows.append((device_label(d, ad), d.address, d, ad))
    rows.sort(key=lambda x: x[0].lower())
    return rows


async def verify_target_visible(target: str, scan_timeout: float = 3.0) -> bool:
    """Quick scan to catch wrong .ble_device MAC (multiple TestBLE in lab)."""
    from bleak import BleakScanner

    want = target.replace("-", ":").upper()
    devices = await BleakScanner.discover(timeout=scan_timeout, return_adv=True)
    for d, _ad in devices.values():
        if d.address.replace("-", ":").upper() == want:
            return True
    return False


async def resolve_target(
    address: str | None,
    name: str | None,
    scan_timeout: float,
    use_first: bool = False,
) -> str:
    if address:
        return address

    saved = load_saved_device()
    if saved:
        print(f"Using saved board MAC {saved} (from {device_file_path()})")
        return saved

    if not name:
        raise SystemExit("Use -d ADDR, bind MAC to .ble_device, or -n NAME")

    print(f"Scanning for {name!r} ({scan_timeout:.0f}s) ...")
    rows = await scan_devices(scan_timeout)
    matches = [(label, addr) for label, addr, d, ad in rows if name_matches(name, d, ad)]

    if len(matches) == 1:
        print(f"Found {matches[0][0]}")
        return matches[0][1]
    if len(matches) > 1:
        print(f"Multiple devices match {name!r}:")
        for label, addr in matches:
            print(f"  {label}")
        if use_first:
            print(f"Using --first: {matches[0][1]} ({matches[0][0]})")
            return matches[0][1]
        print("\nFix: bind your board once, then plain send works:")
        for _label, addr in matches:
            print(f"  python ble_file_tx.py bind -d {addr}")
        print("\nOr one-shot with MAC:")
        for _label, addr in matches:
            print(f"  python ble_file_tx.py send -d {addr} <file>")
        raise SystemExit("Multiple TestBLE — use bind -d MAC or send -d MAC")

    print(f"Device not found: {name!r}\n")
    print("Nearby BLE devices:")
    if not rows:
        print("  (none — check board: start-my-server, btmgmt advertising on)")
    else:
        for label, addr, _d, _ad in rows:
            print(f"  {label}")
    raise SystemExit(1)


def device_file_path() -> str:
    return os.environ.get(
        "BLE_DEVICE_FILE",
        os.path.join(os.path.dirname(os.path.abspath(__file__)), DEVICE_FILE_NAME),
    )


def load_saved_device() -> str | None:
    env = os.environ.get("BLE_DEVICE")
    if env and env.strip():
        return env.strip()
    path = device_file_path()
    if not os.path.isfile(path):
        return None
    with open(path, encoding="utf-8") as fp:
        mac = fp.read().strip()
    return mac or None


def save_device(mac: str) -> None:
    path = device_file_path()
    with open(path, "w", encoding="utf-8") as fp:
        fp.write(mac.strip() + "\n")
    print(f"Saved board MAC {mac.strip()} -> {path}")
    print("Next: python ble_file_tx.py send <file>  (no -d needed)")


def infer_address_type(addr: str) -> str:
    first = int(addr.replace("-", ":").split(":")[0], 16)
    return "random" if (first & 0xC0) == 0xC0 else "public"


def make_client(target: str, connect_timeout: float, mtu_size: int):
    from bleak import BleakClient

    addr_type = infer_address_type(target)
    print(
        f"Connecting to {target} (address_type={addr_type}, "
        f"mtu_request={mtu_size}, timeout={connect_timeout:.0f}s) ..."
    )
    try:
        return BleakClient(
            target, timeout=connect_timeout, address_type=addr_type, mtu_size=mtu_size
        )
    except TypeError:
        return BleakClient(target, timeout=connect_timeout)


async def send_file(
    address: str | None,
    name: str | None,
    path: str,
    chunk_size: int | None,
    scan_timeout: float,
    connect_timeout: float,
    mtu_request: int,
    packet_delay: float,
    fast: bool,
    reliable: bool,
    progress_interval_pkts: int,
    use_first: bool = False,
    use_frame_crc: bool = False,
    force_resync: bool = False,
    skip_prescan: bool = False,
) -> None:
    with open(path, "rb") as fp:
        payload = fp.read()

    basename = os.path.basename(path).encode("utf-8", errors="replace")
    file_crc = crc32_ieee(payload)
    file_size = len(payload)

    target = await resolve_target(address, name, scan_timeout, use_first)
    explicit_mac = bool(address) or bool(load_saved_device())

    if force_resync:
        use_resync = True
    else:
        use_resync = not (sys.platform == "win32" and WIN32_DISABLE_RESYNC)

    print(f"Sending {path}: {file_size} bytes, crc=0x{file_crc:08x}")
    print(
        "Board: start-my-server (build 20250703-rx12-fast-flow). "
        "Do not Ctrl+C my-server during transfer."
    )
    print(
        "While sending, board serial must show: Connect from <PC> + "
        "FILE RX START + FILE RX DATA: first chunk"
    )
    if use_frame_crc:
        print("Per-frame CRC16 enabled (--frame-crc). Board must be rx12-fast-flow.")
    else:
        print("Per-frame CRC16 off (default). Use --frame-crc after flashing rx12-fast-flow.")
    if not use_resync and sys.platform == "win32":
        print("Windows: no GATT resync during TX (WinRT drops link on read).")

    if not skip_prescan and not explicit_mac and not await verify_target_visible(target):
        print(
            f"Warning: {target} not seen in 3s scan — wrong .ble_device?\n"
            "  AIC8800 often does not appear in scan; use --skip-prescan with bound MAC.\n"
            "  python ble_file_tx.py scan\n"
            "  python ble_file_tx.py bind -d <your-board-MAC>"
        )

    from bleak.exc import BleakError

    last_err: Exception | None = None
    for attempt in range(MAX_TRANSFER_ATTEMPTS):
        if attempt > 0:
            wait = 2.0 * attempt
            print(
                f"Retry {attempt + 1}/{MAX_TRANSFER_ATTEMPTS} "
                f"in {wait:.0f}s (restart my-server if board shows Shutting down)..."
            )
            await asyncio.sleep(wait)
        try:
            await _transfer_once(
                target,
                connect_timeout,
                basename,
                payload,
                file_size,
                file_crc,
                chunk_size,
                mtu_request,
                packet_delay,
                fast,
                reliable,
                progress_interval_pkts,
                use_frame_crc,
                use_resync,
            )
            print("Done.")
            return
        except (TimeoutError, OSError, BleakError) as e:
            last_err = e
            print(f"Transfer failed (attempt {attempt + 1}/{MAX_TRANSFER_ATTEMPTS}): {e}")

    print(f"  Check board: ls -l {BLE_RX_DIR}/")
    raise SystemExit(str(last_err)) from last_err


def norm_uuid(u: str) -> str:
    return u.replace("-", "").lower()


def dump_gatt_table(svc_list) -> None:
    print("GATT table on connected device:")
    if not svc_list:
        print("  (empty — not my-server? run start-my-server on board)")
        return
    for svc in svc_list:
        print(f"  service {svc.uuid}")
        for char in svc.characteristics:
            props = ",".join(char.properties)
            print(f"    char {char.uuid} handle={char.handle:04X} [{props}]")


def find_transfer_char(svc_list, quiet: bool = False) -> object:
    want = norm_uuid(CHR_UUID)
    want_svc = norm_uuid(SVC_UUID)

    for svc in svc_list:
        for char in svc.characteristics:
            if norm_uuid(char.uuid) == want:
                if not quiet:
                    print(f"Found transfer char on service {svc.uuid}")
                return char

    # Service matched but char uuid string may differ on WinRT — scan custom service
    for svc in svc_list:
        if norm_uuid(svc.uuid) != want_svc:
            continue
        for char in svc.characteristics:
            props = set(char.properties)
            if "write" in props or "write-without-response" in props:
                if not quiet:
                    print(f"Using writable char {char.uuid} on custom service (uuid alias)")
                return char

    dump_gatt_table(svc_list)
    raise SystemExit(
        f"Characteristic {CHR_UUID} not found.\n"
        "Check board serial when PC connects: must show 'Connect from' and "
        "my-server build 20250703-rx12-fast-flow.\n"
        f"Forget {DEFAULT_BLE_NAME} in Windows BT, use: ble_file_tx.py scan"
    )


def parse_rx_status(data: bytes) -> tuple[int, int, int]:
    """Return (state, received, expected) from custom char read buffer."""
    if len(data) < 12:
        return 0, 0, 0
    return data[1], struct.unpack("<I", data[4:8])[0], struct.unpack("<I", data[8:12])[0]


def parse_rx_status_full(data: bytes) -> dict:
    """Parse full 20-byte board RX status block."""
    received = struct.unpack("<I", data[4:8])[0] if len(data) >= 8 else 0
    expected = struct.unpack("<I", data[8:12])[0] if len(data) >= 12 else 0
    next_seq = struct.unpack("<I", data[12:16])[0] if len(data) >= 16 else 0
    running_crc = struct.unpack("<I", data[16:20])[0] if len(data) >= 20 else 0
    return {
        "magic": data[0] if data else 0,
        "state": data[1] if len(data) > 1 else 0,
        "error": data[2] if len(data) > 2 else 0,
        "received": received,
        "expected": expected,
        "next_seq": next_seq,
        "running_crc": running_crc,
    }


def format_rx_status(data: bytes) -> str:
    info = parse_rx_status_full(data)
    state_name = RX_STATE_NAMES.get(info["state"], f"0x{info['state']:02x}")
    lines = [
        f"state:     {state_name}",
        f"received:  {info['received']} bytes",
        f"expected:  {info['expected']} bytes",
        f"next_seq:  {info['next_seq']}",
        f"running_crc: 0x{info['running_crc']:08x}",
    ]
    if info["error"]:
        err_name = RX_ERR_NAMES.get(info["error"], f"0x{info['error']:02x}")
        lines.append(f"last_error: {err_name}")
    if info["magic"] != 0xA5 and info["magic"] != 0:
        lines.append(f"magic:      0x{info['magic']:02x} (expected 0xA5)")
    return "\n".join(lines)


async def get_svc_list(client) -> list:
    if hasattr(client, "get_services"):
        return list(await client.get_services())
    return list(client.services)


async def cmd_scan(scan_timeout: float) -> None:
    rows = await scan_devices(scan_timeout)
    if not rows:
        print("(no devices — check board: start-my-server)")
    for label, _addr, _d, _ad in rows:
        print(label)


async def cmd_status(
    address: str | None,
    name: str | None,
    scan_timeout: float,
    connect_timeout: float,
    mtu_request: int,
    use_first: bool = False,
) -> None:
    from bleak.exc import BleakError

    target = await resolve_target(address, name, scan_timeout, use_first)
    try:
        async with make_client(target, connect_timeout, mtu_request) as client:
            if not client.is_connected:
                raise OSError("BLE connect failed")
            xfer_char = find_transfer_char(await get_svc_list(client), quiet=True)
            data = await client.read_gatt_char(xfer_char)
            print(format_rx_status(data))
    except (TimeoutError, OSError, BleakError) as e:
        raise SystemExit(str(e)) from e


async def cmd_abort(
    address: str | None,
    name: str | None,
    scan_timeout: float,
    connect_timeout: float,
    mtu_request: int,
    use_first: bool = False,
) -> None:
    from bleak.exc import BleakError

    target = await resolve_target(address, name, scan_timeout, use_first)
    try:
        async with make_client(target, connect_timeout, mtu_request) as client:
            if not client.is_connected:
                raise OSError("BLE connect failed")
            xfer_char = find_transfer_char(await get_svc_list(client), quiet=True)
            await write_char_retry(client, xfer_char, build_abort(), "ABORT")
            await asyncio.sleep(0.5)
            data = await client.read_gatt_char(xfer_char)
            print("ABORT sent.")
            print(format_rx_status(data))
    except (TimeoutError, OSError, BleakError) as e:
        raise SystemExit(str(e)) from e


async def cmd_list_rx(
    address: str | None,
    name: str | None,
    scan_timeout: float,
    connect_timeout: float,
    mtu_request: int,
    use_first: bool = False,
) -> None:
    from bleak.exc import BleakError

    print(f"Board stores files under: {BLE_RX_DIR}/")
    print(f"On board serial: ls -l {BLE_RX_DIR}/")
    print()
    try:
        await cmd_status(
            address, name, scan_timeout, connect_timeout, mtu_request, use_first,
        )
    except SystemExit as e:
        if str(e):
            print(f"(GATT status unavailable: {e})")


async def cmd_gatt(
    address: str | None,
    name: str | None,
    scan_timeout: float,
    connect_timeout: float,
    mtu_request: int,
    use_first: bool = False,
) -> None:
    from bleak.exc import BleakError

    target = await resolve_target(address, name, scan_timeout, use_first)
    try:
        async with make_client(target, connect_timeout, mtu_request) as client:
            print(f"ATT MTU={getattr(client, 'mtu_size', '?')}")
            dump_gatt_table(await get_svc_list(client))
    except (TimeoutError, OSError, BleakError) as e:
        raise SystemExit(str(e)) from e


async def poll_rx_status(client, char) -> tuple[int, int, int] | None:
    """Read RX counters from custom char (no extra delay)."""
    from bleak.exc import BleakError

    try:
        data = await client.read_gatt_char(char)
        return parse_rx_status(data)
    except (BleakError, OSError, TimeoutError):
        return None


async def wait_board_done(
    client,
    char,
    file_size: int,
    timeout: float = 30.0,
) -> bool:
    """Poll until board reports FILE_RX_STATE_DONE. Returns False if link drops."""
    from bleak.exc import BleakError

    deadline = time.perf_counter() + timeout

    while time.perf_counter() < deadline:
        if not client.is_connected:
            return False

        status = await poll_rx_status(client, char)
        if not status:
            await asyncio.sleep(0.15)
            continue

        state, received, expected = status
        if state == RX_STATE_DONE and received == file_size:
            print(f"Board confirmed DONE ({received} bytes)")
            return True
        if state == RX_STATE_ERROR:
            raise BleakError(f"Board RX error after END: received={received}")

        await asyncio.sleep(0.15)

    return False


async def write_char(
    client, char, data: bytes, label: str, quiet: bool = False, reliable: bool = False
) -> None:
    from bleak.exc import BleakError

    if not quiet:
        print(f"  write {label}: {len(data)} bytes")

    # WinRT: is_connected can be False while link still works — always try write
    props = set(char.properties)

    # WinRT: never use write-with-response on this char (aborts after 1-2 packets)
    if reliable and sys.platform != "win32" and "write" in props:
        await client.write_gatt_char(char, data, response=True)
        return

    if "write-without-response" in props:
        await client.write_gatt_char(char, data, response=False)
        return

    if "write" in props:
        await client.write_gatt_char(char, data, response=True)
        return

    raise SystemExit(f"Characteristic {char.uuid} has no write property")


async def write_char_retry(
    client, char, data: bytes, label: str, quiet: bool = False, reliable: bool = False,
    retries: int = 5,
) -> None:
    from bleak.exc import BleakError

    for attempt in range(retries):
        try:
            await write_char(client, char, data, label, quiet=quiet, reliable=reliable)
            return
        except (BleakError, OSError) as e:
            if attempt + 1 >= retries:
                if not client.is_connected:
                    raise BleakError(
                        f"{e} — BLE link down. Check board serial: "
                        "FILE RX CRC/SEQ, my-server build rx12-fast-flow, only one PC connected"
                    ) from e
                raise
            wait = 0.35 * (attempt + 1)
            if not quiet:
                print(f"  {label} retry {attempt + 1}/{retries} after {wait:.1f}s ({e})")
            await asyncio.sleep(wait)


def chunk_at_seq(payload: bytes, seq: int, effective_chunk: int, file_size: int) -> bytes:
    off = seq * effective_chunk
    if off >= file_size:
        return b""
    return payload[off : off + effective_chunk]


async def resend_seq_range(
    client,
    char,
    payload: bytes,
    file_size: int,
    effective_chunk: int,
    start_seq: int,
    end_seq: int,
    packet_delay: float,
    reliable: bool,
    use_frame_crc: bool,
) -> None:
    """Resend DATA packets [start_seq, end_seq)."""
    for s in range(start_seq, end_seq):
        part = chunk_at_seq(payload, s, effective_chunk, file_size)
        if not part:
            break
        await write_char_retry(
            client, char, build_data(s, part, use_frame_crc), f"RESEND {s}",
            quiet=True, reliable=reliable,
        )
        if packet_delay > 0:
            await asyncio.sleep(packet_delay)


async def resync_from_board(
    client,
    char,
    payload: bytes,
    file_size: int,
    effective_chunk: int,
    sent_seq: int,
    packet_delay: float,
    reliable: bool,
    use_frame_crc: bool,
    enabled: bool = True,
) -> None:
    if not enabled:
        return

    if sent_seq < RESYNC_INTERVAL_PKTS:
        return

    if not client.is_connected:
        return

    try:
        raw = await client.read_gatt_char(char)
    except (BleakError, OSError):
        return

    info = parse_rx_status_full(raw)
    board_next = info["next_seq"]
    total_pkts = (file_size + effective_chunk - 1) // effective_chunk

    if info["state"] == RX_STATE_ERROR and info["error"]:
        err = RX_ERR_NAMES.get(info["error"], f"0x{info['error']:02x}")
        print(f"  board error: {err}, next_seq={board_next}")

    if board_next >= sent_seq:
        return

    end = min(sent_seq, total_pkts)
    print(f"  resync: board next_seq={board_next}, resending {board_next}..{end - 1}")
    await resend_seq_range(
        client, char, payload, file_size, effective_chunk,
        board_next, end, packet_delay, reliable, use_frame_crc,
    )
    await asyncio.sleep(0.1)


async def _transfer_once(
    target: str,
    connect_timeout: float,
    basename: bytes,
    payload: bytes,
    file_size: int,
    file_crc: int,
    chunk_size: int | None,
    mtu_request: int,
    packet_delay: float,
    fast: bool,
    reliable: bool,
    progress_interval_pkts: int,
    use_frame_crc: bool = False,
    use_resync: bool = True,
    on_log: Callable[[str], None] | None = None,
    on_progress: Callable[[int, int, float], None] | None = None,
) -> None:
    log = on_log or (lambda msg: print(msg, flush=True))
    disconnect_note = {"fired": False}

    def on_disconnect(_client) -> None:
        disconnect_note["fired"] = True
        log("BLE link dropped (disconnect callback)")

    status_watcher: _StatusWatcher | None = None

    async with make_client(target, connect_timeout, mtu_request) as client:
        if hasattr(client, "set_disconnected_callback"):
            client.set_disconnected_callback(on_disconnect)

        if not client.is_connected:
            raise OSError("BLE connect failed")

        mtu = int(getattr(client, "mtu_size", None) or DEFAULT_MTU_REQUEST)
        mtu_chunk = max_chunk_for_mtu(mtu, use_frame_crc)

        if hasattr(client, "get_services"):
            svcs = await client.get_services()
            svc_list = list(svcs)
        else:
            svc_list = list(client.services)

        xfer_char = find_transfer_char(svc_list)
        props = set(getattr(xfer_char, "properties", []) or [])

        # Bleak exposes the real write-without-response payload limit on some backends.
        # It is more accurate than client.mtu_size on Windows/WinRT.
        max_wwr = int(getattr(xfer_char, "max_write_without_response_size", 0) or 0)
        if max_wwr >= MIN_TRUSTED_WWR_PAYLOAD:
            extra = DATA_CRC_LEN if use_frame_crc else 0
            mtu_chunk = max(1, max_wwr - DATA_HDR_LEN - extra)

        if chunk_size is not None:
            effective_chunk = chunk_size
        elif fast:
            effective_chunk = min(mtu_chunk, FAST_CHUNK_DEFAULT)
        elif sys.platform == "win32":
            effective_chunk = min(mtu_chunk, SAFE_CHUNK_CAP)
        else:
            effective_chunk = mtu_chunk

        fc_window, fc_wait_step = get_fc_params(fast)
        fc_window_req = fc_window
        fc_window = clamp_fc_window(fc_window, effective_chunk)

        # Try Notify-based ACK/status flow control.  This replaces the old fixed
        # 18ms delay and prevents write-without-response from overrunning the board.
        flow_control = False
        if DISABLE_NOTIFY_FC:
            log("  Notify flow-control disabled by BLE_DISABLE_NOTIFY_FC=1; using fixed pacing")
        elif "notify" in props:
            try:
                status_watcher = await _subscribe_board_status(
                    client, xfer_char, log=log, settle=0.35
                )
                flow_control = True
            except Exception as e:  # keep transfer usable on old firmware
                log(f"Status Notify subscribe failed; falling back to pacing ({e})")

        if not flow_control and packet_delay <= 0 and not fast:
            packet_delay = LEGACY_FALLBACK_TX_DELAY

        est_packets = (file_size + effective_chunk - 1) // effective_chunk + 2
        mode = "write-without-response"
        if reliable and sys.platform != "win32":
            mode = "write-with-response"
        pace = (
            f"Notify window={fc_window} step={fc_wait_step}"
            if flow_control
            else (f"{packet_delay}s/pkt" if packet_delay > 0 else "no delay (--fast)")
        )
        crc_note = "CRC16/frame" if use_frame_crc else "no frame CRC"
        resync_note = "Notify ACK/status" if flow_control else (
            f"resync every {RESYNC_INTERVAL_PKTS} pkts" if use_resync else "no resync"
        )
        log(
            f"Connected. ATT MTU={mtu}, char_wwr={max_wwr or '?'}, "
            f"chunk={effective_chunk}, ~{est_packets} packets, mode={mode}, "
            f"pace={pace}, {crc_note}, {resync_note}"
        )
        if flow_control and fc_window_req != fc_window:
            log(
                f"  FC window capped {fc_window_req} -> {fc_window} pkts "
                f"(<= {BOARD_RX_BUF_BYTES // 1024}KB board buffer / chunk {effective_chunk})"
            )

        t0 = time.perf_counter()
        tx_write_time = 0.0
        tx_write_count = 0
        flow_wait_time = 0.0
        flow_wait_count = 0
        await write_char_retry(
            client, xfer_char, build_start(basename, file_size, file_crc), "START",
        )
        startup_delay = startup_delay_for(fast)
        if startup_delay > 0:
            await asyncio.sleep(startup_delay)

        if flow_control and status_watcher:
            try:
                info = await _wait_for_first_status(status_watcher)
                log(
                    f"  board status OK: state={RX_STATE_NAMES.get(info['state'], info['state'])}, "
                    f"next_seq={info['next_seq']}"
                )
            except TimeoutError as e:
                log(f"  {e}")
                log("  Falling back to fixed pacing (no Notify flow-control).")
                flow_control = False
                try:
                    await client.stop_notify(xfer_char)
                except Exception:
                    pass
                status_watcher = None
                if packet_delay <= 0 and not fast:
                    packet_delay = LEGACY_FALLBACK_TX_DELAY
                elif packet_delay <= 0 and fast:
                    packet_delay = DEFAULT_TX_DELAY

        seq = 0
        next_report = 0
        report_step = max(
            effective_chunk * max(progress_interval_pkts, 1), 32768
        )
        total_pkts = (file_size + effective_chunk - 1) // effective_chunk

        for off in range(0, file_size, effective_chunk):
            part = payload[off : off + effective_chunk]
            w0 = time.perf_counter()
            await write_char_retry(
                client, xfer_char, build_data(seq, part, use_frame_crc), f"DATA {seq}",
                quiet=True, reliable=reliable,
            )
            tx_write_time += time.perf_counter() - w0
            tx_write_count += 1
            seq += 1
            bytes_sent = min(off + effective_chunk, file_size)

            if flow_control and status_watcher:
                if should_fc_wait(seq, fc_window, fc_wait_step):
                    fw0 = time.perf_counter()
                    await _wait_for_board_next_seq(
                        status_watcher,
                        seq - fc_window,
                        timeout=FLOW_CONTROL_TIMEOUT,
                        label="flow window",
                    )
                    flow_wait_time += time.perf_counter() - fw0
                    flow_wait_count += 1
                if packet_delay > 0:
                    await asyncio.sleep(packet_delay)
            elif seq <= WARMUP_PACKETS:
                await asyncio.sleep(max(packet_delay, WARMUP_DELAY))
            elif packet_delay > 0:
                await asyncio.sleep(packet_delay)

            if (not flow_control) and use_resync and seq % RESYNC_INTERVAL_PKTS == 0:
                await resync_from_board(
                    client, xfer_char, payload, file_size, effective_chunk,
                    seq, packet_delay, reliable, use_frame_crc, enabled=True,
                )

            if bytes_sent >= next_report or bytes_sent >= file_size:
                elapsed = time.perf_counter() - t0
                rate = bytes_sent / elapsed if elapsed > 0 else 0.0
                log(f"  sent: {bytes_sent}/{file_size} ({rate / 1024:.1f} KB/s)")
                if on_progress:
                    on_progress(bytes_sent, file_size, rate)
                next_report = bytes_sent + report_step

        if flow_control and status_watcher:
            log("Waiting for board to catch up before END...")
            fw0 = time.perf_counter()
            await _wait_for_board_next_seq(
                status_watcher,
                total_pkts,
                timeout=max(10.0, file_size / 8192.0),
                label="final drain",
            )
            flow_wait_time += time.perf_counter() - fw0
            flow_wait_count += 1
        elif use_resync:
            log("Final resync before END...")
            await resync_from_board(
                client, xfer_char, payload, file_size, effective_chunk,
                total_pkts, packet_delay, reliable, use_frame_crc, enabled=True,
            )
        else:
            # Legacy firmware fallback only.  The new rx12 firmware should use Notify flow.
            hold_sec = min(2.0, max(0.2, file_size / 65536.0))
            log(f"Legacy fallback hold {hold_sec:.1f}s before END...")
            await asyncio.sleep(hold_sec)

        await write_char_retry(client, xfer_char, build_end(), "END")
        log("END sent")

        if flow_control and status_watcher:
            await _wait_for_board_done_notify(status_watcher, file_size, timeout=10.0)
            log("Board confirmed transfer complete.")
        elif use_resync:
            await asyncio.sleep(0.3)
            if await wait_board_done(client, xfer_char, file_size):
                log("Board confirmed transfer complete.")
            else:
                log(
                    "Board DONE not confirmed via GATT read.\n"
                    f"Check board serial for FILE RX DONE and: ls -l {BLE_RX_DIR}/"
                )
        else:
            log(
                f"Skipped GATT status read. Verify on board: ls -l {BLE_RX_DIR}/"
            )

        if status_watcher:
            try:
                await client.stop_notify(xfer_char)
            except Exception:
                pass

        elapsed = time.perf_counter() - t0
        rate = file_size / elapsed if elapsed > 0 else 0.0
        if PROFILE_TX and tx_write_count:
            avg_write_ms = tx_write_time / tx_write_count * 1000.0
            avg_wait_ms = (flow_wait_time / flow_wait_count * 1000.0) if flow_wait_count else 0.0
            log(
                "PROFILE: "
                f"DATA writes={tx_write_count}, avg_write={avg_write_ms:.1f}ms, "
                f"total_write={tx_write_time:.1f}s, "
                f"flow_waits={flow_wait_count}, avg_flow_wait={avg_wait_ms:.1f}ms, "
                f"total_flow_wait={flow_wait_time:.1f}s"
            )
            if tx_write_time > elapsed * 0.65:
                log("PROFILE verdict: bottleneck is mostly PC/Windows BLE write pacing or BLE connection interval.")
            elif flow_wait_time > elapsed * 0.65:
                log("PROFILE verdict: bottleneck is mostly board catch-up / flow-control waiting.")
            else:
                log("PROFILE verdict: time is split; try larger chunk first, then inspect board/connection params.")
        if on_progress:
            on_progress(file_size, file_size, rate)
        log(
            f"Finished in {elapsed:.1f}s ({rate / 1024:.1f} KB/s). "
            f"Check: ls -l {BLE_RX_DIR}/"
        )


async def transfer_file(
    path: str,
    *,
    address: str | None = None,
    name: str | None = DEFAULT_BLE_NAME,
    use_frame_crc: bool = True,
    safe: bool = False,
    fast: bool = False,
    scan_timeout: float = 20.0,
    connect_timeout: float = 90.0,
    mtu_request: int = DEFAULT_MTU_REQUEST,
    use_first: bool = False,
    on_log: Callable[[str], None] | None = None,
    on_progress: Callable[[int, int, float], None] | None = None,
) -> None:
    """High-level file transfer API for CLI and GUI."""
    log = on_log or (lambda msg: print(msg, flush=True))

    with open(path, "rb") as fp:
        payload = fp.read()

    basename = os.path.basename(path).encode("utf-8", errors="replace")
    file_crc = crc32_ieee(payload)
    file_size = len(payload)
    target = await resolve_target(address, name, scan_timeout, use_first)

    chunk_size: int | None = None
    packet_delay = 0.0
    reliable = False
    progress_interval_pkts = PROGRESS_INTERVAL_PKTS

    if safe:
        chunk_size = SAFE_CHUNK
        packet_delay = SAFE_DELAY
    elif fast:
        chunk_size = FAST_CHUNK_DEFAULT
        packet_delay = 0.0
    elif not fast:
        packet_delay = DEFAULT_TX_DELAY

    if sys.platform == "win32":
        reliable = False

    use_resync = not (sys.platform == "win32" and WIN32_DISABLE_RESYNC)

    log(f"Sending {path}: {file_size} bytes, crc=0x{file_crc:08x}")
    log(f"Target: {target}")
    if use_frame_crc:
        log("Per-frame CRC16 enabled.")
    if fast:
        fc_w, fc_s = get_fc_params(True)
        log(f"FAST: chunk={chunk_size}, Notify window={fc_w}, fc_step={fc_s}")
    if not use_resync and sys.platform == "win32":
        log("Windows: no GATT resync during TX (WinRT-safe).")

    from bleak.exc import BleakError

    last_err: Exception | None = None
    for attempt in range(MAX_TRANSFER_ATTEMPTS):
        if attempt > 0:
            wait = 2.0 * attempt
            log(f"Retry {attempt + 1}/{MAX_TRANSFER_ATTEMPTS} in {wait:.0f}s...")
            await asyncio.sleep(wait)
        try:
            await _transfer_once(
                target,
                connect_timeout,
                basename,
                payload,
                file_size,
                file_crc,
                chunk_size,
                mtu_request,
                packet_delay,
                fast,
                reliable,
                progress_interval_pkts,
                use_frame_crc,
                use_resync,
                on_log=log,
                on_progress=on_progress,
            )
            return
        except (BleakError, OSError, TimeoutError) as e:
            last_err = e
            log(f"Transfer failed (attempt {attempt + 1}/{MAX_TRANSFER_ATTEMPTS}): {e}")

    raise RuntimeError(str(last_err)) from last_err


def add_target_args(parser: argparse.ArgumentParser) -> None:
    target = parser.add_mutually_exclusive_group()
    target.add_argument("-d", "--device", help="BLE MAC address")
    target.add_argument(
        "-n",
        "--name",
        default=DEFAULT_BLE_NAME,
        help=f"Name substring (default: {DEFAULT_BLE_NAME})",
    )
    parser.add_argument("-t", "--scan-timeout", type=float, default=20.0)
    parser.add_argument("--connect-timeout", type=float, default=90.0)
    parser.add_argument(
        "--mtu",
        type=int,
        default=DEFAULT_MTU_REQUEST,
        help=f"Request ATT MTU (default {DEFAULT_MTU_REQUEST})",
    )
    parser.add_argument(
        "--first",
        action="store_true",
        help="If several devices match -n, connect to the first (lab only)",
    )
    parser.add_argument(
        "--skip-prescan",
        action="store_true",
        help="Connect by MAC without 3s visibility scan (required on AIC8800)",
    )


def target_kwargs(args) -> dict:
    return {
        "address": args.device,
        "name": args.name,
        "scan_timeout": args.scan_timeout,
        "connect_timeout": args.connect_timeout,
        "mtu_request": args.mtu,
        "use_first": getattr(args, "first", False),
        "skip_prescan": getattr(args, "skip_prescan", False),
    }


CCCD_UUID = "00002902-0000-1000-8000-00805f9b34fb"
STATUS_MAGIC = 0xA5
TEST_LOSS_SCRIPT_REV = "20250703-a5-filter"


class _StatusWatcher:
    """Collect latest 20-byte board status from Notify/Indicate."""

    def __init__(self) -> None:
        self.latest: bytes | None = None
        self._loop = asyncio.get_running_loop()
        self.event = asyncio.Event()

    def __call__(self, _sender, data: bytearray) -> None:
        raw = bytes(data)
        if len(raw) >= 20 and raw[0] == STATUS_MAGIC:
            self.latest = raw
            self._loop.call_soon_threadsafe(self.event.set)

    def clear(self) -> None:
        self.latest = None
        self.event.clear()


async def _subscribe_board_status(
    client, char, *, log: Callable[[str], None] | None = None, settle: float = 1.2
) -> _StatusWatcher:
    watcher = _StatusWatcher()
    await client.start_notify(char, watcher)
    (log or print)("  subscribed board status (Notify, magic=0xA5 only)")
    if settle > 0:
        await asyncio.sleep(settle)
    watcher.clear()
    return watcher


async def _wait_for_rx_error(
    watcher: _StatusWatcher, expect_err: int, timeout: float = 15.0
) -> bytes:
    deadline = time.perf_counter() + timeout
    while time.perf_counter() < deadline:
        if watcher.latest:
            info = parse_rx_status_full(watcher.latest)
            if info["state"] == RX_STATE_ERROR:
                return watcher.latest
        await asyncio.sleep(0.25)
    raise SystemExit(
        f"No valid ERROR status (magic 0x{STATUS_MAGIC:02X}) within {timeout:.0f}s.\n"
        "Check board serial for FILE RX SEQ/FRAME CRC mismatch.\n"
        "If serial shows the error, board detection passed; update my-server to rx10 "
        "for immediate Indicate push."
    )


async def _wait_for_first_status(
    watcher: _StatusWatcher,
    *,
    timeout: float = FIRST_STATUS_TIMEOUT,
) -> dict:
    """Wait for first valid 0xA5 status block (after START + CCCD enable)."""
    deadline = time.perf_counter() + timeout
    while time.perf_counter() < deadline:
        raw = watcher.latest
        if raw:
            info = parse_rx_status_full(raw)
            if info["magic"] == STATUS_MAGIC:
                return info
        remaining = deadline - time.perf_counter()
        if remaining <= 0:
            break
        watcher.event.clear()
        try:
            poll = min(FLOW_CONTROL_POLL_SEC, remaining)
            await asyncio.wait_for(watcher.event.wait(), timeout=max(0.01, poll))
        except asyncio.TimeoutError:
            pass
    raise TimeoutError(
        f"No board status Notify (magic 0x{STATUS_MAGIC:02X}) within {timeout:.0f}s after START. "
        "Check board serial: FILE RX START + Custom Notify/Indicate: Enabled. "
        "Rebuild my-server v3-notify8 and restart with Wi-Fi down."
    )


async def _wait_for_board_next_seq(
    watcher: _StatusWatcher,
    min_next_seq: int,
    *,
    timeout: float,
    label: str,
) -> dict:
    """Wait until board status Notify reports next_seq >= min_next_seq."""
    from bleak.exc import BleakError

    if min_next_seq <= 0 and not watcher.latest:
        await _wait_for_first_status(watcher, timeout=min(timeout, FIRST_STATUS_TIMEOUT))

    if min_next_seq <= 0:
        raw = watcher.latest
        if raw:
            return parse_rx_status_full(raw)
        raise TimeoutError(f"Timed out waiting for board {label} (no status)")

    deadline = time.perf_counter() + timeout
    last_info: dict | None = None

    while time.perf_counter() < deadline:
        raw = watcher.latest
        if raw:
            info = parse_rx_status_full(raw)
            last_info = info
            if info["state"] == RX_STATE_ERROR:
                err = RX_ERR_NAMES.get(info["error"], f"0x{info['error']:02x}")
                raise BleakError(
                    f"Board RX error during {label}: {err}, next_seq={info['next_seq']}, "
                    f"received={info['received']}/{info['expected']}"
                )
            if info["next_seq"] >= min_next_seq:
                return info

        remaining = deadline - time.perf_counter()
        if remaining <= 0:
            break
        watcher.event.clear()
        try:
            poll = min(FLOW_CONTROL_POLL_SEC, remaining)
            await asyncio.wait_for(watcher.event.wait(), timeout=max(0.01, poll))
        except asyncio.TimeoutError:
            pass

    detail = "no status"
    if last_info:
        detail = (
            f"last next_seq={last_info['next_seq']}, "
            f"received={last_info['received']}/{last_info['expected']}, "
            f"state={RX_STATE_NAMES.get(last_info['state'], last_info['state'])}"
        )
    raise TimeoutError(f"Timed out waiting for board {label} >= seq {min_next_seq} ({detail})")


async def _wait_for_board_done_notify(
    watcher: _StatusWatcher,
    file_size: int,
    *,
    timeout: float,
) -> dict:
    from bleak.exc import BleakError

    deadline = time.perf_counter() + timeout
    last_info: dict | None = None

    while time.perf_counter() < deadline:
        raw = watcher.latest
        if raw:
            info = parse_rx_status_full(raw)
            last_info = info
            if info["state"] == RX_STATE_ERROR:
                err = RX_ERR_NAMES.get(info["error"], f"0x{info['error']:02x}")
                raise BleakError(f"Board RX error after END: {err}")
            if info["state"] == RX_STATE_DONE and info["received"] == file_size:
                return info

        remaining = deadline - time.perf_counter()
        if remaining <= 0:
            break
        watcher.event.clear()
        try:
            poll = min(FLOW_CONTROL_POLL_SEC, remaining)
            await asyncio.wait_for(watcher.event.wait(), timeout=max(0.01, poll))
        except asyncio.TimeoutError:
            pass

    detail = "no status"
    if last_info:
        detail = (
            f"state={RX_STATE_NAMES.get(last_info['state'], last_info['state'])}, "
            f"received={last_info['received']}/{last_info['expected']}, "
            f"next_seq={last_info['next_seq']}"
        )
    raise TimeoutError(f"Timed out waiting for board DONE ({detail})")


async def read_gatt_char_retry(client, char, *, label: str = "status") -> bytes:
    """Read custom char; WinRT often aborts if read immediately after paced writes."""
    from bleak.exc import BleakError

    initial = 1.5 if sys.platform == "win32" else 0.35
    retries = 10 if sys.platform == "win32" else 5
    last_err: Exception | None = None

    await asyncio.sleep(initial)
    for attempt in range(retries):
        try:
            return await client.read_gatt_char(char)
        except (BleakError, OSError) as e:
            last_err = e
            wait = 0.4 * (attempt + 1)
            print(f"  read {label} retry {attempt + 1}/{retries} after {wait:.1f}s ({e})")
            await asyncio.sleep(wait)

    raise SystemExit(
        f"GATT read failed after test ({last_err}).\n"
        "On Windows this can happen even when the board caught the error.\n"
        "Check board serial for FILE RX SEQ/FRAME CRC mismatch, then run:\n"
        "  python ble_file_tx.py status"
    ) from last_err


async def cmd_test_loss(
    address: str | None,
    name: str | None,
    scan_timeout: float,
    connect_timeout: float,
    mtu_request: int,
    mode: str,
    use_first: bool = False,
) -> None:
    """Send a short transfer with injected seq/CRC fault; board must report error."""
    from bleak.exc import BleakError

    target = await resolve_target(address, name, scan_timeout, use_first)
    payload = bytes(range(256))
    file_size = len(payload)
    file_crc = crc32_ieee(payload)
    chunk_size = 64

    print(f"test-loss mode={mode}: {file_size} bytes, chunk={chunk_size}, frame CRC on")
    print(f"script rev: {TEST_LOSS_SCRIPT_REV} (uses Notify status, not GATT read on Windows)")
    print("Board serial should show FILE RX SEQ mismatch or FRAME CRC mismatch.")

    async with make_client(target, connect_timeout, mtu_request) as client:
        if not client.is_connected:
            raise BleakError("Not connected")

        await asyncio.sleep(STARTUP_EXTRA_DELAY)
        svc_list = await get_svc_list(client)
        char = find_transfer_char(svc_list)
        watcher = await _subscribe_board_status(client, char)

        await write_char_retry(client, char, build_abort(), "ABORT (reset)", quiet=True)
        await asyncio.sleep(0.2)

        await write_char_retry(
            client, char,
            build_start(b"loss_test.bin", file_size, file_crc),
            "START",
        )
        watcher.clear()

        for seq in range(4):
            part = chunk_at_seq(payload, seq, chunk_size, file_size)
            if mode == "seq" and seq == 2:
                print("  inject: skip seq=2 (next write uses seq=3)")
                continue
            if mode == "crc" and seq == 2:
                pkt = build_data_bad_crc(seq, part)
                label = f"DATA {seq} (bad CRC)"
            else:
                pkt = build_data(seq, part, use_frame_crc=True)
                label = f"DATA {seq}"
            await write_char_retry(client, char, pkt, label, quiet=False)
            if mode == "crc" and seq == 2:
                print("  inject: bad CRC on seq=2, stop TX")
                break
            await asyncio.sleep(0.05)

        expect_err = RX_ERR_SEQ if mode == "seq" else RX_ERR_FRAME_CRC
        print(f"  waiting for board ERROR ({RX_ERR_NAMES[expect_err]}) via Indicate...")
        raw = await _wait_for_rx_error(watcher, expect_err)

        if parse_rx_status_full(raw)["state"] != RX_STATE_ERROR and sys.platform != "win32":
            raw = await read_gatt_char_retry(client, char, label="RX status")

        info = parse_rx_status_full(raw)
        print(format_rx_status(raw))

        if info["state"] != RX_STATE_ERROR or info["error"] != expect_err:
            raise SystemExit(
                f"FAIL: expected state=ERROR err={RX_ERR_NAMES[expect_err]}, "
                f"got state={RX_STATE_NAMES.get(info['state'])} "
                f"err={RX_ERR_NAMES.get(info['error'], info['error'])}"
            )

        print(f"PASS: board detected {RX_ERR_NAMES[expect_err]} as expected.")
        try:
            await client.stop_notify(char)
        except (OSError, BleakError):
            pass
        await write_char_retry(client, char, build_abort(), "ABORT (cleanup)", quiet=True)


def normalize_argv() -> None:
    """Legacy: ble_file_tx.py [-n NAME] FILE  ->  ble_file_tx.py send ..."""
    subcmds = frozenset(
        {
            "scan", "send", "status", "abort", "list-rx", "gatt", "bind",
            "test-loss", "help", "-h", "--help",
        }
    )
    if len(sys.argv) >= 2 and sys.argv[1] not in subcmds:
        sys.argv.insert(1, "send")


def configure_send_options(args) -> None:
    print(f"ble_file_tx.py rev {SCRIPT_REV}")
    if args.chunk_size is not None and (args.chunk_size < 1 or args.chunk_size > 512):
        raise SystemExit("chunk-size must be 1..512")
    if not os.path.isfile(args.file):
        raise SystemExit(f"Not a file: {args.file}")

    file_size = os.path.getsize(args.file)
    args._chunk_size = args.chunk_size
    args._reliable = args.reliable
    args._packet_delay = args.delay
    args._progress_interval_pkts = PROGRESS_INTERVAL_PKTS

    if args.safe:
        if args._chunk_size is None:
            args._chunk_size = SAFE_CHUNK
        args._reliable = False
        if args.delay == 0.0:
            args._packet_delay = SAFE_DELAY
        print(
            f"SAFE mode: chunk={args._chunk_size}, write-without-response, "
            f"delay={args._packet_delay}s"
        )
    elif args.fast:
        args._reliable = False
        if args.delay == 0.0:
            args._packet_delay = 0.0
        if args._chunk_size is None:
            args._chunk_size = FAST_CHUNK_DEFAULT
        fc_w, fc_s = get_fc_params(True)
        print(
            f"FAST mode: chunk={args._chunk_size}, delay={args._packet_delay}s, "
            f"Notify window={fc_w}, fc_step={fc_s} "
            f"(target ~30KB/s; tune via BLE_FC_WINDOW_PKTS / BLE_FC_WAIT_STEP_PKTS)"
        )
    elif not args.fast:
        args._reliable = False
        if args.delay == 0.0:
            args._packet_delay = DEFAULT_TX_DELAY

    if sys.platform == "win32" and args._reliable:
        print("Windows: ignoring --reliable (write-with-response aborts WinRT link)")
        args._reliable = False

    if file_size > LARGE_FILE_THRESHOLD and not args.fast and not args.safe:
        if args._chunk_size is None:
            args._chunk_size = LARGE_FILE_CHUNK
        est_packets = (file_size + args._chunk_size - 1) // args._chunk_size
        est_sec = est_packets * args._packet_delay
        if args._packet_delay > 0:
            print(
                f"Large file: chunk={args._chunk_size}, delay={args._packet_delay}s, "
                f"~{est_sec:.0f}s paced-TX floor before BLE/board overhead"
            )
        else:
            print(
                f"Large file: chunk={args._chunk_size}, Notify flow control enabled; "
                "actual speed depends on BLE controller/connection interval"
            )


def main() -> None:
    normalize_argv()

    parser = argparse.ArgumentParser(
        description="BLE file transfer CLI for my-server (TestBLE)",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=f"""
Examples:
  %(prog)s scan
  %(prog)s bind -d AA:BB:CC:DD:EE:FF
  %(prog)s send 111.pdf
  %(prog)s send --fast 111.pdf
  %(prog)s send --fast -c 235 --delay 0 111.pdf
  %(prog)s status
  %(prog)s abort
  %(prog)s list-rx
  %(prog)s test-loss --mode seq
  %(prog)s test-loss --mode crc
""",
    )
    sub = parser.add_subparsers(dest="command", required=True)

    p_scan = sub.add_parser("scan", help="List nearby BLE devices")
    p_scan.add_argument("-t", "--scan-timeout", type=float, default=20.0)

    p_send = sub.add_parser("send", help="Send a file to the board")
    add_target_args(p_send)
    p_send.add_argument("file", help="Local file to send")
    p_send.add_argument("-c", "--chunk-size", type=int, default=None)
    p_send.add_argument("--fast", action="store_true", help="Max speed: chunk=235, window=256, fc_step=16")
    p_send.add_argument("--safe", action="store_true", help="Slow stable mode")
    p_send.add_argument("--reliable", action="store_true", help="Write-with-response (Linux only)")
    p_send.add_argument("--delay", type=float, default=0.0, help="Seconds between DATA packets")
    crc_group = p_send.add_mutually_exclusive_group()
    crc_group.add_argument(
        "--frame-crc",
        dest="frame_crc",
        action="store_true",
        default=True,
        help="CRC16 on each DATA frame (default; recommended for fast transfer)",
    )
    crc_group.add_argument(
        "--no-frame-crc",
        dest="frame_crc",
        action="store_false",
        help="Disable per-frame CRC16 for old firmware",
    )
    p_send.add_argument(
        "--resync",
        action="store_true",
        help="GATT read resync during TX (Linux; on Windows usually drops link)",
    )

    p_status = sub.add_parser("status", help="Read board FILE RX status via GATT")
    add_target_args(p_status)

    p_abort = sub.add_parser("abort", help="Abort current board RX session")
    add_target_args(p_abort)

    p_list = sub.add_parser(
        "list-rx",
        help="Show RX status + board shell command to list received files",
    )
    add_target_args(p_list)

    p_gatt = sub.add_parser("gatt", help="Print GATT table (debug)")
    add_target_args(p_gatt)

    p_bind = sub.add_parser(
        "bind",
        help="Save board MAC to .ble_device (fixes multiple TestBLE)",
    )
    p_bind.add_argument("-d", "--device", required=True, help="Your board MAC address")

    p_test_loss = sub.add_parser(
        "test-loss",
        help="Inject seq skip or bad frame CRC; verify board detects loss",
    )
    add_target_args(p_test_loss)
    p_test_loss.add_argument(
        "--mode",
        choices=("seq", "crc"),
        default="seq",
        help="seq=skip packet index 2; crc=bad CRC on packet 2",
    )

    try:
        args = parser.parse_args()

        if args.command == "scan":
            asyncio.run(cmd_scan(args.scan_timeout))
            return

        if args.command == "bind":
            save_device(args.device)
            return

        if args.command == "send":
            configure_send_options(args)
            tk = target_kwargs(args)
            asyncio.run(
                send_file(
                    tk["address"],
                    tk["name"],
                    args.file,
                    args._chunk_size,
                    tk["scan_timeout"],
                    tk["connect_timeout"],
                    tk["mtu_request"],
                    args._packet_delay,
                    args.fast,
                    args._reliable,
                    args._progress_interval_pkts,
                    tk["use_first"],
                    args.frame_crc,
                    force_resync=args.resync,
                    skip_prescan=tk["skip_prescan"],
                )
            )
            return

        if args.command == "status":
            asyncio.run(cmd_status(**target_kwargs(args)))
            return

        if args.command == "abort":
            asyncio.run(cmd_abort(**target_kwargs(args)))
            return

        if args.command == "list-rx":
            asyncio.run(cmd_list_rx(**target_kwargs(args)))
            return

        if args.command == "gatt":
            asyncio.run(cmd_gatt(**target_kwargs(args)))
            return

        if args.command == "test-loss":
            asyncio.run(
                cmd_test_loss(
                    **target_kwargs(args),
                    mode=args.mode,
                )
            )
            return

    except ImportError as e:
        raise SystemExit(
            f"Missing dependency ({e}). Install with the SAME Python you use to run this script:\n"
            f"  {sys.executable} -m pip install bleak\n"
            "On Windows, if 'pip install bleak' worked but 'python3' fails, use:\n"
            "  python ble_file_tx.py test-loss --mode seq"
        ) from None


if __name__ == "__main__":
    main()
