# Breakdown reuse audit

What the MechAssault port takes from the Breakdown port (`Desktop\Breakdown-Xbox-PC-Recomp`, commit `7a4ac0b`,
2026-11-02), how each piece was judged, and what MechAssault had to add. Breakdown was only read: its tree, build
outputs and git state were not touched (files were taken with `git archive`, which reads the object store).

Categories, as asked:

1. **Proven in Breakdown** — measured working on Windows in Breakdown's own logs.
2. **Implemented, not fully verified** — in the code, but Breakdown's docs say "Windows check pending" or similar.
3. **Experimental** — present for diagnosis or partly done.
4. **Game-specific** — Breakdown addresses and hooks; useless or harmful for MechAssault.
5. **Planned / incomplete** — named in Breakdown's roadmap, not in the code.

The last column says what happened in MechAssault (verified here unless stated).

## How the code is laid out (both projects)

```
game layer      src/, config/          one title (Breakdown, MechAssault)
shared runtime  sdk/runtime/           the Xbox kernel, XDK libraries, NV2A, Windows host; never names a title
toolchain       sdk/tools, sdk/xdk, sdk/cmake   x86 -> C++ recompiler, XDK signatures, build functions
```

`sdk/` is TGP482/XboxXBE-Recomp at `f7b13fd` plus Breakdown's patches (`breakdown-recomp patch`). The MechAssault
repository keeps the same history shape: commit 1 is the pristine SDK, commit 2 is Breakdown's `sdk/` at `7a4ac0b`, so
`git diff 03c6c2d -- sdk` lists exactly what MechAssault changed (each marked `mechassault-recomp patch`).

## Recompilation pipeline

| Piece | Breakdown status | MechAssault |
|---|---|---|
| XBE parsing, sections, memory layout (`runtime/xbe`, `kernel/boot.cpp`) | 1 | Reused unchanged. 33 sections, all mapped; boots. |
| `xbe_recomp` x86 -> C++ (Zydis, jump tables, x87/SSE/MMX, C++ EH, continuations) | 1 | Reused unchanged: 23,160 functions, 845k instructions, 204 jump tables, 154 catch blocks. No unresolved indirect jump in hot code seen so far. |
| Function discovery (direct calls, pointer seeds, gap fill) | 1 | Works; gap fill also decodes some data (138 `aas`, 31 `bound`... "unimplemented" opcodes in `analysis/xbe_recomp.log`), none reached at run time so far. |
| `xbe_symbols` XDK signature scan | 1 (5659 set built for Breakdown) | Reused. MechAssault is XDK **4831**, which the upstream SDK already had: 138 of 248 functions, 12 of 13 data. One false match (XInputGetCapabilities) removed by hand. |
| XbSymbolDatabase reconciliation | 3 (Breakdown built it on Linux as a tool, with j's approval; results in `analysis/xbsdb`) | Building it on this PC was refused by the session's permission rules, so `scripts/oovpa_scan.py` reads XbSymbolDatabase's pattern data directly (MIT, local clone under `.local/`, not vendored). 0 disagreements with the 78 symbols both found; 12 more filled and checked by stack pop. |
| `[[hook]]`, `[xbe] trace`, `[[function]]`, `[abi]` config features | 1 | `trace` used for debugging; one `[[hook]]` for widescreen. |
| Build (`CMakeLists.txt`, presets, `build.bat`, `add_xbe_image`) | 1 | Reused with names changed. Clean build 52 s at `-j 6` on the Ryzen 7 7700X. |
| Player-side game-code build (`app/game_code.cpp`) | 1 (packaged build tested by j) | Reused unchanged. Dev builds now mark the DLL current after an executable-only relink (CMake post-build), otherwise every launch rebuilt the DLL with 16 compilers. |

## Xbox runtime

| Piece | Breakdown | MechAssault |
|---|---|---|
| Kernel exports (`kernel/`) | 1 | All 149 MechAssault imports are implemented (0 missing at boot). |
| Memory, heaps, contiguous memory | 1 | Works; 340-380 MB working set in play. |
| Threads, events, APCs, waits | 1 | Works (MechAssault runs 6+ guest threads). Breakdown hit two single-core ordering races (its `hooks.cpp`); none seen in MechAssault yet. |
| Files: D: from folder or ISO, T:/U:/Z: on the host | 1 | **Two general bugs found and fixed** (shared layer, see below): rename of an open file failed on Windows; overlapped transfers completed synchronously. |
| Direct3D 8 HLE, push buffers, raw NV2A (`xdk/d3d8.cpp`) | 1 for Breakdown's usage | MechAssault draws almost everything as raw NV2A pushes with fixed-function vertex formats (`vs 00000112`, `00000144`); title, menus, text, briefing movies and Mission 1 render correctly with no change. |
| NV2A shaders to HLSL (`gpu/shaders.cpp`) | 1 for what Breakdown uses | Works for MechAssault's frames so far. |
| D3D12 backend, render thread, shader disk cache | 1 | Reused unchanged; 0.66 ms host time per frame in play. |
| DirectSound HLE + XAudio2 | 1 (by ear) | Works by the log (effects image loaded, banks stream); **not checked by ear yet**. One 4831 symbol added by hand (`CDirectSoundStream_SetMixBinVolumes`). |
| Bink | n/a (Breakdown uses XMV) | The game's own recompiled Bink plays the logo and briefing movies at correct speed. |
| XAPI controller HLE, SDL3 pads | 1 | Works (scripted pad drives menus and play). |
| Save data (U:) and title data (T:), the Z: cache | 1 | Profile saves and the 96 MB Z: cache work after the file fixes. |
| Logging, crash handler, thread dumps | 1 | Extended: `XBE_LOG_LEVEL`, `XBE_IOLOG`, `XBE_INPUTLOG`, and the crash log now names the faulting guest function. |

## Rendering and display

| Feature | Breakdown | MechAssault |
|---|---|---|
| Render scale 1x-4x (live) and Auto | 1 (Breakdown progress 42) | Reused. Auto renders at output height (2x at 960p seen in the log). |
| Supersampled downscale when render > window | 2 (measured on Breakdown captures) | Reused, not separately measured here. |
| FXAA (finished frame) | 1 (1% lows unchanged at 1x-3x) | Reused, on by default. |
| FSR | **5: not in Breakdown.** `enhancements-plan.md` lists FSR 1 as planned; no FSR code exists in `sdk/` | Added here to the shared present path: FSR 1 (EASU + RCAS) written from the published algorithm, used when upscaling (GRAPHICS_AND_PERFORMANCE.md). |
| MSAA / SMAA | **5**: planned only | Not implemented. |
| Anisotropic filtering | 1 | Reused. |
| True widescreen (`gpu/widescreen.h` squeeze + title camera hook) | 1 (squeeze), hook is game-specific (4) | Squeeze reused unchanged; a new MechAssault camera hook widens 4:3 perspective cameras. 16:9 verified. |
| Aspect choices 4:3 / Widescreen / 16:9 stretched / fill | 1 | Reused. MechAssault ignores the console widescreen flag (identical frames told 4:3 or 16:9). |
| Display mode, resolution, refresh, V-Sync, FPS cap, live apply | 1 | Reused. |
| Display-locked vertical blank (`app/vblank.cpp`) | 2 (fallback on 100 Hz measured) | Reused; this PC's 100 Hz display falls back to the 60 Hz timer as designed. |
| FPS counter, diagnostics overlay, frame statistics, `XBE_PERF` | 1 | Reused; every measurement in these docs comes from it. |
| F3 frame capture (`XBE_CAPTURE_DIR`) | 1 | Reused; the scripted pad can trigger it. |

## Launcher, setup and input

| Feature | Breakdown | MechAssault |
|---|---|---|
| Portable layout (`resources\{game,settings,saves,logs,runtime}`) | 1 (packaged run by j) | Reused (`app/paths.cpp`); `release.bat` renamed. Not yet packaged and run here. |
| Installer window (Game / Prepare / Play tabs, disc image or folder, extras, shortcut) | 2 (Breakdown 33: "not yet run on Windows" at the time; later runs used it) | Reused. MechAssault's profile (title ID 4D530017, the USA/Europe SHA-1, required files) and extras (mouse aim) are filled in. Not yet run end to end. |
| Game import and validation (`core/game_import`) | 1 (unit tests + real import) | Reused unchanged. |
| Keyboard as pad 1, rebindable (`src/common/keyboard_pad.cpp`) | 2 (rebinding "not yet tested on Windows") | Copied into the game layer and re-labelled for MechAssault's own Normal layout. |
| Mouse look | 4 (writes into Breakdown's controller object at 0x53CA0) | Window/raw-input/capture/menu code reused; the aiming itself is new: a right-stick emulation (see KNOWN_ISSUES.md). |
| Settings from the controller (Back + Start) | 2 | Reused. |
| 60 fps option | 4 (Breakdown's blank-count hook) | Not needed: MechAssault already steps by measured time (GRAPHICS_AND_PERFORMANCE.md). |

## Not reused (Breakdown-only)

- `src/game/hooks.cpp` (Breakdown's two loader races at 0x2C650, 0x2C980), `frame_rate.cpp`, `mouse_look.cpp`'s game
  writes and `widescreen.cpp` — all name Breakdown addresses. Kept for reference under `scratch/` (ignored by git).
- `config/breakdown.toml`, `sdk/xdk/5659` (MechAssault is 4831; the 5659 set stays in `sdk/` for other titles).
- Breakdown's icon (`res/`).

## Changes MechAssault made to the shared layer

Every one is marked `mechassault-recomp patch` and is general, so it belongs upstream / back in Breakdown later
(j's call; nothing was pushed to Breakdown):

| File | Change | Why it is general |
|---|---|---|
| `kernel/files.cpp` | Rename: close and reopen the handle's own file around `fs::rename` | Windows cannot rename a file the renaming handle holds open; MoveFile always failed on Windows |
| `kernel/files.cpp` | Transfers on overlapped handles (no FILE_SYNCHRONOUS_IO flag) with an event or APC return STATUS_PENDING after completing | The console's drive always pends; code written for that (MechAssault's cache copy) skips work on an immediate success |
| `kernel/files.cpp` | `XBE_IOLOG` | diagnostics |
| `xdk/xapi.cpp` | `XBE_INPUTLOG` | diagnostics |
| `core/log.cpp` | `XBE_LOG_LEVEL=info|warn` in release builds | Info and Warn lines were silently dropped in release |
| `cpu/dispatch.cpp`, `app/main.cpp` | Crash log names the guest function | diagnostics |
| `app/overlay.cpp` | Menu title from `GameInfo::short_title` | It said "Breakdown PC settings" in every title |

Before these go back to Breakdown, its own boots should be re-run: the STATUS_PENDING change alters what Breakdown's
XAPI sees for overlapped reads (Breakdown's loader races were around exactly that code).

## Toward a shared pipeline

What made the second title fast: the SDK's layering held (no `sdk/` change names MechAssault), 4831 signatures existed
upstream, and Breakdown's diagnostics (perf log, frame capture, crash stacks) worked unchanged. What would help the
next title (Blinx, MechAssault 2):

1. Move the `sdk/` copies to one shared checkout with `sdk_sync` (already in the SDK) once j decides how Breakdown
   and MechAssault should share it; today each repository carries its own copy.
2. Keep `scripts/oovpa_scan.py` + `reconcile.py` as the standard symbol cross-check (no build of external code).
3. The scripted pad (`src/common/pad_script.cpp`) is title-neutral and could move into `sdk/runtime/core`.
4. Port the two file fixes to Breakdown and the SDK upstream.
