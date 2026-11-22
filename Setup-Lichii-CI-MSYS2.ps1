[CmdletBinding()]
param(
    [string]$MsysRoot = "C:\msys64"
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Write-Section {
    param([Parameter(Mandatory)][string]$Text)
    Write-Host
    Write-Host "============================================================"
    Write-Host $Text -ForegroundColor Cyan
    Write-Host "============================================================"
}

function Find-MsysRoot {
    param([string]$Preferred)

    $candidates = @(
        $Preferred,
        "C:\msys64",
        (Join-Path $env:LOCALAPPDATA "Programs\msys64")
    ) | Select-Object -Unique

    foreach ($candidate in $candidates) {
        if ($candidate -and (Test-Path -LiteralPath (Join-Path $candidate "usr\bin\bash.exe"))) {
            return (Resolve-Path -LiteralPath $candidate).Path
        }
    }

    return $null
}

function Invoke-Ucrt64 {
    param(
        [Parameter(Mandatory)][string]$Command,
        [Parameter(Mandatory)][string]$Description
    )

    Write-Section $Description

    $oldMsystem = $env:MSYSTEM
    $oldChere = $env:CHERE_INVOKING
    $oldPathType = $env:MSYS2_PATH_TYPE

    try {
        $env:MSYSTEM = "UCRT64"
        $env:CHERE_INVOKING = "1"
        $env:MSYS2_PATH_TYPE = "inherit"

        & $script:Bash --login -c $Command
        $exitCode = $LASTEXITCODE

        if ($exitCode -ne 0) {
            throw "$Description failed with exit code $exitCode."
        }
    }
    finally {
        $env:MSYSTEM = $oldMsystem
        $env:CHERE_INVOKING = $oldChere
        $env:MSYS2_PATH_TYPE = $oldPathType
    }
}

Write-Section "Eden CI-compatible MSYS2 bootstrap"

$resolvedRoot = Find-MsysRoot -Preferred $MsysRoot

if (-not $resolvedRoot) {
    $winget = Get-Command winget.exe -ErrorAction SilentlyContinue
    if (-not $winget) {
        throw "MSYS2 was not found and winget.exe is unavailable. Install MSYS2 to C:\msys64, then run this script again."
    }

    Write-Host "MSYS2 was not found. Installing the current MSYS2 package with winget..."
    & $winget.Source install --exact --id MSYS2.MSYS2 `
        --accept-source-agreements `
        --accept-package-agreements `
        --disable-interactivity

    if ($LASTEXITCODE -ne 0) {
        throw "winget failed to install MSYS2. Exit code: $LASTEXITCODE"
    }

    $resolvedRoot = Find-MsysRoot -Preferred $MsysRoot
    if (-not $resolvedRoot) {
        throw "MSYS2 installation completed, but its root could not be located."
    }
}

$script:Bash = Join-Path $resolvedRoot "usr\bin\bash.exe"

Write-Host "MSYS2 root: $resolvedRoot"
Write-Host "Environment: UCRT64"

# Separate invocations are intentional. If the first pass updates the MSYS2 runtime,
# the next process starts against the newly installed runtime.
Invoke-Ucrt64 -Description "Updating MSYS2: pass 1" -Command @'
pacman -Syu --noconfirm
'@

Invoke-Ucrt64 -Description "Updating MSYS2: pass 2" -Command @'
pacman -Syu --noconfirm
'@

Invoke-Ucrt64 -Description "Installing CI base utilities" -Command @'
pacman -S --needed --noconfirm \
  git make autoconf libtool automake-wrapper patch jq pactoys ninja
'@

# This mirrors the current Eden Windows MSYS2 job's pacboy package list.
Invoke-Ucrt64 -Description "Installing Eden UCRT64 build dependencies" -Command @'
pacboy -S --needed --noconfirm \
  qt6-tools:p qt6-translations:p qt6-svg:p \
  7zip:p sccache:p cmake:p toolchain:p clang:p python-pip:p lld:p \
  openssl:p boost:p fmt:p lz4:p nlohmann-json:p zlib:p zstd:p \
  vulkan-memory-allocator:p vulkan-devel:p glslang:p \
  enet:p opus:p mbedtls:p libusb:p unordered_dense:p \
  jbigkit:p xz:p libdeflate:p bzip2:p lerc:p graphite2:p crt-git:p \
  libwebp:p libtiff:p brotli:p
'@

Invoke-Ucrt64 -Description "Verifying the reproduced environment" -Command @'
set -e
printf 'MSYSTEM=%s\n' "$MSYSTEM"
printf '\nGCC:\n'
gcc --version | head -n 1
printf '\nCMake:\n'
cmake --version | head -n 1
printf '\nNinja:\n'
ninja --version
printf '\n7-Zip:\n'
7z i | sed -n '1,2p'
'@

Write-Section "Environment ready"
Write-Host "Copy Build-Eden-UCRT64-GCC.cmd and Build-Eden-UCRT64-GCC.sh into an Eden source root."
Write-Host "Then double-click Build-Eden-UCRT64-GCC.cmd."
