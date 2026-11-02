# Known issues

Status as of 2026-11-02 (overnight session). "Shared" = in `sdk/` (would affect every title), "Game" = MechAssault's
game layer, "Project" = build, tools, packaging.

## Fixed this session

| Issue | Kind | Fix |
|---|---|---|
| First boot crashed writing guest 0x18 in DirectSound (`0x29241E`): `IDirectSound_SetMixBinHeadroom` ran its original code against the host's DirectSound object | Game (missing symbol) | `0x29313F` named in `config/mechassault.toml`, with 11 more from the XbSymbolDatabase pattern scan |
| `xbe_symbols` wrote `[abi] XInputGetCapabilities = ["?", "?"]` for a false match (a USB descriptor routine), and the recompiler refused the config | Project | `[abi]` line removed from `game.toml`, symbol set to 0 in `mechassault.toml` |
| Title screen never left its loading state: the hard disk cache `z:\filecopy.tmp` could not be renamed to `z:\fbd13caa.cache` | Shared | `NtSetInformationFile` rename closes and reopens the handle's own file (Windows refuses to rename a file the renaming handle holds open) |
| With the cache renamed, it was 96 MB of zeros past the first block; then a crash loading `menus\main\main.ggf` (guest 0x30 at `0x84BB0`) | Shared | Overlapped reads/writes now return STATUS_PENDING after completing; the game's copy only decompresses a block on the pending path |
| Crash right after the audio banks load, writing guest 0x30 in `0x2924AE` (CDirectSoundVoiceSettings_SetMixBinVolumes) | Game (missing symbol) | `CDirectSoundStream_SetMixBinVolumes = 0x2930C6` (the stream's body behind a 4831 `jmp` thunk) |
| Dev builds rebuilt `MechAssault.dll` with 16 compilers on every launch after an executable-only relink | Project | CMake post-build marks the DLL current (Ninja still relinks it whenever the executable's exports change) |
| Release builds silently dropped Info and Warn log lines | Shared | `XBE_LOG_LEVEL=info` or `warn` |
| The F1 menu was titled "Breakdown PC settings" | Shared | uses `GameInfo::short_title` |
| A new display mode could not be undone if the display could not show it | Shared | keep-or-revert prompt, reverts after 15 s |
| No FSR | Shared | FSR 1 (EASU + RCAS) in the present path, used when upscaling |
| **Player-side builds ignored the title's own config**: the release's first start recompiled from `game.toml` alone (the config with the `[xbe]` path), not `main.toml`, which includes the hand-checked symbols and hooks. The packaged game then crashed in DirectSound after the audio banks loaded, exactly like the first dev boots | Shared | `app/game_code.cpp` recompiles from the config that includes the xbe's config, and still reads partitions from the latter. **Breakdown's release has the same code**: its `breakdown.toml` symbols (push buffers, palettes, SetLFO) would be missing from players' builds; worth checking there |
| The setup copied 440 MB the game never reads (XDemos, dashboard update, downloader) | Game/Shared | `Profile::skip` in the import |
| Mouse turns came out short (20 and 60 degrees for 45 and 90) | Game | the mouse model follows the game's measured torso ramp; now 45 and 90 |

## Open

| Issue | Kind | Impact / next step |
|---|---|---|
| Audio not checked by ear | Shared | The perf log now prints the output's peak/RMS every 10 s; see PORTING_PROGRESS for the measured levels. Someone has to listen to music, weapon and voice playback. |
| Mouse aiming goes through the right stick (`src/common/mouse_aim.cpp`) with a model of the game's response: dead zone 0.30 (read from its code), top rate ~150 deg/s reached through a ~0.4 s ramp (measured) | Game | Yaw verified with synthetic mouse counts: 45 and 90 degrees asked turned 45 and 90 by the HUD compass. Pitch is only roughly modelled (60% of the yaw rates; 15 degrees asked looked like 9-10 in captures) and its direction follows the game's default Y axis, where the stick's up looks down: with the game's own Invert Y switched, the mouse flips too (use F1 > Invert mouse Y). Not yet moved by a person's hand. Writing the torso angle directly would be exact but needs the player object mapped. |
| Keyboard and mouse not tried by a person | Game | Defaults are set from the game's own layout; the scripted pad proved the same controls through XInput. |
| FSR 1 is an own implementation of AMD's published algorithm, not AMD's reference shader | Shared | Looks right in a 1x -> 1280x960 test; not compared pixel for pixel with `ffx_fsr1.h`. Vendoring AMD's MIT shader would make it the reference one. |
| MSAA / SMAA not available | Shared | FXAA and render-scale supersampling are what exists. MSAA needs resolves before every render-target read; not started. |
| Splitscreen HUDs in Widescreen sit a little toward the middle of the screen | Shared rule | The squeeze of flat draws is around the whole screen's centre, not each half's; readable, but not where a native widescreen layout would put them. |
| Menus' full-screen backgrounds are stretched in Widescreen | Shared rule | Same rule as Breakdown: draws touching both side edges stay full width (fades and backgrounds). The menu items stay 4:3. Acceptable, or detect background quads per title later. |
| Widescreen checked on the first campaign map (16:9, 21:9), its cutscenes, the menus, the briefing and 'Mech select (its preview stays in shape); not on Colosseum or later maps | Game | The hook widens the game's own culling matrix too, so nothing should pop at the sides. |
| 60 Hz emulated vertical blank on this 100 Hz display | Display | The game paces by time, so motion is right, but 60 fps on 100 Hz shows uneven cadence; use 60/120 Hz or VRR (the menu says so). |
| Xbox Live, System Link, voice | Game | Out of scope tonight. The menu shows System Link and Xbox Live greyed out: the game's own network stack (XNET section) runs against no network card and reports the link down. The SDK's XNet HLE (System Link over LAN for other titles) needs the whole XNet/Winsock set named, about 25 functions; the pattern scan finds 8 of them for 4831 (`analysis/oovpa_scan.txt`: XNetStartup, socket, bind, connect, listen, recv, send, ioctlsocket) and not XNetGetEthernetLinkStatus, sendto/recvfrom, select or closesocket. Naming only some would mix the host's sockets with the game's, so none are named yet. |
| Gap fill decodes some data as code (`unimplemented aaa/bound/insb...` in `analysis/xbe_recomp.log`) | Shared | Harmless unless reached; a trap would name the address. Mark the ranges as `[xbe] data_ranges` if one ever is. |
| The shared-layer fixes (file rename, overlapped I/O, player-side config, limiter, FSR, ...) are only in this repository | Shared | They belong in Breakdown's `sdk/` and upstream too, after Breakdown's own boots are re-run with them (j's decision); BREAKDOWN_REUSE_AUDIT.md lists them. |
| Campaign maps after the first not reached | Game | Unlocking needs playing through (the profile save is signed); a person playing, or a longer scripted run, would cover them. |
| Possible artifacts to compare with the console: dark jagged silhouettes near the camera in fog (`runs\stress1\012`), a hard-edged spray sprite in the waterfall mist | Unknown | May be correct; needs console footage. |
| Pushing to GitHub was refused by the session's permission rules | Project | All work is committed locally on `main` in the MechAssault folder; push when ready. |
