#!/bin/bash
# WSL/Linux: build (optional) + adb push my-fnirs + S99myapp + restart
#
# Binaries go to /app_data/golgi/ (ext4, always writable). Root /opt is often
# read-only on device; adb push there fails silently after reboot.
#
# Usage:
#   ./deploy-my-fnirs.sh
#   ./deploy-my-fnirs.sh --build
#
# Set if needed:
#   export ADB=/path/to/adb
LUNCH_COMBO="${LUNCH_COMBO:-x2600halley7.v10_msc_5.10-eng}"
BOARD_GOLGI=/app_data/golgi

set -e

SDK="$(cd "$(dirname "$0")/../.." && pwd)"
HALLEY7="$(cd "$(dirname "$0")" && pwd)"
S99MYAPP="$HALLEY7/rootfs-overlay/etc/init.d/S99myapp"
BUILD=0

for arg in "$@"; do
	case "$arg" in
	--build) BUILD=1 ;;
	esac
done

ADB="${ADB:-adb}"
if [[ ! -x "$ADB" && ! -f "$ADB" ]]; then
	ADB=adb
fi

cd "$SDK"
if [[ "$BUILD" -eq 1 ]]; then
	echo "==> lunch $LUNCH_COMBO && make my-fnirs"
	source build/envsetup.sh
	lunch "$LUNCH_COMBO"
	make my-fnirs
fi

BIN="$(find out -path '*/target/opt/golgi/my-fnirs' -type f 2>/dev/null | head -1)"
if [[ -z "$BIN" ]]; then
	echo "my-fnirs not found — run with --build" >&2
	exit 1
fi

TARGET_ROOT="$(dirname "$(dirname "$(dirname "$BIN")")")"
LIB="$TARGET_ROOT/usr/lib"

echo "==> adb devices"
"$ADB" devices

echo "==> prepare $BOARD_GOLGI on device"
"$ADB" shell "
	mkdir -p /app_data 2>/dev/null
	mount /dev/mmcblk0p4 /app_data 2>/dev/null || mount -o remount,rw /app_data 2>/dev/null || true
	mkdir -p $BOARD_GOLGI $BOARD_GOLGI/lib || exit 1
	touch $BOARD_GOLGI/.rw_test && rm -f $BOARD_GOLGI/.rw_test || exit 1
"

echo "==> push libevent to $BOARD_GOLGI/lib"
"$ADB" push "$LIB/libevent-2.1.so.7.0.1" "$BOARD_GOLGI/lib/"
"$ADB" push "$LIB/libevent_pthreads-2.1.so.7.0.1" "$BOARD_GOLGI/lib/"
"$ADB" shell "ln -sf $BOARD_GOLGI/lib/libevent-2.1.so.7.0.1 $BOARD_GOLGI/lib/libevent-2.1.so.7; ln -sf $BOARD_GOLGI/lib/libevent_pthreads-2.1.so.7.0.1 $BOARD_GOLGI/lib/libevent_pthreads-2.1.so.7"

echo "==> push $BIN -> $BOARD_GOLGI/my-fnirs"
"$ADB" shell "mkdir -p $BOARD_GOLGI"
"$ADB" push "$BIN" "$BOARD_GOLGI/my-fnirs"
"$ADB" shell "chmod 755 $BOARD_GOLGI/my-fnirs && sync"
"$ADB" shell "test -x $BOARD_GOLGI/my-fnirs && ls -la $BOARD_GOLGI/my-fnirs" || {
	echo "ERROR: push failed — $BOARD_GOLGI/my-fnirs not on device" >&2
	exit 1
}

if [[ ! -f "$S99MYAPP" ]]; then
	echo "S99myapp not found: $S99MYAPP" >&2
	exit 1
fi
echo "==> push S99myapp"
"$ADB" shell "mount -o remount,rw / 2>/dev/null; true"
"$ADB" push "$S99MYAPP" /etc/init.d/S99myapp
"$ADB" shell "chmod 755 /etc/init.d/S99myapp 2>/dev/null; sync; true"

echo "==> restart (LD_LIBRARY_PATH=$BOARD_GOLGI/lib)"
"$ADB" shell "sh -c '/etc/init.d/S99myapp restart >/tmp/s99-restart.log 2>&1 </dev/null & sleep 2; cat /tmp/s99-restart.log'"
sleep 4
echo "==> check"
"$ADB" shell "
export LD_LIBRARY_PATH=$BOARD_GOLGI/lib:\$LD_LIBRARY_PATH
test -x $BOARD_GOLGI/my-fnirs || echo 'MISSING: $BOARD_GOLGI/my-fnirs'
LD_LIBRARY_PATH=$BOARD_GOLGI/lib ldd $BOARD_GOLGI/my-fnirs 2>&1
echo ---
ps | grep -v grep | grep my-fnirs || echo 'my-fnirs: not running'
echo tick1: \$(cat /tmp/ev_tick.count 2>/dev/null)
sleep 5
echo tick2: \$(cat /tmp/ev_tick.count 2>/dev/null)
echo ---
tail -8 /tmp/fnirs.log 2>/dev/null
"

echo "Done."
