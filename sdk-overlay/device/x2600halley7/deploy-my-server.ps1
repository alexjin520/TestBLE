param(
    [switch]$Build,
    [switch]$BuildOnly,
    [switch]$NoCheck,
    [string]$Serial = ""
)

# One-click: build (optional) + adb push my-server + restart + quick check
#
# Usage (PowerShell):
#   .\deploy-my-server.ps1                    # push + restart + check
#   .\deploy-my-server.ps1 -Build             # make my-server in WSL, then push
#   .\deploy-my-server.ps1 -BuildOnly         # only compile in WSL, no adb
#   .\deploy-my-server.ps1 -Serial ingenic-x2600-halley7   # multi-device adb

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
  .\deploy-my-server.ps1 -Serial $PreferBoard

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
if (-not (Test-Path $SdkWin)) {
    Write-Error "SDK not visible from Windows: $SdkWin"
}

if ($Build -or $BuildOnly) {
    Write-Host "==> WSL: lunch $LunchCombo && make my-server"
    wsl -d $WslDistro bash -lc "cd '$SdkWslPath' && source build/envsetup.sh && lunch '$LunchCombo' && make my-server"
}

$Bin = Get-ChildItem -Path "$SdkWin\out" -Recurse -Filter "my-server" -ErrorAction SilentlyContinue |
    Where-Object { $_.FullName -match '\\target\\opt\\golgi\\my-server$' } |
    Sort-Object LastWriteTime -Descending |
    Select-Object -First 1

if (-not $Bin) {
    Write-Error "my-server not found under $SdkWin\out — run with -Build first"
}

Write-Host "==> PC binary: $($Bin.FullName)"
Write-Host "    mtime: $($Bin.LastWriteTime)"
$tag = wsl -d $WslDistro bash -lc "strings '$SdkWslPath/out/product/*/obj/buildroot-intermediate/target/opt/golgi/my-server' 2>/dev/null | grep -E 'my-server build:|20260' | head -1"
if ($tag) { Write-Host "    tag: $tag" }

if ($BuildOnly) {
    Write-Host "BuildOnly — skip adb push."
    exit 0
}

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
  .\deploy-my-server.ps1
"@
    exit 1
}

$AdbSerial = Resolve-AdbSerial -Requested $Serial -AdbPath $AdbExe -PreferBoard $BoardSerial
Write-Host "==> adb target: $AdbSerial"

Write-Host "==> push $($Bin.FullName)"
Invoke-Adb $AdbExe $AdbSerial push $Bin.FullName /opt/golgi/my-server
Invoke-Adb $AdbExe $AdbSerial shell chmod 755 /opt/golgi/my-server

Write-Host "==> restart S99myapp (my-fnirs + my-server)"
Invoke-Adb $AdbExe $AdbSerial shell /etc/init.d/S99myapp restart

if (-not $NoCheck) {
    Start-Sleep -Seconds 6
    Write-Host "==> check"
    $out = Get-AdbShellText $AdbExe $AdbSerial "md5sum /opt/golgi/my-server; pidof my-server; pidof my-fnirs; strings /opt/golgi/my-server 2>/dev/null | grep 'my-server build' | head -1"
    if ($out) {
        $out -split "`n" | ForEach-Object {
            if ($_) { Write-Host "    $_" }
        }
    }
}

Write-Host "Done."
