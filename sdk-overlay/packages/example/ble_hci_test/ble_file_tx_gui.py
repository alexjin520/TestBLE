#!/usr/bin/env python3
"""
TestBLE file transfer — small desktop app (Windows / Linux with BLE).

Requires: pip install bleak
Run:      python ble_file_tx_gui.py

Board:    start-my-server -U   (my-server build 20250703-rx10 or later)
Files land on board: /app_data/ble_rx/
"""

from __future__ import annotations

import asyncio
import os
import sys
import threading
import tkinter as tk
from tkinter import filedialog, messagebox, ttk

APP_TITLE = "TestBLE 文件传输"
APP_VERSION = "1.0"

try:
    from ble_file_tx import (
        BLE_RX_DIR,
        DEFAULT_BLE_NAME,
        device_file_path,
        load_saved_device,
        save_device,
        transfer_file,
    )
except ImportError as e:
    raise SystemExit(
        "Cannot import ble_file_tx.py — keep it in the same folder as this app.\n"
        f"Also install: pip install bleak\n({e})"
    ) from e


class BleFileApp(tk.Tk):
    def __init__(self) -> None:
        super().__init__()
        self.title(f"{APP_TITLE} v{APP_VERSION}")
        self.minsize(520, 420)
        self.geometry("640x520")

        self._file_path = tk.StringVar()
        self._device_mac = tk.StringVar(value=load_saved_device() or "")
        self._frame_crc = tk.BooleanVar(value=True)
        self._safe_mode = tk.BooleanVar(value=False)
        self._status = tk.StringVar(value="就绪")
        self._progress = tk.DoubleVar(value=0.0)
        self._busy = False

        self._build_ui()

    def _build_ui(self) -> None:
        pad = {"padx": 10, "pady": 4}

        frm = ttk.Frame(self, padding=10)
        frm.pack(fill=tk.BOTH, expand=True)

        ttk.Label(frm, text="板子蓝牙 MAC").grid(row=0, column=0, sticky=tk.W, **pad)
        row0 = ttk.Frame(frm)
        row0.grid(row=0, column=1, sticky=tk.EW, **pad)
        ttk.Entry(row0, textvariable=self._device_mac, width=22).pack(
            side=tk.LEFT, fill=tk.X, expand=True
        )
        ttk.Button(row0, text="扫描", width=8, command=self._on_scan).pack(
            side=tk.LEFT, padx=(6, 0)
        )
        ttk.Button(row0, text="保存", width=8, command=self._on_save_mac).pack(
            side=tk.LEFT, padx=(4, 0)
        )

        ttk.Label(frm, text="文件").grid(row=1, column=0, sticky=tk.W, **pad)
        row1 = ttk.Frame(frm)
        row1.grid(row=1, column=1, sticky=tk.EW, **pad)
        ttk.Entry(row1, textvariable=self._file_path).pack(
            side=tk.LEFT, fill=tk.X, expand=True
        )
        ttk.Button(row1, text="浏览…", command=self._on_browse).pack(
            side=tk.LEFT, padx=(6, 0)
        )

        opt = ttk.Frame(frm)
        opt.grid(row=2, column=1, sticky=tk.W, **pad)
        ttk.Checkbutton(
            opt, text="帧 CRC 校验（推荐）", variable=self._frame_crc
        ).pack(side=tk.LEFT)
        ttk.Checkbutton(
            opt, text="稳定模式（慢速）", variable=self._safe_mode
        ).pack(side=tk.LEFT, padx=(12, 0))

        ttk.Label(frm, text="进度").grid(row=3, column=0, sticky=tk.W, **pad)
        prog_row = ttk.Frame(frm)
        prog_row.grid(row=3, column=1, sticky=tk.EW, **pad)
        self._prog_bar = ttk.Progressbar(
            prog_row, variable=self._progress, maximum=100.0
        )
        self._prog_bar.pack(fill=tk.X, expand=True)
        ttk.Label(prog_row, textvariable=self._status).pack(anchor=tk.W, pady=(4, 0))

        btn_row = ttk.Frame(frm)
        btn_row.grid(row=4, column=1, sticky=tk.W, **pad)
        self._send_btn = ttk.Button(btn_row, text="发送文件", command=self._on_send)
        self._send_btn.pack(side=tk.LEFT)
        ttk.Label(
            frm,
            text=f"板子需运行 start-my-server；文件保存在 {BLE_RX_DIR}/",
            foreground="#555",
        ).grid(row=5, column=0, columnspan=2, sticky=tk.W, **pad)

        ttk.Label(frm, text="日志").grid(row=6, column=0, sticky=tk.NW, **pad)
        log_frame = ttk.Frame(frm)
        log_frame.grid(row=6, column=1, sticky=tk.NSEW, **pad)
        self._log = tk.Text(log_frame, height=14, wrap=tk.WORD, state=tk.DISABLED)
        scroll = ttk.Scrollbar(log_frame, command=self._log.yview)
        self._log.configure(yscrollcommand=scroll.set)
        self._log.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)
        scroll.pack(side=tk.RIGHT, fill=tk.Y)

        frm.columnconfigure(1, weight=1)
        frm.rowconfigure(6, weight=1)

    def _append_log(self, msg: str) -> None:
        self._log.configure(state=tk.NORMAL)
        self._log.insert(tk.END, msg + "\n")
        self._log.see(tk.END)
        self._log.configure(state=tk.DISABLED)

    def _ui(self, fn) -> None:
        self.after(0, fn)

    def _set_busy(self, busy: bool) -> None:
        self._busy = busy
        state = tk.DISABLED if busy else tk.NORMAL
        self._send_btn.configure(state=state)

    def _on_browse(self) -> None:
        path = filedialog.askopenfilename(title="选择要发送的文件")
        if path:
            self._file_path.set(path)

    def _on_save_mac(self) -> None:
        mac = self._device_mac.get().strip()
        if not mac:
            messagebox.showwarning(APP_TITLE, "请输入 MAC 地址")
            return
        save_device(mac)
        self._append_log(f"已保存 MAC -> {device_file_path()}")

    def _on_scan(self) -> None:
        if self._busy:
            return
        self._set_busy(True)
        self._status.set("正在扫描…")
        self._append_log("扫描附近 BLE 设备（约 20 秒）…")

        def run() -> None:
            try:
                asyncio.run(self._scan_async())
            except Exception as e:
                self._ui(lambda: self._scan_failed(str(e)))

        threading.Thread(target=run, daemon=True).start()

    async def _scan_async(self) -> None:
        from ble_file_tx import scan_devices

        try:
            rows = await scan_devices(20.0)
            if not rows:
                self._ui(lambda: self._append_log("(未发现设备 — 确认板子 start-my-server)"))
            for label, addr, _d, _ad in rows:
                self._ui(lambda lb=label: self._append_log(lb))
                if DEFAULT_BLE_NAME.lower() in label.lower():
                    self._ui(lambda a=addr: self._device_mac.set(a))
        except Exception as e:
            self._ui(lambda: self._scan_failed(str(e)))
            return
        self._ui(lambda: self._status.set("就绪"))
        self._ui(lambda: self._set_busy(False))

    def _scan_failed(self, err: str) -> None:
        self._append_log(f"扫描失败: {err}")
        self._status.set("就绪")
        self._set_busy(False)

    def _on_send(self) -> None:
        if self._busy:
            return
        path = self._file_path.get().strip()
        mac = self._device_mac.get().strip() or None
        if not path or not os.path.isfile(path):
            messagebox.showwarning(APP_TITLE, "请选择有效文件")
            return
        if not mac and not load_saved_device():
            messagebox.showwarning(
                APP_TITLE,
                "请填写板子 MAC，或先扫描/保存设备地址",
            )
            return

        self._progress.set(0.0)
        self._set_busy(True)
        self._status.set("连接中…")
        self._append_log(f"--- 开始发送: {os.path.basename(path)} ---")

        frame_crc = self._frame_crc.get()
        safe = self._safe_mode.get()

        def log_cb(msg: str) -> None:
            self._ui(lambda m=msg: self._append_log(m))

        def prog_cb(sent: int, total: int, rate: float) -> None:
            pct = (100.0 * sent / total) if total else 0.0
            self._ui(lambda: self._progress.set(pct))
            self._ui(
                lambda: self._status.set(
                    f"{sent}/{total} 字节  ({rate / 1024:.1f} KB/s)"
                )
            )

        async def job() -> None:
            await transfer_file(
                path,
                address=mac,
                use_frame_crc=frame_crc,
                safe=safe,
                on_log=log_cb,
                on_progress=prog_cb,
            )

        def run() -> None:
            try:
                asyncio.run(job())
            except ImportError:
                self._ui(
                    lambda: messagebox.showerror(
                        APP_TITLE, "请安装 bleak:\npip install bleak"
                    )
                )
            except Exception as e:
                self._ui(lambda: self._send_failed(str(e)))
            else:
                self._ui(self._send_ok)
            finally:
                self._ui(lambda: self._set_busy(False))

        threading.Thread(target=run, daemon=True).start()

    def _send_ok(self) -> None:
        self._progress.set(100.0)
        self._status.set("发送完成")
        self._append_log("=== 发送成功 ===")
        messagebox.showinfo(
            APP_TITLE,
            f"文件已发送。\n请在板子上查看:\nls -l {BLE_RX_DIR}/",
        )

    def _send_failed(self, err: str) -> None:
        self._status.set("失败")
        self._append_log(f"错误: {err}")
        messagebox.showerror(APP_TITLE, f"发送失败:\n{err}")


def main() -> None:
    if sys.platform == "win32":
        try:
            asyncio.set_event_loop_policy(asyncio.WindowsSelectorEventLoopPolicy())
        except AttributeError:
            pass
    app = BleFileApp()
    app.mainloop()


if __name__ == "__main__":
    main()
