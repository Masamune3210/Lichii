# Run Lichii with complete native shader capture

Run from the root of the **Masamune3210/Lichii** `nx2` checkout in PowerShell 7:

```powershell
.\Run-Lichii-ShaderDump.ps1 -Label MarioKartWorld
```

To build first with the repository's existing MSVC builder:

```powershell
.\Run-Lichii-ShaderDump.ps1 -Build -Label MarioKartWorld
```

Default output root: `G:\Users\Johnny\Downloads\2\LichiiDumps`. The launcher creates a unique, timestamped folder on each run and packages it as a neighboring ZIP when Lichii exits. It does not clear any shader cache or modify emulator configuration. Run as many different games as needed in the same Lichii session; each captured shader metadata file includes its title ID. `-GamePath` may be used to pass a game path directly to Lichii; otherwise select games in the GUI. `-Executable` selects a custom build; `-BuildDir` changes the default `build-msvc` location. `-NoZip` keeps only the run directory.

## Capture environment variables verified in this fork

| Variable | Launcher value | Behavior |
| --- | --- | --- |
| `EDEN_NX2_SHADER_DUMP` | `<run>/shaders` | Original 16-byte native Ampere instruction stream (`.bin`), title/stage/program metadata (`.json`), optional graphics header (`.header.bin`), plus certain rejected stage dumps. Runs before native lowering and shader v2 lookup. |
| `EDEN_NX2_MESH_EMULATION_DUMP` | `<run>/mesh-spv` | Mesh emulation's generated vertex SPIR-V (`.vert.spv`) when applicable. |
| `EDEN_NX2_PIPELINE_BUILD_TRACE` | `<run>/pipeline-builds.txt` | Begin/end host driver pipeline builds with hashes, timestamps and durations. |
| `EDEN_NX2_FRAME_DUMP` | `<run>/frames` | Frame dump armed; no capture until the trigger file appears. |
| `EDEN_NX2_FRAME_DUMP_AT` | `file` | Capture once `<run>/frames/capture.now` appears, rather than at a fixed time. |
| `EDEN_NX2_FRAME_DUMP_SAMPLED` | `0`, or `1` with `-SampledFrame` | Also captures sampled textures and buffers in the triggered frame; this can be very large. |
| `EDEN_NX2_GPU_TRACE` | `<run>/gpu-traces` | GPU command recording armed; no capture until `record.now` appears. |
| `EDEN_NX2_GPU_TRACE_PRESENTS` | `180`, configurable with `-GpuTracePresents` | Frames to record after GPU trace trigger. |

During the running session, from **another** PowerShell terminal, trigger a frame dump with:

```powershell
New-Item -ItemType File -Force -Path 'G:\Users\Johnny\Downloads\2\LichiiDumps\<run>\frames\capture.now'
```

Or a GPU trace with:

```powershell
New-Item -ItemType File -Force -Path 'G:\Users\Johnny\Downloads\2\LichiiDumps\<run>\gpu-traces\record.now'
```

The launcher prints the actual paths including the unique run directory. Trigger files are consumed by the emulator.

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
