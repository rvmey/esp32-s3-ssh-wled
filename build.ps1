<#
.SYNOPSIS
    Build both hardware variants and copy output binaries to docs/firmware/.

.DESCRIPTION
    Each variant is built into its own subdirectory (build_devkitc /
    build_jc3248w535) so incremental rebuilds work correctly without
    clobbering each other.

    After a successful build the following files are updated in docs/firmware/:
        bootloader-<chip>.bin   (chip-specific bootloader)
        partition-table-<chip>.bin (chip-specific partition table)
        nvs_blank.bin           (shared — must already exist in docs/firmware/)
        esp32_ssh_devkitc.bin   (DevKitC-1 application binary)
        esp32_ssh_screen.bin    (JC3248W535 application binary)

.PARAMETER Clean
    Delete both build directories before building (forces a full rebuild).

.PARAMETER Variant
    One or more variant names to build (devkitc, jc3248w535, bike_tracker,
    picture_frame).  Defaults to all variants.  Examples:
        .\build.ps1 -Variant devkitc
        .\build.ps1 -Variant devkitc, jc3248w535
#>
param(
    [switch]$Clean,
    [string[]]$Variant = @()
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# ---------------------------------------------------------------------------
# Variants
# ---------------------------------------------------------------------------
$variants = @(
    [PSCustomObject]@{
        Name      = 'devkitc'
        Config    = 'sdkconfig.devkitc'
        BuildDir  = 'build_devkitc'
        Chip      = 'esp32s3'
        BootloaderBin = 'bootloader-esp32s3.bin'
        PartitionBin  = 'partition-table-esp32s3.bin'
        OutputBin = 'esp32_ssh_devkitc.bin'
    },
    [PSCustomObject]@{
        Name      = 'jc3248w535'
        Config    = 'sdkconfig.jc3248w535'
        BuildDir  = 'build_jc3248w535'
        Chip      = 'esp32s3'
        BootloaderBin = 'bootloader-esp32s3.bin'
        PartitionBin  = 'partition-table-esp32s3.bin'
        OutputBin = 'esp32_ssh_screen.bin'
    },
    [PSCustomObject]@{
        Name         = 'bike_tracker'
        Config       = 'sdkconfig.bike_tracker'
        BuildDir     = 'build_bike_tracker'
        Chip         = 'esp32s3'
        BootloaderBin = 'bootloader-esp32s3.bin'
        OutputBin    = 'esp32_bike_tracker.bin'
        PartitionBin = 'partition-table-bike_tracker.bin'
    },
    [PSCustomObject]@{
        Name      = 'picture_frame'
        Config    = 'sdkconfig.picture_frame'
        BuildDir  = 'build_picture_frame'
        Chip      = 'esp32s3'
        BootloaderBin = 'bootloader-esp32s3.bin'
        PartitionBin  = 'partition-table-esp32s3.bin'
        OutputBin = 'esp32_picture_frame.bin'
    },
    [PSCustomObject]@{
        Name      = 'tcmd_atom_echo'
        Config    = 'sdkconfig.tcmd_atom_echo'
        BuildDir  = 'build_tcmd_atom_echo'
        Chip      = 'esp32'
        BootloaderBin = 'bootloader-esp32.bin'
        PartitionBin  = 'partition-table-esp32.bin'
        OutputBin = 'esp32_tcmd_atom_echo.bin'
        Target    = 'esp32'          # classic ESP32-PICO-D4, not ESP32-S3
    }
)

$FirmwareDir = Join-Path $PSScriptRoot 'docs\firmware'

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------
function Write-Step([string]$msg) {
    Write-Host "`n>>> $msg" -ForegroundColor Cyan
}

function Invoke-IdfBuild([PSCustomObject]$variant) {
    Write-Step "Building variant: $($variant.Name)"

    # idf.py expects SDKCONFIG_DEFAULTS as a cmake variable.
    # Semicolons inside a quoted string are literal in PowerShell, which is
    # exactly what cmake needs for its list separator.
    $defaults = "sdkconfig.defaults;$($variant.Config)"

    # idf.py (python) writes informational messages to stderr.  Under
    # $ErrorActionPreference = 'Stop' that triggers a terminating error, so
    # temporarily relax it and rely solely on $LASTEXITCODE for failure detection.
    $savedEAP = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    $idfArgs = @(
        '--build-dir', $variant.BuildDir,
        "-DSDKCONFIG_DEFAULTS=$defaults",
        "-DSDKCONFIG=$($variant.BuildDir)/sdkconfig"
    )
    if ($variant.PSObject.Properties['Target']) {
        $idfArgs += "-DIDF_TARGET=$($variant.Target)"
    }
    $idfArgs += 'build'
    & idf.py @idfArgs
    $ErrorActionPreference = $savedEAP

    if ($LASTEXITCODE -ne 0) {
        throw "idf.py build failed for variant '$($variant.Name)' (exit $LASTEXITCODE)"
    }
}

function Test-VariantNeedsRebuild([PSCustomObject]$variant) {
    # Always rebuild after -Clean (build dir won't exist)
    $buildDir = Join-Path $PSScriptRoot $variant.BuildDir
    $appBin   = Join-Path $buildDir 'esp32_ssh_led.bin'
    if (-not (Test-Path $appBin)) { return $true }

    $builtAt = (Get-Item $appBin).LastWriteTime

    # Source files that affect this variant
    $sources = @(
        (Join-Path $PSScriptRoot 'CMakeLists.txt'),
        (Join-Path $PSScriptRoot 'sdkconfig.defaults'),
        (Join-Path $PSScriptRoot $variant.Config),
        (Join-Path $PSScriptRoot 'main\CMakeLists.txt')
    )
    # All files under main/ and managed_components/ and patches/
    foreach ($dir in @('main', 'managed_components', 'patches')) {
        $full = Join-Path $PSScriptRoot $dir
        if (Test-Path $full) {
            Get-ChildItem $full -Recurse -File | ForEach-Object { $sources += $_.FullName }
        }
    }

    foreach ($src in $sources) {
        if ((Test-Path $src) -and (Get-Item $src).LastWriteTime -gt $builtAt) {
            Write-Host "  Rebuild triggered by: $($src.Replace($PSScriptRoot + '\', ''))" -ForegroundColor DarkYellow
            return $true
        }
    }
    return $false
}

function Copy-Artifacts([PSCustomObject]$variant) {
    $buildDir = Join-Path $PSScriptRoot $variant.BuildDir

    # Application binary
    $appSrc = Join-Path $buildDir 'esp32_ssh_led.bin'
    $appDst = Join-Path $FirmwareDir $variant.OutputBin
    Copy-Item $appSrc $appDst -Force
    Write-Host "  Copied $($variant.OutputBin)" -ForegroundColor Green

    # Bootloader and partition table must be chip-specific (ESP32 vs ESP32-S3)
    $bootSrc = Join-Path $buildDir 'bootloader\bootloader.bin'
    Copy-Item $bootSrc (Join-Path $FirmwareDir $variant.BootloaderBin) -Force
    Write-Host "  Copied $($variant.BootloaderBin)" -ForegroundColor Green

    $partSrc = Join-Path $buildDir 'partition_table\partition-table.bin'
    Copy-Item $partSrc (Join-Path $FirmwareDir $variant.PartitionBin) -Force
    Write-Host "  Copied $($variant.PartitionBin)" -ForegroundColor Green
}

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

# Verify idf.py is on PATH
if (-not (Get-Command idf.py -ErrorAction SilentlyContinue)) {
    Write-Error @'
idf.py not found on PATH.
Run this script from an ESP-IDF environment (e.g. the ESP-IDF PowerShell
shortcut, or after calling: . $IDF_PATH\export.ps1
'@
    exit 1
}

# Ensure the firmware output directory exists
if (-not (Test-Path $FirmwareDir)) {
    New-Item -ItemType Directory -Path $FirmwareDir | Out-Null
}

# Apply patches to managed_components/ (fixes wolfssl 5.8.2 / IDF v6.0 incompatibilities).
# The script is idempotent — safe to run on every build.
Write-Step "Applying managed_components patches"
& (Join-Path $PSScriptRoot 'apply-patches.ps1')

# Optional clean
if ($Clean) {
    foreach ($v in $variants) {
        if (Test-Path $v.BuildDir) {
            Write-Step "Removing $($v.BuildDir)"
            Remove-Item $v.BuildDir -Recurse -Force
        }
    }
}

# Filter to requested variants (if -Variant was supplied)
if ($Variant.Count -gt 0) {
    $validNames = $variants | Select-Object -ExpandProperty Name
    foreach ($requested in $Variant) {
        if ($requested -notin $validNames) {
            Write-Error "Unknown variant '$requested'. Valid values: $($validNames -join ', ')"
            exit 1
        }
    }
    $variants = $variants | Where-Object { $_.Name -in $Variant }
    # Ensure $variants stays an array even when filtering to a single item
    $variants = @($variants)
}

# Build each variant (skip compiling if sources are unchanged, but refresh artifacts)
$skipped = 0
foreach ($v in $variants) {
    if (-not $Clean -and -not (Test-VariantNeedsRebuild -variant $v)) {
        Write-Host "`n>>> Skipping compile for variant '$($v.Name)' -- no source changes detected" -ForegroundColor DarkGray
        Copy-Artifacts -variant $v
        $skipped++
        continue
    }
    Invoke-IdfBuild -variant $v
    Copy-Artifacts  -variant $v
}

if ($skipped -eq $variants.Count) {
    Write-Host "`nAll variants are up-to-date -- nothing to build.`n" -ForegroundColor Green
} else {
    Write-Host "`nBuild complete.`n" -ForegroundColor Green
}
Write-Host "Output in: $FirmwareDir" -ForegroundColor White
Get-ChildItem $FirmwareDir | Select-Object Name, Length, LastWriteTime | Format-Table -AutoSize
