// breakdown-recomp patch: true widescreen (Hor+) for a title that only draws 4:3. The title's hooks widen its camera
// by app::WideView and the frame is shown that much wider than 4:3, so its 3D needs nothing more here. Flat vertices
// (w exactly 1: screen space, orthographic, or a vertex program's 2D pass; Breakdown draws its HUD, text and menus
// with programs that way) would come out stretched, so each renderer moves them back toward the middle of the target
// by Factor. Left alone: vertices on the target's left or right edge (fades, cutscene bars, full screen passes span
// it; a movie's frame, YUY2 or UYVY, is squeezed edges too, so it keeps its 4:3 shape between bars), draws that read
// a render target (screen effects must stay over what they read), targets that are not 4:3 (shadow maps,
// reflections, effect buffers).
#pragma once

#include <cstdint>

#include <atomic>

namespace gpu::wide {

// mechassault-recomp patch: the title is in play (not a menu or its pause menu): only then may HudShift take a new
// texture for a HUD piece; a paused game's popup (its frame's edges, its prompts) keeps the 4:3 layout
inline std::atomic<bool> g_hud_learn{true};

// once a frame, from the renderer's thread: app::WideView()
void SetView(float view);
float View();

// a draw into a gw x gh target: what its flat vertices' distance from the middle is scaled by; 1 for none
float Factor(uint32_t gw, uint32_t gh, bool reads_target);

// a texture format (D3DFORMAT) a movie's frames come in: its draw is squeezed edges and all
inline bool MovieFormat(uint32_t format) { return format == 0x24 || format == 0x25; }  // YUY2, UYVY

// one vertex: x in target pixels, w its clip w, width the target's
inline float SqueezeX(float x, float w, float width, float k, bool edges) {
    if (k == 1.0f || w != 1.0f || (!edges && (x <= 0.5f || x >= width - 0.5f))) return x;
    return 0.5f * width + (x - 0.5f * width) * k;
}

}  // namespace gpu::wide
