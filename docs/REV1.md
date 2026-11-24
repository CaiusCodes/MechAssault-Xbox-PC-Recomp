# MechAssault Rev 1 (2003): the main release

Most players have the 2003 reissue, so this port is made from it. The original 2002 release keeps its configuration
(`config/2002/`) and its addresses (`src/common/release.h`).

| | Original | Rev 1 |
|---|---|---|
| Disc | MechAssault (USA, Europe) (En,Fr,De) | MechAssault (USA) (En,Fr,De) (Rev 1) |
| default.xbe | 3,559,424 bytes, built 2002-10-13, certificate version 1 | 3,727,360 bytes, built 2003-08-08, version 8 |
| SHA-1 | `682ddd1ecda1e5110f9339dab0c31d296d51777b` | `372a01bbe34b0a12c209126b0978784d724d05da` |
| Code fingerprint (`XbeCodeSha1`) | `919910b4f6fe5ba6b169754088dc61b89e77e2c9` | `014624358ee6a930246748eebdcc6e174d769e93` |
| Entry, kernel thunk | 0x16DB9, 0x2E7D60 | 0x17999, 0x307E20 |
| .text | 0x11000 + 0x227460 | 0x11000 + 0x247130 |

## What Rev 1 is

The game's data is the same: every mission, map, movie and text file is byte for byte the 2002 one. Changed are
`Audio/Music.d1Bank` (same size), `media/dlcustom.xpr`, `media/Sound.xsb` and `media/Wave.xwb` (more front-end
sounds), and the Live executables (`dashupdate.xbe`, `downloader.xbe`); the Xbox Demos folder is gone. The new
`default.xbe` is the Xbox Live update build: downloadable content support (Capture the Flag and Checkpoint modes, the
"Premium DLC" triggers, mechs from content packages, `Package.ini`), Friends/Leaderboard, kicking and muting players,
rejoining, and a rewritten lobby and messaging layer (`Core Technology\nal\Src\Lobby`, `Messaging`). No new maps are
on the disc; the new modes need the downloaded packs.

## How the port was moved

`scripts/rev1/xbe_match.py` maps both images, masks every 32-bit value in the image range (so code that only moved
still compares equal) and indexes Rev 1's code in 32-byte windows. 68% of the 2002 code's blocks are found unchanged:
the libraries 93 to 100% (D3D, DSound, XNet, Bink, WMA, XPP), the game's own `.text` 59% (much of the rest differs
only in relative call offsets). An address is placed where the 48 bytes around it occur exactly once.

- `config/game.toml`: written by `xbe_symbols` for Rev 1 (138 of 248 functions, 12 of 13 data, as for 2002) with the
  same two hand fixes: the WMA decoder method entry (0x2D0BA4, 2002 0x2B0AE4) and no `[abi]` line for the false
  XInputGetCapabilities match (0x3044FF).
- `config/mechassault.toml`: `scripts/rev1/translate_config.py` placed 52 of the 66 hand-checked addresses by
  matching; 13 short XNet stubs that occur several times by the XNet library's shift (+0x200C0, which every placed
  XNet function shares, checked on each stub's bytes); WSAGetLastError (0x27186E) as the stub that jumps to XAPI's
  GetLastError (0x17ACE, matched); the campaign hook (0x8D6D3) by hand, the same code after the profile load in the
  campaign menu (0x8D690). Each line keeps its 2002 address.
- Host addresses (`src/common/release.h`, chosen at run time from `.text`'s size in the loaded header): the pilot and
  'Mech vtables (0x31C708, 0x31FCA0: every placed entry agrees with the 2002 tables), the campaign progress block
  (0x3B1FF8) and memcpy (0x1ED440) by matching; the pilot pointer (0x3B3A24) and the in-play word (0x380100) by the
  data shift of their neighbours (+0x2BD98, +0x29040). Checked in a run (`XBE_PEEK`, `logf`): 0x380100 reads 0 on
  the title and menus and in the pause menu, 0x10 in Mission 1; the pilot chain gives the 'Mech's heading at +0x2B0
  (0 -> 272 degrees while the right stick turns it) and the view pitch (now the controller's +0x178, issue #3; pilot +0x478 only matched it in Mission 1) (50, the stick's limit, with the
  stick held down). The memcpy selftest passes (65 lengths x 16 alignments).

## Building

The default build (`out\build\release`, `assets\`, `generated\`) is left as it was. Rev 1 builds into its own folder:

```
scripts\b.bat cmake --preset release -B out/build/rev1 "-DXBE_ASSETS_DIR=<repo>/game_files/MechAssault (USA) (En,Fr,De) (Rev 1)" -DXBE_GENERATED_DIR=<repo>/generated-rev1
scripts\b.bat cmake --build out/build/rev1 -j 6
```

with `out\build\rev1\assets` a junction to the Rev 1 game folder (the runtime looks for `assets` beside the exe).

## Tested (2026-11-09, background runs)

- Boots at 60 fps; the memcpy selftest passes; Mission 1 plays (walk, turn, fire), widescreen camera and HUD.
- Unlock whole campaign: the hook fires as the campaign menu opens ("2 nodes were played, now 24"); Select Mission
  lists everything and Continue plays the ending.
- Local splitscreen (Grinder, Colosseum) with two players.
- System Link between two Rev 1 copies: hosted, joined, Destruction on Colosseum played in sync.
- A 2002 copy does not see a Rev 1 host's game (same test, the joiner the 2002 build): the two releases don't play
  together, so everyone needs Rev 1 for multiplayer.
- The release package (`release.bat rev1`) installs from the Rev 1 .iso on its own: imports, builds the game code
  on the player's PC from the shipped Rev 1 configs, plays.

Not yet: the original 2002 release in the same package (its configs and addresses are kept for that), a full
mission sweep, Xbox Live/DLC (not supported).
