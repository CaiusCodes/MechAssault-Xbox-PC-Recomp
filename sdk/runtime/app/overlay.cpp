// breakdown-recomp patch: the PC overlay every title gets: FPS counter, frame timing diagnostics and the settings
// menu (F1). Only settings the runtime applies are offered; each applies at once and is saved to the settings file.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include <imgui.h>
#include <imgui_internal.h>  // NavDisableMouseHover, RenderNavHighlight

#include "app/app.h"
#include "app/overlay.h"
#include "core/frame_stats.h"
#include "core/input.h"
#include "core/log.h"
#include "gpu/widescreen.h"
#include "app/update.h"
#ifdef MECHASSAULT_BUILD_INFO
#include "build_info.h"
#endif

#pragma comment(lib, "psapi.lib")

namespace app {

namespace {

std::atomic<bool> g_menu{false};
bool g_menu_drawn = false;  // the menu window was up last frame (render thread)

struct Resolution {
    uint32_t w, h;
    bool operator==(const Resolution&) const = default;
};

// the chosen monitor's modes, largest first, 640x480 and up (mechassault-recomp patch: ChosenMonitor, so a display
// just picked lists its own before the window has moved there)
std::vector<Resolution> Resolutions() {
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(static_cast<HMONITOR>(ChosenMonitor()), &mi);
    std::vector<Resolution> out;
    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    for (DWORD i = 0; EnumDisplaySettingsW(mi.szDevice, i, &dm); i++) {
        Resolution r{dm.dmPelsWidth, dm.dmPelsHeight};
        if (r.w >= 640 && r.h >= 480 && std::find(out.begin(), out.end(), r) == out.end()) out.push_back(r);
    }
    std::sort(out.begin(), out.end(), [](const Resolution& a, const Resolution& b) {
        return a.w != b.w ? a.w > b.w : a.h > b.h;
    });
    return out;
}

// breakdown-recomp patch: the refresh rates the monitor has at this size, highest first
std::vector<uint32_t> RefreshRates(uint32_t w, uint32_t h) {
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(static_cast<HMONITOR>(ChosenMonitor()), &mi);
    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    if (!w || !h) {
        EnumDisplaySettingsW(mi.szDevice, ENUM_REGISTRY_SETTINGS, &dm);
        w = dm.dmPelsWidth;
        h = dm.dmPelsHeight;
    }
    std::vector<uint32_t> out;
    for (DWORD i = 0; EnumDisplaySettingsW(mi.szDevice, i, &dm); i++)
        if (dm.dmPelsWidth == w && dm.dmPelsHeight == h && dm.dmDisplayFrequency > 1 &&
            std::find(out.begin(), out.end(), dm.dmDisplayFrequency) == out.end())
            out.push_back(dm.dmDisplayFrequency);
    std::sort(out.rbegin(), out.rend());
    return out;
}

// a whole multiple of 60 Hz (59 and 119 Hz modes too): every 60 fps frame shows for the same time
bool EvenFor60(uint32_t hz) {
    for (uint32_t k = 1; k <= 4; k++)
        if (hz + 1 >= 60 * k && hz <= 60 * k + 1) return true;
    return false;
}

uint32_t RefreshHz() {
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    if (GetMonitorInfoW(MonitorFromWindow(static_cast<HWND>(Window()), MONITOR_DEFAULTTOPRIMARY), &mi) &&
        EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm))
        return dm.dmDisplayFrequency;
    return 0;
}

// process CPU use as a share of the whole machine, and memory, sampled twice a second
struct ProcessUse {
    double cpu_percent = 0;  // of all logical processors
    double cpu_cores = 0;    // the same as busy cores
    double working_mb = 0, private_mb = 0;
};
ProcessUse SampleProcess() {
    static ProcessUse use;
    static ULONGLONG last_wall = 0, last_cpu = 0;
    static auto next = std::chrono::steady_clock::now();
    auto now = std::chrono::steady_clock::now();
    if (now < next) return use;
    next = now + std::chrono::milliseconds(500);
    FILETIME c, e, k, u, wall;
    GetSystemTimeAsFileTime(&wall);
    if (GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u)) {
        auto q = [](FILETIME f) { return (ULONGLONG(f.dwHighDateTime) << 32) | f.dwLowDateTime; };
        ULONGLONG cpu = q(k) + q(u), w = q(wall);
        if (last_wall && w > last_wall) {
            SYSTEM_INFO si;
            GetSystemInfo(&si);
            use.cpu_cores = double(cpu - last_cpu) / double(w - last_wall);
            use.cpu_percent = 100.0 * use.cpu_cores / std::max<DWORD>(si.dwNumberOfProcessors, 1);
        }
        last_wall = w;
        last_cpu = cpu;
    }
    PROCESS_MEMORY_COUNTERS_EX pm{};
    pm.cb = sizeof(pm);
    if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pm), sizeof(pm))) {
        use.working_mb = pm.WorkingSetSize / 1048576.0;
        use.private_mb = pm.PrivateUsage / 1048576.0;
    }
    return use;
}

void Changed(bool display = false) {
    Save();
    if (display) ApplyDisplay();
}

// mechassault-recomp patch: a new display mode, resolution or refresh rate asks to be kept, and goes back to the
// previous one after 15 seconds without an answer (a mode the display cannot show leaves nothing to click)
struct Revert {
    bool active = false;
    DisplayMode mode{};
    uint32_t width = 0, height = 0, refresh_hz = 0;
    std::chrono::steady_clock::time_point deadline;
};
Revert g_revert;
constexpr int kRevertSeconds = 15;

void DisplayChanged(const Settings& before) {
    if (!g_revert.active) {
        g_revert.active = true;
        g_revert.mode = before.display_mode;
        g_revert.width = before.width;
        g_revert.height = before.height;
        g_revert.refresh_hz = before.refresh_hz;
    }
    g_revert.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(kRevertSeconds);
    Changed(true);
}

void RevertPrompt() {
    if (!g_revert.active) return;
    Settings& s = Get();
    auto left = std::chrono::duration_cast<std::chrono::seconds>(g_revert.deadline - std::chrono::steady_clock::now());
    bool revert = left.count() < 0, keep = false;
    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.2f), ImGuiCond_Always,
                            ImVec2(0.5f, 0.5f));
    if (ImGui::Begin("Keep these display settings?", nullptr,
                     ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
                         ImGuiWindowFlags_NoMove)) {
        ImGui::Text("Going back to the previous ones in %lld s.", (long long)std::max<int64_t>(left.count() + 1, 0));
        if (ImGui::Button("Keep")) keep = true;
        ImGui::SameLine();
        if (ImGui::Button("Revert now")) revert = true;
    }
    ImGui::End();
    if (keep) {
        g_revert.active = false;
        LOGR("display: new settings kept");
    } else if (revert) {
        s.display_mode = g_revert.mode;
        s.width = g_revert.width;
        s.height = g_revert.height;
        s.refresh_hz = g_revert.refresh_hz;
        g_revert.active = false;
        LOGR("display: reverted to the previous settings");
        Changed(true);
    }
}

void FpsCounter(bool detailed) {
    framestats::Summary s = framestats::Title().Summarize(120);
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                             ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                             ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs;
    ImGui::SetNextWindowPos(ImVec2(8, 8), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.45f);
    if (!ImGui::Begin("##fps", nullptr, flags)) {
        ImGui::End();
        return;
    }
    ImGui::Text("%.0f FPS  %.1f ms", s.avg_fps, s.avg_ms);
    if (detailed) {
        framestats::Summary l = framestats::Title().Summarize();  // the whole history, about 17-34 s
        ProcessUse p = SampleProcess();
        ImGui::Separator();
        ImGui::Text("1%% low %.1f FPS   p99 %.1f ms   max %.1f ms", l.low1_fps, l.p99_ms, l.max_ms);
        ImGui::Text("stalls %u of %u frames   jitter %.2f ms", l.stalls, l.frames, l.jitter_ms);
        ImGui::Text("present %.2f ms   limiter %.2f ms", s.present_ms, s.pace_ms);
        static float graph[240];
        size_t n = framestats::Title().Intervals(graph, 240);
        float top = 50.0f;  // ms; taller when a frame took longer
        for (size_t i = 0; i < n; i++) top = std::max(top, graph[i] * 1.1f);
        char label[64];
        std::snprintf(label, sizeof(label), "frame time, last %zu frames (0-%.0f ms)", n, top);
        ImGui::PlotLines("##ft", graph, int(n), 0, label, 0.0f, top,
                         ImVec2(360 * ImGui::GetIO().FontGlobalScale, 70 * ImGui::GetIO().FontGlobalScale));
        ImGui::Text("CPU %.1f%% (%.2f cores)   memory %.0f MB (private %.0f MB)", p.cpu_percent, p.cpu_cores,
                    p.working_mb, p.private_mb);
        Mode out = Output();
        float scale = RenderScale(480);
        ImGui::Text("output %ux%u @ %u Hz  vsync %s  render %.0fx%.0f", out.width, out.height, RefreshHz(),
                    Get().vsync ? "on" : "off", 640 * scale, 480 * scale);
    }
    ImGui::End();
}

// breakdown-recomp patch: every controller drives the menu (SDL, so PlayStation and Switch pads too; the Win32
// backend's own XInput reading is compiled out)
void FeedControllers(const input::MenuPad& m) {
    ImGuiIO& io = ImGui::GetIO();
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    constexpr std::pair<ImGuiKey, uint32_t> kKeys[] = {
        {ImGuiKey_GamepadFaceDown, input::kMenuA},  {ImGuiKey_GamepadFaceRight, input::kMenuB},
        {ImGuiKey_GamepadFaceLeft, input::kMenuX},  {ImGuiKey_GamepadFaceUp, input::kMenuY},
        {ImGuiKey_GamepadDpadUp, input::kMenuUp},   {ImGuiKey_GamepadDpadDown, input::kMenuDown},
        {ImGuiKey_GamepadDpadLeft, input::kMenuLeft}, {ImGuiKey_GamepadDpadRight, input::kMenuRight},
        {ImGuiKey_GamepadL1, input::kMenuLB},       {ImGuiKey_GamepadR1, input::kMenuRB}};
    for (auto [key, bit] : kKeys) io.AddKeyEvent(key, (m.buttons & bit) != 0);
    auto stick = [&](ImGuiKey key, float v) { io.AddKeyAnalogEvent(key, v > 0.3f, std::clamp(v, 0.0f, 1.0f)); };
    stick(ImGuiKey_GamepadLStickLeft, -m.lx);
    stick(ImGuiKey_GamepadLStickRight, m.lx);
    stick(ImGuiKey_GamepadLStickUp, m.ly);
    stick(ImGuiKey_GamepadLStickDown, -m.ly);
}

// mechassault-recomp patch: the F1 menu in the Breakdown port's look (j, 2026-11-24): a black panel down the left with
// the icon's red, spaced capitals, hexagon page markers, a red band for the open page and a faint honeycomb behind; the
// pages listed on the left with Reset and Close under them; each page a two-column table of names and controls; the
// hovered or selected setting's description in a three-line area under it. Pages: Display, Graphics, Game, Controls
// (the game's, GameInfo::menu through MenuPage) and Network.

// ---- the menu's rows (overlay.h): a page is a table of names and controls; Hint fills the foot area ----
bool g_rows = false;     // a rows table is open
int g_rows_id = 0;       // tables this frame
float g_row_top = 0;     // the current row's top, screen y
std::string g_hint;      // the hovered or selected row's description, this frame
bool g_slider_hovered = false;  // Slider's handle is under the mouse or selected (its value text is the last item)
int g_tab = 0;           // the page shown, kept while the menu is closed

bool MouseMode() { return !ImGui::GetCurrentContext()->NavDisableMouseHover; }

struct Look {
    ImVec4 bg, text, muted, accent, accent_hi;
};
const Look& CurrentLook() {
    auto rgb = [](int r, int g, int b, float a = 1.0f) { return ImVec4(r / 255.0f, g / 255.0f, b / 255.0f, a); };
    // MechAssault's icon and menus: near black, warm greys, the reticle's red
    static const Look kLook = {rgb(13, 9, 10), rgb(238, 232, 230), rgb(140, 120, 118), rgb(196, 30, 40),
                               rgb(236, 74, 60)};
    return kLook;
}

// the menu font: Segoe UI from Windows' fonts loaded large and drawn smaller (FontGlobalScale), so it stays sharp up
// to 4K; ImGui's own font when it is missing. MA_MENU_FONT names another .ttf
constexpr float kFontLoadPx = 34.0f;
constexpr float kFontPx = 17.0f;  // at 720p, scaled with the display's height up to 3x
bool g_font_loaded = false;

void Style(float scale) {
    const Look& L = CurrentLook();
    ImGuiStyle& st = ImGui::GetStyle();
    st = ImGuiStyle();
    st.WindowRounding = 6;
    st.ChildRounding = 0;
    st.FrameRounding = 3;
    st.PopupRounding = 3;
    st.GrabRounding = 3;
    st.TabRounding = 3;
    st.ScrollbarRounding = 3;
    st.WindowBorderSize = 0;
    st.ChildBorderSize = 0;
    st.PopupBorderSize = 1;
    st.FrameBorderSize = 0;
    st.TabBorderSize = 0;
    st.WindowPadding = ImVec2(18, 14);
    st.FramePadding = ImVec2(8, 4);
    st.ItemSpacing = ImVec2(8, 6);
    st.ItemInnerSpacing = ImVec2(6, 4);
    st.CellPadding = ImVec2(4, 5);
    st.ScrollbarSize = 8;
    st.GrabMinSize = 10;
    st.WindowTitleAlign = ImVec2(0.0f, 0.5f);
    st.SelectableTextAlign = ImVec2(0.0f, 0.5f);
    st.ScaleAllSizes(scale);
    auto ink = [](float a) { return ImVec4(1, 1, 1, a); };  // the foreground, faint
    const ImVec4 none = ImVec4(0, 0, 0, 0), opaque_bg = ImVec4(L.bg.x, L.bg.y, L.bg.z, 1);
    ImVec4* c = st.Colors;
    c[ImGuiCol_Text] = L.text;
    c[ImGuiCol_TextDisabled] = L.muted;
    c[ImGuiCol_WindowBg] = L.bg;
    c[ImGuiCol_ChildBg] = none;
    c[ImGuiCol_PopupBg] = ImVec4(opaque_bg.x * 1.4f, opaque_bg.y * 1.4f, opaque_bg.z * 1.4f, 1);
    c[ImGuiCol_Border] = ink(0.10f);
    c[ImGuiCol_BorderShadow] = none;
    c[ImGuiCol_FrameBg] = ink(0.07f);
    c[ImGuiCol_FrameBgHovered] = ink(0.11f);
    c[ImGuiCol_FrameBgActive] = ink(0.15f);
    c[ImGuiCol_TitleBg] = opaque_bg;
    c[ImGuiCol_TitleBgActive] = opaque_bg;
    c[ImGuiCol_TitleBgCollapsed] = opaque_bg;
    c[ImGuiCol_ScrollbarBg] = none;
    c[ImGuiCol_ScrollbarGrab] = ink(0.14f);
    c[ImGuiCol_ScrollbarGrabHovered] = ink(0.22f);
    c[ImGuiCol_ScrollbarGrabActive] = ink(0.30f);
    c[ImGuiCol_CheckMark] = L.accent_hi;
    c[ImGuiCol_SliderGrab] = L.accent;
    c[ImGuiCol_SliderGrabActive] = L.accent_hi;
    c[ImGuiCol_Button] = ink(0.07f);
    c[ImGuiCol_ButtonHovered] = ink(0.13f);
    c[ImGuiCol_ButtonActive] = ink(0.19f);
    c[ImGuiCol_Header] = ink(0.09f);
    c[ImGuiCol_HeaderHovered] = ink(0.13f);
    c[ImGuiCol_HeaderActive] = ink(0.17f);
    c[ImGuiCol_Separator] = ink(0.10f);
    c[ImGuiCol_SeparatorHovered] = ink(0.10f);
    c[ImGuiCol_SeparatorActive] = ink(0.10f);
    c[ImGuiCol_ResizeGrip] = none;
    c[ImGuiCol_ResizeGripHovered] = none;
    c[ImGuiCol_ResizeGripActive] = none;
    c[ImGuiCol_Tab] = none;
    c[ImGuiCol_TabHovered] = ink(0.08f);
    c[ImGuiCol_TabSelected] = ink(0.05f);
    c[ImGuiCol_TabSelectedOverline] = L.accent;
    c[ImGuiCol_TabDimmed] = none;
    c[ImGuiCol_TabDimmedSelected] = ink(0.05f);
    c[ImGuiCol_TabDimmedSelectedOverline] = L.accent;
    c[ImGuiCol_TableHeaderBg] = none;
    c[ImGuiCol_TableBorderStrong] = ink(0.08f);
    c[ImGuiCol_TableBorderLight] = ink(0.05f);
    c[ImGuiCol_TableRowBg] = none;
    c[ImGuiCol_TableRowBgAlt] = ink(0.03f);
    c[ImGuiCol_PlotLines] = L.accent_hi;
    c[ImGuiCol_PlotLinesHovered] = L.accent_hi;
    c[ImGuiCol_TextSelectedBg] = ImVec4(L.accent.x, L.accent.y, L.accent.z, 0.45f);
    c[ImGuiCol_NavHighlight] = L.accent_hi;
    c[ImGuiCol_ModalWindowDimBg] = ImVec4(0, 0, 0, 0.5f);
}

// text drawn letter by letter with extra space between, measured the way it is drawn
float TrackedWidth(const char* text, float size, float tracking) {
    float w = 0;
    ImFont* f = ImGui::GetFont();
    for (const char* p = text; *p; p++) w += f->CalcTextSizeA(size, FLT_MAX, 0, p, p + 1).x + tracking;
    return w > 0 ? w - tracking : 0;
}
void DrawTracked(ImDrawList* d, ImVec2 at, float size, ImU32 col, const char* text, float tracking) {
    ImFont* f = ImGui::GetFont();
    for (const char* p = text; *p; p++) {
        d->AddText(f, size, at, col, p, p + 1);
        at.x += f->CalcTextSizeA(size, FLT_MAX, 0, p, p + 1).x + tracking;
    }
}

// a flat-topped hexagon
void Hexagon(ImDrawList* d, ImVec2 c, float r, ImU32 col, bool filled, float thickness = 1.0f) {
    ImVec2 pts[6];
    for (int i = 0; i < 6; i++) {
        float a = 3.14159265f / 3.0f * float(i);
        pts[i] = ImVec2(c.x + r * std::cos(a), c.y + r * std::sin(a));
    }
    if (filled) d->AddConvexPolyFilled(pts, 6, col);
    else d->AddPolyline(pts, 6, col, ImDrawFlags_Closed, thickness);
}

// the panel's honeycomb, faint, fading out toward its right edge
void Honeycomb(ImDrawList* d, ImVec2 lo, ImVec2 hi, float r, ImVec4 ink) {
    const float w = r * 1.5f, h = r * 1.7320508f;
    d->PushClipRect(lo, hi, true);
    int col = 0;
    for (float x = lo.x; x < hi.x + r; x += w, col++) {
        float fade = 1.0f - std::clamp((x - lo.x) / (hi.x - lo.x), 0.0f, 1.0f);
        ImU32 c = ImGui::GetColorU32(ImVec4(ink.x, ink.y, ink.z, ink.w * (0.35f + 0.65f * fade)));
        for (float y = lo.y + (col & 1 ? h * 0.5f : 0.0f); y < hi.y + h; y += h) Hexagon(d, ImVec2(x, y), r * 0.94f, c, false);
    }
    d->PopClipRect();
}

// a combo over (label, value) pairs in the current row; true when the value changed
template <typename T>
bool Choice(const char* id, T& value, const std::vector<std::pair<std::string, T>>& items) {
    std::string preview = "Custom";
    for (auto& [name, v] : items)
        if (v == value) preview = name;
    bool changed = false;
    if (ImGui::BeginCombo(id, preview.c_str())) {
        for (auto& [name, v] : items) {
            bool sel = v == value;
            if (ImGui::Selectable(name.c_str(), sel) && !sel) {
                value = v;
                changed = true;
            }
            if (sel) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    return changed;
}

// a row with a choice, a toggle or a slider, and its description
template <typename T>
bool RowChoice(const char* label, T& value, const std::vector<std::pair<std::string, T>>& items, const char* hint) {
    Row(label);
    ImGui::PushID(label);
    bool changed = Choice<T>("##v", value, items);
    ImGui::PopID();
    Hint(hint);
    return changed;
}
bool RowCheck(const char* label, bool& value, const char* hint) {
    Row(label);
    ImGui::PushID(label);
    bool changed = Toggle("##v", value);
    ImGui::PopID();
    Hint(hint);
    return changed;
}
bool RowSlider(const char* label, uint32_t& value, int lo, int hi, const char* format, const char* hint) {
    Row(label);
    ImGui::PushID(label);
    bool changed = Slider("##v", value, lo, hi, format);
    ImGui::PopID();
    Hint(hint);
    return changed;
}

void DisplayPage(Settings& s, const Settings& before) {
    if (RowChoice<DisplayMode>("Window mode", s.display_mode,
                               {{"Fullscreen", DisplayMode::Fullscreen},
                                {"Borderless", DisplayMode::Borderless},
                                {"Windowed", DisplayMode::Windowed}},
                               "Fullscreen switches the display's mode. Borderless is a window covering the screen."))
        DisplayChanged(before);
    // which display; a new one starts at its desktop resolution and rate
    static std::vector<Monitor> monitors;
    bool new_monitor = false;
    if (ImGui::IsWindowAppearing() || monitors.empty()) monitors = Monitors();
    if (monitors.size() > 1) {
        std::vector<std::pair<std::string, std::string>> list{{"Automatic", ""}};
        for (const Monitor& m : monitors)
            list.push_back({"Display " + std::to_string(m.number) + "  (" + std::to_string(m.width) + " x " +
                                std::to_string(m.height) + (m.primary ? ", main)" : ")"),
                            m.device});
        if (RowChoice<std::string>("Monitor", s.monitor, list, "Which display the game opens on.")) {
            s.width = s.height = 0;
            s.refresh_hz = 0;
            new_monitor = true;
            DisplayChanged(before);
        }
    }
    static std::vector<Resolution> modes;
    if (ImGui::IsWindowAppearing() || modes.empty() || new_monitor) modes = Resolutions();
    std::vector<std::pair<std::string, Resolution>> res{{"Desktop", {0, 0}}};
    for (auto& m : modes) res.push_back({std::to_string(m.w) + " x " + std::to_string(m.h), m});
    Resolution cur{s.width, s.height};
    if (RowChoice<Resolution>("Resolution", cur, res,
                              "The size of the window or screen. How sharp the game is drawn is Render resolution, "
                              "under Graphics.")) {
        s.width = cur.w;
        s.height = cur.h;
        DisplayChanged(before);
    }
    // the display's rate and whether 60 fps divides it evenly: the refresh rate's and V-Sync's description
    char pacing[200] = "";
    if (uint32_t now_hz = RefreshHz(); now_hz > 1 && !EvenFor60(now_hz))
        std::snprintf(pacing, sizeof(pacing), " Your display runs at %u Hz, which 60 fps doesn't divide evenly: use "
                      "G-Sync or FreeSync, or 60 or 120 Hz, for the smoothest motion.", now_hz);
    if (s.display_mode == DisplayMode::Fullscreen) {
        static std::vector<uint32_t> rates;
        static uint64_t rates_for = ~0ull;
        uint64_t key = uint64_t(s.width) << 32 | s.height;
        if (ImGui::IsWindowAppearing() || key != rates_for) {
            rates = RefreshRates(s.width, s.height);
            rates_for = key;
        }
        std::vector<std::pair<std::string, uint32_t>> hz{{"Desktop", 0}};
        for (uint32_t r : rates) hz.push_back({std::to_string(r) + " Hz" + (EvenFor60(r) ? "  (smooth 60)" : ""), r});
        std::string hint = std::string("Rates marked smooth 60 show every 60 fps frame for the same time.") + pacing;
        if (RowChoice<uint32_t>("Refresh rate", s.refresh_hz, hz, hint.c_str())) DisplayChanged(before);
    }
    // MechAssault draws 4:3: the widescreen choices widen its camera (src/game/widescreen.cpp), Stretch stretches
    if (RowChoice<uint32_t>("Aspect ratio", s.aspect_ratio,
                            {{"Fit screen", kAspectWide},
                             {"16:9", kAspectWide169},
                             {"21:9", kAspectWide219},
                             {"4:3 (original)", kAspect43},
                             {"Stretch", kAspectFill}},
                            "Fit screen shows more of the scene at the sides, in your screen's shape. 16:9 and 21:9 do "
                            "the same at those shapes. Menus and movies stay 4:3."))
        Changed();
    if (s.aspect_ratio == kAspectWide || s.aspect_ratio == kAspectWide169 || s.aspect_ratio == kAspectWide219)
        if (RowCheck("HUD at the edges", s.hud_edges,
                     "The radar and weapon gauges move out to the screen's edges. Off keeps them around the middle, "
                     "as on a 4:3 TV."))
            Changed();
    if (RowSlider("Brightness", s.brightness, 50, 200, "%d%%", "100% is the original picture.")) Changed();
    if (kGame.menu) kGame.menu();  // the game's rows on this page (MenuPage("Display"))
    if (RowChoice<uint32_t>("FPS limit", s.fps_cap,
                            {{"Off", 0}, {"Match display", kFpsCapRefresh}, {"30", 30}, {"60", 60}, {"120", 120},
                             {"144", 144}},
                            "A cap only: the game keeps its normal speed at any frame rate."))
        Changed();
    std::string vsync = std::string("Frames wait for the display, so the picture never tears.") + pacing;
    if (RowCheck("V-Sync", s.vsync, vsync.c_str())) Changed();
}

void GraphicsPage(Settings& s) {
    std::vector<std::pair<std::string, uint32_t>> scales;
    for (uint32_t i = 1; i <= kMaxRenderScale; i++) scales.push_back({std::to_string(i) + "x" + (i == 1 ? " (original)" : ""), i});
    scales.push_back({LowEndGpu() ? "Auto (up to 2x)" : "Auto", 0});
    char hint[200];
    float now = RenderScale(480);
    std::snprintf(hint, sizeof(hint), "How sharp the game is drawn: 1x is the Xbox's 640 x 480, now %.0f x %.0f. Auto "
                  "matches your screen.", 640 * now, 480 * now);
    if (RowChoice<uint32_t>("Render resolution", s.render_scale, scales, hint)) {
        if (!s.render_scale) s.render_percent = 100;
        Changed();
    }
    if (RowChoice<uint32_t>("Anti-aliasing", s.anti_aliasing, {{"Off", 0}, {"FXAA", 1}, {"SMAA", 2}},
                            "Smooths jagged edges. SMAA keeps textures sharper than FXAA."))
        Changed();
    if (RowChoice<uint32_t>("Texture filtering", s.aniso,
                            {{"Original", 0}, {"2x", 2}, {"4x", 4}, {"8x", 8}, {"16x", 16}},
                            "Anisotropic filtering: keeps floors and walls sharp at an angle."))
        Changed();
    if (RowSlider("Texture detail", s.texture_detail, 0, 15, s.texture_detail ? "+%d" : "Original",
                  "Sharper textures in the distance. High values can shimmer while moving; texture filtering helps."))
        Changed();
    if (RowChoice<uint32_t>("Upscaling", s.upscaler, {{"FSR 1", 1}, {"Bilinear", 0}},
                            "When the render resolution is below your screen's: FSR 1 keeps edges crisp."))
        Changed();
    if (s.upscaler == 1 && RowSlider("FSR sharpening", s.fsr_sharpness, 0, 20, "%d", "How much FSR 1 sharpens as it upscales."))
        Changed();
    if (RowSlider("Sharpening", s.sharpen, 0, 20, s.sharpen ? "%d" : "Off",
                  "Sharpens the finished picture (contrast-adaptive sharpening) when FSR is not upscaling."))
        Changed();
    if (RowChoice<uint32_t>("FPS counter", s.perf_overlay, {{"Off", 0}, {"On", 1}, {"Detailed", 2}},
                            "Top left corner. F2 steps it. Detailed adds frame times, a graph, CPU and memory."))
        Changed();
    if (kGame.menu) kGame.menu();  // the game's rows on this page (MenuPage("Graphics"): draw distance)
}

// System Link's network (app/network.cpp, xdk/xnet.cpp)
void NetworkPage(Settings& s) {
    static std::vector<NetAdapter> adapters;
    if (ImGui::IsWindowAppearing() || adapters.empty()) adapters = NetworkAdapters();
    std::vector<std::pair<std::string, std::string>> list{{"Automatic", ""}};
    for (const NetAdapter& n : adapters) list.push_back({n.name + "  (" + n.ip + ")", n.ip});
    if (RowChoice<std::string>("Network", s.lan_adapter, list,
                               "For a virtual LAN (Radmin VPN, ZeroTier, Hamachi, Tailscale) pick its adapter on every "
                               "PC. Applies the next time you open System Link."))
        Changed();
    static char join[128] = {};
    static bool loaded = false;
    if (!loaded || ImGui::IsWindowAppearing()) {
        std::snprintf(join, sizeof(join), "%s", s.join_ip.c_str());
        loaded = true;
    }
    Row("Join address");
    if (ImGui::InputTextWithHint("##join", "a host's address", join, sizeof(join))) {
        s.join_ip = join;
        Changed();
    }
    Hint("To join a host over the internet without a virtual LAN. The host forwards UDP ports 1000 to 1002. Host and "
         "join from the game's own Multiplayer > System Link.");
    if (kGame.menu) kGame.menu();  // the game's rows on this page (online games)
}

void ResetVideo(Settings& s) {
    Settings d;
    s.display_mode = d.display_mode;
    s.width = d.width;
    s.height = d.height;
    s.render_percent = d.render_percent;
    s.render_scale = d.render_scale;
    s.fps_cap = d.fps_cap;
    s.vsync = d.vsync;
    s.aniso = d.aniso;
    s.perf_overlay = d.perf_overlay;
    s.diagnostics_log = d.diagnostics_log;
    s.anti_aliasing = d.anti_aliasing;
    s.refresh_hz = d.refresh_hz;
    s.aspect_ratio = d.aspect_ratio;
    s.upscaler = d.upscaler;
    s.brightness = d.brightness;
    s.hud_edges = d.hud_edges;
    s.fsr_sharpness = d.fsr_sharpness;
    s.sharpen = d.sharpen;
    s.texture_detail = d.texture_detail;
    Changed(true);
}

constexpr int kPages = 5;
constexpr const char* kPageNames[kPages] = {"DISPLAY", "GRAPHICS", "GAME", "CONTROLS", "NETWORK"};
constexpr int kDisplay = 0, kGraphics = 1, kGamePage = 2, kControls = 3, kNetwork = 4;

void Page(Settings& s, const Settings& before, int i) {
    Rows(true);
    if (i == kDisplay) DisplayPage(s, before);
    if (i == kGraphics) GraphicsPage(s);
    if (i == kNetwork) NetworkPage(s);
    if ((i == kGamePage || i == kControls) && kGame.menu) kGame.menu();  // the game's pages (MenuPage)
    Rows(false);
}

void HintText(float height) {
    ImGui::BeginChild("##hint", ImVec2(0, height), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs);
    ImGui::PushTextWrapPos(0);
    if (!g_hint.empty())
        ImGui::TextUnformatted(g_hint.c_str());
    else
        ImGui::TextDisabled("Point at a setting to see what it does. On a controller: A changes, B closes, LB and RB "
                            "change the page.");
    ImGui::PopTextWrapPos();
    ImGui::EndChild();
}

// a text button without a frame, muted until hovered
bool QuietButton(const char* label) {
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    bool pressed = ImGui::Button(label);
    ImGui::PopStyleColor(2);
    return pressed;
}

// Check for updates' state, a line under its button (empty when there is nothing to say)
std::string UpdateLine(const update::Status& st) {
    using update::State;
    char line[160] = "";
    switch (st.state) {
        case State::Idle: break;
        case State::Checking: std::snprintf(line, sizeof(line), "Checking..."); break;
        case State::UpToDate: std::snprintf(line, sizeof(line), "Up to date"); break;
        case State::NoReleases: std::snprintf(line, sizeof(line), "No releases yet"); break;
        case State::Failed: std::snprintf(line, sizeof(line), "Could not check for updates"); break;
        case State::Available:
            std::snprintf(line, sizeof(line), "Version %s is available%s", st.latest.c_str(),
                          st.error.empty() ? "" : " (the last try failed, see the log)");
            break;
        case State::Installing:
            std::snprintf(line, sizeof(line), "Updating to %s...  %.0f%%", st.latest.c_str(), st.progress * 100);
            break;
        case State::Installed: std::snprintf(line, sizeof(line), "Restarting..."); break;
    }
    return line;
}

// the foot of the page list, bottom up: Close, Reset (Display and Graphics), Check for updates with its state
void NavFoot(Settings& s, bool video, float w, float body_h, bool& open) {
    using update::State;
    const ImGuiStyle& st = ImGui::GetStyle();
    const float button = ImGui::GetFrameHeight(), gap = st.ItemSpacing.y;
    update::Status u = update::Current();
    std::string line = UpdateLine(u);
    const bool offer = u.state == State::Available;
    auto lines = [&](const std::string& t) { return t.empty() ? 0.0f : ImGui::CalcTextSize(t.c_str(), nullptr, false, w).y + gap; };
    float h = button * (2 + (video ? 1 : 0) + (offer ? 1 : 0)) + gap * (1 + (video ? 1 : 0) + (offer ? 1 : 0)) + lines(line);
    ImGui::SetCursorPosY(std::max(ImGui::GetCursorPosY(), body_h - h));
    bool busy = u.state == State::Checking || u.state == State::Installing || u.state == State::Installed;
    ImGui::BeginDisabled(busy);
    if (QuietButton("Check for updates")) update::Check();
    ImGui::EndDisabled();
    Hint("Looks for a newer version and installs it through the setup. Your saves, settings and game stay as they are.");
    if (!line.empty()) {
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + w);
        ImGui::TextDisabled("%s", line.c_str());
        ImGui::PopTextWrapPos();
    }
    if (offer) {
        if (update::DevelopmentLayout()) {
            QuietButton("Updates through git");
            Hint("This is a build folder, not a release: update it with git pull and build.bat.");
        } else {
            if (QuietButton(("Update to " + u.latest).c_str())) update::Install();
            Hint("Downloads the new version, closes the game and opens the setup, which gets it ready for your PC. "
                 "Your saves, settings and game stay as they are.");
        }
    }
    if (video) {
        if (QuietButton("Reset")) ResetVideo(s);
        Hint("Puts every Display and Graphics setting back to its default.");
    }
    if (QuietButton("Close  F1")) open = false;
    Hint("Closes the menu. On a controller: B, or Back and Start together.");
}

// the version, small, spaced and faint, in the panel's bottom left corner below the page list
void VersionTag(ImDrawList* d, ImVec2 wpos, float panel_w, float x, float H, float em) {
    const std::string& v = update::Version();
    if (v.empty()) return;
    std::string text;
    for (char ch : v) text += char(std::toupper(static_cast<unsigned char>(ch)));
    const Look& L = CurrentLook();
    d->PushClipRect(wpos, ImVec2(wpos.x + panel_w, wpos.y + H), false);
    DrawTracked(d, ImVec2(x, wpos.y + H - 1.5f * em), 0.62f * em,
                ImGui::GetColorU32(ImVec4(L.accent.x, L.accent.y, L.accent.z, 0.55f)), text.c_str(), 0.14f * em);
    d->PopClipRect();
}

// XBE_MENU_TAB=<name> (tests, screenshots): the page the menu opens on
int TestPage() {
    static int page = [] {
        const char* want = std::getenv("XBE_MENU_TAB");
        if (!want) return -1;
        for (int i = 0; i < kPages; i++)
            if (_stricmp(want, kPageNames[i]) == 0) return i;
        if (_stricmp(want, "Gameplay") == 0) return kGamePage;
        return -1;
    }();
    return page;
}

void Menu() {
    Settings& s = Get();
    const Settings before = s;  // what a display change can go back to
    ImGuiIO& io = ImGui::GetIO();
    const float em = ImGui::GetFontSize();
    const ImGuiStyle& st = ImGui::GetStyle();
    const float W = io.DisplaySize.x, H = io.DisplaySize.y;
    float panel_w = std::min(std::max(46.0f * em, W * 0.4f), W);
    ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(panel_w, H), ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(3.0f * em, 2.5f * em));
    static int want_tab = -1;
    if (!g_menu_drawn) {
        ImGui::SetNextWindowFocus();  // a controller can move in it at once
        if (TestPage() >= 0) want_tab = TestPage();
    }
    g_menu_drawn = true;
    static bool popup_before = false;  // B that closed a list this frame must not close the menu too
    bool open = true;
    g_hint.clear();
    g_rows_id = 0;
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                             ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
                             ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
    {  // the panel fades into the game on its right
        ImU32 bg = ImGui::GetColorU32(ImGuiCol_WindowBg), clear = ImGui::GetColorU32(ImGuiCol_WindowBg, 0.0f);
        ImGui::GetBackgroundDrawList()->AddRectFilledMultiColor(ImVec2(panel_w, 0), ImVec2(panel_w + 8 * em, H), bg,
                                                                 clear, clear, bg);
    }
    bool began = ImGui::Begin("Settings##mechassault-recomp", nullptr, flags);
    ImGui::PopStyleVar(2);
    if (began) {
        const bool video = g_tab == kDisplay || g_tab == kGraphics;
        // LB and RB change the page
        if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
            !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId)) {
            if (ImGui::IsKeyPressed(ImGuiKey_GamepadL1, false)) want_tab = (g_tab + kPages - 1) % kPages;
            if (ImGui::IsKeyPressed(ImGuiKey_GamepadR1, false)) want_tab = (g_tab + 1) % kPages;
        }
        const float hint_h = ImGui::GetTextLineHeight() * 3 + st.ItemSpacing.y;  // three lines of description
        const float foot_h = hint_h + st.ItemSpacing.y * 3;
        const Look& L = CurrentLook();
        ImDrawList* d = ImGui::GetWindowDrawList();
        ImVec2 wpos = ImGui::GetWindowPos();
        Honeycomb(d, wpos, ImVec2(wpos.x + panel_w, wpos.y + H), 1.6f * em, ImVec4(1, 1, 1, 0.035f));
        // the title: the game's name in spaced capitals, SETTINGS under it, a rule fading right
        std::string title;
        for (const wchar_t* c = kGame.short_title; *c; c++) title += *c < 0x80 ? char(std::toupper(int(*c))) : '?';
        ImVec2 at = ImGui::GetCursorScreenPos();
        float tsize = 1.7f * em, sub = 0.85f * em;
        DrawTracked(d, at, tsize, ImGui::GetColorU32(ImGuiCol_Text), title.c_str(), 0.32f * em);
        DrawTracked(d, ImVec2(at.x + 0.05f * em, at.y + tsize * 1.1f), sub, ImGui::GetColorU32(ImGuiCol_TextDisabled),
                    "SETTINGS", 0.5f * em);
        float rule_y = at.y + tsize * 1.1f + sub + 1.0f * em;
        ImVec4 ink(1, 1, 1, 0.22f), ink0(1, 1, 1, 0);
        d->AddRectFilledMultiColor(ImVec2(wpos.x, rule_y), ImVec2(wpos.x + panel_w - 2 * em, rule_y + 1.0f),
                                   ImGui::GetColorU32(ink), ImGui::GetColorU32(ink0), ImGui::GetColorU32(ink0),
                                   ImGui::GetColorU32(ink));
        ImGui::SetCursorScreenPos(ImVec2(at.x, rule_y + 1.8f * em));
        const float nav_w = 11.0f * em;
        const float body_h = ImGui::GetContentRegionAvail().y;
        // the pages, each a hexagon and its name; the open one a filled hexagon on a red band
        ImGui::BeginChild("##nav", ImVec2(nav_w, body_h), ImGuiChildFlags_NavFlattened);
        ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(0, 0, 0, 0));
        ImDrawList* nd = ImGui::GetWindowDrawList();
        const float item_h = 2.4f * em, hex_r = 0.55f * em, label = 0.95f * em;
        for (int i = 0; i < kPages; i++) {
            if (want_tab == i && g_tab != i) {
                g_tab = i;
                LOGR("settings menu: page %s", kPageNames[i]);
            }
            ImVec2 p = ImGui::GetCursorScreenPos();
            char id[16];
            std::snprintf(id, sizeof(id), "##page%d", i);
            if (ImGui::Selectable(id, g_tab == i, 0, ImVec2(nav_w, item_h)) && g_tab != i) {
                g_tab = i;
                LOGR("settings menu: page %s", kPageNames[i]);
            }
            bool sel = g_tab == i, hov = ImGui::IsItemHovered();
            float cy = p.y + item_h * 0.5f;
            if (sel) {  // the band runs in from the panel's edge, as the game's menu highlight does
                ImU32 band = ImGui::GetColorU32(ImVec4(L.accent.x, L.accent.y, L.accent.z, 0.55f));
                ImU32 clear = ImGui::GetColorU32(ImVec4(L.accent.x, L.accent.y, L.accent.z, 0));
                nd->PushClipRect(ImVec2(wpos.x, p.y), ImVec2(p.x + nav_w, p.y + item_h), false);
                nd->AddRectFilledMultiColor(ImVec2(wpos.x, p.y + 0.25f * em), ImVec2(p.x + nav_w, p.y + item_h - 0.25f * em),
                                            band, clear, clear, band);
                nd->PopClipRect();
            }
            ImU32 hex = sel ? ImGui::GetColorU32(L.accent) : ImGui::GetColorU32(hov ? ImGuiCol_Text : ImGuiCol_TextDisabled);
            Hexagon(nd, ImVec2(p.x + hex_r + 0.1f * em, cy), hex_r, hex, sel, std::max(1.0f, 0.08f * em));
            ImU32 text = ImGui::GetColorU32(sel || hov ? ImGuiCol_Text : ImGuiCol_TextDisabled);
            DrawTracked(nd, ImVec2(p.x + hex_r * 2 + 0.9f * em, cy - label * 0.55f), label, text, kPageNames[i], 0.28f * em);
        }
        ImGui::PopStyleColor(3);
        NavFoot(s, video, nav_w, body_h, open);
        ImGui::EndChild();
        VersionTag(d, wpos, panel_w, at.x, H, em);
        ImGui::SameLine(0, 1.5f * em);
        // the page's rows, a separator, and the hovered row's description
        ImGui::BeginChild("##body", ImVec2(0, body_h), ImGuiChildFlags_NavFlattened,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::BeginChild("##page", ImVec2(0, -foot_h), ImGuiChildFlags_NavFlattened);
        Page(s, before, g_tab);
        ImGui::EndChild();
        ImGui::Separator();
        ImGui::Dummy(ImVec2(0, st.ItemSpacing.y * 0.5f));
        HintText(hint_h);
        ImGui::EndChild();
        want_tab = -1;

        // B closes the menu once nothing inside it (an open list, an item being edited) takes it
        bool popup = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId);
        if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !popup && !popup_before &&
            ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) && !ImGui::IsAnyItemActive()) {
            open = false;
            LOGR("settings menu: B on a controller closes it");
        }
        popup_before = popup;
    }
    ImGui::End();
    if (!open) {
        g_menu = false;
        LOGR("settings menu closed");
    }
}

}  // namespace

void Rows(bool on) {
    if (on == g_rows) return;
    if (!on) {
        ImGui::EndTable();
        g_rows = false;
        return;
    }
    char id[16];
    std::snprintf(id, sizeof(id), "##rows%d", g_rows_id++);
    // as wide on a page without a scrollbar as on one with it, so the controls line up from page to page
    float w = ImGui::GetContentRegionAvail().x;
    if (!ImGui::GetCurrentWindow()->ScrollbarY) w -= ImGui::GetStyle().ScrollbarSize;
    g_rows = ImGui::BeginTable(id, 2, ImGuiTableFlags_None, ImVec2(std::max(w, 1.0f), 0));
    if (g_rows) {
        ImGui::TableSetupColumn("##name", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("##control", ImGuiTableColumnFlags_WidthFixed, 14.0f * ImGui::GetFontSize());
    }
}

bool Toggle(const char* id, bool& value) {
    float h = ImGui::GetFrameHeight() * 0.8f, w = h * 1.8f;
    ImVec2 at = ImGui::GetCursorScreenPos();
    at.y += (ImGui::GetFrameHeight() - h) * 0.5f;
    ImGui::SetCursorScreenPos(at);
    bool pressed = ImGui::InvisibleButton(id, ImVec2(w, h));
    if (pressed) value = !value;
    ImGuiID iid = ImGui::GetItemID();
    ImRect bb(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    ImGui::RenderNavHighlight(bb, iid);
    bool hovered = ImGui::IsItemHovered();
    ImDrawList* d = ImGui::GetWindowDrawList();
    ImU32 track = value ? ImGui::GetColorU32(ImGuiCol_SliderGrab)
                        : ImGui::GetColorU32(hovered ? ImGuiCol_FrameBgHovered : ImGuiCol_FrameBg);
    d->AddRectFilled(bb.Min, bb.Max, track, h * 0.5f);
    float r = h * 0.5f - std::max(2.0f, h * 0.12f);
    ImVec2 knob(value ? bb.Max.x - h * 0.5f : bb.Min.x + h * 0.5f, bb.Min.y + h * 0.5f);
    d->AddCircleFilled(knob, r, ImGui::GetColorU32(value ? ImGuiCol_Text : ImGuiCol_TextDisabled));
    return pressed;
}

bool Slider(const char* id, uint32_t& value, int lo, int hi, const char* format) {
    char text[32];
    std::snprintf(text, sizeof(text), format, int(value));
    const ImGuiStyle& st = ImGui::GetStyle();
    float text_w = 5.5f * ImGui::GetFontSize();
    ImGui::SetNextItemWidth(std::max(ImGui::GetContentRegionAvail().x - text_w - st.ItemInnerSpacing.x, 1.0f));
    int v = int(value);
    bool changed = ImGui::SliderInt(id, &v, lo, hi, "", ImGuiSliderFlags_AlwaysClamp);
    if (changed) value = uint32_t(v);
    g_slider_hovered = ImGui::IsItemHovered();
    ImGui::SameLine(0, st.ItemInnerSpacing.x);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(text);
    return changed;
}

void Row(const char* label) {
    Rows(true);
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    g_row_top = ImGui::GetCursorScreenPos().y;
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::TableNextColumn();
    ImGui::SetNextItemWidth(-FLT_MIN);
}

void Hint(const char* text) {
    // the control under the mouse or selected with a pad or keys (IsItemHovered follows the selection while they
    // drive), or with the mouse anywhere along its row
    bool row = false;
    if (g_rows && MouseMode() && ImGui::IsWindowHovered()) {
        float y = ImGui::GetIO().MousePos.y;
        row = y >= g_row_top && y < ImGui::GetItemRectMax().y + ImGui::GetStyle().ItemSpacing.y;
    }
    if (row || ImGui::IsItemHovered() || g_slider_hovered) g_hint = text;
    g_slider_hovered = false;
}

void Heading(const char* text) {
    Rows(true);
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::Dummy(ImVec2(0, ImGui::GetStyle().ItemSpacing.y));
    ImGui::TextDisabled("%s", text);
    ImGui::TableNextColumn();
}

void MenuRow(const char* label) { Row(label); }
void MenuHint(const char* text) { Hint(text); }
// the game's rows for a page (GameInfo::menu is called on every page): true on that page, its rows open
bool MenuPage(const char* name) {
    const char* page = kPageNames[g_tab];
    bool on = _stricmp(name, page) == 0 || (g_tab == kGamePage && _stricmp(name, "Gameplay") == 0);
    if (on) Rows(true);
    return on;
}
void EndMenuPage() {}

bool MenuOpen() { return g_menu.load(std::memory_order_relaxed); }
void ToggleMenu() {
    g_menu = !g_menu;
    LOGR("settings menu %s", g_menu ? "opened" : "closed");
}

// the build, small and faint in the bottom right corner while the game shows its menus
void BuildTag() {
#ifdef MECHASSAULT_BUILD_INFO
    if (gpu::wide::g_hud_learn || g_menu) return;  // in play, or under the settings menu
    ImGuiIO& io = ImGui::GetIO();
    std::string tag = update::Version().empty() ? "v" MECHASSAULT_VERSION "  " MECHASSAULT_COMMIT : update::Version();
    const char* text = tag.c_str();
    float size = ImGui::GetFontSize() * 0.9f;
    ImVec2 t = ImGui::GetFont()->CalcTextSizeA(size, FLT_MAX, 0, text);
    float pad = size * 0.8f;
    ImGui::GetForegroundDrawList()->AddText(ImGui::GetFont(), size,
                                            ImVec2(io.DisplaySize.x - t.x - pad, io.DisplaySize.y - t.y - pad),
                                            IM_COL32(200, 196, 192, 90), text);
#endif
}

void LoadMenuFont() {
    ImGuiIO& io = ImGui::GetIO();
    std::string path;
    char dir[MAX_PATH];
    UINT n = GetWindowsDirectoryA(dir, MAX_PATH);
    if (n && n < MAX_PATH) path = std::string(dir) + "\\Fonts\\segoeui.ttf";
    if (const char* f = std::getenv("MA_MENU_FONT")) path = f;  // another font
    if (path.empty() || GetFileAttributesA(path.c_str()) == INVALID_FILE_ATTRIBUTES) return;
    ImFontConfig cfg;
    cfg.OversampleH = 2;
    cfg.OversampleV = 2;
    static const ImWchar kRanges[] = {0x0020, 0x00FF, 0x2013, 0x2014, 0x2018, 0x201D, 0x2026, 0x2026, 0x2039, 0x203A, 0};
    g_font_loaded = io.Fonts->AddFontFromFileTTF(path.c_str(), kFontLoadPx, &cfg, kRanges) != nullptr;
    if (!g_font_loaded) io.Fonts->AddFontDefault();
}

void DrawOverlay() {
    ImGuiIO& io = ImGui::GetIO();
    input::MenuPad pad = input::ReadMenuPad();
    if (pad.toggle) ToggleMenu();
    // which controller buttons reach the open menu (B closes it, LB and RB change the page)
    static uint32_t logged_buttons = 0;
    if (g_menu && pad.buttons != logged_buttons) LOGR("settings menu: controller buttons %03X", pad.buttons);
    logged_buttons = g_menu ? pad.buttons : 0;
    FeedControllers(pad);
    // readable from 720p to 4K: the font at kFontPx on a 720-line display, the menu's sizes with it
    float scale = std::clamp(io.DisplaySize.y / 720.0f, 1.0f, 3.0f);
    io.FontGlobalScale = g_font_loaded ? scale * kFontPx / kFontLoadPx : scale;
    static float styled = 0;
    if (styled != scale) {
        Style(g_font_loaded ? scale * kFontPx / 13.0f : scale);
        styled = scale;
    }
    io.MouseDrawCursor = false;
    if (uint32_t o = Get().perf_overlay) FpsCounter(o >= 2);
    BuildTag();
    if (g_menu) Menu();
    else g_menu_drawn = false;
    RevertPrompt();  // also with the menu closed
    DrawBugToast();  // an F8 mark was saved
}

}  // namespace app
