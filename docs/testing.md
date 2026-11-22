# Test switches

Switches for unattended test runs of the Windows build. None of them changes how a player's copy behaves unless set.

## Environment variables

| Variable | Effect |
|---|---|
| `XBE_BACKGROUND=1` | A small window in the screen's corner that never takes the focus, no splash: runs on a PC someone is using |
| `XBE_PAD_SCRIPT=<file>` | Scripted controller input (`src/common/pad_script.cpp` describes the steps) |
| `XBE_SETUP_TEST=1` | The first-run setup runs by itself (windowed, no shortcut); `XBE_SETUP_SOURCE=<.iso or folder>` names the copy |
| `XBE_SELFTEST=1` | Checks of the recompiled code at the first camera build (`src/game/selftest.cpp`), logged as `selftest:` |
| `XBE_MENU_TAB=<name>` | The F1 menu opens on that tab (screenshots) |
| `XBE_NO_LAN=1` | System Link broadcasts reach only the join address and games listed online (online tests on one PC) |
| `XBE_NETLOG=1`, `XBE_IOLOG=1`, `XBE_KEYLOG=1` | Log every datagram, every file transfer, every texture key |
| `MECHASSAULT_UPDATE_URL=<url>` | The updater asks this address instead of GitHub's `releases/latest` for the newest release |
| `MECHASSAULT_REPORT_DIR=<folder>` | `MechAssault.exe --save-report` ("Make a problem report.bat") writes its zip there instead of the Desktop; exit 0 when written |

## Updates

The F1 menu's "Check for updates" asks GitHub for the latest release of `CaiusCodes/MechAssault-Xbox-PC-Recomp`
(`update_repo` in `src/common/game_info.cpp`, `sdk/runtime/app/update.cpp`). A release must attach the release package
(`release.bat`'s zip of the `MechAssault` folder) as an asset named exactly `MechAssault.zip`. A 404 (no releases, or a
private repository) shows "No releases yet". Versions compare by their numbers only: v0.5 < v0.5.1, and a development
build's `v0.4+21 (abc1234)` counts as v0.4.

Command-line runs, logged to `resources\logs\mechassault-update.log` (`logs\` in a build folder), with no window:

| Run | Exit code |
|---|---|
| `MechAssault.exe --check-updates` | 0 up to date or no releases yet, 10 a newer release, 1 the check failed |
| `MechAssault.exe --install-update` | as above, and a newer release is installed (no restart): 0 when it was |

Installing (portable layout only): the zip goes to `resources\runtime\update\`, Windows' `tar.exe` unpacks it (hidden),
`MechAssault\MechAssault.exe` must be inside; the running exe is renamed `MechAssault.old.exe`, the new one copied in
with `README.txt`, `licenses\` and `resources\runtime\compiler\`. `resources\game`, saves, settings and logs are never
touched. From the menu the new exe then starts with `--setup --updated=<pid>`, waits up to 10 s for the old one to end,
and its setup builds the game code again. Every start removes `MechAssault.old.exe` and `resources\runtime\update\`.

A local test: a release package in a test folder, a fake `releases/latest` JSON and zip served by
`python -m http.server`, and `MECHASSAULT_UPDATE_URL` pointing at the JSON:

```
{"tag_name": "v0.6", "assets": [{"name": "MechAssault.zip", "browser_download_url": "http://127.0.0.1:8799/MechAssault.zip"}]}
```

## Patched copies

The setup accepts the supported default.xbe (SHA-1 `682ddd1e...`) and any copy with the same code fingerprint
(`gameimport::XbeCodeSha1`: base, entry and kernel thunk with either key, each section's flags, address, sizes and bytes),
`919910b4f6fe5ba6b169754088dc61b89e77e2c9`, so region-free, HDD-ready, re-signed or debug-keyed copies import. The setup
and game logs show both hashes. `docs/release/Check my MechAssault copy.bat` reports a player's copy (region, version,
build date, libraries, both fingerprints and the 1 KB blocks that differ from the supported release, with their bytes).
