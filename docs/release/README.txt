MechAssault - native PC version 0.6.3 (unofficial, work in progress)
==================================================================

This is a native Windows version of MechAssault (original Xbox, 2002), made by static recompilation.
It contains no game code and no game files. You need your own copy of the original Xbox game.

First start
-----------
1. Extract this folder anywhere you can write to (not Program Files). No installation, no admin rights.
2. Start MechAssault.exe. The setup window opens on its Game tab.
3. Choose (or drop on the window) your game, either
     - a disc image made from your original disc (.iso: a full disc dump or an xiso), or
     - the folder with the extracted game files (default.xbe with the .mgf files and the Audio, media and Movie
       folders beside it).
   It is checked on the spot: MechAssault (USA) Rev 1, the 2003 reissue most copies are, is the release this version
   supports (the original 2002 disc, MechAssault (USA, Europe), is not yet). A copy patched by a mod
   pack or dump tool (region-free, HDD-ready, re-signed) is fine as long as its game code is unchanged. Your own
   image or folder is only read, never changed.
4. Tick the extras you want (fullscreen, a desktop shortcut, unlocking the whole campaign, online games, the game's
   language; most can be changed later in F1). Saved games and profiles go in resources\saves unless you browse for
   another folder under Game save location. Mouse aim is on; F1 > Controls turns it off. Then press Play.
5. The Prepare tab copies the game into resources\game and builds the game code for your PC from your copy (about a
   minute; this happens once). Then the Play tab shows your setup and the controls: press Play.

The first time the game starts it unpacks its common data into its hard disk cache (resources\saves, about 92 MB),
as the Xbox did. Later starts go straight into the game.

Picture and smoothness
----------------------
The game runs at 60 fps (the Xbox ran it at up to 30), or 120 fps or your monitor's rate (F1 > Display); it times
everything itself, so game speed is unchanged.
Render scale 1x is the Xbox's own 640x480; 2x, 3x and 4x draw at 2, 3 or 4 times that, and Auto (the default) draws
as many pixels as the picture gets on your screen (at most 2x on integrated graphics). When the picture is shown
larger than it is rendered, FSR 1 upscales it (or plain bilinear, your choice). Anti-aliasing (FXAA or SMAA),
anisotropic filtering, sharpening, texture sharpness, upscaling, FSR sharpening and draw distance are in F1 > Graphics;
brightness in F1 > Display; field of view and language in F1 > Game. Every setting applies at once, without a restart.
Widescreen (16:9, 21:9 and wider): the game shows more at the sides, nothing stretched or missing; the radar and weapon gauges
move out to the screen's edges, menus and movies stay 4:3 in the middle. F1 > Display > Aspect ratio can switch back
to the Xbox's 4:3 picture.

Multiplayer
-----------
System Link works on a LAN, and over the internet three ways, all from the game's own Multiplayer > System Link:
  - a virtual LAN (Radmin VPN, ZeroTier, Hamachi, Tailscale): pick its adapter in F1 > Network;
  - a direct address: the host's public address in F1 > Network > Join address (the host forwards UDP 1000-1002);
  - online games (the setup's "Online games"; switched on once its game list service is online, not yet in
    0.5): games hosted with it on show up in everyone's System Link list.
    Hosting needs a router with UPnP on (the game opens UDP 1000-1002 while it plays online) or those ports forwarded.
Xbox Live itself is gone and stays greyed out.
A new display mode or resolution asks to be kept and goes back by itself after 15 seconds.

Keys
----
F1      settings (also: hold Back and press Start on a controller)
F2      FPS counter
F8      mark a bug: saves a picture and a note of the game's state in resources\logs\bugs
Controller: any XInput/SDL controller, as on the Xbox.
Keyboard and mouse (defaults, all rebindable in F1 > Controls):
  WASD move, mouse turns the 'Mech and aims (or IJKL), left click fire, right click, Q or the mouse wheel cycle weapons,
  1 energy weapon, 2 ballistic weapon, 3 missile weapon, Space jump jets (hold), Left Shift or F defense,
  Enter fire / confirm, Backspace back, Esc pause, M start menu, Tab scoreboard, arrows d-pad.
  In the menus (and the pause menu) the highlight follows the mouse, left click selects, right click goes back and the
  wheel moves. With no controller plugged in, the game's button prompts show the mouse and keys instead, and a click
  on a prompt presses it.

Folders
-------
resources\game       your imported game (delete it to run the setup again)
resources\settings   settings.toml
resources\saves      saved games and the game's cache (the console hard disk)
resources\logs       logs and crash dumps: attach mechassault.log to bug reports
resources\runtime    the compiler and the game code built on your PC
licenses             licences of the included software

Reporting a problem
-------------------
After a crash a message box offers to save a problem report: a zip on your Desktop
(MechAssault-report-<date>-<time>.zip) with the logs, the newest crash dumps, settings.toml and a short info.txt.
Nothing is sent by itself: attach the zip to your report. To make one any time (for a freeze, say), start
MechAssault.exe --save-report (Windows key + R, then drag MechAssault.exe into the box and add --save-report).
The log (resources\logs\mechassault.log) lists Windows, the CPU, the graphics card and its driver, the settings, each
controller plugged in, and any warnings and errors; a crash adds the crash address and call stack.

If the setup says your copy is a release it does not support, it also saves MechAssault-copy-check.txt on your
Desktop with the copy's region, version, build date and fingerprints: send that file. MechAssault.exe --check-copy
makes the same check on any default.xbe, game folder or disc image you choose. It only reads; nothing is changed.

MechAssault is a trademark of Microsoft Corporation. This project is not affiliated with or endorsed by Microsoft,
Day 1 Studios or WizKids.
