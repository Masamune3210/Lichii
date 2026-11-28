# Launch this fork of Lichii with per-run native shader and GPU diagnostics.
# Example: .\Run-Lichii-ShaderDump.ps1 -Label MarioKartWorld
# Rebuild first, if needed: .\Run-Lichii-ShaderDump.ps1 -Build -Label MarioKartWorld
[CmdletBinding()]
param(
    [switch]$Build,
    [ValidateRange(1, 64)][int]$Jobs = 8,
    [string]$BuildDir = 'build-msvc',
    [string]$Executable,
    [string]$DumpRoot = 'G:\Users\Johnny\Downloads\2\LichiiDumps',
    [ValidatePattern('^[A-Za-z0-9][A-Za-z0-9_.-]{0,63}$')]
    [string]$Label = 'Lichii',
    [string]$GamePath,
    [switch]$SampledFrame,
    [switch]$FullDiagnostics,
    [ValidateRange(1, 100000)][int]$GpuTracePresents = 180,
    [switch]$NoZip
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Resolve-SourcePath([string]$Path) {
    if ([IO.Path]::IsPathRooted($Path)) {
        return [IO.Path]::GetFullPath($Path)
    }
    return [IO.Path]::GetFullPath((Join-Path $PSScriptRoot $Path))
}

$buildPath = Resolve-SourcePath $BuildDir
if ($Build -and $Executable) {
    throw '-Build and -Executable are mutually exclusive. Supply -BuildDir if necessary.'
}
if ($Build) {
    $builder = Join-Path $PSScriptRoot 'Build-Lichii.cmd'
    if (-not (Test-Path -LiteralPath $builder -PathType Leaf)) {
        throw "Build script missing: $builder"
    }
    $oldNoPause = [Environment]::GetEnvironmentVariable('EDEN_NO_PAUSE', 'Process')
    try {
        [Environment]::SetEnvironmentVariable('EDEN_NO_PAUSE', '1', 'Process')
        & $builder -Jobs $Jobs -BuildDir $buildPath
        if ($LASTEXITCODE -ne 0) {
            throw "Lichii MSVC build failed with exit code $LASTEXITCODE"
        }
    }
    finally {
        [Environment]::SetEnvironmentVariable('EDEN_NO_PAUSE', $oldNoPause, 'Process')
    }
}

$exe = if ($Executable) {
    Resolve-SourcePath $Executable
} else {
    Join-Path $buildPath 'bin\lichii.exe'
}
if (-not (Test-Path -LiteralPath $exe -PathType Leaf)) {
    throw "Lichii executable not found: $exe. Build with -Build or supply -Executable."
}

$launchArgs = @()
if ($GamePath) {
    if (-not (Test-Path -LiteralPath $GamePath)) {
        throw "Game path not found: $GamePath"
    }
    $launchArgs += (Resolve-Path -LiteralPath $GamePath).ProviderPath
}

$dumpRootFull = [IO.Path]::GetFullPath($DumpRoot)
New-Item -ItemType Directory -Path $dumpRootFull -Force | Out-Null
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$runPath = Join-Path $dumpRootFull ("{0}-{1}-{2}" -f $stamp, $Label, [guid]::NewGuid().ToString('N').Substring(0, 8))
$shaderDir = Join-Path $runPath 'shaders'
$meshDir = Join-Path $runPath 'mesh-spv'
$frameDir = Join-Path $runPath 'frames'
$gpuTraceDir = Join-Path $runPath 'gpu-traces'
foreach ($folder in @($runPath, $shaderDir, $meshDir, $frameDir, $gpuTraceDir)) {
    New-Item -ItemType Directory -Path $folder -Force | Out-Null
}

# These paths are verified against Masamune3210/Lichii's nx2 source. Capture
# native shader bytes even if lowering fails; mesh SPIR-V and pipeline builds
# are complementary diagnostics, not replacements for original instructions.
# Shader and CB1 snapshots are the default. Other diagnostics remain opt-in
# because synchronous pipeline trace writes and mesh SPIR-V dumps add I/O.
# Null values explicitly unset inherited instrumentation for this child process.
$vars = [ordered]@{
    EDEN_NX2_SHADER_DUMP          = $shaderDir
    EDEN_NX2_SHADER_CONTEXT_DUMP  = $shaderDir
    EDEN_NX2_MESH_EMULATION_DUMP  = $(if ($FullDiagnostics) { $meshDir } else { $null })
    EDEN_NX2_PIPELINE_BUILD_TRACE = $(if ($FullDiagnostics) { (Join-Path $runPath 'pipeline-builds.txt') } else { $null })
    EDEN_NX2_FRAME_DUMP           = $(if ($FullDiagnostics) { $frameDir } else { $null })
    EDEN_NX2_FRAME_DUMP_AT        = $(if ($FullDiagnostics) { 'file' } else { $null })
    EDEN_NX2_FRAME_DUMP_SAMPLED   = $(if ($FullDiagnostics -and $SampledFrame) { '1' } else { $null })
    EDEN_NX2_GPU_TRACE            = $(if ($FullDiagnostics) { $gpuTraceDir } else { $null })
    EDEN_NX2_GPU_TRACE_PRESENTS   = $(if ($FullDiagnostics) { "$GpuTracePresents" } else { $null })
}
$previous = @{}
foreach ($name in $vars.Keys) {
    $previous[$name] = [Environment]::GetEnvironmentVariable($name, 'Process')
    [Environment]::SetEnvironmentVariable($name, $vars[$name], 'Process')
}

$started = [datetime]::UtcNow
$exitCode = 1
try {
    Write-Host ''
    Write-Host "Lichii:       $exe"
    Write-Host "Run folder:   $runPath"
    Write-Host "Native code:  $shaderDir"
    Write-Host "Runtime CB1:  $shaderDir (*.context.json and *.cb1.bin)"
    if ($FullDiagnostics) {
        Write-Host "Mesh SPIR-V:  $meshDir"
        Write-Host "Build trace:  $($vars.EDEN_NX2_PIPELINE_BUILD_TRACE)"
        Write-Host 'Frame capture armed; trigger from a second PowerShell terminal:'
        Write-Host "  New-Item -ItemType File -Force -Path '$frameDir\capture.now'"
        Write-Host 'GPU trace armed; trigger from a second PowerShell terminal:'
        Write-Host "  New-Item -ItemType File -Force -Path '$gpuTraceDir\record.now'"
    } else {
        Write-Host 'Extra diagnostics: off (enable with -FullDiagnostics)'
    }
    Write-Host 'Run any games in the Lichii GUI. Each shader JSON includes its title ID.'
    Write-Host 'Close Lichii normally to write a manifest and ZIP of the captured files.'
    Write-Host ''

    # Keep Lichii's expected working directory for its Qt files and settings.
    Push-Location -LiteralPath (Split-Path -Parent $exe)
    try {
        & $exe @launchArgs
        $exitCode = $LASTEXITCODE
    }
    finally {
        Pop-Location
    }
}
finally {
    try {
        $metadataFiles = @(Get-ChildItem -LiteralPath $shaderDir -File -Filter '*.json' -ErrorAction SilentlyContinue |
            Where-Object { $_.Name -notlike '*.context.json' })
        $contextFiles = @(Get-ChildItem -LiteralPath $shaderDir -File -Filter '*.context.json' -ErrorAction SilentlyContinue)
        $titleIds = @(
            foreach ($file in @($metadataFiles) + @($contextFiles)) {
                try {
                    $capture = Get-Content -LiteralPath $file.FullName -Raw | ConvertFrom-Json
                    if ($capture.title_id) { [string]$capture.title_id }
                }
                catch {
                    Write-Warning "Skipping malformed shader metadata: $($file.Name)"
                }
            }
        )
        # Pair exact native bytes (SHA-256) with the optional bound CB1 contents.
        # Unlike Lichii's program_key, this is a portable identity the target
        # preflight can compare against both runtime dumps and RomFS sidecars.
        $sha256Cache = @{}
        $runtimeIndex = @(
            foreach ($file in $contextFiles) {
                try {
                    $capture = Get-Content -LiteralPath $file.FullName -Raw | ConvertFrom-Json
                    $codePath = Join-Path $shaderDir ([string]$capture.source_bin)
                    if (-not (Test-Path -LiteralPath $codePath -PathType Leaf)) {
                        Write-Warning "Runtime context has no matching native program: $($file.Name)"
                        continue
                    }
                    if (-not $sha256Cache.ContainsKey($codePath)) {
                        $sha256Cache[$codePath] = (Get-FileHash -LiteralPath $codePath -Algorithm SHA256).Hash.ToLowerInvariant()
                    }
                    $cb1Path = if ($capture.cb1_file) { Join-Path $shaderDir ([string]$capture.cb1_file) } else { $null }
                    $cb1Present = [bool]($cb1Path -and (Test-Path -LiteralPath $cb1Path -PathType Leaf))
                    [ordered]@{
                        titleId = [string]$capture.title_id
                        stage = [string]$capture.stage
                        sourceBin = [string]$capture.source_bin
                        sourceSha256 = $sha256Cache[$codePath]
                        contextFile = $file.Name
                        cb1Status = [string]$capture.cb1_status
                        cb1File = $(if ($cb1Present) { [string]$capture.cb1_file } else { $null })
                        cb1Bytes = $(if ($cb1Present) { (Get-Item -LiteralPath $cb1Path).Length } else { 0 })
                    }
                }
                catch {
                    Write-Warning "Cannot index runtime context $($file.Name): $_"
                }
            }
        )
        ConvertTo-Json -InputObject @($runtimeIndex) -Depth 5 |
            Set-Content -LiteralPath (Join-Path $runPath 'runtime-context-index.json') -Encoding utf8
        # Sort-Object emits no output for zero title IDs and a scalar for
        # exactly one. Force the result back into an array under StrictMode
        # so .Count and -join work for zero, one, or multiple titles.
        $titleIds = @($titleIds | Sort-Object -Unique)
        $revision = $null
        if (Get-Command git -ErrorAction SilentlyContinue) {
            $rawRevision = @(& git -C $PSScriptRoot rev-parse --verify HEAD 2>$null)
            if ($LASTEXITCODE -eq 0 -and $rawRevision.Count -gt 0) {
                $revision = [string]$rawRevision[0]
            }
        }
        $manifest = [ordered]@{
            schema = 'lichii-native-shader-run-v1'
            repository = 'Masamune3210/Lichii'
            revision = $revision
            executable = $exe
            label = $Label
            gameArgument = $launchArgs
            startedUtc = $started.ToString('o')
            finishedUtc = [datetime]::UtcNow.ToString('o')
            exitCode = $exitCode
            nativeShaderCaptures = $metadataFiles.Count
            runtimeContextCaptures = $contextFiles.Count
            recordedCb1Contexts = @($runtimeIndex | Where-Object { $_.cb1Status -in @('captured', 'captured-prefix') }).Count
            titleIds = @($titleIds)
            environment = $vars
        }
        $manifest | ConvertTo-Json -Depth 6 |
            Set-Content -LiteralPath (Join-Path $runPath 'capture-manifest.json') -Encoding utf8

        if (-not $NoZip) {
            Add-Type -AssemblyName System.IO.Compression
            Add-Type -AssemblyName System.IO.Compression.FileSystem -ErrorAction SilentlyContinue
            $zipPath = "$runPath.zip"
            # SmallestSize is the highest standard Deflate effort, while
            # remaining compatible with .NET ZipArchive in the Ryujinx tests.
            [IO.Compression.ZipFile]::CreateFromDirectory(
                $runPath, $zipPath, [IO.Compression.CompressionLevel]::SmallestSize, $false)

            # GitHub warns at 50 MB per blob. Split only after compression,
            # and only if the finished ZIP exceeds 45,000,000 bytes. These
            # are raw consecutive ZIP byte ranges, not PKZIP .z01 volumes.
            # Their concatenation is an ordinary ZIP with unchanged offsets.
            $partLimit = 45000000L
            $archiveBytes = (Get-Item -LiteralPath $zipPath).Length
            if ($archiveBytes -gt $partLimit) {
                $parts = [System.Collections.Generic.List[string]]::new()
                $buffer = [byte[]]::new(1048576)
                $inputZip = [IO.File]::OpenRead($zipPath)
                try {
                    try {
                        while ($inputZip.Position -lt $inputZip.Length) {
                            $partNumber = $parts.Count + 1
                            if ($partNumber -gt 999) {
                                throw 'More than 999 ZIP parts required; extend the corpus segment naming protocol.'
                            }
                            $partPath = '{0}.{1:D3}' -f $zipPath, $partNumber
                            if (Test-Path -LiteralPath $partPath) {
                                throw "ZIP segment already exists: $partPath"
                            }

                            $partStream = [IO.File]::Create($partPath)
                            $parts.Add($partPath)
                            try {
                                $written = 0L
                                while ($written -lt $partLimit -and
                                       $inputZip.Position -lt $inputZip.Length) {
                                    $take = [int][Math]::Min($buffer.Length, $partLimit - $written)
                                    $read = $inputZip.Read($buffer, 0, $take)
                                    if ($read -le 0) {
                                        throw "Unexpected end of compressed capture: $zipPath"
                                    }
                                    $partStream.Write($buffer, 0, $read)
                                    $written += $read
                                }
                            }
                            finally {
                                $partStream.Dispose()
                            }
                        }
                    }
                    catch {
                        foreach ($part in $parts) {
                            Remove-Item -LiteralPath $part -Force -ErrorAction SilentlyContinue
                        }
                        throw
                    }
                }
                finally {
                    $inputZip.Dispose()
                }

                # All pieces were written successfully. Keep the complete ZIP
                # if a split fails; remove it only after every piece is ready.
                Remove-Item -LiteralPath $zipPath -Force
                Write-Host "Capture ZIP:  $($parts.Count) parts (45,000,000 bytes maximum each)"
                foreach ($part in $parts) {
                    Write-Host "  $part"
                }
            }
            else {
                Write-Host "Capture ZIP:  $zipPath"
            }
        }

        Write-Host "Raw captures: $runPath"
        Write-Host "Shaders:      $($metadataFiles.Count) native captures"
        Write-Host "Contexts:     $($contextFiles.Count) stage-specific runtime captures"
        Write-Host "Recorded CB1: $(@($runtimeIndex | Where-Object { $_.cb1Status -in @('captured', 'captured-prefix') }).Count)"
        if ($metadataFiles.Count -gt 0 -and $contextFiles.Count -eq 0) {
            Write-Warning 'No runtime contexts captured. Rebuild the updated Lichii source with -Build.'
        }
        if ($titleIds.Count -gt 0) {
            Write-Host "Title IDs:    $($titleIds -join ', ')"
        }
    }
    finally {
        foreach ($name in $vars.Keys) {
            [Environment]::SetEnvironmentVariable($name, $previous[$name], 'Process')
        }
    }
}
if ($exitCode -ne 0) {
    Write-Warning "Lichii exited with status $exitCode; collected files were retained."
    exit $exitCode
}
