Breakdown - native PC version (unofficial)
==========================================

This is a native Windows version of Namco's Breakdown (original Xbox, 2004), made by static recompilation.
It contains no game code and no game files. You need your own copy of the original Xbox game.

First start
-----------
1. Extract this folder anywhere you can write to (not Program Files). No installation, no admin rights.
2. Start Breakdown.exe. The setup window opens on its Game tab.
3. Choose (or drop on the window) your game, either
     - a disc image made from your original disc (.iso: a full disc dump or an xiso), or
     - the folder with the extracted game files (default.xbe and the romdata and stream folders).
   A copy beside the folder is found by itself. It is checked on the spot: Breakdown (USA) is the release this
   version supports. Your own image or folder is only read, never changed.
4. Tick the extras you want (fullscreen, a desktop shortcut, 60 fps gameplay, mouse look, deleting the compiler
   afterwards) and press Play.
5. The Prepare tab copies the game into resources\game and builds the game code for your PC from your copy (about a
   minute; this happens once). Then the Play tab shows your setup and the controls: press Play.

Later starts go straight into the game. After an update the setup window opens once more to rebuild the game code.

Picture and smoothness
----------------------
Render scale 1x is the Xbox's own 640x480; 2x, 3x and 4x draw at 2, 3 or 4 times that. A scale above your
display's resolution is supersampling: the extra detail is averaged down, which smooths edges further.
Anti-aliasing (FXAA, on by default) and anisotropic filtering (sharper floors and walls at an angle) are in
F1 > Rendering.
Widescreen: on a wide screen (16:9, 16:10, 21:9 ultrawide, up to 32:9) the game shows more of the scene at the
sides, nothing stretched; the HUD, menus and movies stay 4:3 in the middle. F1 > Display > Aspect ratio can switch
back to the Xbox's 4:3 picture, or stretch the 4:3 picture instead. Render scale Auto (the default) draws as many
pixels as the picture gets across your screen, so ultrawide stays sharp; a fixed 1x-4x scale is widened from a 4:3
image and looks softer at 21:9.
60 fps is perfectly even on a 60, 120, 180 or 240 Hz display, or any display with G-Sync or FreeSync. At other rates
(100, 144, 165 Hz) frames can't all show for the same time; set Windows to 120 Hz or 60 Hz, or pick Fullscreen in
F1 > Display and a refresh rate marked "even 60 fps".

Keys
----
F1      settings: display, resolution, refresh rate, V-Sync, frame-rate limit, render scale, texture filtering,
        anti-aliasing, FPS counter, 60 fps gameplay, mouse look and sensitivity, and every key and mouse button
        (Controls)
F3      saves a thread dump to the log (for bug reports)
Controller: any XInput/SDL controller, as on the Xbox. Hold Back and press Start for the settings (A changes,
        B closes).
Keyboard and mouse (defaults, all rebindable in F1 > Controls):
  WASD move, mouse look (or IJKL), left click attack 1, right click attack 2, middle click or Enter lock-on,
  Backspace cancel, E access, Q change weapon, Space jump, G grenade, Left Shift guard, C or Left Ctrl crouch,
  arrows or the mouse wheel D-pad (the wheel moves through menus and changes weapon), Esc pause (Start), Tab Back.

Folders
-------
resources\game       your imported game (delete it to run the setup again)
resources\settings   settings.toml
resources\saves      saved games (the console hard disk)
resources\logs       logs and crash dumps: attach breakdown.log to bug reports
resources\runtime    the compiler and the game code built on your PC
licenses             licences of the included software

Breakdown is a trademark of Bandai Namco Entertainment. This project is not affiliated with or endorsed by Bandai
Namco or Microsoft.
