// True widescreen (Hor+). MechAssault draws 4:3 whatever the console reports (a run told a 16:9 TV drew the same
// frames; it never reads the video flags). Every camera builds its projection in one place, 0x39100's body: from the
// camera object's near (+0x80), far (+0x84), field of view (+0x88) and aspect it calls D3DXMatrixPerspectiveFovLH
// 0x276C5D (or D3DXMatrixOrthoLH 0x276CF1 when +0x8C is set) into a matrix at [esp+0x40], sets it on the device
// (SetTransform(1, ...) at 0x39210), keeps a copy at +0x1A0 and multiplies it with the view into +0x1E0, which the
// culling reads. A [[hook]] at 0x39209 (config/mechassault.toml), between the D3DX call and SetTransform, divides the
// clip x column by app::WideView, so the device, the copy and the culling all get the wider view. Only perspective
// cameras of the screen's shapes are widened (y scale over x scale within 1% of 4/3, or of 2/3 and 8/3 for the two
// halves of two-player splitscreen, which are shown that much wider too): orthographic ones and render-to-texture
// views of other shapes keep theirs. The renderer squeezes flat draws (the HUD, menus, text) back to 4:3 in the
// middle (gpu/widescreen.h).
#include <algorithm>
#include <bit>
#include <cstdlib>
#include <chrono>
#include <cmath>
#include <cstdint>

#include "app/app.h"
#include "core/diag.h"
#include "core/log.h"
#include "cpu/x86_context.h"
#include "gpu/widescreen.h"
#include "kernel/kernel.h"

namespace mechassault {
extern uint32_t g_fov_percent;
extern uint32_t g_draw_distance;
}
void MechAssaultSelfTest(X86Context& ctx, uint8_t* base);  // selftest.cpp

// the camera this build widened, and by how much (tan of the half angles: x, y), for its culling planes below
namespace {
uint32_t g_cull_cam = 0;
float g_cull_x = 1, g_cull_y = 1, g_cull_fov = 0;
constexpr uint32_t kFov = 0x88;  // the camera's horizontal field of view at 4:3, degrees
constexpr float kHalfDegree = 0.00872664626f;  // pi / 360: degrees to a half angle in radians
}  // namespace

// Draw distance (issue #2): a perspective camera's far plane (+0x84) is moved out by the setting, as the projection
// is built, and the fog with it (gpu::wide::g_fog_scale), so the world looks the same up close and goes on further.
// The camera keeps the far it was given, so this frame's matrix gets the new depth terms (D3DX's left-handed
// perspective: zf / (zf - zn) and -zn zf / (zf - zn)), and the frustum built right after reads the new far from +0x84,
// which is put back for the game when that build ends (MechAssaultWidenCullDone).
constexpr uint32_t kNear = 0x80, kFar = 0x84;
uint32_t g_far_cam = 0;
float g_far_game = 0;

template <typename At, typename Get>
void DrawDistance(uint32_t cam, At at, Get get) {
    float d = std::clamp(float(mechassault::g_draw_distance), 100.0f, 300.0f) / 100.0f;
    gpu::wide::g_fog_scale.store(d, std::memory_order_relaxed);
    g_far_cam = 0;
    if (d == 1.0f || !cam) return;
    float zn = std::bit_cast<float>(kernel::R32(cam + kNear)), zf = std::bit_cast<float>(kernel::R32(cam + kFar));
    if (!(zf > zn) || zn <= 0) return;
    float far = zf * d;
    kernel::W32(at(10), std::bit_cast<uint32_t>(far / (far - zn)));
    kernel::W32(at(14), std::bit_cast<uint32_t>(-zn * far / (far - zn)));
    g_far_cam = cam;
    g_far_game = zf;
    kernel::W32(cam + kFar, std::bit_cast<uint32_t>(far));  // for the frustum built next
}

void MechAssaultWidenProjection(X86Context& ctx, uint8_t* base) {
    MechAssaultSelfTest(ctx, base);
    g_cull_cam = 0;
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
    uint32_t m = ctx.esp + 0x40;  // row major 4x4
    auto at = [&](int i) { return m + uint32_t(i) * 4; };
    auto get = [&](int i) { return std::bit_cast<float>(kernel::R32(at(i))); };
    float xs = get(0), ys = get(5);
    if (get(11) == 0.0f || xs == 0.0f) return;  // orthographic
    DrawDistance(ctx.ebx, at, get);
    if (k == 1.0f && f == 1.0f) return;
    auto shape = [&](float a) { return std::fabs(ys / xs - a) <= a * 0.01f; };
    if (!shape(4.0f / 3.0f) && !shape(2.0f / 3.0f) && !shape(8.0f / 3.0f)) return;
    // a wider field of view scales tan(fov / 2), so the clip scales divide by f
    for (int r = 0; r < 4; r++) {
        kernel::W32(at(r * 4), std::bit_cast<uint32_t>(get(r * 4) / (k * f)));
        kernel::W32(at(r * 4 + 1), std::bit_cast<uint32_t>(get(r * 4 + 1) / f));
    }
    g_cull_cam = ctx.ebx;
    g_cull_x = k * f;
    g_cull_y = f;
    static int logged = 0;
    // XBE_CAMLOG=1 (tests): every change of the camera's field of view, with the time it happened
    static const bool camlog = std::getenv("XBE_CAMLOG") != nullptr;
    static float last_xs = 0;
    bool changed = xs != last_xs;
    last_xs = xs;
    if (logged++ < 4 || (camlog && changed))
        LOGR("mechassault: camera x scale %.4f -> %.4f (view %.3f times 4:3, field of view %.0f%%)", xs, xs / (k * f), k,
             f * 100);
}

namespace mechassault {
uint32_t g_fov_percent = 100;  // settings.toml fov_percent, F1 > Gameplay
uint32_t g_draw_distance = 100;  // settings.toml draw_distance, F1 > Graphics: percent of the game's own, 100-300
}

// The game culls with a frustum of its own, built right after the projection (0x381B0, the camera in ebx): its side
// planes from the 4:3 horizontal field of view (+0x88, degrees, half of it in radians at 0x3829F and 0x38348), its top
// and bottom ones from that times 1 / aspect (0x3847A). With the projection widened, scenery past the 4:3 edges was
// culled and the sky showed through at the screen's sides. Three [[hook]]s (config/mechassault.toml) widen the angle
// the planes read as the projection was widened, and put it back after: 0x3829F the sides (tan scaled by g_cull_x),
// 0x383D6 the top and bottom (by g_cull_y, the field of view setting), 0x392B0 (back in the build) the game's own.
void MechAssaultWidenCullSides(X86Context& ctx, uint8_t*) {
    if (!g_cull_cam || ctx.ebx != g_cull_cam) return;
    g_cull_fov = std::bit_cast<float>(kernel::R32(ctx.ebx + kFov));
    float half = std::atan(std::tan(g_cull_fov * kHalfDegree) * g_cull_x);
    kernel::W32(ctx.ebx + kFov, std::bit_cast<uint32_t>(half / kHalfDegree));
}

void MechAssaultWidenCullTops(X86Context& ctx, uint8_t*) {
    if (!g_cull_cam || ctx.ebx != g_cull_cam || g_cull_fov == 0) return;
    int32_t w = int32_t(kernel::R32(ctx.ebx + 0x9C)), h = int32_t(kernel::R32(ctx.ebx + 0xA0));
    float aspect = h > 0 && w > 0 ? float(w) / float(h) : 4.0f / 3.0f;
    float half = std::atan(std::tan(g_cull_fov / aspect * kHalfDegree) * g_cull_y);
    kernel::W32(ctx.ebx + kFov, std::bit_cast<uint32_t>(half * aspect / kHalfDegree));
}

void MechAssaultWidenCullDone(X86Context& ctx, uint8_t*) {
    if (g_far_cam && ctx.ebx == g_far_cam) kernel::W32(ctx.ebx + kFar, std::bit_cast<uint32_t>(g_far_game));  // the game's far
    g_far_cam = 0;
    if (g_cull_cam && ctx.ebx == g_cull_cam && g_cull_fov != 0)
        kernel::W32(ctx.ebx + kFov, std::bit_cast<uint32_t>(g_cull_fov));
    g_cull_fov = 0;
    g_cull_cam = 0;
}
