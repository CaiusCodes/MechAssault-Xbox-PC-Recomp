// Direct3D 8 (XDK 4831 and 5849) entry points the title calls, implemented on the host renderer.
// Resource creation, locking and the XG helpers stay recompiled: they only manage memory.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cstdlib>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <map>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include "app/app.h"
#include "core/frame_stats.h"  // breakdown-recomp patch
#include "core/diag.h"
#include "gpu/backend.h"
#include "gpu/shaders.h"
#include "gpu/simple_dirty.h"
#include "gpu/texture.h"
#include "gpu/vertex.h"
#include "xdk/d3d8.h"
#include "xdk/hle.h"
#include "xdk/layout.h"

using namespace kernel;

namespace dispatch {
bool InCode(uint32_t va);  // cpu/dispatch.cpp
}

// configs written before the SDK knew the name
#ifndef SYM_D3DDevice_BeginPush
#define SYM_D3DDevice_BeginPush FFFFFE00
#define HAS_D3DDevice_BeginPush 0
#endif
#ifndef SYM_D3DDevice_EndPush
#define SYM_D3DDevice_EndPush FFFFFE01
#define HAS_D3DDevice_EndPush 0
#endif
#ifndef HAS_D3D_FvfShader
#define HAS_D3D_FvfShader 0
#endif
#ifndef SYM_D3DDevice_SetPalette
#define SYM_D3DDevice_SetPalette FFFFFE02
#define HAS_D3DDevice_SetPalette 0
#endif
#ifndef SYM_D3DDevice_GetVertexShaderFunction
#define SYM_D3DDevice_GetVertexShaderFunction FFFFFE03
#define HAS_D3DDevice_GetVertexShaderFunction 0
#endif
#ifndef SYM_D3DDevice_GetTile
#define SYM_D3DDevice_GetTile FFFFFE04
#define HAS_D3DDevice_GetTile 0
#endif
#ifndef SYM_D3DDevice_SetRenderTargetFast
#define SYM_D3DDevice_SetRenderTargetFast FFFFFE05
#define HAS_D3DDevice_SetRenderTargetFast 0
#endif
#ifndef SYM_D3DDevice_LoadVertexShaderProgram
#define SYM_D3DDevice_LoadVertexShaderProgram FFFFFE06
#define HAS_D3DDevice_LoadVertexShaderProgram 0
#endif
#ifndef SYM_D3DDevice_GetRasterStatus
#define SYM_D3DDevice_GetRasterStatus FFFFFE07
#define HAS_D3DDevice_GetRasterStatus 0
#endif
#ifndef SYM_D3DDevice_Reset
#define SYM_D3DDevice_Reset FFFFFE08
#define HAS_D3DDevice_Reset 0
#endif
#ifndef SYM_D3DDevice_RunPushBuffer
#define SYM_D3DDevice_RunPushBuffer FFFFFE09
#define HAS_D3DDevice_RunPushBuffer 0
#endif
#ifndef SYM_D3DDevice_InsertCallback
#define SYM_D3DDevice_InsertCallback FFFFFE43
#define HAS_D3DDevice_InsertCallback 0
#endif
#ifndef SYM_D3DDevice_BeginPushBuffer
#define SYM_D3DDevice_BeginPushBuffer FFFFFE40
#define HAS_D3DDevice_BeginPushBuffer 0
#endif
#ifndef SYM_D3DDevice_EndPushBuffer
#define SYM_D3DDevice_EndPushBuffer FFFFFE41
#define HAS_D3DDevice_EndPushBuffer 0
#endif
#ifndef SYM_D3DDevice_GetPushBufferOffset
#define SYM_D3DDevice_GetPushBufferOffset FFFFFE42
#define HAS_D3DDevice_GetPushBufferOffset 0
#endif
#ifndef SYM_D3D_BlockOnNonSurfaceResource
#define SYM_D3D_BlockOnNonSurfaceResource FFFFFE0A
#define HAS_D3D_BlockOnNonSurfaceResource 0
#endif

namespace d3d {
inline bool g_dump_on = false;  // breakdown-recomp debug: XBE_PUSHDUMP dumps methods after the first dropped EndPush


namespace {

// A/B switch: 0 draws on the title's thread, as before
#ifndef XBE_RENDER_THREAD
#define XBE_RENDER_THREAD 1
#endif
constexpr bool kRenderThread = XBE_RENDER_THREAD;

// D3D globals of this build
constexpr uint32_t kDevice = SYM(D3D_Device);        // the static CDevice, g_pDevice points at it
constexpr uint32_t kDevicePtr = SYM(D3D_pDevice);
constexpr uint32_t kIndexData = SYM(D3D_IndexData);  // index data base the inline draws read
constexpr uint32_t kDirtyFlags = SYM(D3D_DirtyFlags);
constexpr uint32_t kTextureState = SYM(D3D_DeferredTextureState);  // [4][32]
constexpr uint32_t kRenderState = SYM(D3D_RenderState);            // [166]
constexpr uint32_t kMCullFaceEnable = 0x308;                       // NV097_SET_CULL_FACE_ENABLE
constexpr uint32_t kStreams = SYM(D3D_Streams);                    // [16] stride, offset, buffer
constexpr uint32_t kSimpleEncode = SYM(D3D_SimpleRenderStateEncode);

// CDevice fields mirrored so the recompiled getters and resource code see what the XDK would
constexpr uint32_t kDevPush = XDK_CDevice_Push, kDevThreshold = XDK_CDevice_PushThreshold,
                   kDevBaseVertex = XDK_CDevice_BaseVertex, kDevIndexBuffer = XDK_CDevice_IndexBuffer,
                   kDevTextures = XDK_CDevice_Textures, kDevRenderTarget = XDK_CDevice_RenderTarget,
                   kDevDepth = XDK_CDevice_Depth, kDevSwapCount = XDK_CDevice_SwapCount,
                   kDevBackBuffer = XDK_CDevice_BackBuffer, kDevFrontBuffer = XDK_CDevice_FrontBuffer,
                   kDevZScale = XDK_CDevice_ZScale, kDevSuperX = XDK_CDevice_SuperSampleX,
                   kDevSuperY = XDK_CDevice_SuperSampleY, kDevTransforms = XDK_CDevice_Transforms,
                   kDevPalettes = XDK_CDevice_Palettes;
constexpr uint32_t kStageStates = XDK_TextureState_Stage, kStreamSize = XDK_Stream_Size;

gpu::State g_state;
std::unique_ptr<gpu::Backend> g_backend;
std::recursive_mutex g_lock;
// breakdown-recomp patch: blanks counted by the vertical blank thread, for BlockUntilVerticalBlank
std::mutex g_blank_mutex;
std::condition_variable g_blank_cv;
uint64_t g_blanks = 0;
uint64_t g_blanks_done = 0;  // blanks whose title callback has returned
std::atomic<bool> g_blank_thread{false};

// every write to g_state.simple goes through here or marks it
void SetSimple(uint32_t method, uint32_t v) {
    // breakdown-recomp patch: the renderers cull by the CullMode render state, but a title's own push buffers
    // (RunPushBuffer, or commands it writes) can switch the hardware's culling with CULL_FACE_ENABLE alone. The
    // hardware then stays unculled until the next SetRenderState_CullMode. Turning it back on restores the mode
    // the library last set, which the library's table holds.
    if (method == kMCullFaceEnable) g_state.rs[gpu::kRsCullMode] = v ? R32(kRenderState + gpu::kRsCullMode * 4) : 0;
    g_state.simple[method >> 2] = v;
    gpu::MarkSimple(method >> 2);
}

std::unordered_map<uint32_t, std::shared_ptr<gpu::VertexShader>> g_vs;
std::unordered_map<uint32_t, std::shared_ptr<gpu::PixelShader>> g_ps;
std::array<uint32_t, 3> g_buffers{};  // front, back surfaces
uint32_t g_buffer_count = 2;
uint32_t g_depth = 0;
uint32_t g_scratch_push = 0, g_scratch_size = 0;
uint32_t g_walk = 0;  // first scratch command not read yet

uint32_t Dev(uint32_t off) { return R32(kDevice + off); }
void SetDev(uint32_t off, uint32_t v) { W32(kDevice + off, v); }

// The device's own references: a count in bits 19 to 22 of the resource's first dword. A surface's first one
// also holds its parent. The library's functions do it when the image has them, inlined ones are done here.
void AddRef(X86Context& ctx, uint32_t res) {
    if (!res) return;
#if HAS_D3D_InternalAddRef
    GUEST_CALL(D3D_InternalAddRef, ctx, {res});
#else
    (void)ctx;
    uint32_t common = R32(res);
    if (!(common & 0x780000) && (common & 0x70000) == 0x50000)
        if (uint32_t parent = R32(res + 0x14)) W32(parent, R32(parent) + 0x80000);
    W32(res, R32(res) + 0x80000);
#endif
}
void Release(X86Context& ctx, uint32_t res) {
    if (!res) return;
#if HAS_D3D_InternalRelease
    GUEST_CALL(D3D_InternalRelease, ctx, {res});
#else
    (void)ctx;
    // the library frees a resource nothing holds any more; without its function the memory stays
    auto drop = [](uint32_t r) {
        W32(r, R32(r) - 0x80000);
        if (!(R32(r) & 0x78FFFF)) LOG_FIRST(8, "d3d: resource %08X is not freed, the image has no D3D_InternalRelease", r);
    };
    uint32_t common = R32(res);
    if ((common & 0x780000) == 0x80000 && (common & 0x70000) == 0x50000)
        if (uint32_t parent = R32(res + 0x14)) drop(parent);
    drop(res);
#endif
}

void Walk(uint32_t end);

// Hardware commands a caller still pushes go to a scratch buffer, rewound at every request once Walk has read it
uint32_t ScratchPush(uint32_t bytes = 0) {
    Walk(Dev(kDevPush));
    uint32_t need = std::max<uint32_t>(0x10000, (bytes + 0x1FFF) & ~0xFFFu);
    if (need > g_scratch_size) {
        if (g_scratch_push) mem::FreeContiguous(g_scratch_push);
        g_scratch_push = mem::AllocContiguous(need, 0, 0xFFFFFFFF, 0x1000);
        g_scratch_size = need;
    }
    SetDev(kDevPush, g_scratch_push);
    SetDev(kDevThreshold, g_scratch_push + g_scratch_size - 0x1000);
    g_walk = g_scratch_push;
    return g_scratch_push;
}

gpu::Surface SurfaceOf(uint32_t header) {
    gpu::Surface s;
    s.header = header;
    if (header) {
        s.data = R32(header + 4);
        s.format = R32(header + 12);
        s.size = R32(header + 16);
    }
    return s;
}

uint32_t NewSurface(uint32_t w, uint32_t h, uint32_t fmt, uint32_t bpp) {
    uint32_t pitch = (w * bpp + 63) & ~63u;
    uint32_t data = mem::AllocContiguous(pitch * h, 0, 0xFFFFFFFF, 0x1000);
    uint32_t hdr = mem::AllocSystem(24);
    W32(hdr + 0, 0x000D0001);  // surface, one reference, one internal reference held by the device
    W32(hdr + 4, mem::PhysicalFromVirtual(data));
    W32(hdr + 8, 0);
    W32(hdr + 12, 0x00010021 | (fmt << 8));  // dma A, 2D, one level
    W32(hdr + 16, (w - 1) | (h - 1) << 12 | (pitch / 64 - 1) << 24);
    W32(hdr + 20, 0);
    return hdr;
}

void SyncDeferred() {
    // deferred states live in the title's memory, the inline SetRenderState and SetTextureStageState write them
    for (uint32_t i = gpu::kRsDeferredFirst; i < gpu::kRsComplexFirst; i++) g_state.rs[i] = R32(kRenderState + i * 4);
    for (int s = 0; s < 4; s++)
        for (uint32_t i = 0; i < gpu::kTssBumpEnvMat00; i++) g_state.tss[s][i] = R32(kTextureState + (s * kStageStates + i) * 4);
}

float ZScale(const gpu::Surface& zs) {
    uint32_t f = zs.header ? (zs.format >> 8) & 0xFF : 0x2E;
    return f == 0x2C || f == 0x30 ? 65535.0f : 16777215.0f;
}

bool g_own_constants = false;  // D3DSCM_NORESERVEDCONSTANTS: c-38 and c-37 are the title's

// c-38 and c-37 hold the viewport scale and offset, as D3D_GetViewportOffsetAndScale computes them
void UpdateViewportConstants() {
    auto& v = g_state.viewport;
    g_state.zscale = ZScale(g_state.zs);
    float hw = v.w * 0.5f, hh = v.h * 0.5f;
    if (!g_own_constants) {
        g_state.vsc[58] = {hw, -hh, (v.max_z - v.min_z) * g_state.zscale, 0};
        g_state.vsc[59] = {v.x + g_state.ss_offset_x + hw, v.y + g_state.ss_offset_y + hh, v.min_z * g_state.zscale, 0};
    }
    // the viewport as the library's SetViewport stores it, for inlined readers
    if (XDK_CDevice_Viewport != xdk::kUnknown) {
        uint32_t p = kDevice + XDK_CDevice_Viewport;
        W32(p, v.x);
        W32(p + 4, v.y);
        W32(p + 8, v.w);
        W32(p + 12, v.h);
        std::memcpy(HostPtr(p + 16), &v.min_z, 4);
        std::memcpy(HostPtr(p + 20), &v.max_z, 4);
    }
}

void SetComplex(uint32_t rs, uint32_t v) {
    g_state.rs[rs] = v;
    W32(kRenderState + rs * 4, v);
}

// The library's default render and texture stage states; its pushes land in the scratch buffer
void ApplyDefaultStates(X86Context& ctx) {
#if HAS_D3D_InitDefaultState
    GUEST_CALL(D3D_InitDefaultState, ctx, {});
#else
    (void)ctx;
    LOG_FIRST(1, "d3d: the image has no D3D_InitDefaultState, default states were not applied");
#endif
}

// The frame buffers a D3DPRESENT_PARAMETERS asks for become the device's targets, the viewport covers them. The
// renderer is made at the first ones' size.
void SetFrameBuffers(const char* caller, uint32_t pp) {
    uint32_t w = R32(pp), h = R32(pp + 4), fmt = R32(pp + 8), count = std::max<uint32_t>(R32(pp + 12), 1);
    uint32_t auto_depth = R32(pp + 0x20), depth_fmt = R32(pp + 0x24), interval = R32(pp + 0x30);
    if (!w) w = 640;
    if (!h) h = 480;
    LOGI("d3d: %s %ux%u format %X buffers %u depth %u format %X interval %X flags %X", caller, w, h, fmt, count,
         auto_depth, depth_fmt, interval, R32(pp + 0x28));
    uint32_t color_fmt = gpu::LinearColorFormat(fmt);
    uint32_t zf = auto_depth ? gpu::LinearDepthFormat(depth_fmt) : 0;
    // surfaces of the same shape are kept: a title wraps their memory in textures of its own across a Reset
    static std::array<uint32_t, 5> made{};
    std::array<uint32_t, 5> asked{w, h, color_fmt, std::min<uint32_t>(count + 1, 3), zf};
    if (asked != made) {
        made = asked;
        g_buffer_count = asked[3];
        for (uint32_t i = 0; i < g_buffer_count; i++) g_buffers[i] = NewSurface(w, h, color_fmt, 4);
        g_depth = zf ? NewSurface(w, h, zf, zf == 0x30 || zf == 0x31 ? 2 : 4) : 0;  // 0x30 LIN_D16, 0x31 LIN_F16
    }
    if (!g_backend) {
        g_backend = gpu::CreateBackend();
        if (!g_backend->Init(w, h)) {
            LOGE("d3d: renderer init failed");
            // no owner window: this runs on the title's thread and the window may already be fullscreen over the box
            MessageBoxW(nullptr,
                        L"Direct3D 12 could not be started. Make sure your graphics card supports it and its driver "
                        L"is up to date.",
                        app::kGame.short_title, MB_ICONERROR | MB_TOPMOST);
            kernel::Shutdown(1);
        }
        if (kRenderThread) g_backend = gpu::CreateThreadedBackend(std::move(g_backend));
    }
    g_state.viewport = {0, 0, w, h, 0.0f, 1.0f};
    SetDev(kDevRenderTarget, g_buffers[1]);
    SetDev(kDevDepth, g_depth);
    SetDev(kDevBackBuffer, g_buffers[1]);
    SetDev(kDevFrontBuffer, g_buffers[1]);
    float one = 1.0f, zs = g_depth && (R32(g_depth + 12) >> 8 & 0xFF) == 0x30 ? 65535.0f : 16777215.0f;
    std::memcpy(HostPtr(kDevice + kDevSuperX), &one, 4);
    std::memcpy(HostPtr(kDevice + kDevSuperY), &one, 4);
    std::memcpy(HostPtr(kDevice + kDevZScale), &zs, 4);
    // fields the library's init sets that inlined library code and the default states read
    if (XDK_CDevice_AutoDepth != xdk::kUnknown) SetDev(XDK_CDevice_AutoDepth, g_depth);
    if (XDK_CDevice_BackBufferScale != xdk::kUnknown) {
        std::memcpy(HostPtr(kDevice + XDK_CDevice_BackBufferScale), &one, 4);
        std::memcpy(HostPtr(kDevice + XDK_CDevice_BackBufferScale + 4), &one, 4);
    }
    g_state.rt = SurfaceOf(g_buffers[1]);
    g_state.zs = SurfaceOf(g_depth);
    UpdateViewportConstants();
}

constexpr bool kVBlank = XDK_CDevice_VBlankCount != xdk::kUnknown && XDK_CDevice_VBlankEvent != xdk::kUnknown &&
                         XDK_CDevice_VBlankCallback != xdk::kUnknown;

// what the library's vertical blank interrupt leaves for the title: count, event, callback
void VerticalBlank(X86Context& ctx) {
    using clock = std::chrono::steady_clock;
    uint32_t info = mem::AllocSystem(12), shown = 0;
    auto next = clock::now();
    for (;;) {
        // breakdown-recomp patch: on the display's own blanks where they fit (app/vblank.cpp), else the timer
        if (app::WaitDisplayVBlank()) {
            next = clock::now();
        } else {
            next = std::max(next + std::chrono::microseconds(16667), clock::now());
            std::this_thread::sleep_until(next);
        }
        uint32_t count = Dev(XDK_CDevice_VBlankCount) + 1, swaps = Dev(kDevSwapCount);
        SetDev(XDK_CDevice_VBlankCount, count);
        {
            std::lock_guard<std::mutex> blank(g_blank_mutex);
            g_blanks++;
        }
        g_blank_cv.notify_all();
        ObjectRef event = FromPointer(kDevice + XDK_CDevice_VBlankEvent);
        SetEvent(*event);
        if (uint32_t callback = Dev(XDK_CDevice_VBlankCallback)) {
            // blank count, swaps shown, 1 when this blank showed one
            W32(info, count);
            W32(info + 4, swaps);
            W32(info + 8, swaps != shown);
            CallGuest(ctx, callback, {info});
        }
        {
            std::lock_guard<std::mutex> blank(g_blank_mutex);
            g_blanks_done++;
        }
        g_blank_cv.notify_all();
        shown = swaps;
        // the title clears the event in its own memory before a wait, which the host object does not see
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        ResetEvent(*event);
    }
}

}  // namespace

gpu::State& State() { return g_state; }
std::recursive_mutex& Lock() { return g_lock; }

}  // namespace d3d

using namespace d3d;

// device

HLE_STDCALL(Direct3D_CreateDevice, 6) {
    XDK_REQUIRE("d3d", XDK_DATA(D3D_Device), XDK_DATA(D3D_pDevice), XDK_DATA(D3D_DirtyFlags),
                XDK_DATA(D3D_DeferredTextureState), XDK_DATA(D3D_RenderState), XDK_DATA(D3D_Streams),
                XDK_DATA(D3D_SimpleRenderStateEncode));
    XDK_REQUIRE("d3d", XDK_KEY(CDevice_Push), XDK_KEY(CDevice_PushThreshold), XDK_KEY(CDevice_BaseVertex),
                XDK_KEY(CDevice_IndexBuffer), XDK_KEY(CDevice_ZScale), XDK_KEY(CDevice_SuperSampleX),
                XDK_KEY(CDevice_SuperSampleY), XDK_KEY(CDevice_Transforms), XDK_KEY(CDevice_Textures),
                XDK_KEY(CDevice_RenderTarget), XDK_KEY(CDevice_Depth), XDK_KEY(CDevice_BackBuffer),
                XDK_KEY(CDevice_FrontBuffer), XDK_KEY(CDevice_SwapCount), XDK_KEY(CDevice_Size),
                XDK_KEY(DirtyFlags_Texture), XDK_KEY(TextureState_Stage), XDK_KEY(Stream_Size),
                XDK_KEY(Stream_Stride), XDK_KEY(Stream_Buffer));
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    std::memset(HostPtr(kDevice), 0, XDK_CDevice_Size);
    W32(kDevicePtr, kDevice);
    ScratchPush();
    g_state = gpu::State{};
    gpu::MarkAllSimple();
    gpu::g_simple_tracked = true;
    SetFrameBuffers("CreateDevice", a[4]);
    // 60 Hz, widescreen
    if (XDK_CDevice_DisplayFlags != xdk::kUnknown) SetDev(XDK_CDevice_DisplayFlags, 0x400000 | (app::Widescreen() ? 0x10000 : 0));
    // the miniport's hardware init leaves an identity ramp in slot 0, which GetGammaRamp reads until one is set
    if (XDK_CDevice_GammaRamps != xdk::kUnknown) {
        uint8_t* ramp = static_cast<uint8_t*>(HostPtr(kDevice + XDK_CDevice_GammaRamps));  // red, green, blue
        for (uint32_t i = 0; i < 256; i++) ramp[i] = ramp[256 + i] = ramp[512 + i] = uint8_t(i);
    }
#if HAS_D3D_FvfShader
    if (XDK_CDevice_VertexShader != xdk::kUnknown) SetDev(XDK_CDevice_VertexShader, SYM(D3D_FvfShader));
#endif
    if (kVBlank) {
        // notification event, signalled
        SetDev(XDK_CDevice_VBlankEvent, 0x00040000);
        SetDev(XDK_CDevice_VBlankEvent + 4, 1);
        static bool started = false;
        if (!started) StartHostThread("vertical blank", VerticalBlank);
        g_blank_thread = true;
        started = true;
    }
    ApplyDefaultStates(ctx);
    if (a[5]) W32(a[5], kDevice);
    return 0;
}

HLE_STDCALL(D3DDevice_Release, 0) { return 1; }

// synchronisation, the renderer never runs behind

HLE_STDCALL(D3D_BlockOnResource, 1) {
    uint32_t res = a[0];
    if (res && g_backend) {
        std::lock_guard<std::recursive_mutex> lock(g_lock);
        uint32_t type = R32(res) & 0x70000;  // 0x40000 texture, 0x50000 surface
        if (type == 0x40000 || type == 0x50000) {
            uint32_t data = mem::VirtualFromPhysical(R32(res + 4));
            // perf diagnostic: who blocks on which surface, once each
            static std::unordered_set<uint32_t> seen;
            if (diag::Perf() && seen.insert(data).second) {
                uint32_t code[4] = {}, n = 0;
                for (uint32_t i = 0; i < 32 && n < 4; i++) {
                    uint32_t v = R32(ctx.esp + i * 4);
                    if (dispatch::InCode(v)) code[n++] = v;
                }
                LOGI("d3d: BlockOnResource on surface %08X, stack %08X %08X %08X %08X", data, code[0], code[1], code[2], code[3]);
            }
            g_backend->Sync(data, 0);
        } else {
            g_backend->WaitResource(res);
        }
    }
    gpu::MarkDirty(res);
    return 0;
}
// lock(resource, face, level, locked, rect, flags): D3DLOCK_NOOVERWRITE skips the block above, yet a
// title may read what the GPU drew
HLE_WRAP(Lock2DSurface, 6) {
    uint32_t res = a[0];
    if ((a[5] & 0x20) && res && g_backend) {  // D3DLOCK_NOOVERWRITE
        std::lock_guard<std::recursive_mutex> lock(g_lock);
        g_backend->SyncLate(mem::VirtualFromPhysical(R32(res + 4)));
    }
}
HLE_STDCALL(D3DResource_BlockUntilNotBusy, 1) {
    if (a[0] && g_backend) {
        std::lock_guard<std::recursive_mutex> lock(g_lock);
        g_backend->WaitResource(a[0]);
    }
    gpu::MarkDirty(a[0]);
    return 0;
}
// (resource): Lock2 of a vertex buffer or palette. The library waits on GPU time stamps, which the host does not keep
// (BlockOnTime returns at once), so a title refilling a buffer would overwrite draws not yet taken.
HLE_STDCALL(D3D_BlockOnNonSurfaceResource, 1) {
    if (a[0] && g_backend) {
        std::lock_guard<std::recursive_mutex> lock(g_lock);
        g_backend->WaitResource(a[0]);
    }
    gpu::MarkDirty(a[0]);
    return 0;
}
HLE_STDCALL(D3DResource_IsBusy, 1) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    uint32_t res = a[0];
    if (!res) return 0;
    // set on the device, a surface also through its texture
    uint32_t common = R32(res), parent = (common & 0x70000) == 0x50000 ? R32(res + 0x14) : 0;
    if ((common & 0x780000) || (parent && (R32(parent) & 0x780000))) return 1;  // internal reference count
    return g_backend && g_backend->Busy(res) ? 1 : 0;
}
HLE_STDCALL(D3D_BlockOnTime, 2) { return 0; }
HLE_STDCALL(D3DDevice_BlockOnFence, 1) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    if (g_backend) g_backend->WaitFence(a[0]);
    return 0;
}
HLE_STDCALL(D3DDevice_InsertFence, 0) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    return g_backend ? g_backend->Fence() : 0;
}
HLE_STDCALL(D3DDevice_IsFencePending, 1) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    return g_backend && g_backend->FencePending(a[0]) ? 1 : 0;
}
HLE_STDCALL(D3D_SetFence, 1) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    return g_backend ? g_backend->Fence() : 0;
}
HLE_STDCALL(D3D_KickOffAndWaitForIdle, 0) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    if (g_backend) g_backend->Flush();
    return 0;
}
HLE_FASTCALL(CDevice_KickOff, 0) { return 0; }

HLE_STDCALL(D3DDevice_MakeSpace, 0) {
    LOG_FIRST(16, "d3d: MakeSpace from %08X, some caller still pushes hardware commands", R32(ctx.esp));
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    return ScratchPush();
}
HLE_STDCALL(D3D_MakeRequestedSpace, 2) {
    LOG_FIRST(16, "d3d: MakeRequestedSpace from %08X", R32(ctx.esp));
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    return ScratchPush();
}

// breakdown-recomp patch: returns at the next blank of the vertical blank thread, as the library's wait on the
// blank event does. A fixed 16 ms sleep added to the title's own frame time: Breakdown's menus, which call this
// once a frame, ran at 56 fps instead of 60.
HLE_STDCALL(D3DDevice_BlockUntilVerticalBlank, 0) {
    if (!g_blank_thread) {
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
        return 0;
    }
    std::unique_lock<std::mutex> blank(g_blank_mutex);
    uint64_t seen = g_blanks;
    g_blank_cv.wait_for(blank, std::chrono::milliseconds(100), [&] { return g_blanks != seen; });
    return 0;
}
// (status): D3DRASTER_STATUS {InVBlank, ScanLine} on a 60 Hz frame clock. The library reads the GPU's scanline,
// which counts the front buffer's lines, then the blank: 45 lines per 480 shown, as on NTSC.
HLE_STDCALL(D3DDevice_GetRasterStatus, 1) {
    constexpr int64_t kFrameMicroseconds = 16667;
    constexpr uint32_t kBlankLines = 45;
    constexpr uint32_t kShownLines = 480;
    uint32_t shown = g_buffers[0] ? (R32(g_buffers[0] + 0x10) >> 12 & 0xFFF) + 1 : kShownLines;
    uint32_t lines = shown + shown * kBlankLines / kShownLines;
    auto now = std::chrono::steady_clock::now().time_since_epoch();
    int64_t into_frame = std::chrono::duration_cast<std::chrono::microseconds>(now).count() % kFrameMicroseconds;
    uint32_t line = uint32_t(into_frame * lines / kFrameMicroseconds);
    W32(a[0], line >= shown);
    W32(a[0] + 4, line < shown ? line : 0);
    return 0;
}
HLE_STDCALL(D3DDevice_SetFlickerFilter, 1) { return 0; }
HLE_STDCALL(D3DDevice_SetSoftDisplayFilter, 1) { return 0; }
HLE_STDCALL(D3DDevice_EnableOverlay, 1) { return 0; }
// the front buffer, read back at the guest size, is kept for the next instance after a quick reboot, which may
// fade from it at its first frames
HLE_STDCALL(D3DDevice_PersistDisplay, 0) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    uint32_t front = g_buffers[0], size = R32(front + 0x10);
    uint32_t pitch = ((size >> 24 & 0xFF) + 1) * 64, height = (size >> 12 & 0xFFF) + 1, bytes = pitch * height;
    uint32_t data = mem::VirtualFromPhysical(R32(front + 4));
    if (g_backend) g_backend->Sync(data, bytes);
    kernel::SavePersistedDisplay(R32(front), R32(front + 0xC), size, HostPtr(data), bytes);
    LOGI("d3d: PersistDisplay, %u bytes", bytes);
    return 0;
}
HLE_STDCALL(D3DDevice_FlushVertexCache, 0) { return 0; }

// The library keeps the ramp in one of two device slots for GetGammaRamp: both get it, whichever one its index names
HLE_STDCALL(D3DDevice_SetGammaRamp, 2) {
    uint8_t ramp[768];
    std::memcpy(ramp, HostPtr(a[1]), 768);
    if (XDK_CDevice_GammaRamps != xdk::kUnknown)
        for (uint32_t slot = 0; slot < 2; slot++)
            std::memcpy(HostPtr(kDevice + XDK_CDevice_GammaRamps + slot * sizeof(ramp)), ramp, sizeof(ramp));
    if (g_backend) g_backend->SetGamma(ramp);
    return 0;
}

// targets and presentation

namespace {
// the device's targets become rt and zs, the references move from the old ones
void ApplyRenderTarget(X86Context& ctx, uint32_t rt, uint32_t zs) {
    AddRef(ctx, rt);
    uint32_t old_rt = Dev(kDevRenderTarget);
    uint32_t old_zs = Dev(kDevDepth);
    SetDev(kDevRenderTarget, rt);
    SetDev(kDevDepth, zs);
    AddRef(ctx, zs);
    Release(ctx, old_rt);
    Release(ctx, old_zs);
    g_state.rt = SurfaceOf(rt);
    g_state.zs = SurfaceOf(zs);
}
}  // namespace

HLE_STDCALL(D3DDevice_SetRenderTarget, 2) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    uint32_t rt = a[0] ? a[0] : Dev(kDevRenderTarget);
    ApplyRenderTarget(ctx, rt, a[1]);
    // SetRenderTarget resets the viewport to the whole target
    uint32_t w, h;
    gpu::SurfaceSize(g_state.rt, w, h);
    g_state.viewport = {0, 0, w, h, 0.0f, 1.0f};
    UpdateViewportConstants();
    return 0;
}

// (rt, zs, flags): the surfaces alone, the viewport stays as it was
HLE_STDCALL(D3DDevice_SetRenderTargetFast, 3) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    ApplyRenderTarget(ctx, a[0], a[1]);
    return 0;
}

HLE_STDCALL(D3DDevice_GetBackBuffer2, 1) {
    int32_t i = int32_t(a[0]);
    uint32_t s = i < 0 ? g_buffers[0] : g_buffers[std::min<uint32_t>(uint32_t(i) + 1, g_buffer_count - 1)];
    W32(s, R32(s) + 1);
    return s;
}

HLE_STDCALL(D3DDevice_GetRenderTarget2, 0) {
    uint32_t s = Dev(kDevRenderTarget);
    if (s) W32(s, R32(s) + 1);
    return s;
}

HLE_STDCALL(D3DDevice_GetDepthStencilSurface2, 0) {
    uint32_t s = Dev(kDevDepth);
    if (s) W32(s, R32(s) + 1);
    return s;
}

namespace {
std::atomic<uint32_t> g_presents{0};
}  // namespace

HLE_STDCALL(D3DDevice_Swap, 1) {
    app::SplashFirstFrame();
    // handed over from the previous instance: the window opens on the second frame, the first picture with
    // anything drawn over it right after
    if (app::HandingOver() && g_presents >= 2) {
        {
            std::lock_guard<std::recursive_mutex> lock(g_lock);
            if (g_backend) g_backend->Flush();
        }
        app::FinishHandover();
    }
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    g_presents++;
    // the kept display is shown until the title presents its own
    kernel::SetSavedDataAddress(0);
    // the ramp an inlined SetGammaRamp left for the vertical blank that shows this swap
    if (g_backend && XDK_CDevice_GammaPending != xdk::kUnknown && XDK_CDevice_GammaRamps != xdk::kUnknown) {
        uint32_t i = Dev(kDevSwapCount) & 1;
        if (Dev(XDK_CDevice_GammaPending + i * 4) == 1) {
            SetDev(XDK_CDevice_GammaPending + i * 4, 0);
            g_backend->SetGamma(static_cast<const uint8_t*>(HostPtr(kDevice + XDK_CDevice_GammaRamps + i * 0x300)));
        }
    }
    Walk(Dev(kDevPush));  // reports asked for at the end of the frame
    // breakdown-recomp patch: frame timing for the FPS counter, diagnostics and the perf log
    {
        using clock = std::chrono::steady_clock;
        auto t0 = clock::now();
        if (g_backend) g_backend->Present(SurfaceOf(g_buffers[1]));
        auto t1 = clock::now();
        framestats::Title().Present(uint32_t(std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count()));
        app::Pace();
        framestats::Title().Paced(
            uint32_t(std::chrono::duration_cast<std::chrono::microseconds>(clock::now() - t1).count()));
        static auto next_log = clock::now() + std::chrono::seconds(10);
        if ((app::Get().diagnostics_log || diag::Perf()) && clock::now() >= next_log) {
            next_log = clock::now() + std::chrono::seconds(10);
            static uint64_t logged = 0;  // the frames since the last line
            uint64_t count = framestats::Title().Count();
            framestats::Summary f = framestats::Title().Summarize(std::min<uint64_t>(count - logged, framestats::kCapacity));
            logged = count;
            LOGR("frames: %u at %.2f fps (%.2f ms), 1%% low %.2f fps, p99 %.2f ms, max %.2f ms, %u stalls, jitter "
                 "%.2f ms, present %.2f ms, limiter %.2f ms",
                 f.frames, f.avg_fps, f.avg_ms, f.low1_fps, f.p99_ms, f.max_ms, f.stalls, f.jitter_ms, f.present_ms,
                 f.pace_ms);
        }
    }
    // like the XDK, the surfaces stay put and their memory rotates: titles keep back buffer pointers and copy its data
    uint32_t last = g_buffers[g_buffer_count - 1], data = R32(last + 4), lk = R32(last + 8);
    for (uint32_t i = g_buffer_count - 1; i > 0; i--) {
        W32(g_buffers[i] + 4, R32(g_buffers[i - 1] + 4));
        W32(g_buffers[i] + 8, R32(g_buffers[i - 1] + 8));
    }
    W32(g_buffers[0] + 4, data);
    W32(g_buffers[0] + 8, lk);
    g_state.rt = SurfaceOf(Dev(kDevRenderTarget));
    uint32_t n = Dev(kDevSwapCount) + 1;
    SetDev(kDevSwapCount, n);
    return n;
}

HLE_STDCALL(D3DDevice_Clear, 6) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    const uint8_t* rects = a[0] && a[1] ? static_cast<const uint8_t*>(HostPtr(a[1])) : nullptr;
    if (g_backend) g_backend->Clear(g_state, a[0], rects, a[2], a[3], ArgF(a, 4), a[5]);
    return 0;
}

// (pp): the frame buffers pp asks for, the targets on them and cleared, as the library's Reset leaves the device.
// The device's references move back to the frame buffers first, so kept ones hold theirs as CreateDevice made them.
HLE_STDCALL(D3DDevice_Reset, 1) {
    constexpr uint32_t kClearTargetAndZ = 3;
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    ApplyRenderTarget(ctx, g_buffers[1], g_depth);
    SetFrameBuffers("Reset", a[0]);
    if (g_backend) g_backend->Clear(g_state, 0, nullptr, kClearTargetAndZ, 0, 1.0f, 0);
    return 0;
}

HLE_STDCALL(D3DDevice_CopyRects, 5) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    if (g_backend) g_backend->Copy(SurfaceOf(a[0]), SurfaceOf(a[3]), a[2], a[1], a[4]);
    return 0;
}

HLE_STDCALL(D3DDevice_SetViewport, 1) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    uint32_t v = a[0];
    if (!v) {
        // breakdown-recomp patch: no viewport covers the whole render target, as the library's does
        // (Breakdown's D3DDevice_SetDepthClipPlanes path 0x1BFD58 passes 0)
        uint32_t size = R32(Dev(kDevRenderTarget) + 16);
        uint32_t w = Dev(kDevRenderTarget) ? (size & 0xFFF) + 1 : g_state.viewport.w;
        uint32_t h = Dev(kDevRenderTarget) ? (size >> 12 & 0xFFF) + 1 : g_state.viewport.h;
        g_state.viewport = {0, 0, w, h, 0.0f, 1.0f};
        g_state.scissor_count = 0;
        UpdateViewportConstants();
        return 0;
    }
    g_state.viewport = {R32(v), R32(v + 4), R32(v + 8), R32(v + 12), GF(v + 16), GF(v + 20)};
    g_state.scissor_count = 0;
    UpdateViewportConstants();
    return 0;
}

HLE_STDCALL(D3DDevice_SetScissors, 3) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    g_state.scissor_count = std::min<uint32_t>(a[0], 8);
    for (uint32_t i = 0; i < g_state.scissor_count * 4; i++) g_state.scissors[i] = R32(a[2] + i * 4);
    return 0;
}

HLE_STDCALL(D3DDevice_SetScreenSpaceOffset, 2) {
    g_state.ss_offset_x = ArgF(a, 0);
    g_state.ss_offset_y = ArgF(a, 1);
    UpdateViewportConstants();
    return 0;
}

// render states

HLE_FASTCALL(D3DDevice_SetRenderState_Simple, 0) {
    uint32_t method = a[0] & 0x1FFC, value = a[1];
    SetSimple(method, value);
    return 0;
}

HLE_STDCALL(D3DDevice_SetRenderStateNotInline, 2) {
    uint32_t rs = a[0], v = a[1];
    if (rs < gpu::kRsDeferredFirst) {
        uint32_t method = R32(kSimpleEncode + rs * 4) & 0x1FFC;
        SetSimple(method, v);
        W32(kRenderState + rs * 4, v);  // titles read their states back from the library's table
    } else if (rs < gpu::kRsMax) {
        SetComplex(rs, v);
    }
    return 0;
}

#define COMPLEX_STATE(name, rs)                       \
    HLE_STDCALL(D3DDevice_SetRenderState_##name, 1) { \
        SetComplex(rs, a[0]);                         \
        return 0;                                     \
    }
COMPLEX_STATE(PSTextureModes, gpu::kRsPsTextureModes)
COMPLEX_STATE(VertexBlend, gpu::kRsVertexBlend)
COMPLEX_STATE(FogColor, gpu::kRsFogColor)
COMPLEX_STATE(FillMode, gpu::kRsFillMode)
COMPLEX_STATE(BackFillMode, gpu::kRsBackFillMode)
COMPLEX_STATE(TwoSidedLighting, gpu::kRsTwoSidedLighting)
COMPLEX_STATE(NormalizeNormals, gpu::kRsNormalizeNormals)
COMPLEX_STATE(ZEnable, gpu::kRsZEnable)
COMPLEX_STATE(StencilEnable, gpu::kRsStencilEnable)
COMPLEX_STATE(StencilFail, gpu::kRsStencilFail)
COMPLEX_STATE(FrontFace, gpu::kRsFrontFace)
COMPLEX_STATE(CullMode, gpu::kRsCullMode)
COMPLEX_STATE(TextureFactor, gpu::kRsTextureFactor)
COMPLEX_STATE(LogicOp, gpu::kRsLogicOp)
COMPLEX_STATE(EdgeAntiAlias, gpu::kRsEdgeAntiAlias)
COMPLEX_STATE(MultiSampleAntiAlias, gpu::kRsMultiSampleAntiAlias)
COMPLEX_STATE(MultiSampleMask, gpu::kRsMultiSampleMask)
COMPLEX_STATE(MultiSampleMode, gpu::kRsMultiSampleMode)
COMPLEX_STATE(MultiSampleRenderTargetMode, gpu::kRsMultiSampleRenderTargetMode)
COMPLEX_STATE(ShadowFunc, gpu::kRsShadowFunc)
COMPLEX_STATE(LineWidth, gpu::kRsLineWidth)
COMPLEX_STATE(SampleAlpha, gpu::kRsSampleAlpha)
COMPLEX_STATE(Dxt1NoiseEnable, gpu::kRsDxt1NoiseEnable)
COMPLEX_STATE(YuvEnable, gpu::kRsYuvEnable)
COMPLEX_STATE(OcclusionCullEnable, gpu::kRsOcclusionCullEnable)
COMPLEX_STATE(StencilCullEnable, gpu::kRsStencilCullEnable)
COMPLEX_STATE(RopZCmpAlwaysRead, gpu::kRsRopZCmpAlwaysRead)
COMPLEX_STATE(RopZRead, gpu::kRsRopZRead)
COMPLEX_STATE(DoNotCullUncompressed, gpu::kRsDoNotCullUncompressed)

HLE_STDCALL(D3DDevice_SetTextureState_TexCoordIndex, 2) {
    g_state.tss[a[0] & 3][gpu::kTssTexCoordIndex] = a[1];
    W32(kTextureState + ((a[0] & 3) * kStageStates + gpu::kTssTexCoordIndex) * 4, a[1]);
    return 0;
}
HLE_STDCALL(D3DDevice_SetTextureState_BumpEnv, 3) {
    if (a[1] >= gpu::kTssBumpEnvMat00 && a[1] <= gpu::kTssBumpEnvLOffset) g_state.tss[a[0] & 3][a[1]] = a[2];
    return 0;
}
HLE_STDCALL(D3DDevice_SetTextureState_BorderColor, 2) {
    g_state.tss[a[0] & 3][gpu::kTssBorderColor] = a[1];
    return 0;
}
HLE_STDCALL(D3DDevice_SetTextureState_ColorKeyColor, 2) {
    g_state.tss[a[0] & 3][gpu::kTssColorKeyColor] = a[1];
    return 0;
}
HLE_STDCALL(D3DDevice_SetTextureStageStateNotInline, 3) {
    uint32_t stage = a[0] & 3, type = a[1], v = a[2];
    if (type < gpu::kTssCount) {
        g_state.tss[stage][type] = v;
        W32(kTextureState + (stage * kStageStates + type) * 4, v);
    }
    return 0;
}

// the XDK's transform types: view 0, projection 1, texture 2-5, world 6-9; the renderer keeps Direct3D 8's numbers
// (view 2, projection 3, texture 16, world 256). The device copy is what GetTransform reads.
HLE_STDCALL(D3DDevice_SetTransform, 2) {
    uint32_t t = a[0];
    if (t >= 10) return 0;
    std::memcpy(HostPtr(kDevice + kDevTransforms + t * 64), HostPtr(a[1]), 64);
    uint32_t pc = t < 2 ? t + 2 : t < 6 ? t + 14 : t + 250;
    std::memcpy(g_state.transforms[pc].data(), HostPtr(a[1]), 64);
    return 0;
}

HLE_STDCALL(D3DDevice_SetMaterial, 1) {
    std::memcpy(g_state.material.data(), HostPtr(a[0]), 17 * 4);
    return 0;
}

// the XDK keeps its own light lists, the renderer gets a copy
HLE_WRAP(D3DDevice_SetLight, 2) {
    uint32_t i = a[0], l = a[1];
    if (i < 8 && l) {
        std::lock_guard<std::recursive_mutex> lock(g_lock);
        std::memcpy(g_state.lights[i].data(), HostPtr(l), 26 * 4);
    }
}
HLE_WRAP(D3DDevice_LightEnable, 2) {
    uint32_t i = a[0], on = a[1];
    if (i < 8) {
        std::lock_guard<std::recursive_mutex> lock(g_lock);
        g_state.light_enable = on ? g_state.light_enable | 1u << i : g_state.light_enable & ~(1u << i);
    }
}

// resources in use

HLE_STDCALL(D3DDevice_SetTexture, 2) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    uint32_t stage = a[0] & 3, tex = a[1];
    uint32_t old = Dev(kDevTextures + stage * 4);
    // bound again: the AddRef and Release would cancel out
    bool same = tex == old;
    if (!same) AddRef(ctx, tex);
    SetDev(kDevTextures + stage * 4, tex);
    if (!same) Release(ctx, old);
    g_state.textures[stage] = tex;
    W32(kDirtyFlags, R32(kDirtyFlags) | XDK_DirtyFlags_Texture);
    return 0;
}

// SetPalette(stage, palette): the colours a P8 texture of the stage looks up
HLE_STDCALL(D3DDevice_SetPalette, 2) {
    XDK_REQUIRE("d3d", XDK_KEY(CDevice_Palettes));
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    uint32_t stage = a[0] & 3, pal = a[1];
    uint32_t old = Dev(kDevPalettes + stage * 4);
    AddRef(ctx, pal);
    SetDev(kDevPalettes + stage * 4, pal);
    Release(ctx, old);
    g_state.palettes[stage] = pal;
    return 0;
}

// SwitchTexture(method, data, format), fastcall: a swizzled texture of the bound one's shape replaces it. It is
// bound through a header of the stage's own holding the bound texture's fields with the new data and format.
HLE_FASTCALL(D3DDevice_SwitchTexture, 1) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    static uint32_t headers[4];
    // texture methods from 0x1B00, 0x40 per stage
    uint32_t stage = (((a[0] & 0x1FFC) - 0x1B00) >> 6) & 3, cur = g_state.textures[stage];
    uint32_t& h = headers[stage];
    if (!h) h = mem::AllocSystem(20);
    if (cur && cur != h) std::memcpy(HostPtr(h), HostPtr(cur), 20);
    W32(h + 4, a[1]);
    W32(h + 12, a[2]);
    g_state.textures[stage] = h;
    return 0;
}

HLE_STDCALL(D3DDevice_SetStreamSource, 3) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    uint32_t n = a[0] & 15, vb = a[1], stride = a[2];
    uint32_t old = R32(kStreams + n * kStreamSize + XDK_Stream_Buffer);
    bool same = vb == old;
    if (!same) AddRef(ctx, vb);
    W32(kStreams + n * kStreamSize + XDK_Stream_Stride, stride);
    W32(kStreams + n * kStreamSize + XDK_Stream_Buffer, vb);
    if (!same) Release(ctx, old);
    g_state.streams[n] = {vb, stride};
    return 0;
}

HLE_STDCALL(D3DDevice_SetIndices, 2) {
    XDK_REQUIRE("d3d", XDK_DATA(D3D_IndexData));
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    uint32_t ib = a[0];
    uint32_t old = Dev(kDevIndexBuffer);
    bool same = ib == old;
    if (!same) AddRef(ctx, ib);
    W32(kIndexData, ib ? R32(ib + 4) : 0);
    SetDev(kDevIndexBuffer, ib);
    SetDev(kDevBaseVertex, a[1]);
    if (!same) Release(ctx, old);
    g_state.index_buffer = ib;
    g_state.index_data = ib ? R32(ib + 4) : 0;
    g_state.base_vertex = a[1];
    return 0;
}

// shaders

HLE_STDCALL(D3DDevice_CreateVertexShader, 4) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    uint32_t decl = a[0], func = a[1], out = a[2];
    auto vs = std::make_shared<gpu::VertexShader>();
    if (decl) {
        for (uint32_t i = 0; i < 1024; i++) {
            uint32_t t = R32(decl + i * 4);
            vs->decl.push_back(t);
            if (t == 0xFFFFFFFF) break;
        }
    }
    if (func) {
        uint32_t hdr = R32(func);
        vs->program_type = hdr & 0xFFFF;
        uint32_t n = hdr >> 16;
        for (uint32_t i = 0; i < n * 4; i++) vs->program.push_back(R32(func + 4 + i * 4));
    }
    gpu::ParseDeclaration(*vs);
    uint32_t rec = mem::AllocSystem(16, 16);
    vs->handle = rec | 1;
    g_vs[vs->handle] = vs;
    W32(out, vs->handle);
    LOG_FIRST(1000, "d3d: vertex shader %08X, %zu decl tokens, %zu instructions, type %X", vs->handle, vs->decl.size(),
              vs->program.size() / 4, vs->program_type);
    return 0;
}

HLE_STDCALL(D3DDevice_DeleteVertexShader, 1) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    auto it = g_vs.find(a[0]);
    if (it != g_vs.end()) {
        mem::FreeSystem(a[0] & ~1u);
        g_vs.erase(it);
    }
    return 0;
}

// (handle, data, size in and out): the program as it was given, the size alone without data or room
HLE_STDCALL(D3DDevice_GetVertexShaderFunction, 3) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    auto it = g_vs.find(a[0]);
    uint32_t words = it == g_vs.end() ? 0 : uint32_t(it->second->program.size());
    uint32_t need = words ? 4 + words * 4 : 0, room = R32(a[2]);
    W32(a[2], need);
    if (need > room) return a[1] ? 0x88760867 : 0;  // D3DERR_MOREDATA
    if (!a[1] || !need) return 0;
    W32(a[1], it->second->program_type | words / 4 << 16);
    for (uint32_t i = 0; i < words; i++) W32(a[1] + 4 + i * 4, it->second->program[i]);
    return 0;
}

HLE_STDCALL(D3DDevice_SetVertexShader, 1) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    uint32_t h = a[0];
    g_state.vs_handle = h;
    g_state.vs = nullptr;
    // breakdown-recomp patch: the device's current shader object as the library keeps it (a handle is the object's
    // address plus 1, an FVF selects the FVF shader object), for library code the host does not replace
#if HAS_D3D_FvfShader
    if (XDK_CDevice_VertexShader != xdk::kUnknown) SetDev(XDK_CDevice_VertexShader, h & 1 ? h - 1 : SYM(D3D_FvfShader));
#endif
    if (h & 1) {
        auto it = g_vs.find(h);
        if (it != g_vs.end()) {
            g_state.vs = it->second;
            // constants declared with the shader load when it is selected
            for (auto& [reg, v] : it->second->constants)
                if (reg < 192) g_state.vsc[reg] = v;
        } else {
            LOG_FIRST(8, "d3d: SetVertexShader with unknown handle %08X", h);
        }
    }
    return 0;
}

HLE_FASTCALL(D3DDevice_SetVertexShaderConstant1Fast, 0) {
    uint32_t reg = a[0];
    if (reg < 192) std::memcpy(g_state.vsc[reg].data(), HostPtr(a[1]), 16);
    return 0;
}

HLE_FASTCALL(D3DDevice_SetVertexShaderConstant1, 0) {
    uint32_t reg = a[0];
    if (reg < 192) std::memcpy(g_state.vsc[reg].data(), HostPtr(a[1]), 16);
    return 0;
}

HLE_FASTCALL(D3DDevice_SetVertexShaderConstant4, 0) {
    uint32_t reg = a[0], src = a[1];
    for (uint32_t i = 0; i < 4 && reg + i < 192; i++) std::memcpy(g_state.vsc[reg + i].data(), HostPtr(src + i * 16), 16);
    return 0;
}

HLE_FASTCALL(D3DDevice_SetVertexShaderConstantNotInlineFast, 1) {
    uint32_t reg = a[0], src = a[1], dwords = a[2];
    for (uint32_t i = 0; i < dwords / 4 && reg + i < 192; i++) std::memcpy(g_state.vsc[reg + i].data(), HostPtr(src + i * 16), 16);
    return 0;
}

// Vertex program memory: Load writes instructions at a slot, Select runs the program that starts at one
namespace {
constexpr uint32_t kProgramSlots = 136;
constexpr uint32_t kInstructionDwords = 4;
constexpr uint32_t kLastInstruction = 1;  // bit 0 of an instruction's fourth dword
constexpr uint32_t kVertexProgramType = 0x2078;
constexpr uint32_t kInputRegisters = 16;

// guest vertex shader object, the record a handle points at
constexpr uint32_t kShaderInputs = 0x14;  // 16 inputs of {stream, offset, D3DVSDT type, unused}
constexpr uint32_t kShaderInputBytes = 16;
constexpr uint32_t kTypeNone = 0x02;  // D3DVSDT_NONE

std::array<uint32_t, kProgramSlots * kInstructionDwords> g_program_memory{};
std::map<uint64_t, std::shared_ptr<gpu::VertexShader>> g_selected;  // kept alive, the render thread keys by pointer

// bytes of one D3DVSDT element: kind in the low nibble, count in the next
uint32_t ElementBytes(uint32_t type) {
    uint32_t kind = type & 15;
    uint32_t count = (type >> 4) & 15;
    if (kind == 0 || kind == 6) return 4;          // D3DCOLOR, packed 11:11:10
    if (kind == 1 || kind == 5) return 2 * count;  // shorts
    if (kind == 4) return count;                   // bytes
    return 4 * count;                              // floats
}

void StoreProgram(uint32_t slot, const std::vector<uint32_t>& words) {
    if (slot >= kProgramSlots) return;
    size_t room = g_program_memory.size() - slot * kInstructionDwords;
    std::copy_n(words.begin(), std::min(words.size(), room), g_program_memory.begin() + slot * kInstructionDwords);
}

// a {type | count << 16, instructions} function's instructions
std::vector<uint32_t> FunctionWords(uint32_t function) {
    uint32_t count = R32(function) >> 16;
    std::vector<uint32_t> words(count * kInstructionDwords);
    for (uint32_t i = 0; i < words.size(); i++) words[i] = R32(function + 4 + i * 4);
    return words;
}

// the instructions from slot to the last one
std::vector<uint32_t> ProgramAt(uint32_t slot) {
    std::vector<uint32_t> program;
    for (; slot < kProgramSlots; slot++) {
        const uint32_t* insn = &g_program_memory[slot * kInstructionDwords];
        program.insert(program.end(), insn, insn + kInstructionDwords);
        if (insn[3] & kLastInstruction) break;
    }
    return program;
}

// a guest made shader's inputs, as its object holds them
void ApplyGuestInputs(uint32_t handle, gpu::VertexShader& vs) {
    uint32_t inputs = (handle & ~1u) + kShaderInputs;
    for (uint32_t reg = 0; reg < kInputRegisters; reg++) {
        uint32_t entry = inputs + reg * kShaderInputBytes;
        uint32_t stream = R32(entry);
        uint32_t offset = R32(entry + 4);
        uint32_t type = R32(entry + 8);
        if (type == kTypeNone || stream >= vs.stream_stride.size()) continue;
        vs.inputs[reg] = {int(stream), offset, type};
        uint32_t end = offset + ElementBytes(type);
        vs.stream_stride[stream] = std::max(vs.stream_stride[stream], end);
    }
}

// the vertex inputs a selected program reads: a host made shader's, else the guest object's
void ApplyShaderInputs(uint32_t handle, gpu::VertexShader& vs) {
    auto it = g_vs.find(handle);
    if (it == g_vs.end()) {
        ApplyGuestInputs(handle, vs);
        return;
    }
    vs.inputs = it->second->inputs;
    vs.stream_stride = it->second->stream_stride;
}

// one shader object per program and input layout; handle 0 keeps the bound shader's inputs
std::shared_ptr<gpu::VertexShader> SelectedShader(uint32_t handle, uint32_t slot) {
    gpu::VertexShader vs;
    vs.program = ProgramAt(slot);
    vs.program_type = kVertexProgramType;
    if (handle) {
        ApplyShaderInputs(handle, vs);
    } else {
        vs.inputs = g_state.vs->inputs;
        vs.stream_stride = g_state.vs->stream_stride;
    }
    uint64_t key = gpu::Hash(vs.program.data(), vs.program.size() * 4);
    key = gpu::Hash(vs.inputs.data(), sizeof(vs.inputs), key);
    key = gpu::Hash(vs.stream_stride.data(), sizeof(vs.stream_stride), key);
    auto& kept = g_selected[key];
    if (!kept) kept = std::make_shared<gpu::VertexShader>(std::move(vs));
    return kept;
}
}  // namespace

// (handle, slot): the program a shader was made with, stored at the slot
HLE_STDCALL(D3DDevice_LoadVertexShader, 2) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    auto it = g_vs.find(a[0]);
    if (it == g_vs.end()) {
        LOG_FIRST(8, "d3d: LoadVertexShader with unknown handle %08X", a[0]);
        return 0;
    }
    StoreProgram(a[1], it->second->program);
    return 0;
}

// (function, slot)
HLE_STDCALL(D3DDevice_LoadVertexShaderProgram, 2) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    StoreProgram(a[1], FunctionWords(a[0]));
    return 0;
}

// (handle, slot): the program at the slot runs with the handle's inputs; handle 0 only moves the start
HLE_STDCALL(D3DDevice_SelectVertexShader, 2) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    uint32_t handle = a[0];
    bool program_bound = (g_state.vs_handle & 1) && g_state.vs;
    if (!handle && !program_bound) {
        LOG_FIRST(8, "d3d: SelectVertexShader at slot %u without a bound vertex shader", a[1]);
        return 0;
    }
    g_state.vs = SelectedShader(handle, a[1]);
    if (handle) g_state.vs_handle = handle;
    return 0;
}
HLE_STDCALL(D3DDevice_SetShaderConstantMode, 1) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    g_own_constants = a[0] & 0x10;  // D3DSCM_NORESERVEDCONSTANTS
    UpdateViewportConstants();
    return 0;
}

HLE_STDCALL(D3DDevice_CreatePixelShader, 2) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    auto ps = std::make_shared<gpu::PixelShader>();
    std::memcpy(ps->def, HostPtr(a[0]), sizeof(ps->def));
    uint32_t rec = mem::AllocSystem(16, 16);
    ps->handle = rec;
    g_ps[rec] = ps;
    W32(a[1], rec);
    return 0;
}

HLE_STDCALL(D3DDevice_DeletePixelShader, 1) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    if (g_ps.erase(a[0])) mem::FreeSystem(a[0]);
    return 0;
}

HLE_STDCALL(D3DDevice_SetPixelShader, 1) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    g_state.ps = nullptr;
    if (!a[0]) return 0;
    auto it = g_ps.find(a[0]);
    if (it != g_ps.end()) {
        g_state.ps = it->second;
    } else if (XDK_PixelShader_Definition != xdk::kUnknown) {
        // made by library code the title inlined: the object points at its definition, handles are reused
        static std::unordered_map<uint32_t, std::shared_ptr<gpu::PixelShader>> made;
        const void* def = HostPtr(R32(a[0] + XDK_PixelShader_Definition));
        auto& ps = made[a[0]];
        if (!ps || std::memcmp(ps->def, def, sizeof(ps->def))) {
            ps = std::make_shared<gpu::PixelShader>();
            ps->handle = a[0];
            std::memcpy(ps->def, def, sizeof(ps->def));
        }
        g_state.ps = ps;
    } else {
        LOG_FIRST(8, "d3d: SetPixelShader with unknown handle %08X", a[0]);
        return 0;
    }
    // the definition loads the combiner state like the XDK pushes it
    const uint32_t* d = g_state.ps->def;
    for (int i = 0; i < 8; i++) {
        SetSimple(gpu::kMPsAlphaInputs + i * 4, d[0 + i]);
        SetSimple(gpu::kMPsConstant0 + i * 4, d[10 + i]);
        SetSimple(gpu::kMPsConstant1 + i * 4, d[18 + i]);
        SetSimple(gpu::kMPsAlphaOutputs + i * 4, d[26 + i]);
        SetSimple(gpu::kMPsRgbInputs + i * 4, d[34 + i]);
        SetSimple(gpu::kMPsRgbOutputs + i * 4, d[45 + i]);
    }
    SetSimple(gpu::kMPsFinalAbcd, d[8]);
    SetSimple(gpu::kMPsFinalEfgh, d[9]);
    SetSimple(gpu::kMPsCompareMode, d[42]);
    SetSimple(gpu::kMPsFinalConstant0, d[43]);
    SetSimple(gpu::kMPsFinalConstant1, d[44]);
    SetSimple(gpu::kMPsCombinerCount, d[53]);
    SetSimple(gpu::kMPsDotMapping, d[55]);
    SetSimple(gpu::kMPsInputTexture, d[56]);
    SetComplex(gpu::kRsPsTextureModes, d[54]);
    return 0;
}

HLE_STDCALL(D3DDevice_SetPixelShaderConstant, 3) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    uint32_t reg = a[0], src = a[1], n = a[2];
    for (uint32_t i = 0; i < n && reg + i < 16; i++) std::memcpy(g_state.psc[reg + i].data(), HostPtr(src + i * 16), 16);
    gpu::ApplyPixelConstants(g_state, reg, n);
    gpu::MarkAllSimple();  // it writes the combiner constants into simple
    return 0;
}

// drawing

HLE_STDCALL(D3DDevice_DrawVertices, 3) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    Walk(Dev(kDevPush));  // a visibility test begun since the last draw counts this one
    SyncDeferred();
    if (g_backend) g_backend->Draw(g_state, a[0], a[1], a[2], nullptr);
    return 0;
}

HLE_STDCALL(D3DDevice_DrawIndexedVertices, 3) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    Walk(Dev(kDevPush));
    SyncDeferred();
    const uint8_t* indices = a[2] ? static_cast<const uint8_t*>(HostPtr(a[2])) : nullptr;
    if (g_backend) g_backend->Draw(g_state, a[0], 0, a[1], indices);
    return 0;
}

// DrawVerticesUP(prim, count, vertices, stride): the vertices are copied to a ring the render thread reads, stream 0
// points there through a vertex buffer header for the draw. A half of the ring is written again once the draws
// that read it are done.
namespace {
constexpr uint32_t kUpRing = 4u << 20, kUpHalf = kUpRing / 2;
uint32_t g_up_ring = 0, g_up_pos = 0, g_up_header = 0, g_up_fence[2] = {};

uint32_t UpVertices(const void* src, uint32_t bytes) {
    if (!g_up_ring) {
        g_up_ring = mem::AllocContiguous(kUpRing, 0, 0xFFFFFFFF, 0x1000);
        g_up_header = mem::AllocSystem(20);
        std::memset(HostPtr(g_up_header), 0, 20);
        W32(g_up_header, 1);  // a vertex buffer, one reference
    }
    bytes = std::min(bytes, kUpHalf);
    uint32_t room = (bytes + 63) & ~63u;
    uint32_t pos = g_up_pos + room > kUpRing ? 0 : g_up_pos;
    uint32_t from = g_up_pos ? (g_up_pos - 1) / kUpHalf : 1, to = (pos + room - 1) / kUpHalf;
    if (to != from) {
        g_up_fence[from] = g_backend->Fence();
        g_backend->WaitFence(g_up_fence[to]);
    }
    std::memcpy(HostPtr(g_up_ring + pos), src, bytes);
    g_up_pos = pos + room;
    W32(g_up_header + 4, mem::PhysicalFromVirtual(g_up_ring + pos));
    return g_up_header;
}

void DrawUp(uint32_t prim, uint32_t count, const void* vertices, uint32_t stride) {
    if (!g_backend || !count || !stride) return;
    SyncDeferred();
    auto stream = g_state.streams[0];
    g_state.streams[0] = {UpVertices(vertices, count * stride), stride};
    g_backend->Draw(g_state, prim, 0, count, nullptr);
    g_state.streams[0] = stream;
}

// bytes per vertex of the bound format: stream 0 of the FVF or of the shader's declaration
uint32_t VertexBytes() {
    if (g_state.vs_handle & 1) return g_state.vs ? g_state.vs->stream_stride[0] : 0;
    gpu::VertexShader fvf;
    gpu::FvfInputs(g_state.vs_handle, fvf);
    return fvf.stream_stride[0];
}

uint32_t g_push_begin = 0;
}  // namespace

// Begin/End immediate mode. Titles inline Begin and the vertex data calls, so their commands are read from the scratch buffer.
namespace d3d {
namespace {
uint32_t g_im_prim = 0, g_im_mask = 0;  // open primitive, slots it wrote
float g_im_attr[64];                    // 16 slots of x y z w
// breakdown-recomp patch: current attribute values start as diffuse white, others (0, 0, 0, 1), mirrored into the
// constants a program reads them from (gpu::State::vsc 192 to 207)
struct ImAttrInit {
    ImAttrInit() {
        for (int r = 0; r < 16; r++) {
            float d[4] = {r == 3 ? 1.f : 0.f, r == 3 ? 1.f : 0.f, r == 3 ? 1.f : 0.f, 1.f};
            std::memcpy(g_im_attr + r * 4, d, 16);
            std::memcpy(g_state.vsc[size_t(192 + r)].data(), d, 16);
        }
    }
} g_im_attr_init;
std::vector<float> g_im_vertices;       // every slot per vertex

// visibility tests: depth passing pixels are counted while enabled, a report gets the count since the clear,
// at a physical address in the first 16 MB
void ApplyVisibilityMethod(uint32_t m, uint32_t v) {
    constexpr uint32_t kReportAddressMask = 0xFFFFFF;
    if (m == gpu::kMCountEnable) SetSimple(gpu::kMCountEnable, v);
    if (!g_backend) return;
    if (m == gpu::kMClearReport) g_backend->ClearReport();
    if (m == gpu::kMGetReport) g_backend->GetReport(mem::VirtualFromPhysical(v & kReportAddressMask));
}

// draws the open primitive: the slots it wrote are FLOAT4 inputs of the bound program, or of the fixed pipeline
// draws vertices given as FLOAT4 per slot in mask (packed: count vertices of popcount(mask) float4s) with the bound
// program, or the fixed pipeline
void DrawSlots(uint32_t prim, uint32_t count, uint32_t mask, const float* packed) {
    struct Made {
        std::shared_ptr<gpu::VertexShader> from, vs;
    };
    // the render thread tells shaders apart by pointer: one object per program and slot set, the program kept alive
    static std::map<std::pair<gpu::VertexShader*, uint32_t>, Made> made;
    uint32_t handle = g_state.vs_handle;
    if (!prim || !count || !mask) return;
    auto bound = g_state.vs, from = handle & 1 ? bound : nullptr;
    Made& m = made[{from.get(), mask}];
    if (!m.vs) {
        m.from = from;
        m.vs = std::make_shared<gpu::VertexShader>();
        if (from) {
            m.vs->program = from->program;
            m.vs->program_type = from->program_type;
        }
        for (uint32_t s = 0; s < 16; s++)
            if (mask >> s & 1) {
                m.vs->inputs[s] = {0, m.vs->stream_stride[0], 0x42};
                m.vs->stream_stride[0] += 16;
            }
    }
    g_state.vs = m.vs;
    g_state.vs_handle = handle | 1;
    DrawUp(prim, count, packed, m.vs->stream_stride[0]);
    g_state.vs = bound;
    g_state.vs_handle = handle;
}

void EndPrimitive() {
    static std::vector<float> packed;
    uint32_t prim = g_im_prim, count = uint32_t(g_im_vertices.size() / 64), handle = g_state.vs_handle;
    g_im_prim = 0;
    if (!prim || !count) return;
    if (!(handle & 1) && (handle & 0xE) == 4) LOG_FIRST(1, "d3d: pretransformed immediate mode vertices are not supported");
    packed.clear();
    for (uint32_t v = 0; v < count; v++)
        for (uint32_t s = 0; s < 16; s++)
            if (g_im_mask >> s & 1) packed.insert(packed.end(), &g_im_vertices[v * 64 + s * 4], &g_im_vertices[v * 64 + s * 4] + 4);
    DrawSlots(prim, count, g_im_mask, packed.data());
}

// breakdown-recomp patch: vertex arrays a title sets up with raw commands (precompiled push buffers): per slot an
// offset (0x1720, physical) and a format (0x1760: type bits 0-3, size 4-7, stride 8-31). Draws read them when the
// commands set formats since the walk began; otherwise the arrays are the ones the D3D state bound (the streams).
struct RawArrays {
    uint32_t offset[16] = {}, format[16] = {};
    bool valid = false;
    bool Method(uint32_t m, uint32_t v) {
        if (m >= 0x1720 && m < 0x1760) {
            offset[(m - 0x1720) / 4] = v;
        } else if (m >= 0x1760 && m < 0x17A0) {
            format[(m - 0x1760) / 4] = v;
            valid = true;
        } else {
            return false;
        }
        return true;
    }
    static uint32_t Components(uint32_t f) { return (f >> 4) & 0xF; }
    static uint32_t Bytes(uint32_t f) {
        uint32_t n = Components(f);
        switch (f & 0xF) {
            case 0: case 4: return n;          // UB_D3D, UB_OGL
            case 1: case 5: return n * 2;      // S1, S32K
            case 2: return n * 4;              // F
            case 6: return n * 4;              // CMP
            default: return 0;
        }
    }
    uint32_t Mask() const {
        uint32_t m = 0;
        for (int s = 0; s < 16; s++)
            if (Components(format[s]) && Bytes(format[s])) m |= 1u << s;
        return m;
    }
    // one attribute from guest bytes into x y z w
    static void Decode(uint32_t f, const uint8_t* p, float* out) {
        uint32_t n = Components(f);
        float v[4] = {0, 0, 0, 1};
        switch (f & 0xF) {
            case 0:  // D3DCOLOR order: bytes b g r a
                if (n >= 4) { v[0] = p[2] / 255.f; v[1] = p[1] / 255.f; v[2] = p[0] / 255.f; v[3] = p[3] / 255.f; }
                else for (uint32_t i = 0; i < n; i++) v[i] = p[i] / 255.f;
                break;
            case 4:
                for (uint32_t i = 0; i < n && i < 4; i++) v[i] = p[i] / 255.f;
                break;
            case 1:
                for (uint32_t i = 0; i < n && i < 4; i++) { int16_t x; std::memcpy(&x, p + i * 2, 2); v[i] = std::max(x / 32767.f, -1.f); }
                break;
            case 5:
                for (uint32_t i = 0; i < n && i < 4; i++) { int16_t x; std::memcpy(&x, p + i * 2, 2); v[i] = float(x); }
                break;
            case 2:
                for (uint32_t i = 0; i < n && i < 4; i++) std::memcpy(&v[i], p + i * 4, 4);
                break;
            case 6: {  // packed 11 11 10, signed normalized
                uint32_t w;
                std::memcpy(&w, p, 4);
                int32_t x = int32_t(w << 21) >> 21, y = int32_t(w << 10) >> 21, z = int32_t(w) >> 22;
                v[0] = std::max(x / 1023.f, -1.f);
                v[1] = std::max(y / 1023.f, -1.f);
                v[2] = std::max(z / 511.f, -1.f);
                break;
            }
        }
        std::memcpy(out, v, 16);
    }
    // vertices by index from the arrays
    void DrawIndexed(uint32_t prim, const uint32_t* idx, uint32_t n) const {
        static std::vector<float> packed;
        uint32_t mask = Mask();
        packed.clear();
        uint8_t* base[16] = {};
        for (int s = 0; s < 16; s++)
            if (mask >> s & 1) base[s] = static_cast<uint8_t*>(HostPtr(mem::VirtualFromPhysical(offset[s] & 0x7FFFFFFF)));
        for (uint32_t i = 0; i < n; i++)
            for (int s = 0; s < 16; s++)
                if (mask >> s & 1) {
                    float o[4];
                    Decode(format[s], base[s] + size_t(idx[i]) * (format[s] >> 8), o);
                    packed.insert(packed.end(), o, o + 4);
                }
        DrawSlots(prim, n, mask, packed.data());
    }
    // inline vertices in the array formats, enabled slots in order, no padding
    bool DrawInline(uint32_t prim, const std::vector<uint32_t>& words) const {
        static std::vector<float> packed;
        uint32_t mask = Mask(), bytes = 0;
        for (int s = 0; s < 16; s++)
            if (mask >> s & 1) bytes += (Bytes(format[s]) + 3) & ~3u;
        if (!bytes || (words.size() * 4) % bytes) return false;
        uint32_t count = uint32_t(words.size() * 4 / bytes);
        const uint8_t* p = reinterpret_cast<const uint8_t*>(words.data());
        packed.clear();
        for (uint32_t i = 0; i < count; i++)
            for (int s = 0; s < 16; s++)
                if (mask >> s & 1) {
                    float o[4];
                    Decode(format[s], p, o);
                    p += (Bytes(format[s]) + 3) & ~3u;
                    packed.insert(packed.end(), o, o + 4);
                }
        DrawSlots(prim, count, mask, packed.data());
        return true;
    }
};

// a begin/end or vertex data method opens, feeds or draws the immediate primitive; false for any other method
bool ApplyImmediate(uint32_t m, uint32_t v) {
    float f;
    float* a = g_im_attr;
    bool last = true;  // the slot's last component
    std::memcpy(&f, &v, 4);
    if (m == 0x17FC) {  // begin/end, 0 ends
        if (v) {
            g_im_prim = v;
            g_im_mask = 0;
            g_im_vertices.clear();
        } else {
            EndPrimitive();
        }
        return true;
    } else if (m >= 0x1880 && m < 0x1900) {  // 2 floats, z = 0 and w = 1
        a += (m - 0x1880) / 8 * 4;
        last = (m - 0x1880) / 4 & 1;
        a[last] = f;
        a[2] = 0;
        a[3] = 1;
    } else if (m >= 0x1940 && m < 0x1980) {  // 4 bytes
        a += (m - 0x1940) / 4 * 4;
        for (int c = 0; c < 4; c++) a[c] = (v >> c * 8 & 0xFF) / 255.0f;
    } else if (m >= 0x1A00 && m < 0x1B00) {  // 4 floats
        a += (m - 0x1A00) / 16 * 4;
        last = ((m - 0x1A00) / 4 & 3) == 3;
        a[(m - 0x1A00) / 4 & 3] = f;
    } else if (m >= 0x1500 && m < 0x150C) {  // position, 3 floats and w = 1
        last = m == 0x1508;
        a[(m - 0x1500) / 4] = f;
        a[3] = 1;
    } else if (m >= 0x1518 && m < 0x1528) {  // position, 4 floats
        last = m == 0x1524;
        a[(m - 0x1518) / 4] = f;
    } else {
        return false;
    }
    {
        size_t slot = size_t(a - g_im_attr) / 4;
        std::memcpy(g_state.vsc[192 + slot].data(), a, 16);
    }
    if (!g_im_prim) return true;
    g_im_mask |= 1u << (a - g_im_attr) / 4;
    if (a == g_im_attr && last) g_im_vertices.insert(g_im_vertices.end(), g_im_attr, g_im_attr + 64);
    return true;
}

// Vertex constants a title pushes itself (007 does a skinned mesh's ambient and material colour so): a load
// register, then four dwords per register from it on
constexpr uint32_t kConstantLoad = 0x1EA4;
constexpr uint32_t kConstantData = 0x0B80;
constexpr uint32_t kConstantDataEnd = 0x0C00;
constexpr uint32_t kConstantRegisters = 192;
uint32_t g_constant_register = 0;
uint32_t g_constant_component = 0;

// a constant load or data method updates the vertex constants; false for any other method
bool ApplyConstant(uint32_t m, uint32_t v) {
    if (m == kConstantLoad) {
        g_constant_register = v;
        g_constant_component = 0;
        return true;
    }
    if (m < kConstantData || m >= kConstantDataEnd) return false;
    if (g_constant_register < kConstantRegisters) {
        std::memcpy(&g_state.vsc[g_constant_register][g_constant_component], &v, 4);
    }
    if (++g_constant_component == 4) {
        g_constant_component = 0;
        g_constant_register++;
    }
    return true;
}

// breakdown-recomp patch: the render state methods, alpha test enable to normalize enable. Breakdown's engine pushes
// some material states itself (alpha test, blend, depth write) instead of calling SetRenderState; they were skipped
// here, so the next draws kept whatever the library calls had set last.
constexpr uint32_t kMRenderStateFirst = 0x300;
constexpr uint32_t kMRenderStateEnd = 0x3A8;

// reads the commands pushed since the last call: vertex data between a begin and an end is drawn, visibility tests
// are run, constants and render states are stored, the rest is skipped
void Walk(uint32_t end) {
    if (!g_walk || end < g_walk || end > g_scratch_push + g_scratch_size) return;
    for (uint32_t p = g_walk; p + 4 <= end;) {
        // count in bits 18 to 28, bit 30 keeps the method for every value
        uint32_t head = R32(p), method = head & 0x1FFC, n = (head >> 18) & 0x7FF;
        p += 4;
        if ((head & 0xA0030003) || n * 4 > end - p) break;  // anything but a method header
        for (uint32_t i = 0; i < n; i++, p += 4) {
            uint32_t m = head & 0x40000000 ? method : method + i * 4;
            uint32_t v = R32(p);
            ApplyVisibilityMethod(m, v);
            if (!ApplyConstant(m, v) && !ApplyImmediate(m, v) && m >= kMRenderStateFirst && m < kMRenderStateEnd) {
                LOG_FIRST(8, "d3d: pushed render state %04X = %08X", m, v);
                SetSimple(m, v);
            }
        }
    }
    g_walk = end;
}
}  // namespace
}  // namespace d3d

HLE_STDCALL(D3DDevice_DrawVerticesUP, 4) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    Walk(Dev(kDevPush));
    DrawUp(a[0], a[1], HostPtr(a[2]), a[3]);
    return 0;
}

// BeginPush(dwords) hands out room for raw hardware commands, EndPush(end) runs what the title wrote there:
// a primitive between two begin/end writes with its vertices inline or as vertex data, other methods as simple states.
// breakdown-recomp patch: while a push buffer records on this thread, the raw commands go into that buffer, in order
// with the captured calls, and run when it is replayed; recorded state calls have not happened yet, so running the
// commands now would draw with the wrong state (Breakdown's full screen passes at 0x192560 rely on a captured
// SetVertexShader for their vertex format)
bool PushIntoRecording(X86Context& ctx);
bool g_push_recorded = false;
HLE_STDCALL(D3DDevice_BeginPush, 1) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    if (PushIntoRecording(ctx)) {
        g_push_recorded = true;
        return Dev(kDevPush);
    }
    g_push_begin = ScratchPush(a[0] * 4);
    if (std::getenv("XBE_PUSHDUMP")) LOG_FIRST(16, "d3d: BeginPush %u dwords from %08X", a[0], R32(ctx.esp));
    return g_push_begin;
}

// a primitive whose vertices came as an inline array, in the bound vertex format
static void DrawInline(uint32_t prim, const std::vector<uint32_t>& vertices) {
    if (vertices.empty()) return;
    uint32_t stride = VertexBytes();
    uint32_t bytes = uint32_t(vertices.size()) * 4;
    if (stride && bytes % stride == 0) DrawUp(prim, bytes / stride, vertices.data(), stride);
    else if (std::getenv("XBE_PUSHDUMP")) {
        static int n = 0;
        g_dump_on = true;
        if (n++ < 4)
            for (size_t i = 0; i < vertices.size(); i++) LOGI("d3d: dropped vertex word %zu %08X", i, vertices[i]);
    }
    if (!(stride && bytes % stride == 0)) LOG_FIRST(8, "d3d: EndPush primitive %u dropped, %u bytes of vertices of %u (vertex shader %08X)", prim, bytes,
                   stride, g_state.vs_handle);
}


// breakdown-recomp patch: indexed and ranged draws pushed as raw commands between a begin and an end read the vertex
// arrays the D3D state set up (the streams of the bound shader), as on the hardware: ARRAY_ELEMENT16 (0x1800, two
// indices per dword), ARRAY_ELEMENT32 (0x1808), DRAW_ARRAYS (0x1810, start | (count - 1) << 24). Breakdown pushes
// index pairs for strips this way.
struct ArrayDraws {
    std::vector<uint32_t> indices;
    std::vector<std::pair<uint32_t, uint32_t>> ranges;
    bool Add(uint32_t m, uint32_t v) {
        if (m >= 0x1800 && m < 0x1808) {
            indices.push_back(v & 0xFFFF);
            indices.push_back(v >> 16);
        } else if (m >= 0x1808 && m < 0x1810) {
            indices.push_back(v);
        } else if (m >= 0x1810 && m < 0x1818) {
            ranges.push_back({v & 0xFFFFFF, (v >> 24) + 1});
        } else {
            return false;
        }
        return true;
    }
    void Flush(uint32_t prim, const RawArrays& raw) {
        if (prim && g_backend && raw.valid && (!indices.empty() || !ranges.empty())) {
            static std::vector<uint32_t> idx;
            idx.assign(indices.begin(), indices.end());
            for (auto [start, count] : ranges)
                for (uint32_t i = 0; i < count; i++) idx.push_back(start + i);
            raw.DrawIndexed(prim, idx.data(), uint32_t(idx.size()));
        } else if (prim && g_backend && (!indices.empty() || !ranges.empty())) {
            SyncDeferred();
            if (!indices.empty()) {
                // the backend copies 16 bit indices
                static std::vector<uint16_t> i16;
                i16.clear();
                for (uint32_t i : indices) {
                    if (i > 0xFFFF) LOG_FIRST(4, "d3d: pushed index %u above 16 bits", i);
                    i16.push_back(uint16_t(i));
                }
                g_backend->Draw(g_state, prim, 0, uint32_t(i16.size()), reinterpret_cast<const uint8_t*>(i16.data()));
            }
            for (auto [start, count] : ranges) g_backend->Draw(g_state, prim, start, count, nullptr);
        }
        indices.clear();
        ranges.clear();
    }
};
static void RunCommands(uint32_t begin, uint32_t end) {
    constexpr uint32_t kBeginEnd = 0x17FC, kInlineArray = 0x1818;
    // vertex array offsets and formats, then index elements and draw arrays up to the inline array
    constexpr uint32_t kArrayOffsets = 0x1720, kArrayFormatsEnd = 0x17A0, kArrayDraws = 0x1800;
    static std::vector<uint32_t> vertices;
    static ArrayDraws arrays;
    static RawArrays raw;
    raw.valid = false;
    uint32_t prim = 0;
    for (uint32_t p = begin; begin && p + 4 <= end;) {
        // count in bits 18 to 28, bit 30 keeps the method for every value
        uint32_t head = R32(p), method = head & 0x1FFC, n = (head >> 18) & 0x7FF;
        p += 4;
        if ((head & 0xA0030003) || n * 4 > end - p) {  // anything but a method header
            LOG_FIRST(8, "d3d: pushed commands stop at %08X, %u bytes before the end", head, end - p);
            break;
        }
        for (uint32_t i = 0; i < n; i++, p += 4) {
            uint32_t m = head & 0x40000000 ? method : method + i * 4, v = R32(p);
            static int dumped = 0;
            static const bool fmt_dump = std::getenv("XBE_FMTDUMP") != nullptr;
            if (fmt_dump && m >= 0x1760 && m < 0x17A0) LOGI("d3d: format method %04X = %08X", m, v);
            if (g_dump_on && dumped < 400) {
                dumped++;
                LOGI("d3d: pushed %04X = %08X", m, v);
            }
            ApplyVisibilityMethod(m, v);
            if (m == kInlineArray) {
                if (prim) vertices.push_back(v);
            } else if (m == kBeginEnd) {
                if (!v && prim && !(raw.valid && raw.DrawInline(prim, vertices))) DrawInline(prim, vertices);
                if (!v) arrays.Flush(prim, raw);
                prim = v;
                vertices.clear();
                ApplyImmediate(m, v);
            } else if (prim && arrays.Add(m, v)) {
            } else if (raw.Method(m, v)) {
            } else if ((m >= kArrayOffsets && m < kArrayFormatsEnd) || (m >= kArrayDraws && m < kInlineArray)) {
                LOG_FIRST(16, "d3d: pushed commands draw from vertex arrays (method %04X = %08X), which is not run", m, v);
            } else if (!ApplyConstant(m, v) && !ApplyImmediate(m, v) && m >> 2 < g_state.simple.size()) {
                SetSimple(m, v);
            }
        }
    }
}

HLE_STDCALL(D3DDevice_EndPush, 1) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    if (g_push_recorded) {
        g_push_recorded = false;
        SetDev(kDevPush, a[0]);
        return a[0];
    }
    RunCommands(g_push_begin, a[0]);
    g_push_begin = 0;
    ScratchPush();
    return a[0];
}

// ---- breakdown-recomp patch: recorded push buffers -------------------------------------------------------------
// D3DDevice_BeginPushBuffer points the device's push pointer into a title's D3DPushBuffer (Common, Data, Lock, Size,
// AllocationSize), EndPushBuffer stores how much was written, RunPushBuffer runs it, and titles may patch the commands
// (Breakdown chains its depth sorted lists with jumps written into each buffer's last dword). The host runs no
// hardware commands for the D3D calls it implements, so while a buffer records those calls are captured: each one
// writes a no-operation method into the buffer that carries its index, and RunPushBuffer walks the commands, follows
// jumps and replays the captured calls where their markers sit. Raw commands the title or inlined library code
// pushed meanwhile stay in the buffer and run as EndPush runs them.
namespace {
constexpr uint32_t kNopMethod = 0x0100, kMarkerMagic = 0xB0DA5EC0;
struct Captured {
    X86Func* fn = nullptr;
    const char* name = nullptr;
    X86Context regs{};
    std::vector<uint32_t> stack;  // the return address slot, then the arguments
    std::vector<uint32_t> copies; // guest allocations holding pointed to data, freed when the buffer records again
};
std::vector<Captured> g_captured;
std::unordered_map<uint32_t, std::vector<uint32_t>> g_captured_of;  // push buffer -> its captured indices
// breakdown-recomp patch: slots of buffers that recorded again, reused; without it the list grew by every call a
// title recorded each frame (Breakdown re-records its push buffers every frame: about 18 MB a second)
std::vector<uint32_t> g_captured_free;
uint32_t g_recording = 0, g_rec_thread = 0;
uint32_t g_saved_push = 0, g_saved_threshold = 0;
bool g_replaying = false;
}  // namespace
bool PushIntoRecording(X86Context& ctx) { return g_recording && !g_replaying && ctx.thread_handle == g_rec_thread; }
namespace {

// API argument data a captured call points at: (argument location, bytes, count argument location or -1, count unit)
struct PtrArg { int8_t loc; uint32_t bytes; int8_t count_loc; };
// loc: 0..15 stack argument i, 100 ecx, 101 edx
const std::unordered_map<std::string_view, std::vector<PtrArg>>& PointerArgs() {
    static const std::unordered_map<std::string_view, std::vector<PtrArg>> k = {
        {"hle_D3DDevice_SetTransform", {{1, 64, -1}}},
        {"hle_D3DDevice_SetViewport", {{0, 24, -1}}},
        {"hle_D3DDevice_SetMaterial", {{0, 68, -1}}},
        {"hle_D3DDevice_SetLight", {{1, 104, -1}}},
        {"hle_D3DDevice_SetScissors", {{2, 16, 0}}},
        {"hle_D3DDevice_SetPixelShaderConstant", {{1, 16, 2}}},
        {"hle_D3DDevice_SetVertexShaderConstant1", {{101, 16, -1}}},
        {"hle_D3DDevice_SetVertexShaderConstant1Fast", {{101, 16, -1}}},
        {"hle_D3DDevice_SetVertexShaderConstant4", {{101, 64, -1}}},
        {"hle_D3DDevice_SetVertexShaderConstantNotInlineFast", {{101, 4, 0}}},  // count in dwords
    };
    return k;
}
bool Recordable(std::string_view n) {
    if (n.rfind("hle_D3DDevice_", 0) != 0) return false;
    n.remove_prefix(14);
    static const std::unordered_set<std::string_view> no = {
        "SetGammaRamp", "SetFlickerFilter", "SetSoftDisplayFilter", "SetRenderTarget", "SetRenderTargetFast",
        "SetTile", "BeginPush", "EndPush", "MakeSpace"};
    if (no.count(n)) return false;
    return n.rfind("Set", 0) == 0 || n.rfind("Draw", 0) == 0 || n == "Begin" || n == "End" ||
           n == "LoadVertexShader" || n == "LoadVertexShaderProgram" || n == "SelectVertexShader" ||
           n == "SwitchTexture" || n == "LightEnable" || n == "RunPushBuffer" || n == "InsertCallback";
}
uint32_t ArgAt(const X86Context& c, int loc) {
    if (loc == 100) return c.ecx;
    if (loc == 101) return c.edx;
    return R32(c.esp + 4 + 4 * uint32_t(loc));
}
void SetArgAt(X86Context& c, std::vector<uint32_t>& stack, int loc, uint32_t v) {
    if (loc == 100) c.ecx = v;
    else if (loc == 101) c.edx = v;
    else stack[1 + loc] = v;
}

bool Record(X86Context& ctx, uint8_t*, const char* name, X86Func* self, int pops) {
    if (g_replaying || !g_recording || ctx.thread_handle != g_rec_thread || !Recordable(name)) return false;
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    Captured c;
    c.fn = self;
    c.name = name;
    c.regs = ctx;
    // the HLE reads at most 16 arguments; a cdecl or register call pops none but may read the stack
    int n = std::max(pops, 8);
    for (int i = 0; i <= n; i++) c.stack.push_back(R32(ctx.esp + 4 * uint32_t(i)));
    auto it = PointerArgs().find(name);
    if (it != PointerArgs().end())
        for (const PtrArg& p : it->second) {
            uint32_t src = ArgAt(ctx, p.loc);
            if (!src) continue;
            uint32_t count = p.count_loc < 0 ? 1 : ArgAt(ctx, p.count_loc);
            uint32_t bytes = p.bytes * std::max<uint32_t>(count, 1);
            uint32_t dst = mem::AllocSystem(bytes);
            std::memcpy(HostPtr(dst), HostPtr(src), bytes);
            SetArgAt(c.regs, c.stack, p.loc, dst);
            c.copies.push_back(dst);
        }
    if (std::string_view(name) == "hle_D3DDevice_DrawVerticesUP") {
        // (type, vertex count, data, stride)
        uint32_t bytes = ArgAt(ctx, 1) * ArgAt(ctx, 3), src = ArgAt(ctx, 2);
        if (src && bytes) {
            uint32_t dst = mem::AllocSystem(bytes);
            std::memcpy(HostPtr(dst), HostPtr(src), bytes);
            SetArgAt(c.regs, c.stack, 2, dst);
            c.copies.push_back(dst);
        }
    }
    uint32_t index;
    if (!g_captured_free.empty()) {
        index = g_captured_free.back();
        g_captured_free.pop_back();
        g_captured[index] = std::move(c);
    } else {
        index = uint32_t(g_captured.size());
        g_captured.push_back(std::move(c));
    }
    g_captured_of[g_recording].push_back(index);
    if (static size_t logged = 0; g_captured.size() >= logged * 2 && g_captured.size() >= 4096) {
        logged = g_captured.size();
        LOGI("d3d: %zu captured calls kept (%zu free) for %zu push buffers", g_captured.size(), g_captured_free.size(),
             g_captured_of.size());
    }
    // the marker: a two dword no-operation method, magic then index
    uint32_t p = Dev(kDevPush);
    W32(p, (2u << 18) | kNopMethod);
    W32(p + 4, kMarkerMagic);
    W32(p + 8, index);
    SetDev(kDevPush, p + 12);
    LOG_FIRST(32, "d3d: push buffer %08X captures %s", g_recording, name + 4);
    ::kernel::Return(ctx, 0, pops);
    return true;
}

void Replay(X86Context& ctx, uint32_t index) {
    if (index >= g_captured.size()) return;
    Captured& c = g_captured[index];
    X86Context r = ctx;
    r.eax = c.regs.eax; r.ecx = c.regs.ecx; r.edx = c.regs.edx; r.ebx = c.regs.ebx;
    r.esi = c.regs.esi; r.edi = c.regs.edi; r.ebp = c.regs.ebp;
    // the arguments go below the current guest stack, as a call would put them
    uint32_t sp = (ctx.esp - 0x200 - 4 * uint32_t(c.stack.size())) & ~15u;
    for (size_t i = 0; i < c.stack.size(); i++) W32(sp + 4 * uint32_t(i), c.stack[i]);
    r.esp = sp;
    c.fn(r, ::mem::Base());
    ctx.eax = r.eax;
}

// Runs commands from begin to stop, following jumps. Markers replay captured calls, other methods run as EndPush runs
// them. Returns false when the stream stops being commands.
void Execute(X86Context& ctx, uint32_t begin, uint32_t stop) {
    constexpr uint32_t kBeginEnd = 0x17FC, kInlineArray = 0x1818;
    constexpr uint32_t kArrayOffsets = 0x1720, kArrayFormatsEnd = 0x17A0, kArrayDraws = 0x1800;
    static std::vector<uint32_t> vertices;
    static ArrayDraws arrays;
    static RawArrays raw;
    raw.valid = false;
    uint32_t prim = 0;
    uint32_t p = begin, steps = 0;
    while (p && p != stop) {
        if (++steps > (1u << 22)) {
            LOG_FIRST(8, "d3d: push buffer walk from %08X did not reach %08X", begin, stop);
            return;
        }
        uint32_t head = R32(p);
        if ((head & 3) == 1 || (head & 0xE0000003) == 0x20000000) {  // jump, old jump: to a physical address
            uint32_t target = (head & 3) == 1 ? head & 0xFFFFFFFC : head & 0x1FFFFFFC;
            p = mem::VirtualFromPhysical(target);
            continue;
        }
        if ((head & 3) == 2 || head == 0x00020000) {
            LOG_FIRST(8, "d3d: push buffer call or return %08X at %08X is not followed", head, p);
            return;
        }
        uint32_t method = head & 0x1FFC, n = (head >> 18) & 0x7FF;
        if (head & 0xA0030003) {
            LOG_FIRST(8, "d3d: push buffer commands stop at %08X (%08X)", p, head);
            return;
        }
        p += 4;
        if (method == kNopMethod && n == 2 && R32(p) == kMarkerMagic) {
            Replay(ctx, R32(p + 4));
            p += 8;
            continue;
        }
        for (uint32_t i = 0; i < n; i++, p += 4) {
            uint32_t m = head & 0x40000000 ? method : method + i * 4, v = R32(p);
            ApplyVisibilityMethod(m, v);
            if (m == kNopMethod) continue;
            if (m == kInlineArray) {
                if (prim) vertices.push_back(v);
            } else if (m == kBeginEnd) {
                if (!v && prim && !(raw.valid && raw.DrawInline(prim, vertices))) DrawInline(prim, vertices);
                if (!v) arrays.Flush(prim, raw);
                prim = v;
                vertices.clear();
                ApplyImmediate(m, v);
            } else if (prim && arrays.Add(m, v)) {
            } else if (raw.Method(m, v)) {
            } else if ((m >= kArrayOffsets && m < kArrayFormatsEnd) || (m >= kArrayDraws && m < kInlineArray)) {
                LOG_FIRST(16, "d3d: pushed commands draw from vertex arrays (method %04X = %08X), which is not run", m, v);
            } else if (!ApplyConstant(m, v) && !ApplyImmediate(m, v) && m >> 2 < g_state.simple.size()) {
                SetSimple(m, v);
            }
        }
    }
}
}  // namespace

// breakdown-recomp patch: (type, callback, context). The library pushes a method the GPU raises an interrupt for once
// the commands before it are done, and the interrupt calls callback(context). The host draws the commands as they
// come, so the callback runs here; in a recording push buffer it is captured and runs when the buffer runs.
// Breakdown counts frames in flight down in its callback 0x18E870 and waits for the count in its main loop.
HLE_STDCALL(D3DDevice_InsertCallback, 3) {
    Walk(Dev(kDevPush));
    LOG_FIRST(4, "d3d: InsertCallback type %u callback %08X context %08X", a[0], a[1], a[2]);
    if (a[1]) ::kernel::CallGuest(ctx, a[1], {a[2]});
    return 0;
}

// (push buffer): later pushes and the D3D calls the host implements go into it
HLE_STDCALL(D3DDevice_BeginPushBuffer, 1) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    uint32_t pb = a[0], data = R32(pb + 4), alloc = R32(pb + 16);
    if (g_recording) LOG_FIRST(4, "d3d: BeginPushBuffer %08X while %08X records", pb, g_recording);
    Walk(Dev(kDevPush));
    // the buffer records again: what it captured before is dropped
    if (auto it = g_captured_of.find(pb); it != g_captured_of.end()) {
        for (uint32_t i : it->second) {
            for (uint32_t c : g_captured[i].copies) mem::FreeSystem(c);
            g_captured[i] = Captured{};
            g_captured_free.push_back(i);
        }
        it->second.clear();
    }
    g_saved_push = Dev(kDevPush);
    g_saved_threshold = Dev(kDevThreshold);
    W32(pb, R32(pb) + 0x80000);  // the device holds it, as the library's does
    SetDev(kDevPush, data);
    SetDev(kDevThreshold, data + alloc - 0x204);
    g_recording = pb;
    g_rec_thread = ctx.thread_handle;
    ::hle::g_recorder = Record;
    LOG_FIRST(8, "d3d: BeginPushBuffer %08X data %08X allocation %u", pb, data, alloc);
    return 0;
}

// the size counts the commands and one dword after them, which RunPushBuffer leaves for its jump back
HLE_STDCALL(D3DDevice_EndPushBuffer, 0) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    uint32_t pb = g_recording;
    if (!pb) return 0;
    uint32_t data = R32(pb + 4), used = Dev(kDevPush) - data;
    W32(pb + 12, used + 4);
    W32(pb, R32(pb) - 0x80000);
    g_recording = 0;
    ::hle::g_recorder = nullptr;
    SetDev(kDevPush, g_saved_push);
    SetDev(kDevThreshold, g_saved_threshold);
    g_walk = g_saved_push;
    LOG_FIRST(8, "d3d: EndPushBuffer %08X, %u bytes, %zu calls captured", pb, used + 4,
              g_captured_of.count(pb) ? g_captured_of[pb].size() : size_t(0));
    if (used + 4 > R32(pb + 16)) return 0x88760829;  // D3DERR_BUFFERTOOSMALL, as the library's
    return 0;
}

// (push buffer, offset out): how far the recording has got
HLE_STDCALL(D3DDevice_GetPushBufferOffset, 1) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    W32(a[0], g_recording ? Dev(kDevPush) - R32(g_recording + 4) : 0);
    return 0;
}

// (push buffer, fixup): the commands from Data to the size's last dword, with the jumps a title wrote into them
HLE_STDCALL(D3DDevice_RunPushBuffer, 2) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    uint32_t data = R32(a[0] + 4), size = R32(a[0] + 12);
    LOG_FIRST(8, "d3d: RunPushBuffer %08X, %u bytes, first command %08X", data, size, size ? R32(data) : 0);
    if (a[1]) LOG_FIRST(1, "d3d: RunPushBuffer fixups are not applied");
    Walk(Dev(kDevPush));
    if (size < 4) return 0;
    bool was = g_replaying;
    g_replaying = true;
    Execute(ctx, data, data + size - 4);
    g_replaying = was;
    return 0;
}
// ---- end of breakdown-recomp patch ---------------------------------------------------------------------------------

// frame buffer compression tiles are the console's memory controller's business
HLE_STDCALL(D3DDevice_SetTile, 2) { return 0; }

// D3DTILE: flags, memory, size, pitch, z start tag, z offset
constexpr uint32_t kTileMemory = 4;
constexpr uint32_t kTileSize = 8;
constexpr uint32_t kTilePitch = 12;
constexpr uint32_t kTileBytes = 24;
// tiles the library's CreateDevice sets
constexpr uint32_t kBackBufferTile = 0;
constexpr uint32_t kDepthTile = 1;
// surface header fields
constexpr uint32_t kSurfaceData = 4;
constexpr uint32_t kSurfaceSize = 16;

// (index, tile): the frame buffer memory a title wraps in its own textures, as 007 does for a depth texture
HLE_STDCALL(D3DDevice_GetTile, 2) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    uint32_t tile = a[1];
    std::memset(HostPtr(tile), 0, kTileBytes);
    uint32_t surface = 0;
    if (a[0] == kBackBufferTile) surface = g_buffers[1];
    if (a[0] == kDepthTile) surface = g_depth;
    if (!surface) return 0;
    uint32_t size = R32(surface + kSurfaceSize);
    uint32_t pitch = ((size >> 24 & 0xFF) + 1) * 64;
    uint32_t height = (size >> 12 & 0xFFF) + 1;
    W32(tile + kTileMemory, mem::VirtualFromPhysical(R32(surface + kSurfaceData)));
    W32(tile + kTileSize, pitch * height);
    W32(tile + kTilePitch, pitch);
    return 0;
}

// the library's Begin pushes the start of the primitive, here it is opened
HLE_STDCALL(D3DDevice_Begin, 1) {
    XDK_REQUIRE("d3d", XDK_KEY(CDevice_Flags));
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    Walk(Dev(kDevPush));
    g_im_prim = a[0];
    g_im_mask = 0;
    g_im_vertices.clear();
    SetDev(XDK_CDevice_Flags, Dev(XDK_CDevice_Flags) | 0x800);
    return 0;
}
// the library's End pushes the end of the primitive, here it is drawn
HLE_STDCALL(D3DDevice_End, 0) {
    XDK_REQUIRE("d3d", XDK_KEY(CDevice_Flags));
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    Walk(Dev(kDevPush));
    EndPrimitive();
    SetDev(XDK_CDevice_Flags, Dev(XDK_CDevice_Flags) & ~0x1800u);
    return 0;
}

// breakdown-recomp patch: for title hooks that replace a spin on a count the title's vertical blank callback keeps
namespace d3d {
uint64_t BlanksDone() {
    std::lock_guard<std::mutex> blank(g_blank_mutex);
    return g_blanks_done;
}
bool WaitBlankDone(uint64_t seen, uint32_t timeout_ms) {
    if (!g_blank_thread) return false;
    std::unique_lock<std::mutex> blank(g_blank_mutex);
    return g_blank_cv.wait_for(blank, std::chrono::milliseconds(timeout_ms), [&] { return g_blanks_done != seen; });
}
}  // namespace d3d
