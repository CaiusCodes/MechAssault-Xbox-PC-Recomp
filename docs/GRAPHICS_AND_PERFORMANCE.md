# Graphics and performance

All numbers measured on j's PC (Ryzen 7 7700X, RTX 4070 Ti, Windows 11, 3440x1440 desktop at 100 Hz), windowed,
with `XBE_PERF=1`, in Mission 1 under the scripted pad unless stated. "Host" time is the renderer's own CPU time per
frame; "frames:" lines are the shared frame statistics (average, 1% low, p99, worst frame, stalls over 50 ms).

## Renderer architecture (shared with Breakdown)

Guest D3D8 calls and the game's raw NV2A pushes update one console GPU state; the D3D12 backend turns it into
pipeline states on a render thread, translates vertex programs, register combiners and the fixed-function pipeline
to HLSL (compiled on first use, cached on disk beside the DLL), and keeps console render targets as GPU textures. The
picture is rendered at a multiple of the console's 640x480 (render scale), then FXAA (optional) runs on the finished
frame, then it is scaled to the window (filtered downscale when the render is larger; FSR 1, or bilinear, when
smaller), then the brightness/gamma ramp, then the ImGui overlay.

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

Frame limiter (shared `app::Pace`): it slept in 1 ms steps and then yielded in a loop for the last 2 ms of every
frame. It now waits on a high-resolution waitable timer to within 0.3 ms and only yields for that. Measured with the
same scripted run (`runs\cpu_before`, `runs\cpu_after`, process CPU per 10 s): menus 0.15-0.23 -> 0.08-0.17
cores, play 0.21-0.25 -> 0.18-0.19 cores, with 60.00 fps, 1% low 56.5-57.9 and p99 17.2 ms unchanged (frame jitter
0.16-0.20 -> 0.30 ms). The game itself does not busy-wait.

## Render scale (Auto, 1x, 2x, 3x, 4x)

Reused from Breakdown unchanged: the console's 640x480 targets are created on the GPU at N times their size
(viewport, scissor and render-target sampling scaled with them); Auto picks the scale from the output size. It is a
real internal resolution change (more pixels rendered), not an upscale of a 640x480 image. Switching is live (targets
are recreated on the next frame; the game session is untouched).

Measured in Mission 1 (scripted walk, `scripts\bench_scales.ps1`, 1280x960 window, 4:3):

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
rates (its clock allows them; the default cap is 60).

Low end: the same PC's integrated **AMD Radeon Graphics** (Ryzen 7 7700X, 2 RDNA2 compute units, 485 MB dedicated),
picked with `XBE_GPU=low`; it renders while the window is on the RTX's display, so every frame is also copied across
adapters (the "present" 2.6-2.9 ms below), which a real iGPU-only PC would not pay:

| Render scale | AA | 60 fps cap: avg / 1% low / worst | Uncapped |
|---|---|---|---|
| 1x | FXAA | 60.00 / 49.1 / 35.8 ms (one load hitch) | 245 fps (4.1 ms) |
| 1x | off | | 255 fps (3.9 ms) |
| 2x | FXAA | 60.00 / 57.2 / 18.2 ms | 246-248 fps (4.0 ms) |
| 2x | off | 60.00 / 57.1 / 18.3 ms | |
| 3x | FXAA | 60.00 / 57.4 / 18.3 ms, but 21 ms p99 and a 92 ms frame in the next window | 122-146 fps (6.9-8.2 ms), 54 stalls in one 10 s window |

So even this very small iGPU holds 60 fps at 2x; 3x starts to stall. **Auto now stays at 2x or below on an
integrated GPU** (under 1 GB of dedicated memory; logged as "integrated GPU ... render scale Auto at most 2x", and the
menu says so), and FSR 1 upscales the rest of the way. A fixed 3x or 4x can still be chosen.

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

When: only when the picture is shown larger than it was rendered (a fixed render scale below the screen's
resolution, or Auto on an integrated GPU, where Auto stops at 2x). F1 > Rendering > Upscaling: Bilinear or FSR 1 (default), and "FSR sharpening" 0-20 (tenths of
FSR's stops; 18 = FSR's usual 0.2 stops, 0 = off). Live, no restart. FXAA, when on, runs before it at the render
size.

Checked: 640x480 (1x) to 1280x960 in Mission 1: the log says "FSR 1 upscaling 640x480 to 1280x960", 60.00 fps, 1%
low 57.3 (same as bilinear); edges visibly crisper than bilinear with no halos or ringing in a 2x zoom
(`runs\fsr_compare.png`: bilinear left, FSR right). Not compared against AMD's reference output, so treat it as
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

21:9 verified (window 1680x720: view 1.75 times 4:3, camera x scale 1.6845 -> 0.9626; `runs\wide219\009`): wider
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

Verified live in Mission 1 with `scripts\pad_live_settings.txt` (the pad script's `set` step changes a setting the
way the menu does): render scale Auto -> 1x (FSR engaged) -> 3x -> 1x -> Auto, aspect 4:3 -> Widescreen -> 4:3,
window 1280x960 -> 1600x900 -> 1280x960, FSR off/on, brightness 130% and back, FXAA off, while walking. No restart,
no crash, the mission carried on; 60 fps throughout with the worst frame 35 ms at a render-scale switch
(`runs\live1&.png`: the 1600x900 window in Widescreen).

Nothing needs a restart. Monitor selection is "the monitor the window is on". Brightness: F1 >
Display > Brightness (50-200%), a gamma curve applied through the renderer's gamma ramp on the finished picture
(the title's own ramp, when it sets one, is bent the same way); live. Field of view: F1 > Gameplay > Field of
view, 80-130% of the game's own (default 100%), through the same camera hook as widescreen, so culling follows;
live (`runs\fov_sheet.png`: 100%, 125%, 90%). It applies to every 4:3 perspective camera, cutscenes included, and
wide values show more sky and scenery than the designers framed; nothing broke at 125% in Mission 1.

## Performance diagnostics

F2 cycles the overlay: off, FPS counter, counter + diagnostics (frame time graph, 1% low, CPU/GPU-side timings,
process CPU and memory). `diagnostics_log` writes a frame timing summary every 10 s. `XBE_PERF=1` adds per-second
renderer lines (draws, uploads, shader compiles, waits) and, new here, the audio output's peak and RMS every 10 s.
The shader disk cache removes the first-use compile stalls on later runs. `XBE_D3D12_DEBUG=1` enables the D3D12
debug layer and now also writes its messages to the log: a run from boot through the menus, the briefing and
Mission 1's start logged **no** debug-layer error or warning.

## Low-end defaults

Defaults: Auto render scale (output-sized; at most 2x on an integrated GPU), FSR 1 when upscaling, FXAA on,
V-Sync on, 60 fps cap, anisotropic filtering as the game asks. The renderer costs under 1 ms of CPU per frame at 2x
here; the GPU cost scales with render scale (tables above). GPU detection is by dedicated memory only (integrated or not); a weak discrete GPU on a 4K display would still
render at 4K under Auto. Next: an Auto that lowers the scale when the measured GPU frame time exceeds a budget.
