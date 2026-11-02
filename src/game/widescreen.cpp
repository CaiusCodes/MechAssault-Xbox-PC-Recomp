// True widescreen (Hor+). MechAssault draws 4:3 whatever the console reports (a run told a 16:9 TV drew the same
// frames; it never reads the video flags). Every camera builds its projection in one place, 0x39100's body: from the
// camera object's near (+0x80), far (+0x84), field of view (+0x88) and aspect it calls D3DXMatrixPerspectiveFovLH
// 0x276C5D (or D3DXMatrixOrthoLH 0x276CF1 when +0x8C is set) into a matrix at [esp+0x40], sets it on the device
// (SetTransform(1, ...) at 0x39210), keeps a copy at +0x1A0 and multiplies it with the view into +0x1E0, which the
// culling reads. A [[hook]] at 0x39209 (config/mechassault.toml), between the D3DX call and SetTransform, divides the
// clip x column by app::WideView, so the device, the copy and the culling all get the wider view. Only 4:3
// perspective cameras are widened (y scale over x scale within 1% of 4/3): orthographic ones and render-to-texture
// views of another shape (the 'Mech preview in the menus) keep theirs. The renderer squeezes flat draws (the HUD,
// menus, text) back to 4:3 in the middle (gpu/widescreen.h).
#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>

#include "app/app.h"
#include "core/diag.h"
#include "core/log.h"
#include "cpu/x86_context.h"
#include "kernel/kernel.h"

namespace mechassault {
extern uint32_t g_fov_percent;
}

void MechAssaultWidenProjection(X86Context& ctx, uint8_t*) {
    // how often cameras are rebuilt (a setting change applies from the next one): with XBE_PERF, every 10 s
    static int builds = 0;
    static auto since = std::chrono::steady_clock::now();
    builds++;
    if (diag::Perf()) {
        double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - since).count();
        if (s >= 10) {
            LOGR("mechassault: %.1f camera builds a second", builds / s);
            builds = 0;
            since = std::chrono::steady_clock::now();
        }
    }
    float k = app::WideView();
    // field of view setting (percent of the game's own, both axes): every 4:3 perspective camera, cutscenes too
    float f = std::clamp(float(mechassault::g_fov_percent), 80.0f, 130.0f) / 100.0f;
    if (k == 1.0f && f == 1.0f) return;
    uint32_t m = ctx.esp + 0x40;  // row major 4x4
    auto at = [&](int i) { return m + uint32_t(i) * 4; };
    auto get = [&](int i) { return std::bit_cast<float>(kernel::R32(at(i))); };
    float xs = get(0), ys = get(5);
    if (get(11) == 0.0f || xs == 0.0f) return;  // orthographic
    if (std::fabs(ys / xs - 4.0f / 3.0f) > 4.0f / 3.0f * 0.01f) return;
    // a wider field of view scales tan(fov / 2), so the clip scales divide by f
    for (int r = 0; r < 4; r++) {
        kernel::W32(at(r * 4), std::bit_cast<uint32_t>(get(r * 4) / (k * f)));
        kernel::W32(at(r * 4 + 1), std::bit_cast<uint32_t>(get(r * 4 + 1) / f));
    }
    static int logged = 0;
    if (logged++ < 4)
        LOGR("mechassault: camera x scale %.4f -> %.4f (view %.3f times 4:3, field of view %.0f%%)", xs, xs / (k * f), k,
             f * 100);
}

namespace mechassault {
uint32_t g_fov_percent = 100;  // settings.toml fov_percent, F1 > Gameplay
}
