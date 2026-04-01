<#
.SYNOPSIS
    Build both hardware variants and copy output binaries to docs/firmware/.

.DESCRIPTION
    Each variant is built into its own subdirectory (build_devkitc /
    build_jc3248w535) so incremental rebuilds work correctly without
    clobbering each other.

    After a successful build the following files are updated in docs/firmware/:
        bootloader.bin          (shared — taken from the last successful build)
        partition-table.bin     (shared — taken from the last successful build)
        nvs_blank.bin           (shared — must already exist in docs/firmware/)
        esp32_ssh_devkitc.bin   (DevKitC-1 application binary)
        esp32_ssh_screen.bin    (JC3248W535 application binary)

.PARAMETER Clean
    Delete both build directories before building (forces a full rebuild).
#>
param(
    [switch]$Clean
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
        OutputBin = 'esp32_ssh_devkitc.bin'
    },
    [PSCustomObject]@{
        Name      = 'jc3248w535'
        Config    = 'sdkconfig.jc3248w535'
        BuildDir  = 'build_jc3248w535'
        OutputBin = 'esp32_ssh_screen.bin'
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

    & idf.py `
        --build-dir $variant.BuildDir `
        "-DSDKCONFIG_DEFAULTS=$defaults" `
        "-DSDKCONFIG=$($variant.BuildDir)/sdkconfig" `
        build

    if ($LASTEXITCODE -ne 0) {
        throw "idf.py build failed for variant '$($variant.Name)' (exit $LASTEXITCODE)"
    }
}

function Copy-Artifacts([PSCustomObject]$variant, [bool]$copyShared) {
    $buildDir = Join-Path $PSScriptRoot $variant.BuildDir

    # Application binary
    $appSrc = Join-Path $buildDir 'esp32_ssh_led.bin'
    $appDst = Join-Path $FirmwareDir $variant.OutputBin
    Copy-Item $appSrc $appDst -Force
    Write-Host "  Copied $($variant.OutputBin)" -ForegroundColor Green

    if ($copyShared) {
        # Bootloader and partition table are hardware-independent
        $bootSrc  = Join-Path $buildDir 'bootloader\bootloader.bin'
        $partSrc  = Join-Path $buildDir 'partition_table\partition-table.bin'
        Copy-Item $bootSrc  (Join-Path $FirmwareDir 'bootloader.bin')       -Force
        Copy-Item $partSrc  (Join-Path $FirmwareDir 'partition-table.bin')  -Force
        Write-Host '  Copied bootloader.bin' -ForegroundColor Green
        Write-Host '  Copied partition-table.bin' -ForegroundColor Green
    }
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

# Optional clean
if ($Clean) {
    foreach ($v in $variants) {
        if (Test-Path $v.BuildDir) {
            Write-Step "Removing $($v.BuildDir)"
            Remove-Item $v.BuildDir -Recurse -Force
        }
    }
}

# Build each variant
$first = $true
foreach ($v in $variants) {
    Invoke-IdfBuild -variant $v
    Copy-Artifacts  -variant $v -copyShared $first
    $first = $false
}

Write-Host "`nAll variants built successfully.`n" -ForegroundColor Green
Write-Host "Output in: $FirmwareDir" -ForegroundColor White
Get-ChildItem $FirmwareDir | Select-Object Name, Length, LastWriteTime | Format-Table -AutoSize
