MechAssault - native PC version (unofficial, work in progress)
=============================================================

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
   It is checked on the spot: MechAssault (USA, Europe) is the release this version supports. Your own image or
   folder is only read, never changed.
4. Tick the extras you want (fullscreen, a desktop shortcut, mouse aim, deleting the compiler afterwards) and
   press Play.
5. The Prepare tab copies the game into resources\game and builds the game code for your PC from your copy (about a
   minute; this happens once). Then the Play tab shows your setup and the controls: press Play.

The first time the game starts it unpacks its common data into its hard disk cache (resources\saves, about 92 MB),
as the Xbox did. Later starts go straight into the game.

Picture and smoothness
----------------------
The game runs at 60 fps (the Xbox ran it at up to 30); it times everything itself, so game speed is unchanged.
Render scale 1x is the Xbox's own 640x480; 2x, 3x and 4x draw at 2, 3 or 4 times that, and Auto (the default) draws
as many pixels as the picture gets on your screen (at most 2x on integrated graphics). When the picture is shown
larger than it is rendered, FSR 1 upscales it (or plain bilinear, your choice). Anti-aliasing (FXAA), anisotropic
filtering, upscaling and FSR sharpening are in F1 > Rendering; brightness in F1 > Display; field of view in
F1 > Gameplay. Every setting applies at once, without a restart.
Widescreen: on a wide screen the game shows more at the sides, nothing stretched; the HUD, menus and movies stay
4:3 in the middle. F1 > Display > Aspect ratio can switch back to the Xbox's 4:3 picture.
A new display mode or resolution asks to be kept and goes back by itself after 15 seconds.

Keys
----
F1      settings (also: hold Back and press Start on a controller)
F2      FPS counter
Controller: any XInput/SDL controller, as on the Xbox.
Keyboard and mouse (defaults, all rebindable in F1 > Controls):
  WASD move, mouse turns the torso and aims (or IJKL), left click fire, right click or Q cycle weapons,
  1 energy weapon, 2 ballistic weapon, 3 missile weapon, Space jump jets (hold), Left Shift or F defense,
  Enter fire / confirm, Backspace back, Esc pause, M start menu, Tab scoreboard, arrows or the mouse wheel d-pad.

Folders
-------
resources\game       your imported game (delete it to run the setup again)
resources\settings   settings.toml
resources\saves      saved games and the game's cache (the console hard disk)
resources\logs       logs and crash dumps: attach mechassault.log to bug reports
resources\runtime    the compiler and the game code built on your PC
licenses             licences of the included software

MechAssault is a trademark of Microsoft Corporation. This project is not affiliated with or endorsed by Microsoft,
Day 1 Studios or WizKids.
