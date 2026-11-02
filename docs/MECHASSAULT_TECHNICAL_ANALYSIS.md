# MechAssault technical analysis

The executable is `default.xbe` at the root of the extracted disc (not the `XDemos` one). Everything below was read
from it or measured on Windows in this session; addresses are guest virtual addresses.

## Identity

| Field | Value |
|---|---|
| Title | MechAssault, title ID `4D530017` (MS-023), version 1, region 5 (North America + Europe), media: DVD |
| Release | USA/Europe (En, Fr, De) |
| `default.xbe` | 3,559,424 bytes, SHA-1 `682ddd1ecda1e5110f9339dab0c31d296d51777b`, SHA-256 `7222e438b256c94f5997aca8e50ce191cc8501ec08517a0b9a0400b7f77e5bce` |
| Built | XBE time stamp 2002-10-13, certificate 2002-10-17; debug path `c:\mrmake\buildMA\src\Game\Final\default.exe` |
| Key | retail entry and thunk keys; entry `0x16DB9`, kernel thunk `0x2E7D60` |
| Image | base `0x10000`, size `0x52A8A0`, stack commit 128 KB, no TLS |
| Disc | 2,122 files, 3.42 GiB: 53 `.mgf` archives (`Common`, `text`, `movies`, 20 campaign missions, 30 multiplayer arenas), `Audio\*.d1Bank` (1.36 GB of music and voices), `Movie\NTSC|PAL\*.bik`, `media\` (ArialUni.Ttf, dlcustom.xpr), plus `XDemos\`, `dashupdate.xbe`, `downloader.xbe`, `update.xbe` that the game does not need to run |

## XDK

Every library is build **4831**, which the upstream SDK already had a signature and layout set for
(`sdk/xdk/4831`):

| Library | Version | QFE | Notes |
|---|---|---|---|
| XAPILIB | 1.0.4831 | 5 | XAPI and the CRT live in `.text` and `XPP` |
| XONLINES | 1.0.4831 | 8 | Xbox Live (MechAssault was a launch Live title); `XONLINE` section |
| XBOXKRNL | 1.0.4831 | 1 | |
| LIBCMT, LIBCPMT | 1.0.4831 | 1 | |
| XVOICE | 1.0.4831 | 1 | Live voice chat |
| D3D8, D3DX8, XGRAPHC | 1.0.4831 | 1 | |
| DSOUND | 1.0.4831 | 1 | with the `WMADEC` decoder section |

Symbol coverage (`analysis/xbe_symbols.log`, `analysis/reconcile.md`): `xbe_symbols` found 138 of the 248 functions
the SDK knows and 12 of 13 data symbols. `scripts/oovpa_scan.py` (XbSymbolDatabase patterns) found 216 names; the
78 both found agree exactly. 12 more were added from the scan and 2 by hand (`config/mechassault.toml`, each with its
evidence). 99 names are found by neither; most are functions this title does not link (Bink HLE hooks for other
titles, Winsock, many DirectSound setters). The rule for adding one is "the game calls it and it touches host-owned
state" — `scripts/libcalls_ma.py` lists library functions the game calls that have no name.

Two traps found:

- `xbe_symbols` matched `XInputGetCapabilities` "by constants" at `0x2E443F`, a USB descriptor routine that pops 4
  bytes, and wrote an `[abi]` line the recompiler rejects. Removed; the game never calls XInputGetCapabilities.
- In 4831 the `IDirectSoundStream_*` exports are 5-byte `jmp` thunks (`0x2932D3`..`0x2932E7`) into
  `CDirectSoundStream_*` bodies. A body with no name ran its original code against the host's stream object
  (`CDirectSoundStream_SetMixBinVolumes`, `0x2930C6`) and crashed after the audio banks loaded.

## Sections

| Section | VA | Size | |
|---|---|---|---|
| `.text` | 0x011000 | 2.15 MB | game code, XAPI, CRT |
| XONLINE, XNET | 0x238460, 0x250C80 | 98 KB, 74 KB | Xbox Live, network stack |
| D3D, D3DX, XGRPH | 0x2631E0.. | 76 KB, 106 KB, 8 KB | |
| DSOUND, WMADEC | 0x292160, 0x2B04C0 | 121 KB, 102 KB | |
| BINK, BINK* (15 small sections) | 0x2C9CC0.. | ~80 KB | RAD Bink 1 decoder and blitters |
| XPP | 0x2DF2A0 | 35 KB | controller and memory unit driver |
| `.rdata`, `.data` | 0x2E7D60, 0x33C840 | 339 KB, 1.98 MB (84 KB on disc) | |
| XON_RD, `.data1`, DOLBY, BINKDATA | | | |
| `$$XTIMAGE`, `$$XSIMAGE` | | | dashboard title and save images (inserted) |

The raw offset of each section is not `va - base` (unlike Breakdown), which Breakdown's `scripts/xbeinfo.py`
assumed; fixed in the copy here.

## Kernel imports

149 imports, every one implemented by the shared runtime (the boot log says "kernel imports resolved, 0 missing").
Notable groups: the full Nt* file and memory set, `PsCreateSystemThreadEx`, Ke timers and DPCs, `Hal*` disk and SMC
calls, `Xc*` crypto (Live), `PhyInitialize`/`PhyGetLinkState` (network PHY), `XeLoadSection`/`XeUnloadSection`.
Full list: `analysis/xbe.json` (`kernel_imports`) or `python scripts/thunk.py <slot>` for one call site.

## Code size and recompilation

`xbe_recomp`: 770 functions reached by direct calls from the entry, 10,717 accepted pointer seeds, 4,012 gap-filled
functions, 23,160 in total, 845,169 instructions over 2.72 MB of the 2.98 MB of code sections, 204 jump tables,
154 C++ catch blocks. The generated C++ is 120 MB in 64 partitions and compiles and links in 52 s with 6 jobs.

## Boot sequence (as observed)

1. Entry `0x16DB9` starts the main thread (`0x16D45`), mounts D:, T:, U: (`4d530017`), Z: (utility partition).
2. `CreateDevice` 640x480, X8R8G8B8, presentation interval `0x80000002` (two vertical blanks, immediate).
3. Opens `Movies.mgf`, `Text.mgf`, `T:\GlobalSettings.dat`, the profile `U:\59E5F0453F4D\SaveMeta.xbx`.
4. A Bink thread plays `Movie\NTSC\MAIntro.bik` (Microsoft Game Studios, WizKids, Day 1) while a worker thread
   (`0x29E80`) **decompresses `Common.mgf` (17 MB) into a 96 MB cache on Z:** (`z:\filecopy.tmp`, renamed to
   `z:\fbd13caa.cache`, plus `Z:\Control.cache`). On later boots the cache is reused.
5. At the movie's end (11 s) the menu state machine (`0x68530` looks up a state by hash, `0x685E0` switches) loads
   `menus\main\main.ggf` from the cache and the five audio banks.
6. Title "Press START" -> profile select -> main menu (Campaign, Multiplayer, Player Setup, Options) -> mission select
   -> Bink briefing -> 'Mech select -> mission load (a few seconds) -> play.

## Graphics

- One 640x480 back buffer; the game keeps its own render targets for the 'Mech preview (256x256) and effects.
- Nearly every draw is a raw NV2A push the engine writes itself (the perf log counts "171 draws (171 raw)" in play),
  with **fixed-function** vertex formats (FVF handles such as `0x112` XYZ|NORMAL|TEX1, `0x144` XYZRHW|DIFFUSE|TEX1
  for 2D), `SetTransform` for view/projection and `SetLight`/`SetMaterial` (both mapped by hand).
- Camera: `0x39100`'s body builds every camera's projection from the camera object (+0x80 near, +0x84 far, +0x88
  field of view, +0x8C orthographic flag) with `D3DXMatrixPerspectiveFovLH` (`0x276C5D`) or `D3DXMatrixOrthoLH`
  (`0x276CF1`), sets it (`SetTransform(1)` at `0x39210`), stores it at +0x1A0 and view*projection at +0x1E0 (culling).
  The widescreen hook sits between (`0x39209`).
- The game never reads the console's widescreen flag (only non-volatile settings 7, 9, 0x103, 0x104 and
  0x11/0xA are queried): it is a 4:3 title.
- In play: ~171 draws, 27k vertices, 1.4 MB of uploads per frame; 0.66 ms of host render time per frame.

## Timing

- The game times itself with `rdtsc`, calibrated at boot against the XAPI `QueryPerformanceCounter` (`0x2D0C0`).
- The frame clock (`0x1F0C4`..`0x1F3A0`) keeps a 64-entry ring of frame start times and a time scale
  (`0x33D8C0`, 1.0). Each frame's step `0x364600` is the scaled average of up to the last 4 frame times; a step over
  0.2 s (a load) is replaced by the previous step, or 0.03 s. Simulation code reads that step (seconds).
- So the simulation is **variable timestep**, not frame-locked. The console asks for a present every second blank
  (30 fps), but at 60 fps the game simply takes 1/60 s steps. A scripted walk at a 30 fps cap and at 60 fps ended in
  the same spot (captures `captures\215438` and `215558`).
- Bink movies keep their own clock and play at the correct speed at 60 fps.

## Audio

DirectSound HLE: the effects image comes from memory (1 effect), streams and buffers are created for music
(`Music*.d1Bank` streamed) and effects (`SFX*.d1Bank`). The game plays through the HLE without errors in the log;
whether every sound is right has not been checked by ear yet.

## Input

- XAPI gamepad only (`XGetDevices`, `XGetDeviceChanges`, `XInputOpen`, `XInputGetState`, `XInputSetState` for
  rumble). It never calls `XInputGetCapabilities`.
- Layouts (Player Setup > Edit Profile > Controls), "Normal": left stick move forward/back and left/right, right stick
  look up/down and rotate the torso; RT and A Fire Weapon; LT Cycle Weapons; X energy, Y ballistic, B missile weapon;
  left stick click Jump (jump jets: hold and steer); right stick click Defense; Back Start Menu; White Scoreboard;
  Black Toggle Global/Team Chat. Also "Southpaw" sticks and "Alternate"/"Wacky" buttons.

## Files and saves

- Profiles: `U:\<id>\MASave.sav` (6 KB), `SaveMeta.xbx`; settings `T:\GlobalSettings.dat` (36 bytes).
- The Z: cache needs ~92 MB on the host (`hdd\Z` in a dev build, `resources\saves\Z` in a release).
- `.mgf` archives start with `mgf ` + version; `Common.mgf` is compressed on disc ("ZZ" blocks), the cache holds it
  decompressed, with CRC checks per block (`0x1C210`).

## Unusual requirements

- Overlapped file I/O must report STATUS_PENDING (the cache copy skips decompressing a block otherwise).
- The cache file is renamed while open by the renaming handle.
- Xbox Live / System Link: XONLINE, XNET, XVOICE present; not exercised (single-player first).
