param(
    [switch]$Build,
    [switch]$BuildOnly,
    [switch]$Libs,       # kept for compatibility; libs are pushed by default now
    [switch]$SkipLibs,   # skip libevent .so push (only if already on board image)
    [switch]$NoCheck,
    [string]$Serial = ""
)

# One-click: build (optional) + adb push my-fnirs + S99myapp + libevent + restart + check
#
# Usage (PowerShell):
#   .\deploy-my-fnirs-ev.ps1           # push binary + S99myapp + libevent + restart
#   .\deploy-my-fnirs-ev.ps1 -Build    # make my-fnirs in WSL, then push
#   .\deploy-my-fnirs-ev.ps1 -BuildOnly # only compile in WSL, no adb
#   .\deploy-my-fnirs-ev.ps1 -Serial ingenic-x2600-halley7   # multi-device adb

# Edit if your paths differ:
$WslDistro  = "Ubuntu"
$SdkWslPath  = "/root/ingenic/x2600/sdk_v4.1_unzip/ingenic-linux-kernel5.10-x2600-v4.1-20250625"
$LunchCombo  = "x2600halley7.v10_msc_5.10-eng"
$AdbExe      = "$env:LOCALAPPDATA\Android\Sdk\platform-tools\adb.exe"
$BoardSerial = "ingenic-x2600-halley7"

$ErrorActionPreference = "Stop"

function Resolve-AdbSerial {
    param(
        [string]$Requested,
        [string]$AdbPath,
        [string]$PreferBoard
    )

    if ($Requested) {
        return $Requested
    }

    $ready = @(& $AdbPath devices | Select-String "`tdevice$" | ForEach-Object {
        ($_.Line -split "`t")[0]
    })

    if ($ready.Count -eq 1) {
        return $ready[0]
    }

    if ($ready -contains $PreferBoard) {
        Write-Host "==> adb: multiple devices, using board $PreferBoard"
        return $PreferBoard
    }

    Write-Error @"
multiple adb devices — specify board serial, e.g.:
  .\deploy-my-fnirs-ev.ps1 -Serial $PreferBoard

Or unplug the phone (RFCT* unauthorized also counts as a device).

Current:
$(& $AdbPath devices | Out-String)
"@
}

function Invoke-Adb {
    param(
        [string]$AdbPath,
        [string]$DeviceSerial,
        [Parameter(ValueFromRemainingArguments = $true)]
        [string[]]$Args
    )

    if ($DeviceSerial) {
        & $AdbPath -s $DeviceSerial @Args
    } else {
        & $AdbPath @Args
    }
}

function Get-AdbShellText {
    param(
        [string]$AdbPath,
        [string]$DeviceSerial,
        [string]$Command
    )

    $raw = Invoke-Adb $AdbPath $DeviceSerial shell $Command 2>&1
    if ($null -eq $raw) {
        return ""
    }
    if ($raw -is [System.Array]) {
        return (($raw | ForEach-Object { "$_" }) -join "`n").Trim()
    }
    return "$raw".Trim()
}

if (-not (Test-Path $AdbExe)) {
    Write-Error "adb not found: $AdbExe"
}

$SdkWin = "\\wsl$\$WslDistro$($SdkWslPath.Replace('/', '\'))"
$InitScript = Join-Path $SdkWin "device\x2600halley7\rootfs-overlay\etc\init.d\S99myapp"
if (-not (Test-Path $SdkWin)) {
    Write-Error "SDK not visible from Windows: $SdkWin"
}

if ($Build -or $BuildOnly) {
    Write-Host "==> WSL: lunch $LunchCombo && make my-fnirs"
    wsl -d $WslDistro bash -lc "cd '$SdkWslPath' && source build/envsetup.sh && lunch '$LunchCombo' && make my-fnirs"
}

$Bin = Get-ChildItem -Path "$SdkWin\out" -Recurse -Filter "my-fnirs" -ErrorAction SilentlyContinue |
    Where-Object { $_.FullName -match '\\target\\opt\\golgi\\my-fnirs$' } |
    Sort-Object LastWriteTime -Descending |
    Select-Object -First 1

if (-not $Bin) {
    Write-Error "my-fnirs not found under $SdkWin\out — run with -Build first"
}

Write-Host "==> PC binary: $($Bin.FullName)"
Write-Host "    mtime: $($Bin.LastWriteTime)"
$tag = wsl -d $WslDistro bash -lc "strings '$SdkWslPath/out/product/*/obj/buildroot-intermediate/target/opt/golgi/my-fnirs' 2>/dev/null | grep -E 'pipe \+|periodic tick|unified' | head -1"
if ($tag) { Write-Host "    tag: $tag" }

if ($BuildOnly) {
    Write-Host "BuildOnly — skip adb push."
    exit 0
}

# Binary: .../target/opt/golgi/my-fnirs  ->  libs: .../target/usr/lib
$TargetRoot = Split-Path (Split-Path (Split-Path $Bin.FullName -Parent) -Parent) -Parent
$LibDir = Join-Path $TargetRoot "usr\lib"

Write-Host "==> adb devices"
& $AdbExe devices | Out-Host
$dev = & $AdbExe devices | Select-String "`tdevice$"
if (-not $dev) {
    Write-Warning @"
no adb device — push skipped (PC build is ready).

板子串口恢复 USB:
  echo 13500000.otg > /sys/kernel/config/usb_gadget/demo/UDC

Windows 确认:
  adb devices

然后只推送(不用再编译):
  .\deploy-my-fnirs-ev.ps1
"@
    exit 1
}

$AdbSerial = Resolve-AdbSerial -Requested $Serial -AdbPath $AdbExe -PreferBoard $BoardSerial
Write-Host "==> adb target: $AdbSerial"

$BoardGolgi = "/app_data/golgi"

function Push-ToBoard {
    param(
        [string]$LocalPath,
        [string]$RemotePath
    )
    Invoke-Adb $AdbExe $AdbSerial push $LocalPath $RemotePath
}

function Prepare-BoardGolgi {
    Write-Host "==> prepare $BoardGolgi on device"
    Invoke-Adb $AdbExe $AdbSerial shell "mkdir -p /app_data $BoardGolgi/lib 2>/dev/null; mount /dev/mmcblk0p4 /app_data 2>/dev/null; mount -o remount,rw /app_data 2>/dev/null; true"
}

function Push-LibEventLibs {
    param([string]$LibPath)

    $ev = Join-Path $LibPath "libevent-2.1.so.7.0.1"
    $evp = Join-Path $LibPath "libevent_pthreads-2.1.so.7.0.1"
    if (-not (Test-Path $ev) -or -not (Test-Path $evp)) {
        Write-Error "libevent not found under $LibPath — run with -Build first"
    }

    Prepare-BoardGolgi
    Write-Host "==> push libevent -> $BoardGolgi/lib (and /usr/lib if writable)"
    Push-ToBoard $ev "$BoardGolgi/lib/"
    Push-ToBoard $evp "$BoardGolgi/lib/"
    Invoke-Adb $AdbExe $AdbSerial shell "ln -sf libevent-2.1.so.7.0.1 $BoardGolgi/lib/libevent-2.1.so.7; ln -sf libevent_pthreads-2.1.so.7.0.1 $BoardGolgi/lib/libevent_pthreads-2.1.so.7"
    Invoke-Adb $AdbExe $AdbSerial shell "mount -o remount,rw / 2>/dev/null; cp -f $BoardGolgi/lib/libevent-2.1.so.7.0.1 /usr/lib/ 2>/dev/null; cp -f $BoardGolgi/lib/libevent_pthreads-2.1.so.7.0.1 /usr/lib/ 2>/dev/null; ln -sf libevent-2.1.so.7.0.1 /usr/lib/libevent-2.1.so.7 2>/dev/null; ln -sf libevent_pthreads-2.1.so.7.0.1 /usr/lib/libevent_pthreads-2.1.so.7 2>/dev/null; true"
}

if (-not $SkipLibs) {
    Push-LibEventLibs -LibPath $LibDir
}

Write-Host "==> push $($Bin.FullName)"
Prepare-BoardGolgi
Push-ToBoard $Bin.FullName "$BoardGolgi/my-fnirs"
Invoke-Adb $AdbExe $AdbSerial shell "chmod 755 $BoardGolgi/my-fnirs; mount -o remount,rw / 2>/dev/null; cp -f $BoardGolgi/my-fnirs /opt/golgi/my-fnirs 2>/dev/null; chmod 755 /opt/golgi/my-fnirs 2>/dev/null; sync; test -x $BoardGolgi/my-fnirs && ls -la $BoardGolgi/my-fnirs"

if (-not (Test-Path $InitScript)) {
    Write-Error "S99myapp not found: $InitScript"
}
Write-Host "==> push S99myapp (my-fnirs with embedded GATT)"
Invoke-Adb $AdbExe $AdbSerial push $InitScript /etc/init.d/S99myapp
Invoke-Adb $AdbExe $AdbSerial shell chmod 755 /etc/init.d/S99myapp

Write-Host "==> restart S99myapp (non-blocking)"
Invoke-Adb $AdbExe $AdbSerial shell "sh -c '/etc/init.d/S99myapp restart >/tmp/s99-restart.log 2>&1 </dev/null & sleep 2; cat /tmp/s99-restart.log'"

if (-not $NoCheck) {
    Start-Sleep -Seconds 10
    Write-Host "==> check"
    $fnirsPid = Get-AdbShellText $AdbExe $AdbSerial "pidof my-fnirs 2>/dev/null || ps | grep -v grep | grep '[m]y-fnirs' | awk '{print `$1}' | head -1"
    $msrvPid = Get-AdbShellText $AdbExe $AdbSerial "pidof my-server 2>/dev/null || echo stopped"
    $s99Tag = Get-AdbShellText $AdbExe $AdbSerial "grep -q 'embedded GATT' /etc/init.d/S99myapp && echo unified || echo legacy"
    $md5 = Get-AdbShellText $AdbExe $AdbSerial "md5sum $BoardGolgi/my-fnirs /opt/golgi/my-fnirs 2>/dev/null"
    $ldd = Get-AdbShellText $AdbExe $AdbSerial "LD_LIBRARY_PATH=$BoardGolgi/lib:/usr/lib ldd $BoardGolgi/my-fnirs 2>&1"
    $tick1 = Get-AdbShellText $AdbExe $AdbSerial "cat /tmp/ev_tick.count 2>/dev/null"
    Start-Sleep -Seconds 5
    $tick2 = Get-AdbShellText $AdbExe $AdbSerial "cat /tmp/ev_tick.count 2>/dev/null"
    $tickLog = Get-AdbShellText $AdbExe $AdbSerial "tail -5 /tmp/ev_tick.log 2>/dev/null"
    $log = Get-AdbShellText $AdbExe $AdbSerial "tail -20 /tmp/fnirs.log 2>/dev/null"

    Write-Host "    pid: $fnirsPid"
    Write-Host "    my-server: $msrvPid"
    Write-Host "    S99myapp: $s99Tag"
    Write-Host "    md5: $md5"
    Write-Host "    ev_tick.count: $tick1 -> $tick2 (5s)"
    if ($tickLog) {
        Write-Host "    --- /tmp/ev_tick.log (tail) ---"
        $tickLog -split "`n" | ForEach-Object {
            if ($_) { Write-Host "    $_" }
        }
    }
    if ($ldd) {
        $ldd -split "`n" | ForEach-Object {
            if ($_ -match 'not found') { Write-Warning "    ldd: $_" }
        }
    }
    if ($log) {
        Write-Host "    --- /tmp/fnirs.log (tail) ---"
        $log -split "`n" | ForEach-Object {
            if ($_) { Write-Host "    $_" }
        }
    }

    if ($fnirsPid -match '^\d+') {
        Write-Host "OK: my-fnirs running (pid $fnirsPid)"
    } elseif ($tick1 -match '^\d+$' -and $tick2 -match '^\d+$' -and [int]$tick2 -gt [int]$tick1) {
        Write-Host "OK: my-fnirs running (ev_tick $tick1->$tick2, pidof unavailable on board)"
    } elseif ($tick2 -match '^\d+$' -and [int]$tick2 -gt 0) {
        Write-Host "OK: my-fnirs running (ev_tick.count=$tick2, pidof unavailable on board)"
    } else {
        Write-Warning "my-fnirs not running — check: adb shell dmesg | tail; cat /tmp/fnirs.log"
    }
}

Write-Host "Done."
