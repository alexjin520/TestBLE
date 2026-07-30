#!/usr/bin/env python3
"""Generate a fake fNIRS-like signal file on the board for FILE TX phase-2 demo."""
import math
import os
import struct
import sys

DEFAULT_OUT = "/app_data/ble_tx/sim_signal.bin"
SAMPLES = 4096
SAMPLE_RATE_HZ = 10
CHANNELS = 1
BITS_PER_SAMPLE = 16
MAGIC = b"fSIM"
VERSION = 1


def write_header(f) -> None:
    f.write(
        struct.pack(
            "<4sHHIIHHI",
            MAGIC,
            VERSION,
            0,
            SAMPLE_RATE_HZ,
            SAMPLES,
            CHANNELS,
            BITS_PER_SAMPLE,
            0,
        )
    )


def main() -> None:
    out = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_OUT
    os.makedirs(os.path.dirname(out), exist_ok=True)
    with open(out, "wb") as f:
        write_header(f)
        for i in range(SAMPLES):
            v = int(32767 * math.sin(2 * math.pi * i / 64))
            f.write(struct.pack("<h", v))
    payload = SAMPLES * CHANNELS * (BITS_PER_SAMPLE // 8)
    total = 24 + payload
    print(
        f"Wrote {total} bytes to {out} "
        f"(fSIM v{VERSION}, {SAMPLES} samples @ {SAMPLE_RATE_HZ} Hz)"
    )


if __name__ == "__main__":
    main()
