# TestBLE

Embedded BLE file transfer, fNIRS acquisition, OTA, and a Flutter monitor for
the Ingenic X2600 platform.

TestBLE combines an embedded BlueZ GATT server with the `my-fnirs` board
service. The board discovers CAN-FD sensor nodes, drives dual-wavelength LEDs,
records timestamped samples, streams live data, transfers finalized recordings,
and updates node firmware. A Flutter client provides scan, control, waveform,
and file-transfer workflows.

> This is hardware-specific engineering and research software. It is not a
> medical device and must not be used for diagnosis or treatment.

## Highlights

- BLE GATT upload, download, and live-tail synchronization
- CRC-protected packets, cumulative ACK flow control, and retry handling
- CAN-FD fNIRS node discovery and 10 Hz sample/upload state machine
- Per-frame millisecond timestamps in recorded data
- Node OTA file validation, transfer, completion bitmap, and result reporting
- One process for fNIRS business logic and embedded GATT
- Production `libevent` backend plus an opt-in `libuv` comparison build
- Flutter monitor with node scan, LED sequence control, waveforms, and file sync

## Repository layout

| Path | Purpose |
| --- | --- |
| `sdk-overlay/packages/example/x2600-fNIRS/` | Board service, CAN-FD, UART, recording, OTA, event/UV backends |
| `sdk-overlay/packages/example/ble_hci_test/` | Embedded GATT server, protocol notes, and PC utilities |
| `sdk-overlay/packages/example/ble_hci_test/mobile_app/` | Flutter application |
| `sdk-overlay/device/x2600halley7/` | Build integration, init configuration, deployment, and validation scripts |
| `docs/` | Architecture and refactor notes |

The repository is a source overlay, not a copy of the full Ingenic SDK. This
keeps generated output, vendor firmware, release keys, and board-test binaries
out of the public history.

## Architecture

```text
Flutter / PC tools
        │ BLE GATT
        ▼
my-fnirs
├── embedded BlueZ GATT server
├── libevent main loop (default)
│   ├── CAN-FD node scan / sample / OTA
│   ├── UART host protocol
│   ├── recording and live-file synchronization
│   └── timers and signal handling
└── libuv compatibility backend (optional comparison build)
        │
        ▼
X2600 hub ── CAN-FD ── fNIRS sensor nodes
```

The two backends compile the same business sources. `my-fnirs` uses libevent
and remains the default. `my-fnirs-uv` is an explicitly selected comparison
binary; both processes must never own the hardware at the same time.

## Build

Requirements:

- Ingenic Linux SDK v4.1 for `x2600halley7`
- the matching Buildroot/BlueZ source tree
- Flutter 3.16 or newer for the mobile app
- ADB for deployment

Copy the overlay into an existing SDK:

```bash
export SDK_ROOT=/path/to/ingenic-linux-kernel5.10-x2600-v4.1-20250625
rsync -a sdk-overlay/ "$SDK_ROOT"/
cd "$SDK_ROOT"
source build/envsetup.sh
lunch x2600halley7.v10_msc_5.10-eng
make my-fnirs my-fnirs-uv
```

The production event backend can then be deployed from PowerShell:

```powershell
cd device\x2600halley7
.\deploy-my-fnirs-ev.ps1 -Build
```

The UV comparison build is deployed separately:

```powershell
.\deploy-my-fnirs-uv.ps1 -Build
```

Runtime selection is stored in `/etc/default/fnirs`. The checked-in safe
default is:

```text
FNIRS_BACKEND=event
FNIRS_UV_AUTOBOOT=0
```

## Mobile app

```bash
cd sdk-overlay/packages/example/ble_hci_test/mobile_app
flutter create . --project-name testble_send
flutter pub get
flutter run
```

The checked-in Dart sources contain the application-specific BLE protocol and
UI. `flutter create .` supplies platform scaffolding when it is not already
present.

## Validation helpers

After starting and stopping one recording, validate embedded timestamps from
PowerShell:

```powershell
.\check-fnirs-timestamp.ps1
```

A passing file reports two timestamps and their millisecond delta. Runtime
diagnostics are written under `/app_data/golgi/` on the board.

## Protocol documentation

- [BLE framing and tuning](sdk-overlay/packages/example/ble_hci_test/BLE文件传输参数说明.md)
- [GATT and mobile workflow](sdk-overlay/packages/example/ble_hci_test/README.md)
- [libevent refactor report](docs/fNIRS项目Libevent重构分析报告.md)
- [libuv comparison backend](sdk-overlay/packages/example/x2600-fNIRS/uv/README.md)

## Security and licensing

Release signing keys, device logs, generated binaries, vendor firmware, and
the full proprietary SDK are intentionally excluded. Use your own credentials
and obtain the Ingenic SDK through an authorized channel.

Licensing is file-specific. The BlueZ-derived GATT server is
`GPL-2.0-or-later`; Linux UAPI headers retain their SPDX terms. No
project-wide license is granted for files without an explicit license. See
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
