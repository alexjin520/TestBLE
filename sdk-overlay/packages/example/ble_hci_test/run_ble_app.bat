@echo off
REM TestBLE file transfer GUI — double-click or run from cmd
cd /d "%~dp0"
python ble_file_tx_gui.py
if errorlevel 1 pause
