# MechAssault (Xbox, 2002) native PC recompilation — work in progress

A static recompilation of the original Xbox game *MechAssault* (Day 1 Studios / Microsoft, 2002) to native x64
Windows, built on the same pipeline as the Breakdown port: [TGP482/XboxXBE-Recomp](https://github.com/TGP482/XboxXBE-Recomp)
(vendored in `sdk/`, pristine in commit 1) with the Breakdown port's shared runtime work (commit 2) and MechAssault's
own changes on top.

**This repository contains no game code, no game assets and no recompiled output.** The game code is recompiled
locally from the user's own `default.xbe`; `assets/`, `game_files/` and `generated/` are git-ignored.

## Status

Boots, plays the intro movies, runs the menus and briefings, loads Mission 1 and plays it (walking, aiming, firing,
weapons, jump jets) at 60 fps with the game's own speed, true widescreen, keyboard and mouse. See `docs/`:

- `docs/PORTING_PROGRESS.md` — milestones, session log, how to build and test
- `docs/MECHASSAULT_TECHNICAL_ANALYSIS.md` — the XBE: XDK 4831, sections, imports, boot, graphics, timing, input
- `docs/BREAKDOWN_REUSE_AUDIT.md` — what came from Breakdown, how proven it was, what changed
- `docs/GRAPHICS_AND_PERFORMANCE.md` — 60 fps, render scale, AA, widescreen, settings, measurements
- `docs/KNOWN_ISSUES.md` — fixed and open problems

## Layout

- `sdk/` — XboxXBE-Recomp + Breakdown's patches (`breakdown-recomp patch`) + MechAssault's (`mechassault-recomp patch`)
- `config/` — `game.toml` (from `xbe_symbols`, one hand fix), `mechassault.toml` (hand-verified symbols, the
  widescreen hook), `main.toml`
- `src/common/` — game profile and options (`game_info.cpp`), keyboard, mouse aim, scripted test pad
- `src/game/` — hooks into the recompiled code (widescreen)
- `scripts/` — analysis (`xbeinfo.py`, `xdis.py`, `oovpa_scan.py`, `reconcile.py`, `libcalls_ma.py`, `thunk.py`),
  build wrapper (`b.bat`, `env.bat`), test runner (`run.ps1`, pad scripts, `bench_scales.ps1`)
- `analysis/` — metadata from the XBE (no code bytes)

## Building

Visual Studio 2022+ with the C++ workload, LLVM clang, CMake and Ninja (the same toolchain as Breakdown). Put the
extracted game at `assets\` (or a junction to it), then:

```
scripts\b.bat cmake --preset release
scripts\b.bat cmake --build out\build\release -j 6
out\build\release\MechAssault.exe
```

`build.bat` does the same with prompts; `release.bat` makes the portable package (no game code: the player's PC
builds it from their own copy on first start).

**Releases:** the game's updater (F1 > Check for updates) installs the latest GitHub release, so each release must
attach the package `release.bat` makes as an asset named exactly `MechAssault.zip` (the zip of the `MechAssault`
folder). Tag releases with their version (`v0.5.1`); the updater compares those numbers. See `docs/testing.md`.
