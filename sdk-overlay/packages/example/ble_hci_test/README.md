# TestBLE GATT and client tools

This directory contains the protocol-facing part of TestBLE:

| Path | Purpose |
| --- | --- |
| `mybtgatt-server.c` | BlueZ-based GATT server, embedded in `my-fnirs` for production |
| `ble_file_tx.py` | PC command-line upload, download, status, and loss tests |
| `ble_file_tx_gui.py` | Minimal PC GUI |
| `scan_ble.py` | BLE discovery helper |
| `BLE文件传输参数说明.md` | Current packet formats and flow-control parameters |
| `mobile_app/` | Flutter scan, control, waveform, and file-sync client |

## Production runtime

`my-fnirs` links this GATT server with `MY_SERVER_EMBEDDED=1`. The process owns
BLE, CAN-FD, recording, and OTA integration; a second standalone `my-server`
must not be started at the same time.

Board paths:

- incoming files: `/app_data/ble_rx/`
- finalized recordings: `/app_data/ble_tx/`
- deployed binaries and logs: `/app_data/golgi/`

The legacy standalone command remains available for isolated GATT debugging:

```bash
my-server -U
```

## PC tools

```bash
python3 -m pip install -r requirements-app.txt
python3 scan_ble.py
python3 ble_file_tx.py scan
python3 ble_file_tx.py bind -d AA:BB:CC:DD:EE:FF
python3 ble_file_tx.py send firmware.bin --fast
```

Replace the example MAC address with the target board address.

## Protocol

The C server, Python client, and Flutter client share the same opcodes and
little-endian framing. See
[BLE文件传输参数说明.md](BLE文件传输参数说明.md) before changing packet size,
ACK window, timing, or CRC behavior.
