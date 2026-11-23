# Run Lichii with complete native shader capture

Run from the root of the **Masamune3210/Lichii** `nx2` checkout in PowerShell 7:

```powershell
.\Run-Lichii-ShaderDump.ps1 -Label MarioKartWorld
```

To build first with the repository's existing MSVC builder:

```powershell
.\Run-Lichii-ShaderDump.ps1 -Build -Label MarioKartWorld
```

Default output root: `G:\Users\Johnny\Downloads\2\LichiiDumps`. The launcher creates a unique, timestamped folder on each run and packages it as a neighboring ZIP when Lichii exits. ZIPs are written with .NET's highest standard Deflate compression effort (`SmallestSize`). Archives no larger than **45,000,000 bytes** remain single `.zip` files; larger archives are split into `.zip.001`, `.zip.002`, etc., with **45,000,000 bytes maximum per part**. The original full ZIP is removed only after all numbered parts are successfully written. It does not clear any shader cache or modify emulator configuration. Run as many different games as needed in the same Lichii session; each captured shader metadata file includes its title ID. `-GamePath` may be used to pass a game path directly to Lichii; otherwise select games in the GUI. `-Executable` selects a custom build; `-BuildDir` changes the default `build-msvc` location. `-NoZip` keeps only the run directory. **Default mode captures native shaders and CB1 only**, to minimize gameplay slowdown. Use `-FullDiagnostics` only when pipeline build logs, mesh SPIR-V, or frame/GPU captures are specifically needed.

## Capture environment variables verified in this fork

| Variable | Launcher value | Behavior |
| --- | --- | --- |
| `EDEN_NX2_SHADER_DUMP` | `<run>/shaders` | Original 16-byte native Ampere instruction stream (`.bin`), title/stage/program metadata (`.json`), optional graphics header (`.header.bin`), plus certain rejected stage dumps. Runs before native lowering and shader v2 lookup. |
| `EDEN_NX2_SHADER_CONTEXT_DUMP` | `<run>/shaders` | New shader-binding capture: re-exports native `.bin` and `.header.bin` on warm shader-cache hits; stores up to eight distinct live CB1 snapshots per native program, sampling the first binding and then every 2048th binding, plus `.context.json` with title ID, stage, GPU binding status and task/mesh register values. Mesh primitive-header bytes are saved when bound. |
| `EDEN_NX2_MESH_EMULATION_DUMP` | With `-FullDiagnostics`: `<run>/mesh-spv` | Mesh emulation's generated vertex SPIR-V (`.vert.spv`) when applicable. |
| `EDEN_NX2_PIPELINE_BUILD_TRACE` | With `-FullDiagnostics`: `<run>/pipeline-builds.txt` | Begin/end host driver pipeline builds with hashes, timestamps and durations. |
| `EDEN_NX2_FRAME_DUMP` | With `-FullDiagnostics`: `<run>/frames` | Frame dump armed; no capture until the trigger file appears. |
| `EDEN_NX2_FRAME_DUMP_AT` | With `-FullDiagnostics`: `file` | Capture once `<run>/frames/capture.now` appears, rather than at a fixed time. |
| `EDEN_NX2_FRAME_DUMP_SAMPLED` | With `-FullDiagnostics -SampledFrame`: `1` | Also captures sampled textures and buffers in the triggered frame; this can be very large. |
| `EDEN_NX2_GPU_TRACE` | With `-FullDiagnostics`: `<run>/gpu-traces` | GPU command recording armed; no capture until `record.now` appears. |
| `EDEN_NX2_GPU_TRACE_PRESENTS` | With `-FullDiagnostics`: `180`, configurable with `-GpuTracePresents` | Frames to record after GPU trace trigger. |

When launched with `-FullDiagnostics`, from **another** PowerShell terminal, trigger a frame dump with:

```powershell
New-Item -ItemType File -Force -Path 'G:\Users\Johnny\Downloads\2\LichiiDumps\<run>\frames\capture.now'
```

Or a GPU trace with:

```powershell
New-Item -ItemType File -Force -Path 'G:\Users\Johnny\Downloads\2\LichiiDumps\<run>\gpu-traces\record.now'
```

The launcher prints the actual paths including the unique run directory. Trigger files are consumed by the emulator.

## Runtime context capture and matching

This capture needs the **updated Lichii executable**, not just the launcher. Use `-Build` at least once after pulling the source change. The new instrumentation is guarded by `EDEN_NX2_SHADER_CONTEXT_DUMP`, which the launcher enables only for its child process. It does not clear shader caches, alter program translation, or require an uncached run. Lichii has no Ryujinx-style PTC.

Each native program still has the original `.bin`, `.header.bin` when graphics, and `.json` metadata. New `*.context.json` records identify the exact source `.bin` and one observed CB1 binding. `cb1_status` is `captured`, `unbound`, `empty`, or `unmapped`. Only `captured` records have matching `*.cb1.bin` binary constants; no missing CB1 is substituted with zero. These bytes are **runtime snapshots**, not proof that an offline BRX uses a given table, and are not automatically valid for a different shader byte sequence.

At exit the launcher writes `runtime-context-index.json`, pairing each context with the **SHA-256 of the original native `.bin`**. This is the appropriate identity for matching a runtime snapshot to a RomFS sidecar or the Ryujinx native preflight. `capture-manifest.json` now counts native programs, context records and CB1 captures separately. The index and captured constants are part of the ZIP.

Task/mesh records also include the three `SET_MESH_*` registers and, where available, the separate 80-byte primitive output header. The normal graphics shader header is always separate. Only executed shaders can have live CB1 data. Static RomFS sidecars that never run cannot acquire valid runtime constant snapshots through an emulator run.

Play through the relevant menu/cutscene/gameplay transition before closing Lichii. The capture runs when a program is actually bound, including warm shader-cache reuse. Repeated binds are sampled at intervals to avoid stalling guest GPU execution on every draw/dispatch. Up to eight distinct valid CB1 snapshots are retained per program/title/stage. Capture a new session per game or title-state transition if further variants are needed. The launcher disables pipeline trace, mesh SPIR-V dumping, and frame/GPU instrumentation entirely by default; opt into them with `-FullDiagnostics`. Frame/GPU recording additionally requires the separate trigger file.

**Check after the run:** the console must show a nonzero `Contexts` count. `Recorded CB1` can legitimately be zero for a title that never binds CB1. A nonzero shader count with zero contexts indicates that the executable probably predates this update.

## Other shader/GPU diagnostics intentionally **not enabled**

| Variable | Reason it is opt-in / not part of raw shader collection |
| --- | --- |
| `EDEN_NATIVE_DRAW_CAPTURE` | Captures first native draws and associated registers/CBUF/vertex/RT bytes in the **current working directory**, not the native shader dump directory; also changes draw path behavior. |
| `EDEN_NATIVE_FRAME_CAPTURE` | Logs display/frame and macro details, rather than exporting a native shader corpus. |
| `EDEN_NX2_RENDER_TRACE` | Verbose render trace, potentially high-volume and performance-affecting. |
| `EDEN_NX2_TEXTURE_DUMP` | Writes a sampled texture's memory, not native shader instructions. |
| `EDEN_NX2_PIPELINE_DIFF` | Diagnoses fixed-function state differences across pipelines sharing shaders; log-only. |
| `EDEN_NX2_MESH_EMULATION_PROBE` | Performs a periodic mesh output readback, not a shader corpus dump. |
| `EDEN_NATIVE_SHADER_OVERRIDE_DIR`, `EDEN_NX2_REJECT`, `EDEN_NX2_REJECT_HASH` | Shader override/rejection debugging; can stall or intentionally invalidate game shaders. Do **not** enable for unattended capture. |

The UI options `dump_guest_shaders` and `gpu_log_shader_dumps` are not environment variables; they create additional lowered guest code or SPIR-V outputs when enabled through settings.

**Scope:** This is a *runtime shader capture*. It collects shaders observed while running games. It does **not** extract unused static RomFS shader sidecars. For those, retain the original game sidecars or use the existing Lichii source-based extractor. Original instructions and stage metadata captured here are suited to the `titillatingseahorses` native Ampere corpus preflight; generated SPIR-V, frame and GPU traces provide complementary evidence.

On forced termination, the original dump files remain in the run folder even if the final ZIP and manifest could not be created. All environment variables changed by the script are restored to their previous values afterward.

## Zero- or single-title sessions

The final title-ID summary explicitly retains an array after `Sort-Object -Unique`, including when no shader metadata exists. This fixes a `Set-StrictMode -Version Latest` error where the post-exit `Count` property was missing. Earlier launcher versions wrote `capture-manifest.json` and the ZIP before reaching the final title-ID summary, so a completed capture from the earlier build ordinarily does **not** need to be repeated. If the launcher displayed `Capture ZIP:`, use that archive directly.

## Git-sized archives and native preflight compatibility

The launcher splits the **completed compressed ZIP**, not individual shader files.
A 147 MB compressed capture will yield multiple files named
`<capture>.zip.001`, `<capture>.zip.002`, and so on. Each file is
at most 45,000,000 bytes, below GitHub's 50 MB blob warning. If maximum
Deflate compression brings the whole ZIP down to that threshold or less,
the output is the original single `.zip`.

These are consecutive byte ranges of one standard ZIP, **not** PKZIP
multi-volume archives (`.z01`). Keep every numbered part together; none
is individually extractable. The `titillatingseahorses` native Ampere
NUnit harness treats the set as one corpus and reads it with a seekable
virtual stream, including ZIP central-directory seeks. An incomplete set
must fail, not silently omit shaders. `-NoZip` still skips packaging.
