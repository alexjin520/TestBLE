param(
    [string]$Serial = "ingenic-x2600-halley7",
    [string]$AdbExe = "$env:LOCALAPPDATA\Android\Sdk\platform-tools\adb.exe",
    [switch]$KeepFile
)

$ErrorActionPreference = "Stop"

function Test-RecordHeader {
    param(
        [byte[]]$Data,
        [int]$Offset
    )

    return $Offset -ge 0 -and
        ($Offset + 7) -lt $Data.Length -and
        $Data[$Offset] -eq 0xAA -and
        $Data[$Offset + 1] -eq 0x55 -and
        $Data[$Offset + 6] -eq 0xA5 -and
        $Data[$Offset + 7] -eq 0x5A
}

function Read-UInt64BigEndian {
    param(
        [byte[]]$Data,
        [int]$Offset
    )

    [uint64]$value = 0
    for ($i = 0; $i -lt 8; $i++) {
        $value = [uint64](($value * 256) + [uint64]$Data[$Offset + $i])
    }
    return $value
}

if (-not (Test-Path $AdbExe)) {
    throw "adb not found: $AdbExe"
}

$devicePattern = "^$([regex]::Escape($Serial))\s+device\b"
$deviceLines = & $AdbExe devices
if (-not ($deviceLines | Select-String -Pattern $devicePattern)) {
    throw "fNIRS board is not connected: $Serial"
}

$name = (& $AdbExe -s $Serial shell "cat /app_data/ble_tx/.last 2>/dev/null" |
    Out-String).Trim()
if (-not $name) {
    throw "No finalized recording marker. Start sampling, stop it, then run this script."
}
if ($name -notmatch '^[A-Za-z0-9_.-]+$') {
    throw "Unsafe recording filename returned by board: $name"
}

$tempName = "fnirs-timestamp-$([Guid]::NewGuid().ToString('N')).bin"
$tempPath = Join-Path ([IO.Path]::GetTempPath()) $tempName

try {
    & $AdbExe -s $Serial pull "/app_data/$name" $tempPath
    if ($LASTEXITCODE -ne 0) {
        throw "adb pull failed with exit code $LASTEXITCODE"
    }

    [byte[]]$bytes = [IO.File]::ReadAllBytes($tempPath)
    if ($bytes.Length -lt 64) {
        throw "Recording is too short: $($bytes.Length) bytes"
    }
    if (-not (Test-RecordHeader $bytes 0)) {
        throw "Recording header AA55...A55A was not found at offset 0"
    }
    if ($bytes[8] -ne 0xBB -or $bytes[9] -ne 0x66) {
        throw "LED-array metadata BB66 was not found in the first record"
    }

    $ledCount = [int]$bytes[10]
    if ($ledCount -lt 1 -or $ledCount -gt 36) {
        throw "Invalid LED count in recording metadata: $ledCount"
    }

    $metadataPayloadSize = 4 + 4 * $ledCount
    $firstSampleHeader = 8 + $metadataPayloadSize
    $samplePayloadSize = 920
    $sampleRecordSize = 8 + $samplePayloadSize
    $secondSampleHeader = $firstSampleHeader + $sampleRecordSize

    if (-not (Test-RecordHeader $bytes $firstSampleHeader)) {
        throw "First sample record header was not found at offset $firstSampleHeader"
    }
    if (-not (Test-RecordHeader $bytes $secondSampleHeader)) {
        throw "Second 920-byte sample record was not found at offset $secondSampleHeader"
    }

    $timestamp1 = Read-UInt64BigEndian $bytes ($firstSampleHeader + 8)
    $timestamp2 = Read-UInt64BigEndian $bytes ($secondSampleHeader + 8)
    $delta = [int64]$timestamp2 - [int64]$timestamp1
    $status = if ($timestamp1 -gt 0 -and $timestamp2 -gt $timestamp1) {
        "PASS"
    } else {
        "FAIL"
    }

    [pscustomobject]@{
        Status       = $status
        File         = $name
        FileBytes    = $bytes.Length
        LedCount     = $ledCount
        Timestamp1Ms = $timestamp1
        Timestamp2Ms = $timestamp2
        DeltaMs      = $delta
    } | Format-List

    if ($status -ne "PASS") {
        throw "Timestamp is zero or not increasing"
    }
} finally {
    if ($KeepFile) {
        Write-Host "Downloaded file kept at: $tempPath"
    } elseif (Test-Path $tempPath) {
        Remove-Item -LiteralPath $tempPath -Force
    }
}
