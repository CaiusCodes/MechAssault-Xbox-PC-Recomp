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
#include <initializer_list>
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
// mechassault-recomp patch: XBE_MENU_KEYS=<seconds>:<button>,... (tests): controller buttons held for a fifth of a
// second at those times after the menu opens (up down left right a b lb rb), as the held menu buttons
uint32_t TestMenuButtons() {
    struct Press {
        double at;
        uint32_t bit;
    };
    static const std::vector<Press> presses = [] {
        std::vector<Press> out;
        const char* spec = std::getenv("XBE_MENU_KEYS");
        if (!spec) return out;
        static const std::pair<const char*, uint32_t> kNames[] = {
            {"up", input::kMenuUp},     {"down", input::kMenuDown}, {"left", input::kMenuLeft}, {"right", input::kMenuRight},
            {"a", input::kMenuA},       {"b", input::kMenuB},       {"lb", input::kMenuLB},     {"rb", input::kMenuRB}};
        std::string s = spec;
        for (size_t i = 0; i < s.size();) {
            size_t end = s.find(',', i);
            std::string item = s.substr(i, end == std::string::npos ? std::string::npos : end - i);
            i = end == std::string::npos ? s.size() : end + 1;
            size_t colon = item.find(':');
            if (colon == std::string::npos) continue;
            for (auto [name, bit] : kNames)
                if (_stricmp(item.substr(colon + 1).c_str(), name) == 0) out.push_back({std::atof(item.c_str()), bit});
        }
        return out;
    }();
    static std::chrono::steady_clock::time_point opened{};
    if (presses.empty() || !g_menu) {
        opened = {};
        return 0;
    }
    auto now = std::chrono::steady_clock::now();
    if (opened == std::chrono::steady_clock::time_point{}) opened = now;
    double t = std::chrono::duration<double>(now - opened).count();
    uint32_t held = 0;
    for (const Press& p : presses)
        if (t >= p.at && t < p.at + 0.2) held |= p.bit;
    static uint32_t last = 0;
    if (held & ~last) LOGR("settings menu test: buttons %03X at %.2f s", held & ~last, t);
    last = held;
    return held;
}

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

// mechassault-recomp patch: the F1 menu, design A "Command" (j, 2026-11-24): a panel in the middle of the screen with
// the game dimmed behind it; the reticle and MECHASSAULT / SETTINGS at the top, the pages as tabs under it (LB and RB),
// the open one with a glowing red line; each page a two-column table of names and controls (a < value > picker, a red
// switch, a thin red slider), the pointed-at or chosen row lit by a red-to-purple bar; its description under the table;
// a foot strip with the buttons' uses, Reset, Check for updates and the version. Pages: Display, Graphics, Game,
// Controls (the game's, GameInfo::menu through MenuPage) and Network.

// ---- the menu's rows (overlay.h): a page is a table of names and controls; Hint fills the foot area ----
bool g_rows = false;     // a rows table is open
int g_rows_id = 0;       // tables this frame
float g_row_top = 0;     // the current row's top, screen y
std::string g_hint;      // the hovered or selected row's description, this frame
bool g_slider_hovered = false;  // Slider's handle is under the mouse or selected (its value text is the last item)
int g_tab = 0;           // the page shown, kept while the menu is closed
ImVec2 g_sel_row, g_sel_row_next;  // the lit row's top and bottom, screen y: last frame's (drawn), this frame's
int g_focus_first = 0;  // the page's first control takes the focus: 1 (menu opened, page clicked), 2 (LB or RB: lit)

// right after a row's control: the page's first one takes the focus when the menu opens or the page changes, so a
// controller starts at the top of the page
void FocusIfFirst() {
    if (!g_focus_first || !g_rows) return;
    const bool lit = g_focus_first == 2;
    g_focus_first = 0;
    if (ImGui::GetCurrentWindow()->Appearing) {
        ImGui::SetItemDefaultFocus();
    } else {
        ImGui::SetFocusID(ImGui::GetItemID(), ImGui::GetCurrentWindow());
        if (lit) ImGui::GetCurrentContext()->NavDisableHighlight = false;
    }
}

bool MouseMode() { return !ImGui::GetCurrentContext()->NavDisableMouseHover; }

struct Look {
    ImVec4 bg, text, muted, accent, accent_hi;
};
const Look& CurrentLook() {
    auto rgb = [](int r, int g, int b, float a = 1.0f) { return ImVec4(r / 255.0f, g / 255.0f, b / 255.0f, a); };
    // MechAssault's icon and menus: near black, warm greys, the reticle's red
    static const Look kLook = {rgb(17, 14, 15), rgb(236, 230, 226), rgb(142, 127, 123), rgb(196, 40, 47),
                               rgb(236, 74, 60)};
    return kLook;
}

// the menu font: Bahnschrift (else Segoe UI) from Windows' fonts loaded large and drawn smaller (FontGlobalScale), so
// it stays sharp up to 4K; ImGui's own font when it is missing. MA_MENU_FONT names another .ttf
constexpr float kFontLoadPx = 34.0f;
constexpr float kFontPx = 15.0f;  // at 720p, scaled with the display's height up to 3x
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

ImVec4 Rgba(int r, int g, int b, float a) { return ImVec4(r / 255.0f, g / 255.0f, b / 255.0f, a); }

// a < value > picker over (label, value) pairs in the current row: its arrows, or left and right on a controller or
// the keys, step through them; a click in the middle, A or Enter opens the whole list. True when the value changed
template <typename T>
bool Choice(const char* id, T& value, const std::vector<std::pair<std::string, T>>& items) {
    const int n = int(items.size());
    int cur = -1;
    for (int i = 0; i < n; i++)
        if (items[size_t(i)].second == value) cur = i;
    const float em = ImGui::GetFontSize(), h = ImGui::GetFrameHeight();
    const float w = std::max(ImGui::GetContentRegionAvail().x, 4.0f * h);
    const ImVec2 a = ImGui::GetCursorScreenPos(), z(a.x + w, a.y + h);
    const bool pressed = ImGui::InvisibleButton(id, ImVec2(w, h));
    FocusIfFirst();
    const ImGuiID iid = ImGui::GetItemID(), popup = ImHashStr("##ChoicePopup", 0, iid);
    const bool hovered = ImGui::IsItemHovered(), focused = ImGui::IsItemFocused();
    const bool open = ImGui::IsPopupOpen(popup, ImGuiPopupFlags_None);
    const float lx = a.x + 0.9f * em, rx = z.x - 0.9f * em;  // the arrows' middles
    int step = 0;
    if (pressed) {
        const float x = ImGui::GetIO().MousePos.x;
        const bool mouse = MouseMode() && ImGui::IsMouseReleased(ImGuiMouseButton_Left);
        step = mouse && x < lx + 0.9f * em ? -1 : mouse && x > rx - 0.9f * em ? 1 : 0;
        if (!step && !open) ImGui::OpenPopupEx(popup);
    }
    if (focused && !open) {  // left and right step it, and do not move to another setting
        for (ImGuiKey k : {ImGuiKey_GamepadDpadLeft, ImGuiKey_GamepadDpadRight, ImGuiKey_LeftArrow, ImGuiKey_RightArrow})
            ImGui::SetKeyOwner(k, iid);
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadDpadLeft) || ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) step = -1;
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadDpadRight) || ImGui::IsKeyPressed(ImGuiKey_RightArrow)) step = 1;
    }
    bool changed = false;
    if (step && n) {
        int next = cur < 0 ? 0 : std::clamp(cur + step, 0, n - 1);
        if (next != cur) {
            cur = next;
            value = items[size_t(cur)].second;
            changed = true;
            if (std::getenv("XBE_MENU_KEYS")) LOGR("settings menu test: stepped to %s", items[size_t(cur)].first.c_str());
        }
    }
    ImDrawList* d = ImGui::GetWindowDrawList();
    const Look& L = CurrentLook();
    d->AddRectFilled(a, z, IM_COL32(255, 255, 255, hovered || open ? 26 : 15), 0.2f * em);
    ImGui::RenderNavHighlight(ImRect(a, z), iid);
    const ImU32 lit = ImGui::GetColorU32(L.accent), dim = ImGui::GetColorU32(ImVec4(L.accent.x, L.accent.y, L.accent.z, 0.3f));
    const float cy = a.y + h * 0.5f, s = 0.27f * em, th = std::max(1.5f, 0.11f * em);
    const ImVec2 left[3] = {{lx + s * 0.5f, cy - s}, {lx - s * 0.5f, cy}, {lx + s * 0.5f, cy + s}};
    const ImVec2 right[3] = {{rx - s * 0.5f, cy - s}, {rx + s * 0.5f, cy}, {rx - s * 0.5f, cy + s}};
    d->AddPolyline(left, 3, cur > 0 ? lit : dim, ImDrawFlags_None, th);
    d->AddPolyline(right, 3, cur + 1 < n ? lit : dim, ImDrawFlags_None, th);
    const std::string shown = cur >= 0 ? items[size_t(cur)].first : std::string("Custom");
    const ImVec2 ts = ImGui::CalcTextSize(shown.c_str());
    d->PushClipRect(ImVec2(lx + s, a.y), ImVec2(rx - s, z.y), true);
    d->AddText(ImVec2(std::max(a.x + (w - ts.x) * 0.5f, lx + s), cy - ts.y * 0.5f), ImGui::GetColorU32(ImGuiCol_Text),
               shown.c_str());
    d->PopClipRect();
    if (ImGui::BeginComboPopup(popup, ImRect(a, z), ImGuiComboFlags_HeightLarge)) {
        for (int i = 0; i < n; i++) {
            bool sel = i == cur;
            ImGui::PushID(i);
            if (ImGui::Selectable(items[size_t(i)].first.c_str(), sel) && !sel) {
                value = items[size_t(i)].second;
                changed = true;
            }
            ImGui::PopID();
            if (sel && ImGui::IsWindowAppearing()) ImGui::SetItemDefaultFocus();
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
    // no "Desktop" entry (j, 2026-11-24): the default (0 x 0) shows as the desktop's own size
    std::vector<std::pair<std::string, Resolution>> res;
    for (auto& m : modes) res.push_back({std::to_string(m.w) + " x " + std::to_string(m.h), m});
    Resolution cur{s.width, s.height};
    if (!cur.w || !cur.h) {
        MONITORINFOEXW mi{};
        mi.cbSize = sizeof(mi);
        DEVMODEW dm{};
        dm.dmSize = sizeof(dm);
        if (GetMonitorInfoW(static_cast<HMONITOR>(ChosenMonitor()), &mi) &&
            EnumDisplaySettingsW(mi.szDevice, ENUM_REGISTRY_SETTINGS, &dm))
            cur = {dm.dmPelsWidth, dm.dmPelsHeight};
    }
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

// the version for the foot, in capitals
std::string VersionText() {
    std::string text;
    for (char ch : update::Version()) text += char(std::toupper(static_cast<unsigned char>(ch)));
    return text;
}

// a button's use in the foot: its key caps ("<>" draws the left and right arrows), then what it does, centred on y;
// the width it took
float KeyHint(ImDrawList* d, float x, float y, float em, std::initializer_list<const char*> keys, const char* what) {
    ImFont* f = ImGui::GetFont();
    const float ks = 0.6f * em, kh = 1.15f * em, kp = 0.3f * em, x0 = x;
    const ImU32 line = IM_COL32(255, 255, 255, 64), text = ImGui::GetColorU32(ImGuiCol_Text),
                muted = ImGui::GetColorU32(ImGuiCol_TextDisabled);
    for (const char* k : keys) {
        const bool arrows = std::strcmp(k, "<>") == 0;
        const float tw = arrows ? 1.3f * em : f->CalcTextSizeA(ks, FLT_MAX, 0, k).x, kw = std::max(tw + 2 * kp, kh);
        d->AddRect(ImVec2(x, y - kh * 0.5f), ImVec2(x + kw, y + kh * 0.5f), line, 0.18f * em, 0, std::max(1.0f, 0.045f * em));
        if (arrows) {
            const float cx = x + kw * 0.5f, s = 0.2f * em;
            d->AddTriangleFilled(ImVec2(cx - 0.6f * em, y), ImVec2(cx - 0.2f * em, y - s), ImVec2(cx - 0.2f * em, y + s), text);
            d->AddTriangleFilled(ImVec2(cx + 0.6f * em, y), ImVec2(cx + 0.2f * em, y + s), ImVec2(cx + 0.2f * em, y - s), text);
        } else {
            d->AddText(f, ks, ImVec2(x + (kw - tw) * 0.5f, y - ks * 0.56f), text, k);
        }
        x += kw + 0.25f * em;
    }
    x += 0.2f * em;
    const float ws = 0.68f * em, wt = 0.04f * em;
    DrawTracked(d, ImVec2(x, y - ws * 0.56f), ws, muted, what, wt);
    return x - x0 + TrackedWidth(what, ws, wt) + 1.4f * em;
}

// a text button in the foot, muted until pointed at or chosen; at.x its left, at.y its middle. True when pressed
bool FootButton(const char* id, const char* label, ImVec2 at, float size) {
    const ImVec2 ts = ImGui::GetFont()->CalcTextSizeA(size, FLT_MAX, 0, label);
    const float m = size * 0.45f;
    ImGui::SetCursorScreenPos(ImVec2(at.x - m, at.y - ts.y * 0.5f - m));
    const bool pressed = ImGui::InvisibleButton(id, ImVec2(ts.x + 2 * m, ts.y + 2 * m));
    const bool lit = ImGui::IsItemHovered() || ImGui::IsItemFocused();
    ImGui::RenderNavHighlight(ImRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax()), ImGui::GetItemID());
    ImGui::GetWindowDrawList()->AddText(ImGui::GetFont(), size, ImVec2(at.x, at.y - ts.y * 0.5f),
                                        ImGui::GetColorU32(lit ? ImGuiCol_Text : ImGuiCol_TextDisabled), label);
    return pressed;
}

// the reticle beside the title: a ring, its cross reaching past it
void Reticle(ImDrawList* d, ImVec2 c, float r, float reach, float thick, ImU32 col) {
    d->AddCircle(c, r, col, 40, thick);
    d->AddLine(ImVec2(c.x, c.y - r - reach), ImVec2(c.x, c.y + r + reach), col, thick);
    d->AddLine(ImVec2(c.x - r - reach, c.y), ImVec2(c.x + r + reach, c.y), col, thick);
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
    const Look& L = CurrentLook();
    const float W = io.DisplaySize.x, H = io.DisplaySize.y;
    // the panel in the middle of the screen, the game dimmed behind it and a soft shadow under it
    const float pw = std::min(46.0f * em, W - 2.0f * em), ph = std::min(35.0f * em, H - 1.5f * em);
    const ImVec2 p0(std::floor((W - pw) * 0.5f), std::floor((H - ph) * 0.5f)), p1(p0.x + pw, p0.y + ph);
    const float r = 0.45f * em, pad = 1.9f * em;
    {
        ImDrawList* bg = ImGui::GetBackgroundDrawList();
        bg->AddRectFilled(ImVec2(0, 0), ImVec2(W, H), IM_COL32(6, 4, 5, 140));
        for (int i = 1; i <= 8; i++) {
            float g = float(i) * 0.4f * em;
            bg->AddRectFilled(ImVec2(p0.x - g, p0.y - g + 1.2f * em), ImVec2(p1.x + g, p1.y + g + 1.2f * em),
                              IM_COL32(0, 0, 0, 16), r + g);
        }
    }
    ImGui::SetNextWindowPos(p0, ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(pw, ph), ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    static int want_tab = -1;
    if (!g_menu_drawn) {
        ImGui::SetNextWindowFocus();  // a controller can move in it at once
        if (TestPage() >= 0) want_tab = TestPage();
        g_focus_first = 1;
    }
    g_menu_drawn = true;
    static bool popup_before = false;  // B that closed a list this frame must not close the menu too
    bool open = true;
    g_hint.clear();
    g_rows_id = 0;
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                             ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
                             ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
                             ImGuiWindowFlags_NoBackground;
    bool began = ImGui::Begin("Settings##mechassault-recomp", nullptr, flags);
    ImGui::PopStyleVar();
    if (began) {
        ImDrawList* d = ImGui::GetWindowDrawList();
        const ImU32 text = ImGui::GetColorU32(ImGuiCol_Text), muted = ImGui::GetColorU32(ImGuiCol_TextDisabled),
                    red = ImGui::GetColorU32(L.accent);
        {  // near black, a little lighter at the top. It stops short of 13, 11, 12: Windows' window captures
           // (PrintWindow, so the test screenshots) show that exact colour as black
            const int v0 = d->VtxBuffer.Size;
            d->AddRectFilled(p0, p1, IM_COL32(18, 15, 16, 245), r);
            ImGui::ShadeVertsLinearColorGradientKeepAlpha(d, v0, d->VtxBuffer.Size, p0, ImVec2(p0.x, p1.y),
                                                          IM_COL32(22, 18, 19, 255), IM_COL32(14, 12, 13, 255));
        }
        d->AddRect(p0, p1, IM_COL32(255, 255, 255, 15), r);
        // LB and RB change the page
        if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
            !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId)) {
            if (ImGui::IsKeyPressed(ImGuiKey_GamepadL1, false)) want_tab = (g_tab + kPages - 1) % kPages;
            if (ImGui::IsKeyPressed(ImGuiKey_GamepadR1, false)) want_tab = (g_tab + 1) % kPages;
            if (want_tab >= 0 && want_tab != g_tab) g_focus_first = 2;
        }
        // the title: the reticle, the game's name in spaced capitals, SETTINGS under it; F1 CLOSE at the right
        std::string title;
        for (const wchar_t* c = kGame.short_title; *c; c++) title += *c < 0x80 ? char(std::toupper(int(*c))) : '?';
        const float top = p0.y + 1.3f * em, tsize = 1.3f * em;
        Reticle(d, ImVec2(p0.x + pad + 0.75f * em, top + 0.85f * em), 0.68f * em, 0.26f * em, std::max(1.5f, 0.09f * em), red);
        const float tx = p0.x + pad + 2.15f * em;
        DrawTracked(d, ImVec2(tx, top), tsize, text, title.c_str(), 0.26f * em);
        DrawTracked(d, ImVec2(tx + std::max(1.0f, 0.035f * em), top), tsize, text, title.c_str(), 0.26f * em);  // heavier
        DrawTracked(d, ImVec2(tx + 0.03f * em, top + tsize * 1.1f), 0.56f * em, muted, "SETTINGS", 0.22f * em);
        {
            const char* close = "F1  CLOSE";
            const float cs = 0.64f * em, ct = 0.09f * em, cw = TrackedWidth(close, cs, ct), m = 0.4f * em;
            const ImVec2 at(p1.x - pad - cw, top + 0.6f * em);
            ImGui::SetCursorScreenPos(ImVec2(at.x - m, at.y - m));
            if (ImGui::InvisibleButton("##close", ImVec2(cw + 2 * m, cs + 2 * m))) open = false;
            const bool lit = ImGui::IsItemHovered() || ImGui::IsItemFocused();
            ImGui::RenderNavHighlight(ImRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax()), ImGui::GetItemID());
            DrawTracked(d, at, cs, lit ? text : muted, close, ct);
            Hint("Closes the menu. On a controller: B, or Back and Start together.");
        }
        // the pages as tabs: spaced capitals, the open one white over a glowing red line
        const float tabs_y = p0.y + 4.3f * em, tab_h = 2.1f * em, ls = 0.7f * em, lt = 0.13f * em;
        float x = p0.x + pad;
        for (int i = 0; i < kPages; i++) {
            if (want_tab == i && g_tab != i) {
                g_tab = i;
                LOGR("settings menu: page %s", kPageNames[i]);
            }
            const float w = TrackedWidth(kPageNames[i], ls, lt) + 1.9f * em;
            ImGui::SetCursorScreenPos(ImVec2(x, tabs_y));
            ImGui::PushID(i);
            if (ImGui::InvisibleButton("##tab", ImVec2(w, tab_h)) && g_tab != i) {
                g_tab = i;
                g_focus_first = 1;
                LOGR("settings menu: page %s", kPageNames[i]);
            }
            const bool lit = ImGui::IsItemHovered() || ImGui::IsItemFocused();
            ImGui::RenderNavHighlight(ImRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax()), ImGui::GetItemID());
            ImGui::PopID();
            const bool sel = g_tab == i;
            DrawTracked(d, ImVec2(x + 0.95f * em, tabs_y + (tab_h - ls) * 0.5f - 0.08f * em), ls, sel || lit ? text : muted,
                        kPageNames[i], lt);
            if (sel) {
                const float lh = std::max(2.0f, 0.13f * em), y = tabs_y + tab_h - lh;
                for (int g = 4; g >= 1; g--) {
                    const float e = float(g) * 0.16f * em;
                    d->AddRectFilled(ImVec2(x - e, y - e), ImVec2(x + w + e, y + lh + e),
                                     ImGui::GetColorU32(ImVec4(L.accent.x, L.accent.y, L.accent.z, 0.09f)), e);
                }
                d->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + lh), red);
            }
            x += w + 0.25f * em;
        }
        const float rule = tabs_y + tab_h;
        d->AddLine(ImVec2(p0.x, rule), ImVec2(p1.x, rule), IM_COL32(255, 255, 255, 18));
        // the page's rows; the lit row's bar (last frame's place) under them
        const float foot_h = 2.5f * em, hint_h = 4.1f * em, inset = 0.8f * em;
        const float body_top = rule + 0.6f * em, body_bottom = p1.y - foot_h - hint_h;
        ImGui::SetCursorScreenPos(ImVec2(p0.x + pad - inset, body_top));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(inset, 0));
        ImGui::BeginChild("##page", ImVec2(pw - 2 * (pad - inset), body_bottom - body_top),
                          ImGuiChildFlags_NavFlattened | ImGuiChildFlags_AlwaysUseWindowPadding);
        ImGui::PopStyleVar();
        if (g_sel_row.y > g_sel_row.x) {
            ImDrawList* cd = ImGui::GetWindowDrawList();
            const ImVec2 wp = ImGui::GetWindowPos();
            const float right = wp.x + ImGui::GetWindowWidth() -
                                (ImGui::GetCurrentWindow()->ScrollbarY ? ImGui::GetStyle().ScrollbarSize : 0.0f);
            const float mid = wp.x + (right - wp.x) * 0.7f;  // red into purple, then fading out
            const ImU32 red = IM_COL32(196, 40, 47, 107), purple = IM_COL32(150, 40, 110, 46), clear = IM_COL32(150, 40, 110, 0);
            cd->AddRectFilledMultiColor(ImVec2(wp.x, g_sel_row.x), ImVec2(mid, g_sel_row.y), red, purple, purple, red);
            cd->AddRectFilledMultiColor(ImVec2(mid, g_sel_row.x), ImVec2(right, g_sel_row.y), purple, clear, clear, purple);
        }
        g_sel_row_next = ImVec2(0, 0);
        Page(s, before, g_tab);
        ImGui::EndChild();
        g_sel_row = g_sel_row_next;
        g_focus_first = 0;  // a page without rows
        if (static ImGuiID logged = 0; std::getenv("XBE_MENU_KEYS") && ImGui::GetFocusID() != logged) {  // tests
            logged = ImGui::GetFocusID();
            LOGR("settings menu test: focus %08X (%.40s)", logged, g_hint.c_str());
        }
        // the foot: a darker strip with the buttons' uses; the version at the right, Check for updates and Reset before it
        const ImVec2 f0(p0.x, p1.y - foot_h);
        d->AddRectFilled(f0, p1, IM_COL32(0, 0, 0, 90), r, ImDrawFlags_RoundCornersBottom);
        const float fy = f0.y + foot_h * 0.5f, fs = 0.68f * em;
        float fx = p0.x + pad;
        fx += KeyHint(d, fx, fy, em, {"A"}, "Change");
        fx += KeyHint(d, fx, fy, em, {"B"}, "Close");
        fx += KeyHint(d, fx, fy, em, {"LB", "RB"}, "Page");
        fx += KeyHint(d, fx, fy, em, {"<>"}, "Choose");
        float bx = p1.x - pad;
        if (std::string ver = VersionText(); !ver.empty()) {
            const float vs = 0.6f * em, vt = 0.13f * em;
            bx -= TrackedWidth(ver.c_str(), vs, vt);
            DrawTracked(d, ImVec2(bx, fy - vs * 0.56f), vs, ImGui::GetColorU32(ImVec4(L.accent.x, L.accent.y, L.accent.z, 0.7f)),
                        ver.c_str(), vt);
            bx -= 1.6f * em;
        }
        auto foot_button = [&](const char* id, const std::string& label, const char* hint) {
            bx -= ImGui::GetFont()->CalcTextSizeA(fs, FLT_MAX, 0, label.c_str()).x;
            bool pressed = FootButton(id, label.c_str(), ImVec2(bx, fy), fs);
            if (ImGui::IsItemHovered() || ImGui::IsItemFocused()) g_hint = hint;
            bx -= 1.5f * em;
            return pressed;
        };
        const update::Status u = update::Current();
        const bool busy = u.state == update::State::Checking || u.state == update::State::Installing ||
                          u.state == update::State::Installed;
        if (u.state == update::State::Available && !update::DevelopmentLayout()) {
            if (foot_button("##update", "Update to " + u.latest,
                            "Downloads the new version, closes the game and opens the setup, which gets it ready for your "
                            "PC. Your saves, settings and game stay as they are."))
                update::Install();
        } else {
            std::string state = UpdateLine(u);  // Checking..., Up to date and so on, in the button's place
            if (foot_button("##update", state.empty() ? std::string("Check for updates") : state,
                            update::DevelopmentLayout() && u.state == update::State::Available
                                ? "This is a build folder, not a release: update it with git pull and build.bat."
                                : "Looks for a newer version and installs it through the setup. Your saves, settings and "
                                  "game stay as they are.") &&
                !busy)
                update::Check();
        }
        if ((g_tab == kDisplay || g_tab == kGraphics) &&
            foot_button("##reset", "Reset", "Puts every Display and Graphics setting back to its default."))
            ResetVideo(s);
        // the description: the pointed-at or chosen setting's
        const float hy = body_bottom + 0.3f * em;
        d->AddLine(ImVec2(p0.x + pad, hy), ImVec2(p1.x - pad, hy), IM_COL32(255, 255, 255, 18));
        ImGui::SetCursorScreenPos(ImVec2(p0.x + pad, hy + 0.65f * em));
        ImGui::PushTextWrapPos(pw - pad);
        ImGui::PushStyleColor(ImGuiCol_Text, Rgba(181, 169, 165, 1.0f));
        ImGui::TextUnformatted(g_hint.empty() ? "Point at a setting to see what it does." : g_hint.c_str());
        ImGui::PopStyleColor();
        ImGui::PopTextWrapPos();
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
        ImGui::TableSetupColumn("##control", ImGuiTableColumnFlags_WidthFixed, 13.0f * ImGui::GetFontSize());
    }
}

bool Toggle(const char* id, bool& value) {
    float h = ImGui::GetFrameHeight() * 0.8f, w = h * 1.8f;
    ImVec2 at = ImGui::GetCursorScreenPos();
    at.x += std::max(0.0f, (ImGui::GetContentRegionAvail().x - w) * 0.5f);  // in the middle of the control column
    at.y += (ImGui::GetFrameHeight() - h) * 0.5f;
    ImGui::SetCursorScreenPos(at);
    bool pressed = ImGui::InvisibleButton(id, ImVec2(w, h));
    FocusIfFirst();
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

// a thin red track with a white handle, its value right of it; dragged with the mouse, stepped with left and right
bool Slider(const char* id, uint32_t& value, int lo, int hi, const char* format) {
    const ImGuiStyle& st = ImGui::GetStyle();
    const float em = ImGui::GetFontSize(), h = ImGui::GetFrameHeight(), text_w = 4.0f * em;
    const float w = std::max(ImGui::GetContentRegionAvail().x - text_w - st.ItemInnerSpacing.x, 1.0f);
    const ImVec2 a = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton(id, ImVec2(w, h));
    FocusIfFirst();
    const ImGuiID iid = ImGui::GetItemID();
    int v = std::clamp(int(value), lo, hi);
    if (ImGui::IsItemActive() && ImGui::IsMouseDown(ImGuiMouseButton_Left) && hi > lo) {
        float f = std::clamp((ImGui::GetIO().MousePos.x - a.x) / w, 0.0f, 1.0f);
        v = lo + int(std::lround(f * float(hi - lo)));
    }
    if (ImGui::IsItemFocused()) {  // left and right step it, and do not move to another setting
        for (ImGuiKey k : {ImGuiKey_GamepadDpadLeft, ImGuiKey_GamepadDpadRight, ImGuiKey_LeftArrow, ImGuiKey_RightArrow})
            ImGui::SetKeyOwner(k, iid);
        int dir = (ImGui::IsKeyPressed(ImGuiKey_GamepadDpadRight) || ImGui::IsKeyPressed(ImGuiKey_RightArrow) ? 1 : 0) -
                  (ImGui::IsKeyPressed(ImGuiKey_GamepadDpadLeft) || ImGui::IsKeyPressed(ImGuiKey_LeftArrow) ? 1 : 0);
        if (dir) {  // a round step, about a thirtieth of the range
            float raw = float(hi - lo) / 30.0f;
            int step = raw < 1.5f ? 1 : raw < 3.5f ? 2 : raw < 7.5f ? 5 : raw < 15.0f ? 10 : 20;
            v = std::clamp(lo + int(std::lround(float(v + dir * step - lo) / float(step))) * step, lo, hi);
            if (std::getenv("XBE_MENU_KEYS")) LOGR("settings menu test: slider to %d", v);
        }
    }
    const bool changed = uint32_t(v) != value;
    value = uint32_t(v);
    g_slider_hovered = ImGui::IsItemHovered();
    ImDrawList* d = ImGui::GetWindowDrawList();
    const Look& L = CurrentLook();
    const float cy = a.y + h * 0.5f, t = std::max(1.5f, 0.13f * em), f = hi > lo ? float(v - lo) / float(hi - lo) : 0.0f;
    d->AddRectFilled(ImVec2(a.x, cy - t), ImVec2(a.x + w, cy + t), IM_COL32(255, 255, 255, 31), t);
    if (f > 0) d->AddRectFilled(ImVec2(a.x, cy - t), ImVec2(a.x + w * f, cy + t), ImGui::GetColorU32(L.accent), t);
    const float hx = a.x + w * f, hw = std::max(1.0f, 0.09f * em);
    d->AddRectFilled(ImVec2(hx - hw, cy - 0.4f * em), ImVec2(hx + hw, cy + 0.4f * em), IM_COL32_WHITE, hw);
    ImGui::RenderNavHighlight(ImRect(a, ImVec2(a.x + w, a.y + h)), iid);
    char text[32];
    std::snprintf(text, sizeof(text), format, v);
    ImGui::SameLine(0, st.ItemInnerSpacing.x);
    ImGui::AlignTextToFramePadding();
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, text_w - ImGui::CalcTextSize(text).x));
    ImGui::PushStyleColor(ImGuiCol_Text, Rgba(207, 198, 194, 1.0f));
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
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
    if (row || ImGui::IsItemHovered() || g_slider_hovered) {
        g_hint = text;
        if (g_rows) {  // the row's bar (Menu)
            float pad = ImGui::GetStyle().CellPadding.y;
            g_sel_row_next = ImVec2(g_row_top - pad, ImGui::GetItemRectMax().y + pad);
        }
    }
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
bool Pick(const char* id, int& index, const char* const* names, int count) {
    std::vector<std::pair<std::string, int>> items;
    for (int i = 0; i < count; i++) items.push_back({names[i], i});
    return Choice<int>(id, index, items);
}

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
    if (n && n < MAX_PATH) {
        path = std::string(dir) + "\\Fonts\\bahnschrift.ttf";  // as design A was drawn
        if (GetFileAttributesA(path.c_str()) == INVALID_FILE_ATTRIBUTES) path = std::string(dir) + "\\Fonts\\segoeui.ttf";
    }
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
    pad.buttons |= TestMenuButtons();
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
