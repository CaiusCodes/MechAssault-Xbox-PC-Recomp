# Porting progress

Milestone words as in the Breakdown port: parses, recompiles, links, boots, renders, playable. All runs on j's PC
(Windows 11, Ryzen 7 7700X, RTX 4070 Ti, 3440x1440 at 100 Hz), windowed, unattended, driven by scripted pad input.

## Current state (2026-11-02)

| Milestone | State | Evidence |
|---|---|---|
| Parses | yes | `analysis/xbe.json`, 33 sections, 149 kernel imports |
| Recompiles | yes | 23,160 functions, `analysis/xbe_recomp.log` |
| Links | yes | `out\build\release\MechAssault.exe` + `MechAssault.dll`, 52 s clean build at `-j 6` |
| Boots (runtime up, entry runs, threads, files) | yes | log: "kernel imports resolved, 0 missing" |
| Renders: logo movie (Bink) | yes | `runs\title10\000-002` |
| Renders: title, profile select, main menu, mission select, 'Mech select (with its live 3D preview), controls screens | yes | `runs\title10\004`, `runs\m1a\009`, `runs\menu2`, `runs\menu4` |
| Plays briefing movies | yes | `runs\title10\012` |
| Loads a level (Mission 1, Planet Fall) | yes | `runs\m1b\012` |
| Controllable gameplay: walk, turn, torso, fire, cycle weapons, jump jets | yes, by scripted XInput | `runs\m1play\037-042` (an explosion where it fired) |
| In-engine cutscenes with subtitles | yes | `runs\wide1\004` |
| 60 fps with correct game speed | yes | GRAPHICS_AND_PERFORMANCE.md |
| Audio | output running; levels in the log; not heard yet | see the stress run below |
| Playable start to end by a person | not yet tried | |

## Session log

1. **Breakdown audit** (read only): docs, then the code of `sdk/` (toolchain, runtime, renderer, setup) and the
   game layer. Result: BREAKDOWN_REUSE_AUDIT.md.
2. **Repository**: `git init` in the MechAssault folder; commit 1 = pristine SDK (`git archive 0a1356d sdk` from
   Breakdown), commit 2 = Breakdown's `sdk/` at `7a4ac0b`, then the game layer. The extracted game is reached through
   an `assets` directory junction to `game_files\MechAssault (USA, Europe) (En,Fr,De)`; nothing writes into it
   (the emulated hard disk is `out\build\release\hdd`). `.gitignore` excludes `assets/`, `game_files/`, `*.xbe`,
   `*.mgf`, `generated/`, `captures/`, `runs/`.
3. **XBE profile**: XDK 4831 throughout (MECHASSAULT_TECHNICAL_ANALYSIS.md). Breakdown's `xbeinfo.py` assumed
   raw = va - base; MechAssault's sections are laid out differently (fixed).
4. **Symbols**: `xbe_symbols` 138/248. Its `XInputGetCapabilities` match was wrong and broke the config. Building
   XbSymbolDatabase was refused by the session's permission rules, so `scripts/oovpa_scan.py` reads its patterns:
   0 disagreements in 78, 12 filled. **Recompiled and linked first try** (51 s).
5. **First boot**: crash in DirectSound (`SetMixBinHeadroom` running original code) -> symbol. Then the logo movie
   and title screen at 60 fps, but the title never accepted Start and its prompt box stayed empty.
6. **Scripted testing on Windows**: `src/common/pad_script.cpp` (`XBE_PAD_SCRIPT`: timed presses, stick values,
   thread dumps, frame captures, memory peeks, F1 menu), `scripts/run.ps1` (window screenshots every N seconds),
   `XBE_LOG_LEVEL` for release builds. PostMessage keys never reached the game window, so the pad script drives
   everything.
7. **The stuck title**: the input reached the game; thread dumps showed the main loop alive. Tracing the worker
   thread (`0x29E80`) showed the hard disk cache copy: `Common.mgf` decompressed to `z:\filecopy.tmp` and renamed.
   Two shared-runtime bugs: (a) the rename failed on Windows because the renaming handle had the file open; (b) after
   that, the cache was empty because every overlapped read completed synchronously, and the game only decompresses a
   block on the STATUS_PENDING path. Both fixed generally (KNOWN_ISSUES.md).
8. **Menus**: then a crash after the audio banks loaded: `CDirectSoundStream_SetMixBinVolumes` behind a 4831 jmp
   thunk ran original code. Named; **menus, profile, mission select and the briefing work**.
9. **Gameplay**: Launch Mission from Select 'Mech loads Planet Fall; walking, turning, firing (with a hit), weapon
   cycling and jump jets respond.
10. **60 fps**: the game's clock (`0x1F0C4`) steps by measured time (average of up to 4 frames, clamped after
    loads), so 60 fps keeps game speed; a 30 vs 60 fps scripted walk ends in the same place.
11. **True widescreen**: one `[[hook]]` in the camera build; HUD/menus stay 4:3 by the shared renderer rule.
12. **Input**: keyboard defaults from the game's own Controls screen. Mouse aim through the right stick, calibrated:
    the stick dead zone (0.30) read from the pad code; the torso's response measured from the HUD compass (full stick:
    45 degrees in 0.5 s, ~122 in 1 s, i.e. ~150 deg/s after a ~0.4 s ramp, instant stop). A first model without the
    ramp turned 20 and 60 degrees for 45 and 90 asked; with it, 45 and 90 (`XBE_MOUSE_TEST` injects counts,
    `runs\mouse_compass*.png`).
13. **Settings**: the F1 menu works (opened by the script); display changes now ask to be kept and revert after 15 s.
14. **Long run**: see "Stress run" below.
15. **More of the game**: local multiplayer (Grinder, Colosseum, ATLAS) loads a second map and plays against
    attacking 'Mechs. The campaign's first unlocked missions (Planet Fall area) load; later campaign maps were not
    reached (the profile save is signed, so it cannot simply be edited to unlock them).
16. **Packaging**: `release.bat` builds `outuildelease\package\MechAssault` (exe, README, licences,
    resources with the compiler, 169 MB of it). Its setup window found the game beside it and validated it
    (`runs\setup1.png`). Play (copying 3.4 GB into resources\game and building the code) was not run, so as not to
    put a second copy of the game into OneDrive or a shortcut on the desktop.

## How to build and test

```
scripts\b.bat cmake --preset release
scripts\b.bat cmake --build out\build\release -j 6
```

`build.bat` does the same interactively (and writes `config\game.toml` with `xbe_symbols` on a fresh checkout; the
committed one has the hand fix described in the technical analysis, so keep it). The game must be reachable as
`assets\default.xbe` (a junction or a copy of the extracted disc).

Run windowed with screenshots and a pad script (PowerShell, from the repository root):

```
$env:XBE_PAD_SCRIPT = "$PWD\scripts\pad_mission1_play.txt"   # title -> Mission 1 -> walk, turn, fire
$env:XBE_LOG_LEVEL = "warn"
powershell -ExecutionPolicy Bypass -File scripts\run.ps1 -Seconds 95 -Every 3 -Tag mytest
```

- Screenshots: `runs\<tag>\NNN.png`; frame captures (`dump` steps): `captures\<time>\` (frame.txt lists every draw).
- Log: `out\build\release\logs\mechassault.log` (`XBE_PERF=1`, set by run.ps1, adds per-second renderer and
  10-second frame timing and audio level lines).
- Scripts: `scripts\pad_title.txt` (title), `pad_mission1.txt`, `pad_mission1_play.txt`, `pad_speed.txt` (same walk
  for 30/60 fps comparisons), `pad_stress.txt` (5 minutes of random play).
- `scripts\run.ps1` stops only the process it started.
- The dev settings file is `out\build\release\settings.toml` (windowed 1280x960 for tests).

Diagnostics: `XBE_IOLOG=1` (every file transfer), `XBE_INPUTLOG=1` (pad states the game reads), `XBE_PEEK=addr[*][+off],...`
(logged at each `dump`), `[xbe] trace = [...]` in `config/main.toml` (function calls, Info level).

## Stress run

`scripts\pad_stress.txt`: Mission 1, then ~5 minutes of random walking, strafing, torso turns, looking, firing,
jump jets and weapon changes (seed 7). Run `stress1` (390 s):

- no crash, hang or error line; the mech crossed the valley, the river below the waterfall, climbed on jump jets and
  fired lasers, the autocannon and missiles (`runs\stress1
`, `018`, `020`).
- 60.00 fps in every 10 s window of play; 1% low 55-58 fps; p99 17.1-17.5 ms; worst frame 17.3-21 ms, with one
  37 ms frame in 6 minutes; 0 stalls in play.
- renderer 0.6-0.8 ms host time per frame, 170-350 draws, 20-32k vertices.
- audio output alive the whole time: peak 0.3-1.0, RMS 0.06-0.25 per 10 s (the peaks reaching 1.0 mean the mix
  clips at times; to be judged by ear).
- one thing to compare with the console: in `runs\stress1
` dark jagged silhouettes cover the right edge
  (cliff and foliage near the camera in fog); may be correct.
