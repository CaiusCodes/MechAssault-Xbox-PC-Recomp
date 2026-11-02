# Graphics and performance

All numbers measured on j's PC (Ryzen 7 7700X, RTX 4070 Ti, Windows 11, 3440x1440 desktop at 100 Hz), windowed,
with `XBE_PERF=1`, in Mission 1 under the scripted pad unless stated. "Host" time is the renderer's own CPU time per
frame; "frames:" lines are the shared frame statistics (average, 1% low, p99, worst frame, stalls over 50 ms).

## Renderer architecture (shared with Breakdown)

Guest D3D8 calls and the game's raw NV2A pushes update one console GPU state; the D3D12 backend turns it into
pipeline states on a render thread, translates vertex programs, register combiners and the fixed-function pipeline
to HLSL (compiled on first use, cached on disk beside the DLL), and keeps console render targets as GPU textures. The
picture is rendered at a multiple of the console's 640x480 (render scale), then FXAA (optional) runs on the finished
frame, then it is scaled to the window (filtered downscale when the render is larger, bilinear when smaller), then
the ImGui overlay.

MechAssault specifics: nearly all draws are raw pushes the engine writes itself with fixed-function vertex formats,
plus pretransformed (XYZRHW) 2D for the HUD and menus. Its own render targets: the 'Mech preview (256x256) and a few
effect targets.

## 60 fps

| Question | Answer | Evidence |
|---|---|---|
| What does the console run at? | The device asks for a present every second vertical blank (interval `0x80000002`): 30 fps at best | `d3d: CreateDevice ... interval 80000002` |
| What paces the simulation? | Its own clock: `rdtsc` (calibrated against `QueryPerformanceCounter` at boot) -> a 64-entry ring of frame times; each frame's step (`0x364600`, seconds) is the time scale times the average of up to the last 4 frames; a step over 0.2 s (a load) is replaced by the previous one or 0.03 s | disassembly of `0x1F0C4`..`0x1F3A0`, MECHASSAULT_TECHNICAL_ANALYSIS.md |
| Frame-count dependence? | None found in the clock; animation, physics, AI, projectiles and cooldowns read the step | the clock above; the same scripted 4 s walk + 1 s turn at a 30 fps cap and at 60 fps end at the same spot (`captures\215438` vs `215558`) |
| Movies | Bink keeps its own clock; plays at normal speed at 60 fps | briefing and logo movies |
| Result | **60 fps is the default and safe**; no decoupling or interpolation needed | |

Measured at 60 fps in play (Mission 1, 1280x960 window, Auto scale = 2x, FXAA): 60.00 fps, 1% low 56.7-57.8 fps,
p99 17.2 ms, worst frame 17.5-18.5 ms with 0 stalls in steady play; the only stalls are at level loads and the
first use of a shader (100-150 ms once). Menus: 60.00 fps. CPU: 0.27 cores for the whole process; 380 MB working set.
Renderer: 0.66-0.8 ms host time per frame (171-351 draws, 20-32k vertices, 1.4-1.7 MB of uploads).

Display rates: the console's vertical blank follows the display when it is a multiple of 60 Hz (60, 120, 180, 240)
and otherwise runs on a 60 Hz timer (this PC's 100 Hz display: measured fallback). The game itself paces by time, so
speed is right at any rate; the cadence is only even on 60/120 Hz or with VRR. 59.94, 120 and 144 Hz displays were
not available to test here (the session must not change the desktop's mode).

Frame cap and V-Sync: F1 > Display (Off, display refresh, 30, 60, 120, 144). Above 60 the game would step at the
higher rate too (its clock allows it); only 30 and 60 were measured.

No busy-wait: the main thread waits in the frame limiter (`limiter 15 ms` of each 16.7 ms frame in the log), CPU
use stays at about a quarter of one core.

## Render scale (Auto, 1x, 2x, 3x, 4x)

Reused from Breakdown unchanged: the console's 640x480 targets are created on the GPU at N times their size
(viewport, scissor and render-target sampling scaled with them); Auto picks the scale from the output size. It is a
real internal resolution change (more pixels rendered), not an upscale of a 640x480 image. Switching is live (targets
are recreated on the next frame; the game session is untouched).

Measured in Mission 1 (scripted walk, `scriptsench_scales.ps1`, 1280x960 window, 4:3):

| Render scale (internal size) | AA | 60 fps cap: avg / 1% low / p99 / worst | Uncapped (V-Sync off): avg fps, frame time |
|---|---|---|---|
| 1x (640x480) | FXAA | 60.00 / 57.7-58.0 / 17.2 ms / 17.5 ms | 746-862 fps, 1.16-1.34 ms |
| 2x (1280x960) | FXAA | 60.00 / 57.4-57.7 / 17.2 ms / 17.7 ms | 741-848 fps, 1.18-1.35 ms |
| 3x (1920x1440) | FXAA | 60.00 / 58.3 / 17.1 ms / 17.6 ms | 715-809 fps, 1.24-1.40 ms |
| 4x (2560x1920) | FXAA | 60.00 / 57.8-58.2 / 17.1 ms / 17.5 ms | 684-780 fps, 1.28-1.46 ms |
| 4x | off | | 711-816 fps, 1.23-1.41 ms |
| 2x | off | 60.00 / 57.2-57.8 / 17.4 ms / 17.6 ms | |

So on this PC the game is nowhere near GPU-bound: 4x costs about 0.1 ms a frame over 1x, FXAA at 4x about 0.05 ms,
and the renderer's own CPU time stays 0.47-0.62 ms. Uncapped the game ran at 700-860 fps, which says the
renderer/runtime overhead per frame is about 1.2 ms in total; the game was not checked for correctness at such
rates (its clock allows them; the default cap is 60). Low-end GPUs were not available to test.

## Anti-aliasing

| Mode | State |
|---|---|
| Off | yes |
| FXAA (on the finished frame, at render scale; the game never reads the smoothed copy) | yes, default |
| Supersampling (render scale above the window) | yes: the downscale filters up to 4x4 samples per pixel |
| MSAA | not implemented: needs multisampled targets plus a resolve before every place the game reads a target as a texture |
| SMAA | not implemented |

## FSR

Breakdown has **no FSR implementation** (its `enhancements-plan.md` lists FSR 1 as planned; there is no FSR code in
its `sdk/`), so there was nothing proven to reuse. This session added **FSR 1** to the shared D3D12 present path
(`sdk/runtime/gpu/d3d12.cpp`, `kEasuHlsl`/`kRcasHlsl`, `Upscale()`), written from AMD's published description of the
algorithm, not AMD's `ffx_fsr1.h` (which this session did not download):

- **EASU**: per output pixel, 12 input texels; luma gradients of the four nearest texels give the edge direction and
  strength; a Lanczos-like kernel (polynomial approximation) stretched along the edge and narrowed across it; the
  result clamped to the four nearest texels (no ringing).
- **RCAS**: sharpening on a 5-texel cross, its lobe limited so no channel leaves the neighbours' range, and reduced
  on noise.

When: only when the picture is shown larger than it was rendered (render scale 1x-3x below the screen's resolution;
Auto never upscales). F1 > Rendering > Upscaling: Bilinear or FSR 1 (default), and "FSR sharpening" 0-20 (tenths of
FSR's stops; 18 = FSR's usual 0.2 stops, 0 = off). Live, no restart. FXAA, when on, runs before it at the render
size.

Checked: 640x480 (1x) to 1280x960 in Mission 1: the log says "FSR 1 upscaling 640x480 to 1280x960", 60.00 fps, 1%
low 57.3 (same as bilinear); edges visibly crisper than bilinear with no halos or ringing in a 2x zoom
(`runssr_compare.png`: bilinear left, FSR right). Not compared against AMD's reference output, so treat it as
"FSR 1 algorithm, own implementation".

## Widescreen and ultrawide

| Mode | What it does | State |
|---|---|---|
| Widescreen (default) | The game's cameras are widened to the display's shape (Hor+); the HUD, text and menus are squeezed back into a centred 4:3 area by the shared renderer rule; movies keep 4:3 between bars | 16:9 (gameplay, cutscene, menus) and 21:9 (gameplay) verified |
| 4:3 | the console picture, pillarboxed | yes |
| 16:9 stretched / fill | the 4:3 picture stretched | yes (also tells the title it is on a widescreen TV; MechAssault ignores that) |

How (src/game/widescreen.cpp): every camera's projection is built in one function; a `[[hook]]` between
`D3DXMatrixPerspectiveFovLH` and `SetTransform` divides the clip x column by the display's width over 4:3, only for
4:3 perspective cameras. The same widened matrix feeds the game's culling, so nothing at the new edges is culled.
Cameras are rebuilt 120 times a second in play (two cameras a frame at 60 fps; `XBE_PERF` logs "camera builds a
second"), so a change applies on the next frame.

21:9 verified (window 1680x720: view 1.75 times 4:3, camera x scale 1.6845 -> 0.9626; `runs\wide219 9`): wider
view, nothing stretched, HUD at its 4:3 place in the middle. 32:9 uses the same code (WideView is clamped to 32:9),
not looked at. Render scale Auto renders as many pixels as the picture is wide, so ultrawide stays sharp.

## Display settings and live application

| Setting | Applies |
|---|---|
| Display mode (fullscreen / borderless / windowed), resolution, refresh rate | immediately (window/swap chain recreated); **now with a 15 s keep-or-revert prompt** |
| V-Sync, frame-rate limit | immediately |
| Render scale | immediately (targets recreated, session kept) |
| Anti-aliasing, anisotropic filtering, upscaling (FSR 1) and its sharpening | immediately |
| Aspect ratio | immediately (next camera build and frame) |
| Performance overlay, frame-timing log | immediately |
| Mouse aim, sensitivity, invert, key bindings | immediately |

Nothing needs a restart. Monitor selection is "the monitor the window is on". Brightness/gamma: no PC slider yet
(the shared renderer supports a gamma ramp; MechAssault was not seen setting one). FOV: the widescreen hook is the only FOV change; a separate FOV slider would use
the same hook but is not exposed.

## Performance diagnostics

F2 cycles the overlay: off, FPS counter, counter + diagnostics (frame time graph, 1% low, CPU/GPU-side timings,
process CPU and memory). `diagnostics_log` writes a frame timing summary every 10 s. `XBE_PERF=1` adds per-second
renderer lines (draws, uploads, shader compiles, waits) and, new here, the audio output's peak and RMS every 10 s.
The shader disk cache removes the first-use compile stalls on later runs.

## Low-end defaults

Defaults: Auto render scale (output-sized), FXAA on, V-Sync on, 60 fps cap, anisotropic filtering as the game
asks. The renderer costs under 1 ms of CPU per frame at 2x here; the GPU cost scales with render scale (table
above). There is no GPU-tier detection yet: Auto is resolution-based, so a weak GPU on a 4K display would render at
4K. Next: an "Auto" that caps the scale when the measured GPU frame time exceeds a budget.
