param(
    [switch]$Build,
    [switch]$BuildOnly,
    [switch]$SkipLibs,
    [switch]$NoActivate,
    [switch]$NoCheck,
    [string]$Serial = ""
)

# One-click build/deploy for the libuv backend. The event backend remains the
# persistent reboot fallback, but a normal deployment probes and activates the
# full UV hardware/GATT profile for the current boot.
#
# PowerShell usage:
#   .\deploy-my-fnirs-uv.ps1             # safe install + probe + activate UV
#   .\deploy-my-fnirs-uv.ps1 -Build      # clean build, then the same flow
#   .\deploy-my-fnirs-uv.ps1 -BuildOnly
#   .\deploy-my-fnirs-uv.ps1 -NoActivate # install, then leave event running

$WslDistro = "Ubuntu"
$SdkWslPath = "/root/ingenic/x2600/sdk_v4.1_unzip/ingenic-linux-kernel5.10-x2600-v4.1-20250625"
$LunchCombo = "x2600halley7.v10_msc_5.10-eng"
$AdbExe = "$env:LOCALAPPDATA\Android\Sdk\platform-tools\adb.exe"
$BoardSerial = "ingenic-x2600-halley7"
$BoardGolgi = "/app_data/golgi"

$ErrorActionPreference = "Stop"

function Invoke-Adb {
    param(
        [Parameter(ValueFromRemainingArguments = $true)]
        [string[]]$Args
    )

    if ($script:AdbSerial) {
        & $AdbExe -s $script:AdbSerial @Args
    } else {
        & $AdbExe @Args
    }
    if ($LASTEXITCODE -ne 0) {
        throw "adb failed with exit code $LASTEXITCODE"
    }
}

if (-not (Test-Path $AdbExe)) {
    throw "adb not found: $AdbExe"
}

$SdkWin = "\\wsl$\$WslDistro$($SdkWslPath.Replace('/', '\'))"
if (-not (Test-Path $SdkWin)) {
    throw "SDK not visible from Windows: $SdkWin"
}
$InitScript = Join-Path $SdkWin "device\x2600halley7\rootfs-overlay\etc\init.d\S99myapp"
$DefaultConfig = Join-Path $SdkWin "device\x2600halley7\rootfs-overlay\etc\default\fnirs"

if ($Build -or $BuildOnly) {
    Write-Host "==> WSL: clean rebuild my-fnirs-uv"
    wsl -d $WslDistro -- bash -lc "cd '$SdkWslPath' && source build/envsetup.sh && lunch '$LunchCombo' && make my-fnirs-uv-clean && make my-fnirs-uv"
    if ($LASTEXITCODE -ne 0) {
        throw "my-fnirs-uv build failed with exit code $LASTEXITCODE"
    }
}

$Bin = Get-ChildItem -Path "$SdkWin\out" -Recurse -File -Filter "my-fnirs-uv" -ErrorAction SilentlyContinue |
    Where-Object { $_.FullName -match '\\target\\opt\\golgi\\my-fnirs-uv$' } |
    Sort-Object LastWriteTime -Descending |
    Select-Object -First 1

if (-not $Bin) {
    throw "my-fnirs-uv not found under $SdkWin\out — run with -Build first"
}

$ElfHeader = [System.IO.File]::ReadAllBytes($Bin.FullName)
if ($ElfHeader.Length -lt 18 -or
    $ElfHeader[0] -ne 0x7f -or $ElfHeader[1] -ne 0x45 -or
    $ElfHeader[2] -ne 0x4c -or $ElfHeader[3] -ne 0x46) {
    throw "not an ELF binary: $($Bin.FullName)"
}
$ElfType = [int]$ElfHeader[16] -bor ([int]$ElfHeader[17] -shl 8)
if ($ElfType -ne 3) {
    throw "unsafe non-PIE UV binary (ELF type=$ElfType); rebuild before deployment"
}
$BinMd5 = (Get-FileHash -Algorithm MD5 $Bin.FullName).Hash.ToLowerInvariant()

Write-Host "==> PC binary: $($Bin.FullName)"
Write-Host "    mtime: $($Bin.LastWriteTime)"
Write-Host "    ELF: PIE (ET_DYN)"
Write-Host "    MD5: $BinMd5"

if ($BuildOnly) {
    Write-Host "BuildOnly — skip adb push."
    exit 0
}

$ready = @(& $AdbExe devices | Select-String "`tdevice$" | ForEach-Object {
    ($_.Line -split "`t")[0]
})
if ($Serial) {
    $script:AdbSerial = $Serial
} elseif ($ready.Count -eq 1) {
    $script:AdbSerial = $ready[0]
} elseif ($ready -contains $BoardSerial) {
    $script:AdbSerial = $BoardSerial
} else {
    throw "No unique adb board found. Connect the board or use -Serial $BoardSerial"
}

# Do not mistake a phone for the board when the X2600 has crashed off ADB.
& $AdbExe -s $script:AdbSerial shell "test -x /etc/init.d/S99myapp -o -x /opt/golgi/my-fnirs"
if ($LASTEXITCODE -ne 0) {
    throw "ADB device $script:AdbSerial is not the X2600 fNIRS board; deployment refused"
}
Write-Host "==> adb target: $script:AdbSerial"

# Binary path: target/opt/golgi/my-fnirs-uv; runtime library: target/usr/lib.
$TargetRoot = Split-Path (Split-Path (Split-Path $Bin.FullName -Parent) -Parent) -Parent
$LibUv = Join-Path $TargetRoot "usr\lib\libuv.so.1.0.0"
if (-not $SkipLibs -and -not (Test-Path $LibUv)) {
    throw "libuv runtime not found: $LibUv — run with -Build first"
}

Write-Host "==> prepare $BoardGolgi"
Invoke-Adb shell "mkdir -p /app_data $BoardGolgi/lib 2>/dev/null; mount /dev/mmcblk0p4 /app_data 2>/dev/null; mount -o remount,rw /app_data 2>/dev/null; true"

#
# Never adb-push over the inode of a running executable. If UV is active,
# truncating my-fnirs-uv in place can SIGBUS/crash it as unmapped code pages
# are touched. Stage every runtime file first, stop through S99myapp, and then
# atomically rename the completed files into place.
#
$UvNew = "$BoardGolgi/my-fnirs-uv.new"
$LibUvNew = "$BoardGolgi/lib/libuv.so.1.0.0.new"

Write-Host "==> stage my-fnirs-uv (do not overwrite a running executable)"
Invoke-Adb push $Bin.FullName $UvNew
Invoke-Adb shell "chmod 755 $UvNew"

if (-not $SkipLibs) {
    Write-Host "==> stage libuv"
    Invoke-Adb push $LibUv $LibUvNew
    Invoke-Adb shell "chmod 755 $LibUvNew"
}

Write-Host "==> update S99myapp"
Invoke-Adb shell "mount -o remount,rw / 2>/dev/null; true"
Invoke-Adb push $InitScript "/etc/init.d/S99myapp"
Invoke-Adb shell "chmod 755 /etc/init.d/S99myapp"

Write-Host "==> install event reboot fallback"
Invoke-Adb push $DefaultConfig "/etc/default/fnirs"
Invoke-Adb shell "chmod 644 /etc/default/fnirs"

Write-Host "==> stop current backend before atomic install"
Invoke-Adb shell "/etc/init.d/S99myapp stop"

if (-not $SkipLibs) {
    Write-Host "==> atomically install libuv"
    Invoke-Adb shell "set -e; if [ -f $BoardGolgi/lib/libuv.so.1.0.0 ]; then cp -p $BoardGolgi/lib/libuv.so.1.0.0 $BoardGolgi/lib/libuv.so.1.0.0.previous; fi; mv -f $LibUvNew $BoardGolgi/lib/libuv.so.1.0.0; chmod 755 $BoardGolgi/lib/libuv.so.1.0.0; ln -sf libuv.so.1.0.0 $BoardGolgi/lib/libuv.so.1"
}

Write-Host "==> atomically install my-fnirs-uv"
Invoke-Adb shell "set -e; if [ -f $BoardGolgi/my-fnirs-uv ]; then cp -p $BoardGolgi/my-fnirs-uv $BoardGolgi/my-fnirs-uv.previous; fi; mv -f $UvNew $BoardGolgi/my-fnirs-uv; chmod 755 $BoardGolgi/my-fnirs-uv; sync"

$BoardMd5 = (Invoke-Adb shell "md5sum $BoardGolgi/my-fnirs-uv 2>/dev/null" |
    Out-String).Trim()
Write-Host "    board: $BoardMd5"
if (-not $BoardMd5 -or
    -not $BoardMd5.ToLowerInvariant().StartsWith($BinMd5)) {
    throw "board my-fnirs-uv MD5 does not match PC binary"
}

# The checked-in default deliberately selects event after every reboot. Start
# that known-good backend first so the LEDs/hardware remain owned while the
# new UV binary runs its isolated safe probe.
Write-Host "==> start event fallback"
Invoke-Adb shell "/etc/init.d/S99myapp start"

if (-not $NoActivate) {
    Write-Host "==> safe UV preflight (event remains active)"
    Invoke-Adb shell "/etc/init.d/S99myapp probe uv"

    Write-Host "==> activate UV full hardware/GATT profile"
    Invoke-Adb shell "/etc/init.d/S99myapp switch uv full"
} else {
    Write-Host "NoActivate — UV installed; event backend remains active."
}

if (-not $NoCheck) {
    Start-Sleep -Seconds 8
    Write-Host "==> check"
    Invoke-Adb shell "/etc/init.d/S99myapp status"
    $RuntimePid = (Invoke-Adb shell "cat /var/run/my-fnirs.pid 2>/dev/null" |
        Out-String).Trim()
    Invoke-Adb shell "pid=`$(cat /var/run/my-fnirs.pid 2>/dev/null); echo pid=`$pid exe=`$(readlink /proc/`$pid/exe 2>/dev/null); strings $BoardGolgi/my-fnirs-uv 2>/dev/null | grep 20260728 | tail -1; tail -80 $BoardGolgi/fnirs-uv.log 2>/dev/null | grep -E 'UV profile=|module ready:|event loop start|my-server build|UV GATT worker|dedicated UV worker' | tail -25"

    if (-not $NoActivate) {
        $GattReady = (Invoke-Adb shell "grep -F 'pid=$RuntimePid uv-stage: module gatt init ready' $BoardGolgi/uv-stage.log 2>/dev/null || true" |
            Out-String).Trim()
        if (-not $GattReady) {
            Write-Warning "UV process is alive, but its GATT module did not become ready; rolling back to event."
            Invoke-Adb shell "/etc/init.d/S99myapp switch event"
            throw "UV GATT readiness check failed; event backend restored"
        }
        Write-Host "    GATT: ready for current UV pid=$RuntimePid"
    }
}

Write-Host "Done: $BoardGolgi/my-fnirs-uv"
if ($NoActivate) {
    Write-Host "Active backend: event (requested with -NoActivate)"
} else {
    Write-Host "Active backend: uv/full for this boot"
}
Write-Host "Reboot fallback: event"
Write-Host "Recovery binary: $BoardGolgi/my-fnirs-uv.previous"
