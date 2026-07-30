#!/bin/sh
# Phase 2 demo: simulated signal -> file on board -> App downloads via FILE TX.
set -e

OUT="/app_data/ble_tx/sim_signal.bin"
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

if [ -x "$SCRIPT_DIR/ble-sim-signal" ]; then
	"$SCRIPT_DIR/ble-sim-signal" "$OUT"
elif [ -f "$SCRIPT_DIR/ble_sim_signal.sh" ]; then
	sh "$SCRIPT_DIR/ble_sim_signal.sh" "$OUT"
elif command -v ble-sim-signal >/dev/null 2>&1; then
	ble-sim-signal "$OUT"
elif command -v python3 >/dev/null 2>&1 && [ -f "$SCRIPT_DIR/ble_sim_signal.py" ]; then
	python3 "$SCRIPT_DIR/ble_sim_signal.py" "$OUT"
else
	sh ble_sim_signal.sh "$OUT"
fi

echo ""
echo "========== 阶段2 演示就绪 =========="
echo "板子文件: $OUT"
ls -l "$OUT"
echo ""
echo "手机操作:"
echo "  1. 打开 TestBLE App，扫描并连接 TestBLE"
echo "  2. 点「演示：下载模拟信号」"
echo "  3. 完成后在 内部存储/下载/TestBLE/sim_signal.bin 查看"
echo "===================================="
