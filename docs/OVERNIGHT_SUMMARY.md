# MechAssault PC port: overnight summary (2026-11-02/03)

Repository: `C:\Users\jaybr\OneDrive\Desktop\MechAssault-Xbox-PC-Recomp`, branch `main`, local commits only (pushing
to github.com/CaiusCodes/mechassault-xbox-pc-recomp was refused by the session's permission rules; run
`git push -u origin main` there). Breakdown and the other game folders were only read. No game files, XBE or
generated code are in the repository.

Working = verified on this PC in a run (screenshots/logs named). Partial = implemented and partly checked.
Planned = not built.

## 1. Reused from Breakdown

The whole shared layer, copied (not linked) from Breakdown at `7a4ac0b`: the XboxXBE-Recomp toolchain (`xbe_symbols`,
`xbe_recomp`, CMake), the kernel and XDK HLE runtime, the D3D12 renderer with NV2A shader translation, render scale,
FXAA, supersampling, true-widescreen squeeze rule, aspect modes, vertical-blank pacing, frame statistics, FPS and
diagnostics overlay, F1 settings menu, settings file, portable layout, installer/setup screen with game import and
validation, player-side game-code build, keyboard-as-pad with rebinding, mouse window/capture code. Details and how
proven each piece was in Breakdown: `docs/BREAKDOWN_REUSE_AUDIT.md`. Not reused: Breakdown's game hooks (addresses),
its 5659 symbols, its 60 fps and mouse hooks, its icon.

## 2. MechAssault's XDK and characteristics

XDK **4831** for every library (XAPI, XOnline, D3D8, D3DX8, XGraphics, DSound, LIBCMT/CPMT, XVoice); title ID
4D530017, the USA/Europe disc, `default.xbe` SHA-1 `682ddd1e...`. 149 kernel imports, all implemented. Bink movies,
WMA decoder, Xbox Live libraries present. Draws are almost all raw NV2A pushes with fixed-function vertex formats.
Simulation steps by measured time (rdtsc-based clock), not by frame count. First boot decompresses `Common.mgf`
into a 96 MB hard-disk cache. Full analysis: `docs/MECHASSAULT_TECHNICAL_ANALYSIS.md`.

## 3. Recompilation and build

Working. 23,160 functions recompiled; `scripts\b.bat cmake --build out\build\release -j 6` builds
`out\build\release\MechAssault.exe` + `MechAssault.dll` in ~52 s clean. Symbols: `config/game.toml`
(xbe_symbols, one hand fix) + `config/mechassault.toml` (14 hand-checked symbols, the widescreen hook).
`release.bat` makes `out\build\release\package\MechAssault` (no game code; the player's PC builds it). A fresh
`git clone` of the repository, with the game junctioned in as `assets`, configured and built in 121 s with nothing
else needed (the dependencies came from the existing build's sources: in that deep temp path git could not fetch
Zydis's submodule, a Windows path-length limit, not a repository problem).

## 4. Furthest verified milestone

Working: logos -> title -> profile -> menus -> mission select -> briefing movie -> 'Mech select -> the first
campaign map (Planet Fall / Going Down Hard) played with walking, torso aiming, firing, weapon switching, jump jets,
enemies destroyed; local multiplayer Grinder on Colosseum against attacking 'Mechs, with one player and as
two-player splitscreen (also in Widescreen); a 31-minute random-play run
of the campaign map with no crash or hang (60 fps in all 187 ten-second windows, stalls only while loading plus one
57 ms frame, memory flat at ~400 MB); the packaged release's first run (setup -> import -> player-side build -> play). Not reached:
most later campaign maps (after the night, an "Unlock the whole campaign" option made all of them selectable and
Mission 19 was played). Not done: a person playing with a controller or keyboard/mouse.

## 5. Rendering that works

Everything seen so far renders: movies, menus and text, HUD, terrain, water, waterfall mist, fire, explosions,
particles, shadows, cutscenes with subtitles. D3D12 debug layer: no errors or warnings boot -> mission. Two things
to compare with console footage (KNOWN_ISSUES.md). Audio output is active (levels logged) but nobody has listened.

## 6. 60 fps

Working and the default. The console asked for 30 fps (present interval 2); the game's own clock makes it run at
the right speed at any rate (a 30 vs 60 fps scripted walk ends in the same place). Measured in play: 60.00 fps,
1% low 55-58, p99 ~17.2 ms, worst frames 17-21 ms with no stalls outside loads; 0.18-0.25 CPU cores; 380 MB.
Uncapped it runs at 700-860 fps on the RTX and ~250 fps on the 7700X's iGPU. Only 100 Hz was available here: even
cadence needs 60/120 Hz or VRR (the menu says so). `docs/GRAPHICS_AND_PERFORMANCE.md`.

## 7. Render scale, FSR, AA, widescreen

- Render scale Auto/1x/2x/3x/4x: working, live; real internal resolution. Auto stops at 2x on integrated GPUs
  (measured on the iGPU).
- FSR 1 (EASU + RCAS): new (Breakdown had none); own implementation of AMD's published algorithm, used when the
  render is smaller than the screen; sharpening slider; live. Verified 640x480 -> 1280x960.
- AA: FXAA working (default); supersampling through render scale; MSAA/SMAA not implemented.
- Widescreen: true Hor+ through one camera hook; HUD/menus/movies kept 4:3; 16:9 and 21:9 verified; culling widened
  with it.

## 8. Native menu and live settings

Working: Breakdown's F1 menu (also Back+Start on a controller) titled for MechAssault, with Display (mode,
resolution, aspect, brightness, V-Sync, frame cap, refresh), Rendering (render scale, anisotropic, AA, upscaling,
FSR sharpening), Performance (overlay, frame log), Gameplay (field of view 80-130%) and Controls (mouse aim,
sensitivity, invert, every key and mouse button). Display changes now ask "Keep these display settings?" and revert after 15 s. All settings apply without a
restart; changing them in the middle of a mission was verified (`scripts\pad_live_settings.txt`).

## 9. Significant bugs fixed

Shared runtime (also relevant to Breakdown):
1. Rename of a file held open by the renaming handle failed on Windows (MoveFile) -> the game's HDD cache.
2. Overlapped reads completed synchronously -> the game's cache copy skipped decompressing (empty cache, crash).
3. **Player-side builds used `game.toml` alone, skipping the title's hand-checked config** -> the packaged game
   crashed. Breakdown's release has the same code.
4. Frame limiter spun for 2 ms every frame -> high-resolution timer (20-45% less CPU).
5. Release builds dropped warnings; crash logs now name the faulting guest function; D3D12 debug messages logged.

MechAssault: DirectSound functions that ran original code against host objects (`SetMixBinHeadroom`,
stream `SetMixBinVolumes`), the false `XInputGetCapabilities` match, mouse response (dead zone read from code, torso
ramp measured).

## 10. Remaining blockers and next steps

No blocker to playing. Next, in order:
1. j plays a mission with a controller and with keyboard/mouse, and listens to the audio.
2. Push the repository; consider taking the shared fixes (item 9) into Breakdown, especially #3.
3. Play through the campaign to reach later maps; fix what they need (new effects, new XDK calls).
4. Compare the two possible artifacts with console footage.
5. Optional: AMD's reference FSR shader (MIT) to replace the own implementation; MSAA with resolves; pitch
   calibration for the mouse; a GPU-time-based Auto render scale.

## How to reproduce

```
cd C:\Users\jaybr\OneDrive\Desktop\MechAssault-Xbox-PC-Recomp
scripts\b.bat cmake --preset release
scripts\b.bat cmake --build out\build\release -j 6
$env:XBE_PAD_SCRIPT = "$PWD\scripts\pad_mission1_play.txt"
powershell -ExecutionPolicy Bypass -File scripts\run.ps1 -Seconds 95 -Every 3 -Tag mytest
```

Screenshots in `runs\mytest`, log in `out\build\release\logs\mechassault.log`. Or simply start
`out\build\release\MechAssault.exe` and play (F1 for settings). More in `docs/PORTING_PROGRESS.md`.
