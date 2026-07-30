#!/bin/bash
# Build and deploy the libuv comparison binary without replacing my-fnirs.

set -e

LUNCH_COMBO="${LUNCH_COMBO:-x2600halley7.v10_msc_5.10-eng}"
ADB="${ADB:-adb}"
BOARD_GOLGI=/app_data/golgi
BUILD=0

for arg in "$@"; do
	case "$arg" in
	--build) BUILD=1 ;;
	esac
done

if [[ ! -x "$ADB" && ! -f "$ADB" ]]; then
	ADB=adb
fi

SDK="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$SDK"

if [[ "$BUILD" -eq 1 ]]; then
	source build/envsetup.sh
	lunch "$LUNCH_COMBO"
	make my-fnirs-uv
fi

BIN="$(find out -path '*/target/opt/golgi/my-fnirs-uv' -type f 2>/dev/null | head -1)"
if [[ -z "$BIN" ]]; then
	echo "my-fnirs-uv not found — run with --build" >&2
	exit 1
fi

TARGET_ROOT="$(dirname "$(dirname "$(dirname "$BIN")")")"
LIB="$TARGET_ROOT/usr/lib"
LIBUV="$LIB/libuv.so.1.0.0"

if [[ ! -f "$LIBUV" ]]; then
	echo "libuv runtime not found: $LIBUV" >&2
	exit 1
fi

"$ADB" shell "mkdir -p $BOARD_GOLGI/lib"
"$ADB" push "$BIN" "$BOARD_GOLGI/my-fnirs-uv"
"$ADB" push "$LIBUV" "$BOARD_GOLGI/lib/libuv.so.1.0.0"
"$ADB" shell "chmod 755 $BOARD_GOLGI/my-fnirs-uv; \
	ln -sf libuv.so.1.0.0 $BOARD_GOLGI/lib/libuv.so.1; sync"

echo "deployed: $BOARD_GOLGI/my-fnirs-uv"
echo "switch with: echo FNIRS_BACKEND=uv > /etc/default/fnirs; /etc/init.d/S99myapp restart"
