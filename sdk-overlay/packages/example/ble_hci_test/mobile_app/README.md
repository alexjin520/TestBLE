# TestBLE Flutter app

Flutter client for the TestBLE GATT service on the X2600 fNIRS hub.

## Features

- scan, connect, reconnect, and manual MAC entry
- fNIRS node discovery and acquisition control
- gain and LED-sequence configuration
- real-time waveform preview and channel filtering
- phone-to-board file upload
- finalized board-to-phone file download
- reliable LIVE FILE synchronization while recording
- CRC, sequence, offset, and transfer-progress validation

## Requirements

- Flutter 3.16 or newer
- Android Studio for Android builds
- Xcode and macOS for iOS builds
- a BLE-capable phone

## Create platform scaffolding

This repository keeps the application-specific Dart sources small. Generate
the standard Flutter platform files on first use:

```bash
flutter create . --project-name testble_send --org com.example.testble
flutter pub get
flutter run
```

Build an Android release APK:

```bash
flutter build apk --release
```

The output is normally:

```text
build/app/outputs/flutter-apk/app-release.apk
```

## Android permissions

Confirm that `android/app/src/main/AndroidManifest.xml` includes:

```xml
<uses-permission android:name="android.permission.BLUETOOTH_SCAN"
    android:usesPermissionFlags="neverForLocation" />
<uses-permission android:name="android.permission.BLUETOOTH_CONNECT" />
<uses-permission android:name="android.permission.ACCESS_FINE_LOCATION"
    android:maxSdkVersion="30" />
```

Android 12 and newer require the Nearby devices permission at runtime.

## iOS permission

Add a Bluetooth usage description to `ios/Runner/Info.plist`:

```xml
<key>NSBluetoothAlwaysUsageDescription</key>
<string>Connect to the TestBLE acquisition hub.</string>
```

## Main sources

| File | Purpose |
| --- | --- |
| `lib/main.dart` | application entry and BLE device selection |
| `lib/protocol.dart` | shared opcodes, packet builders, and parsers |
| `lib/fnirs_monitor_page.dart` | acquisition workflow and waveform UI |
| `lib/fnirs_ble.dart` | fNIRS request/response client |
| `lib/ble_transfer.dart` | phone-to-board upload |
| `lib/ble_board_rx.dart` | finalized file download |
| `lib/ble_live_sync.dart` | growing-file synchronization and tail recovery |
| `lib/ble_stream.dart` | real-time preview stream |

## Operational notes

- Product firmware runs GATT inside `my-fnirs`; no separate server process is
  required.
- Replace the example MAC address with the target board address when manual
  connection is needed.
- A `.part` file is renamed only after final size and CRC32 validation.
- Real-time STREAM is a preview path. Use LIVE FILE or FILE TX when complete
  recorded data is required.
