// Button prompts for the keyboard and mouse. With no controller connected, the game's A, B, Y, Start and Back icons
// (the textures its menus and popups draw beside "Select", "Back", "Continue", ...) are drawn as PC icons instead: A
// and B as a mouse with its left or right button lit (in menus the left button selects and the right goes back,
// src/common/keyboard_pad.cpp), the others as a keycap with the key bound to them. They are made here, in the port's
// dark steel look with each button's own colour as an accent, and handed to the renderer through the texture pack's
// provider (gpu/texturedumper.h) for the textures' keys; plugging a controller in brings the game's icons back.
// The keys are the textures' names in a texture dump (textures\dumps\32x32_<key>_*.png).
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <objidl.h>  // IStream, which GDI+ needs and the lean Windows header leaves out

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace Gdiplus {
using std::max;
using std::min;
}  // namespace Gdiplus
#include <gdiplus.h>

#include "core/input.h"
#include "core/log.h"
#include "app/app.h"
#include "gpu/prompts.h"
#include "gpu/texturedumper.h"

#pragma comment(lib, "gdiplus.lib")

namespace mechassault {
uint8_t BoundVk(int control);  // src/common/keyboard_pad.cpp
std::string BoundName(int control);
}  // namespace mechassault

namespace {

namespace tex = gpu::texturedumper;

enum class Kind { MouseLeft, MouseRight, Key };

struct Prompt {
    uint64_t key;    // the game's texture
    int control;     // keyboard_pad's control index
    Kind kind;
    COLORREF accent;
};
constexpr Prompt kPrompts[] = {
    {0x97bdae0a07e49873ull, 0, Kind::MouseLeft, RGB(52, 190, 66)},    // A, green
    {0x53bd6254fd773d0full, 1, Kind::MouseRight, RGB(214, 40, 48)},   // B, red
    {0xcab926a0e28d44f3ull, 3, Kind::Key, RGB(232, 180, 44)},         // Y, yellow
    {0xd768c661f535f683ull, 8, Kind::Key, RGB(200, 200, 206)},        // Start
    {0x95e58e0a5a9c1a44ull, 9, Kind::Key, RGB(200, 200, 206)},        // Back
};

std::mutex g_lock;
bool g_pc = false;                        // drawing PC icons (no controller)
std::map<uint64_t, tex::Image> g_images;  // made for the current bindings

// what a keycap says: short and in capitals
std::wstring Label(const std::string& name) {
    static const std::map<std::string, std::string> kShort = {
        {"Enter", "ENTER"}, {"Escape", "ESC"}, {"Backspace", "BKSP"}, {"Space", "SPACE"}, {"LShift", "SHIFT"},
        {"RShift", "SHIFT"}, {"LCtrl", "CTRL"}, {"RCtrl", "CTRL"}, {"LAlt", "ALT"}, {"RAlt", "ALT"},
        {"Tab", "TAB"}, {"CapsLock", "CAPS"}, {"PageUp", "PGUP"}, {"PageDown", "PGDN"}, {"Delete", "DEL"},
        {"Insert", "INS"}, {"Mouse3", "MMB"}, {"Mouse4", "M4"}, {"Mouse5", "M5"}, {"WheelUp", "WHL"},
        {"WheelDown", "WHL"}, {"Left", "←"}, {"Right", "→"}, {"Up", "↑"}, {"Down", "↓"},
    };
    std::string s = name;
    if (auto it = kShort.find(name); it != kShort.end()) s = it->second;
    if (s.size() > 5) s.resize(5);
    std::wstring w;
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()), nullptr, 0);
    w.resize(size_t(n));
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()), w.data(), n);
    for (wchar_t& c : w) c = wchar_t(towupper(c));
    return w;
}

Gdiplus::Color Col(COLORREF c, BYTE a = 255) { return Gdiplus::Color(a, GetRValue(c), GetGValue(c), GetBValue(c)); }

void RoundRect(Gdiplus::GraphicsPath& p, float x, float y, float w, float h, float r) {
    float d = r * 2;
    p.AddArc(x, y, d, d, 180, 90);
    p.AddArc(x + w - d, y, d, d, 270, 90);
    p.AddArc(x + w - d, y + h - d, d, d, 0, 90);
    p.AddArc(x, y + h - d, d, d, 90, 90);
    p.CloseFigure();
}

// 128x128, then its mip levels: the icons are drawn at 20 to 60 pixels
constexpr int kSize = 128;

void DrawKey(Gdiplus::Graphics& g, const std::wstring& text, COLORREF accent) {
    using namespace Gdiplus;
    GraphicsPath shadow, body;
    RoundRect(shadow, 10, 14, 112, 108, 20);
    RoundRect(body, 6, 6, 112, 108, 20);
    SolidBrush shade(Color(110, 0, 0, 0));
    g.FillPath(&shade, &shadow);
    LinearGradientBrush fill(Point(0, 6), Point(0, 114), Color(255, 62, 63, 70), Color(255, 18, 18, 22));
    g.FillPath(&fill, &body);
    // the key's lower edge in the button's own colour
    GraphicsPath lip;
    RoundRect(lip, 14, 94, 96, 12, 6);
    SolidBrush accent_brush(Col(accent, 235));
    g.FillPath(&accent_brush, &lip);
    Pen rim(Color(255, 214, 214, 220), 5);
    g.DrawPath(&rim, &body);
    Pen inner(Color(90, 255, 255, 255), 2);
    GraphicsPath in;
    RoundRect(in, 12, 12, 100, 96, 15);
    g.DrawPath(&inner, &in);
    float size = text.size() <= 1 ? 62.0f : text.size() == 2 ? 50.0f : text.size() == 3 ? 40.0f : text.size() == 4 ? 31.0f : 26.0f;
    FontFamily family(L"Bahnschrift");
    FontFamily fallback(L"Arial");
    Font font(family.IsAvailable() ? &family : &fallback, size, FontStyleBold, UnitPixel);
    StringFormat fmt;
    fmt.SetAlignment(StringAlignmentCenter);
    fmt.SetLineAlignment(StringAlignmentCenter);
    SolidBrush white(Color(255, 245, 245, 248));
    g.DrawString(text.c_str(), int(text.size()), &font, RectF(6, 8, 112, 86), &fmt, &white);
}

void DrawMouse(Gdiplus::Graphics& g, bool left, COLORREF accent) {
    using namespace Gdiplus;
    GraphicsPath shadow, body;
    RoundRect(shadow, 30, 12, 72, 112, 34);
    RoundRect(body, 26, 6, 72, 112, 34);
    SolidBrush shade(Color(110, 0, 0, 0));
    g.FillPath(&shade, &shadow);
    LinearGradientBrush fill(Point(0, 6), Point(0, 118), Color(255, 62, 63, 70), Color(255, 18, 18, 22));
    g.FillPath(&fill, &body);
    // the pressed button: its quarter of the body, in the button's own colour
    Region button(&body);
    RectF half(left ? 26.0f : 62.0f, 6.0f, 36.0f, 46.0f);
    button.Intersect(half);
    SolidBrush lit(Col(accent, 240));
    g.FillRegion(&lit, &button);
    Pen line(Color(255, 214, 214, 220), 4);
    g.DrawLine(&line, 62.0f, 8.0f, 62.0f, 52.0f);
    g.DrawLine(&line, 28.0f, 52.0f, 96.0f, 52.0f);
    Pen rim(Color(255, 214, 214, 220), 5);
    g.DrawPath(&rim, &body);
    // the wheel
    SolidBrush wheel(Color(255, 214, 214, 220));
    g.FillRectangle(&wheel, RectF(58.0f, 18.0f, 8.0f, 20.0f));
}

tex::Image Make(const Prompt& p) {
    using namespace Gdiplus;
    Bitmap bmp(kSize, kSize, PixelFormat32bppARGB);
    {
        Graphics g(&bmp);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
        g.Clear(Color(0, 0, 0, 0));
        if (p.kind == Kind::Key) DrawKey(g, Label(mechassault::BoundName(p.control)), p.accent);
        else DrawMouse(g, p.kind == Kind::MouseLeft, p.accent);
    }
    tex::Image img;
    img.format = DXGI_FORMAT_B8G8R8A8_UNORM;
    img.width = img.height = kSize;
    tex::Level top;
    top.pitch = kSize * 4;
    top.px.resize(size_t(kSize) * kSize * 4);
    BitmapData data;
    Gdiplus::Rect all(0, 0, kSize, kSize);
    if (bmp.LockBits(&all, ImageLockModeRead, PixelFormat32bppARGB, &data) == Ok) {
        for (int y = 0; y < kSize; y++)
            std::memcpy(top.px.data() + size_t(y) * top.pitch, static_cast<uint8_t*>(data.Scan0) + y * data.Stride,
                        top.pitch);
        bmp.UnlockBits(&data);
    }
    img.levels.push_back(std::move(top));
    // mips, averaged with the colour weighted by alpha so the edges do not darken
    for (uint32_t w = kSize / 2; w >= 1; w /= 2) {
        const tex::Level& up = img.levels.back();
        tex::Level l;
        l.pitch = w * 4;
        l.px.resize(size_t(w) * w * 4);
        for (uint32_t y = 0; y < w; y++)
            for (uint32_t x = 0; x < w; x++) {
                float c[3] = {}, a = 0;
                for (uint32_t k = 0; k < 4; k++) {
                    const uint8_t* s = up.px.data() + size_t(y * 2 + k / 2) * up.pitch + size_t(x * 2 + k % 2) * 4;
                    float sa = s[3] / 255.0f;
                    for (int i = 0; i < 3; i++) c[i] += s[i] * sa;
                    a += sa;
                }
                uint8_t* d = l.px.data() + size_t(y) * l.pitch + size_t(x) * 4;
                for (int i = 0; i < 3; i++) d[i] = uint8_t(a > 0 ? std::lround(c[i] / a) : 0);
                d[3] = uint8_t(std::lround(a / 4 * 255));
            }
        img.levels.push_back(std::move(l));
        if (w == 1) break;
    }
    return img;
}

bool Provide(uint64_t key, tex::Image& out) {
    std::lock_guard lock(g_lock);
    if (!g_pc) return false;
    auto it = g_images.find(key);
    if (it == g_images.end()) return false;
    out = it->second;  // a copy: the renderer takes it, and asks again after an Invalidate
    return true;
}

}  // namespace

namespace mechassault {

// every pad poll (game_info.cpp): the icons follow a controller being plugged in or out and the bindings
void UpdatePrompts() {
    static bool started = false;
    static ULONG_PTR gdiplus = 0;
    static uint64_t last = ~0ull;
    bool pc = !input::AnyController();
    uint64_t state = pc ? 1 : 0;
    for (const Prompt& p : kPrompts) state = state * 1315423911ull + BoundVk(p.control);
    if (state == last) return;
    last = state;
    if (!started) {
        Gdiplus::GdiplusStartupInput in;
        started = Gdiplus::GdiplusStartup(&gdiplus, &in, nullptr) == Gdiplus::Ok;
        gpu::texturedumper::SetProvider(&Provide);
    }
    std::map<uint64_t, tex::Image> images;
    if (pc && started)
        for (const Prompt& p : kPrompts) images[p.key] = Make(p);
    {
        std::lock_guard lock(g_lock);
        g_pc = pc && started;
        g_images = std::move(images);
    }
    gpu::texturedumper::Invalidate();
    LOGI("prompts: %s", pc ? "keyboard and mouse icons" : "controller icons");
}

// the control of the prompt under the cursor (its icon, or the label beside it), -1 for none: a click there presses
// that button (keyboard_pad.cpp)
int PromptUnderCursor() {
    HWND hwnd = static_cast<HWND>(app::Window());
    POINT pt;
    RECT client;
    if (!hwnd || !GetCursorPos(&pt) || !ScreenToClient(hwnd, &pt) || !GetClientRect(hwnd, &client)) return -1;
    gpu::PromptFrame f = gpu::Prompts();
    if (f.w <= 0 || f.h <= 0 || client.right <= 0 || client.bottom <= 0) return -1;
    float bx = float(pt.x) * f.bw / float(client.right), by = float(pt.y) * f.bh / float(client.bottom);
    float nx = (bx - f.x) / f.w, ny = (by - f.y) / f.h;
    auto control = [](uint64_t key) {
        for (const Prompt& p : kPrompts)
            if (p.key == key) return p.control;
        return -1;
    };
    // the icons themselves first, then the labels to their right: about four icons wide, up to the next icon
    for (const gpu::PromptRect& r : f.rects) {
        float pad = (r.y1 - r.y0) * 0.25f;
        if (nx >= r.x0 - pad && nx <= r.x1 + pad && ny >= r.y0 - pad && ny <= r.y1 + pad) return control(r.key);
    }
    for (const gpu::PromptRect& r : f.rects) {
        float end = r.x1 + (r.x1 - r.x0) * 4;
        for (const gpu::PromptRect& o : f.rects)
            if (&o != &r && o.x0 > r.x1 && o.y0 < r.y1 && o.y1 > r.y0) end = std::min(end, o.x0);
        if (nx > r.x1 && nx < end && ny >= r.y0 && ny <= r.y1) return control(r.key);
    }
    return -1;
}

}  // namespace mechassault
