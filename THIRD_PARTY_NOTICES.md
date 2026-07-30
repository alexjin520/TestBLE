# Third-party notices

This repository is a source overlay for an Ingenic X2600 SDK installation.
The full SDK, vendor firmware, release signing material, and generated target
binaries are not distributed here.

## BlueZ

`sdk-overlay/packages/example/ble_hci_test/mybtgatt-server.c` is derived from
the BlueZ GATT server example and is marked `GPL-2.0-or-later`. Its existing
copyright and SPDX notices must be retained.

## Linux UAPI headers

Files under `sdk-overlay/packages/example/x2600-fNIRS/include/linux/` retain
their original Linux SPDX identifiers and copyright notices. Those per-file
terms govern redistribution.

## Ingenic SDK integration

Paths under `sdk-overlay/device/` and `sdk-overlay/packages/` are designed to
be copied into a separately obtained Ingenic SDK. Ingenic product names and
platform identifiers belong to their respective owners. This repository does
not grant a license to the SDK or other vendor components.

## Project files without an SPDX identifier

Public availability alone does not grant permission to copy, modify, or
redistribute files that do not carry an explicit license. Contact the
repository owner before reusing those files.
