# Switch 2 native Ampere shader oracle (2026-11-16)

## Purpose

Collect the **original Switch 2/Ampere GPU shader instructions** while Lichii runs a
title it can advance further than another emulator. Capture before converting
them to Maxwell, so the other emulator can reproduce and test the actual
instructions, including those its own runtime has not encountered.

This is a local development workflow, not a host shader or SPIR-V cache export.

## Existing implementation and correction

The existing `EDEN_NX2_SHADER_DUMP=<directory>` handler already wrote a
16-byte-instruction native code stream and a separate graphics shader header.
However, in `src/video_core/shader_environment.cpp` it ran only **after**
`LowerNativeProgram(native, inputs)`. Capture was skipped if the lowering
threw or returned failure before reaching the dump handler.

As of this change, the handler runs **immediately after the bounded native
instruction reader finishes**, before trap-guard verification, shader cache v2
lookup, or native-to-Maxwell lowering. The early invalid-branch path retains
its existing `rejected-*.bin` diagnostic. Dumping remains opt-in.

Filename convention, retained for existing callers:

- `<native-key>-cs-<guest-program-base>.bin`: native compute instructions only
- `<native-key>-gfx-<guest-program-base>.bin`: native graphics instructions only
- `<same-stem>.header.bin`: original graphics header, when mapped
- `<same-stem>.json`: new `lichii-native-ampere-v1` metadata, recording title
  ID, stage (`va/vs/tc/te/gs/fs/cs/ts/ms`), program key, guest addresses, header
  size, and code size

Raw .bin files contain **only original native instructions**, in their
original four little-endian `u32` / 16-byte format. Headers are not padded or
synthesized. The metadata is not a substitute for the header.

## Windows usage

Start Lichii with this variable set in **its own process environment**:

```powershell
$env:EDEN_NX2_SHADER_DUMP = 'G:\Users\Johnny\Downloads\2\Lichii-Ampere-oracle'
```

Run Lichii from that PowerShell session or configure the equivalent
environment variable in its debugger launch profile. Let the title reach the
scene of interest. Captures are written as programs are first read and do not
wait for a successful Maxwell translation.

A separate development tool in the Ryujinx fork,
`tools/import_lichii_native_oracle_v1.py`, converts the resulting directory
to the existing `AmpereNativeCorpusPreflightTests` ZIP layout. It verifies
native allocation sizes, pairs the actual 0x80-byte graphics header with code,
deduplicates by stage and SHA-256, and preserves original title/guest address
provenance in `oracle-manifest.json`.

## Scope and limitations

- The oracle captures **encountered native programs**, not an exhaustive list
  of every shader the game's assets or renderer could ever use.
- Runtime constant-buffer data, including CB1 indirect-branch jump tables,
  is **not** included. Offline SPIR-V may still reject BRX even when Lichii
  compiles the shader successfully in its GPU context.
- An already-provided `LoweredNativeCode` passed to
  `GenericEnvironment::InitializeNativeCode` can bypass `LowerNativeCode`
  entirely. Existing caches may therefore reduce new capture opportunities.
  For complete first-pass capture, use a fresh Lichii shader cache/profile.
  The v2 cache lookup *inside* `LowerNativeCode` does not bypass the new dump.
- If the native capture fails before reaching its end (unmapped first
  instruction, invalid branch target), it may have only the existing rejected
  diagnostic, not a complete metadata-bearing program.
- The capture does **not** change shader translation, Maxwell output,
  resource bindings, GPU state, or rendering behavior.

## Windows UCRT64 GCC build blocker (2026-11-16)

User build using `Build-Eden-UCRT64-GCC.cmd` on `nx2` at commit
`504128d19fb6155ce957e43a7e2958253de3c64c` successfully
configured CMake 4.5.0 with GCC 16.2.0 and Ninja 1.13.2 but stopped
at `[15/1849]` on `src/common/nx2_sampler.cpp`:
`-Werror=cast-function-type` for six `GetProcAddress` conversions
(`NtQueryInformationThread`, `GetThreadDescription`,
`SymSetOptions`, `SymInitialize`, `SymFromAddr`,
`SymRefreshModuleList`). This is an unrelated pre-existing GCC portability
problem, not a native shader capture or CMake configuration error.

- Commit `4686342c`: introduces `ResolveProc<Function>` with a null
  check, equal-pointer-size assertion, and `std::bit_cast` to preserve
  the Windows API function pointer representation without the prohibited
  incompatible-function-pointer `reinterpret_cast`. The six sites use
  this shared loader. No warning policy is disabled.
- Commit `ceecace1`: fixes the existing UCRT64 shell build script's
  final expected output from the old `bin/eden.exe` to the actual
  CMake target `bin/lichii.exe`. The `.cmd` wrapper and build directory
  remain unchanged. Existing legacy `YUZU_*` option warnings are
  nonfatal and are not part of this failure.
- The standalone C++20 function-pointer `std::bit_cast` pattern passed
  a warning-as-error syntax check with GCC 14.2 on Linux, but the actual
  Lichii Windows UCRT64 build **has not been rerun or confirmed**.
  Rerun `./Build-Eden-UCRT64-GCC.cmd` after pulling `nx2`.
  Ninja should reuse already completed objects; no clean rebuild requested.

## Verification

Source-level inspection and edits completed in `Masamune3210/Lichii`,
branch `nx2`. **No Windows C++ compilation, guest GPU run or generated
corpus verification has been performed yet.** Verify with a fresh runtime
session, inspect the .bin/.header.bin/.json trio, import with the Ryujinx
converter, and run the existing preflight.

Do not promote this change to unrelated branches automatically.
