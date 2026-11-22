[CmdletBinding()]
param(
    [ValidateRange(1, 64)][int]$Jobs = 8,
    [string]$BuildDir = ""
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Section([string]$Message) {
    Write-Host ""
    Write-Host ("=" * 60)
    Write-Host $Message
    Write-Host ("=" * 60)
}

function Find-BuildTool([string]$Name, [string]$VisualStudioRelativePath) {
    $command = Get-Command $Name -ErrorAction SilentlyContinue
    if ($command) { return $command.Source }

    if ($env:VSINSTALLDIR) {
        $bundled = Join-Path $env:VSINSTALLDIR $VisualStudioRelativePath
        if (Test-Path -LiteralPath $bundled -PathType Leaf) { return $bundled }
    }
    throw "Cannot find $Name. Install CMake/Ninja in Visual Studio, or add them to PATH."
}

function Run([string]$Executable, [string[]]$ToolArguments) {
    & $Executable @ToolArguments
    if ($LASTEXITCODE -ne 0) {
        throw "Command failed with exit code $LASTEXITCODE : $Executable"
    }
}

$root = (Resolve-Path -LiteralPath $PSScriptRoot).Path
if (-not $BuildDir) {
    $build = Join-Path $root "build-msvc"
} elseif ([IO.Path]::IsPathRooted($BuildDir)) {
    $build = [IO.Path]::GetFullPath($BuildDir)
} else {
    $build = [IO.Path]::GetFullPath((Join-Path $root $BuildDir))
}

$compiler = Get-Command cl.exe -ErrorAction SilentlyContinue
if (-not $compiler -or $env:VSCMD_ARG_TGT_ARCH -ne "x64") {
    throw "The x64 MSVC developer environment is required. Run Build-Eden.cmd."
}
$cmake = Find-BuildTool "cmake.exe" "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
$ninja = Find-BuildTool "ninja.exe" "Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"

# Read all output rather than terminating the native command's pipeline early.
# Under StrictMode, $LASTEXITCODE is not defined until a native invocation
# actually sets it, and Select-Object -First can stop the pipeline before then.
$versionOutput = @(& $cmake --version)
$versionLine = $versionOutput | Select-Object -First 1
if ($versionLine -notmatch "(\d+\.\d+\.\d+)") {
    throw "Unable to determine the CMake version."
}
if ([version]$Matches[1] -lt [version]"3.31.0") {
    throw "CMake 3.31 or later is required; found $versionLine."
}

Section "Eden x64 MSVC build with native PDB symbols"
Write-Host "Source:   $root"
Write-Host "Build:    $build"
Write-Host "Compiler: $($compiler.Source)"
Write-Host "CMake:    $cmake ($versionLine)"
Write-Host "Ninja:    $ninja"
Write-Host "Jobs:     $Jobs"

# The source CMakeLists fixes CPM_SOURCE_CACHE at <source>/.cache/cpm.
# Preserve a GCC-populated cache and restore a pre-GCC MSVC cache if present.
$cacheParent = Join-Path $root ".cache"
$cache = Join-Path $cacheParent "cpm"
$gccMarker = Join-Path $cache ".eden-ucrt64-gcc-cache"
New-Item -ItemType Directory -Force -Path $cacheParent | Out-Null

if (Test-Path -LiteralPath $gccMarker -PathType Leaf) {
    $stamp = Get-Date -Format "yyyyMMdd-HHmmss-fffffff"
    $backup = Join-Path $cacheParent "cpm-before-msvc-$stamp"
    Section "Preserving GCC-specific CPM cache"
    Move-Item -LiteralPath $cache -Destination $backup
    Write-Host "GCC cache: $backup"

    $priorMsvcCache = Get-ChildItem -LiteralPath $cacheParent -Directory -Filter "cpm-before-ucrt64-gcc-*" |
        Sort-Object LastWriteTimeUtc -Descending |
        Select-Object -First 1
    if ($priorMsvcCache) {
        Move-Item -LiteralPath $priorMsvcCache.FullName -Destination $cache
        Write-Host "Restored earlier pre-GCC cache: $($priorMsvcCache.FullName)"
    }
}

New-Item -ItemType Directory -Force -Path $cache | Out-Null
New-Item -ItemType File -Force -Path (Join-Path $cache ".eden-msvc-cache") | Out-Null

# Never delete user build artifacts when changing generators.
$cacheFile = Join-Path $build "CMakeCache.txt"
if (Test-Path -LiteralPath $cacheFile -PathType Leaf) {
    $cached = Get-Content -LiteralPath $cacheFile -Raw
    if ($cached -notmatch "(?m)^CMAKE_GENERATOR:INTERNAL=Ninja\r?$") {
        throw "An incompatible CMake generator already uses $build. Supply -BuildDir with a new directory."
    }
    if ($cached -match "(?m)^CMAKE_CXX_COMPILER:(?:FILEPATH|STRING)=(.+)\r?$") {
        if ($Matches[1] -match "g\+\+|clang\+\+|ucrt64") {
            throw "A non-MSVC compiler was cached in $build. Supply -BuildDir with a new directory."
        }
    }
}

# /Z7 embeds compiler debug information in .obj files, avoiding shared
# compiler-PDB contention (C1041). /DEBUG:FULL creates a native eden.pdb.
# RelWithDebInfo retains optimized Release-style code generation.
$configure = @(
    "-S", $root,
    "-B", $build,
    "-G", "Ninja",
    "-DCMAKE_MAKE_PROGRAM:FILEPATH=$ninja",
    "-DCMAKE_C_COMPILER:FILEPATH=$($compiler.Source)",
    "-DCMAKE_CXX_COMPILER:FILEPATH=$($compiler.Source)",
    "-DCMAKE_BUILD_TYPE:STRING=RelWithDebInfo",
    "-DCMAKE_MSVC_DEBUG_INFORMATION_FORMAT:STRING=Embedded",
    "-DCMAKE_EXE_LINKER_FLAGS_RELWITHDEBINFO:STRING=/DEBUG:FULL /INCREMENTAL:NO /OPT:REF /OPT:ICF",
    "-DCMAKE_SHARED_LINKER_FLAGS_RELWITHDEBINFO:STRING=/DEBUG:FULL /INCREMENTAL:NO /OPT:REF /OPT:ICF",
    "-DBUILD_TESTING=OFF",
    "-DYUZU_TESTS=OFF",
    "-DYUZU_USE_BUNDLED_QT=ON",
    "-DYUZU_USE_QT_MULTIMEDIA=OFF",
    "-DYUZU_USE_QT_WEB_ENGINE=OFF",
    "-DENABLE_QT_TRANSLATION=ON",
    "-DENABLE_UPDATE_CHECKER=OFF",
    "-DUSE_CCACHE=OFF",
    "-DUSE_DISCORD_PRESENCE=ON",
    "-DENABLE_LTO=OFF",
    "-DYUZU_USE_BUNDLED_SIRIT=ON",
    "-DYUZU_USE_BUNDLED_FFMPEG=ON",
    "-DYUZU_DISABLE_LLVM=ON",
    "-DYUZU_STATIC_BUILD=ON",
    "-DYUZU_CMD=ON",
    "-DYUZU_ROOM_STANDALONE=OFF",
    "-DBUILD_ID=amd64",
    "-DNIGHTLY_BUILD=ON",
    "-DYUZU_USE_BUNDLED_SDL3=ON",
    "-DYUZU_USE_BUNDLED_SDL2=ON",
    "-DYUZU_USE_BUNDLED_OPENSSL=ON",
    "-DCMAKE_INSTALL_PREFIX:PATH=$build/install",
    "-DCMAKE_FIND_USE_INSTALL_PREFIX=FALSE"
)

Section "Configuring MSVC + Ninja (RelWithDebInfo, /Z7, /DEBUG:FULL)"
Run $cmake $configure

Section "Building Eden"
Run $cmake @("--build", $build, "--parallel", "$Jobs")

$exe = Join-Path $build "bin\lichii.exe"
$pdb = Join-Path $build "bin\lichii.pdb"
if (-not (Test-Path -LiteralPath $exe -PathType Leaf)) {
    throw "Build completed without the expected executable: $exe"
}
if (-not (Test-Path -LiteralPath $pdb -PathType Leaf)) {
    throw "Build completed without the native WinDbg PDB: $pdb"
}

Section "BUILD SUCCEEDED"
Write-Host "Executable: $exe"
Write-Host "PDB:        $pdb"
Write-Host "WinDbg:     .sympath+ $build\bin"
Write-Host "            .reload /f lichii.exe"
Write-Host "            !analyze -v"
Write-Host "Use the executable and PDB from the same build when investigating a dump."
