#!/bin/sh
# Generate phase-2 demo file for board->phone FILE TX (no python required).
# Usage: sh ble_sim_signal.sh [output_path]

OUT="${1:-/app_data/ble_tx/sim_signal.bin}"
DIR=$(dirname "$OUT")
SAMPLES=4096

mkdir -p "$DIR" || exit 1

# 24-byte fSIM header: magic, v1, 10Hz, 4096 samples, 1ch, 16bit.
printf 'fSIM\001\000\000\000\012\000\000\000\000\020\000\000\001\000\020\000\000\000\000\000' > "$OUT"

# Payload: 16-bit ramp (8192 bytes).
i=0
while [ "$i" -lt "$SAMPLES" ]; do
	lo=$((i & 255))
	hi=$(((i >> 8) & 255))
	printf "\\$(printf '%03o' "$lo")\\$(printf '%03o' "$hi")" >> "$OUT"
	i=$((i + 1))
done

bytes=$(wc -c < "$OUT" | tr -d ' ')
echo "Wrote ${bytes} bytes to $OUT (fSIM v1, ${SAMPLES} samples @ 10 Hz)"
