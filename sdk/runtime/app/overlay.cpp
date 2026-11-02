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
#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include <imgui.h>

#include "app/app.h"
#include "app/overlay.h"
#include "core/frame_stats.h"
#include "core/input.h"
#include "core/log.h"

#pragma comment(lib, "psapi.lib")

namespace app {

namespace {

std::atomic<bool> g_menu{false};
bool g_menu_drawn = false;  // the menu window was up last frame (render thread)

struct Resolution {
    uint32_t w, h;
    bool operator==(const Resolution&) const = default;
};

// the window's monitor's modes, largest first, 640x480 and up
std::vector<Resolution> Resolutions() {
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(MonitorFromWindow(static_cast<HWND>(Window()), MONITOR_DEFAULTTOPRIMARY), &mi);
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
    GetMonitorInfoW(MonitorFromWindow(static_cast<HWND>(Window()), MONITOR_DEFAULTTOPRIMARY), &mi);
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

// a combo over (label, value) pairs; true when the value changed
template <typename T>
bool Choice(const char* label, T& value, const std::vector<std::pair<std::string, T>>& items) {
    std::string preview = "Custom";
    for (auto& [name, v] : items)
        if (v == value) preview = name;
    bool changed = false;
    if (ImGui::BeginCombo(label, preview.c_str())) {
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

// breakdown-recomp patch: the menu in the game's colours, blood red on near black, square corners
void Style() {
    ImGuiStyle& st = ImGui::GetStyle();
    st.WindowRounding = 0;
    st.ChildRounding = 0;
    st.FrameRounding = 2;
    st.PopupRounding = 0;
    st.GrabRounding = 2;
    st.TabRounding = 0;
    st.WindowBorderSize = 1;
    st.FrameBorderSize = 0;
    st.WindowPadding = ImVec2(14, 12);
    st.FramePadding = ImVec2(8, 5);
    st.ItemSpacing = ImVec2(10, 7);
    st.WindowTitleAlign = ImVec2(0.0f, 0.5f);
    auto rgb = [](int r, int g, int b, float a = 1.0f) { return ImVec4(r / 255.0f, g / 255.0f, b / 255.0f, a); };
    ImVec4* c = st.Colors;
    c[ImGuiCol_Text] = rgb(236, 232, 228);
    c[ImGuiCol_TextDisabled] = rgb(156, 145, 141);
    c[ImGuiCol_WindowBg] = rgb(14, 12, 12, 0.97f);
    c[ImGuiCol_PopupBg] = rgb(18, 14, 14, 0.98f);
    c[ImGuiCol_Border] = rgb(110, 28, 32);
    c[ImGuiCol_FrameBg] = rgb(29, 21, 22);
    c[ImGuiCol_FrameBgHovered] = rgb(54, 28, 30);
    c[ImGuiCol_FrameBgActive] = rgb(74, 26, 30);
    c[ImGuiCol_TitleBg] = rgb(38, 8, 10);
    c[ImGuiCol_TitleBgActive] = rgb(106, 12, 18);
    c[ImGuiCol_TitleBgCollapsed] = rgb(38, 8, 10);
    c[ImGuiCol_ScrollbarBg] = rgb(14, 12, 12);
    c[ImGuiCol_ScrollbarGrab] = rgb(74, 49, 51);
    c[ImGuiCol_ScrollbarGrabHovered] = rgb(110, 28, 32);
    c[ImGuiCol_ScrollbarGrabActive] = rgb(163, 18, 28);
    c[ImGuiCol_CheckMark] = rgb(236, 232, 228);
    c[ImGuiCol_SliderGrab] = rgb(163, 18, 28);
    c[ImGuiCol_SliderGrabActive] = rgb(212, 42, 47);
    c[ImGuiCol_Button] = rgb(48, 22, 24);
    c[ImGuiCol_ButtonHovered] = rgb(130, 18, 26);
    c[ImGuiCol_ButtonActive] = rgb(163, 18, 28);
    c[ImGuiCol_Header] = rgb(80, 14, 20);
    c[ImGuiCol_HeaderHovered] = rgb(130, 18, 26);
    c[ImGuiCol_HeaderActive] = rgb(163, 18, 28);
    c[ImGuiCol_Separator] = rgb(58, 20, 22);
    c[ImGuiCol_SeparatorHovered] = rgb(163, 18, 28);
    c[ImGuiCol_SeparatorActive] = rgb(212, 42, 47);
    c[ImGuiCol_ResizeGrip] = rgb(58, 20, 22);
    c[ImGuiCol_ResizeGripHovered] = rgb(163, 18, 28);
    c[ImGuiCol_ResizeGripActive] = rgb(212, 42, 47);
    c[ImGuiCol_TableHeaderBg] = rgb(38, 12, 14);
    c[ImGuiCol_TableBorderStrong] = rgb(58, 20, 22);
    c[ImGuiCol_TableBorderLight] = rgb(40, 18, 20);
    c[ImGuiCol_TableRowBgAlt] = rgb(255, 255, 255, 0.03f);
    c[ImGuiCol_PlotLines] = rgb(212, 42, 47);
    c[ImGuiCol_PlotLinesHovered] = rgb(255, 120, 90);
    c[ImGuiCol_TextSelectedBg] = rgb(163, 18, 28, 0.5f);
    c[ImGuiCol_NavHighlight] = rgb(233, 183, 180);
    c[ImGuiCol_ModalWindowDimBg] = rgb(0, 0, 0, 0.5f);
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

void Menu() {
    Settings& s = Get();
    const Settings before = s;  // what a display change can go back to
    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Appearing,
                            ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(560, 0), ImGuiCond_Appearing);
    if (!g_menu_drawn) ImGui::SetNextWindowFocus();  // a controller can move in it at once
    g_menu_drawn = true;
    static bool popup_before = false;  // B that closed a list this frame must not close the menu too
    bool open = true;
    // mechassault-recomp patch: the title's own name, not Breakdown's (the shared layer never names a title)
    std::string title;
    for (const wchar_t* c = kGame.short_title; *c; c++) title += *c < 0x80 ? char(*c) : '?';
    title += " PC settings";
    if (ImGui::Begin(title.c_str(), &open, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::TextDisabled("Controller: hold Back and press Start to open or close, A to change, B to close.");
        ImGui::PushItemWidth(260);
        if (ImGui::CollapsingHeader("Display", ImGuiTreeNodeFlags_DefaultOpen)) {
            if (Choice<DisplayMode>("Display mode", s.display_mode,
                                    {{"Fullscreen", DisplayMode::Fullscreen},
                                     {"Borderless", DisplayMode::Borderless},
                                     {"Windowed", DisplayMode::Windowed}}))
                DisplayChanged(before);
            static std::vector<Resolution> modes;
            if (ImGui::IsWindowAppearing() || modes.empty()) modes = Resolutions();
            std::vector<std::pair<std::string, Resolution>> res{{"Desktop", {0, 0}}};
            for (auto& m : modes) res.push_back({std::to_string(m.w) + " x " + std::to_string(m.h), m});
            Resolution cur{s.width, s.height};
            if (Choice<Resolution>(s.display_mode == DisplayMode::Windowed ? "Window size" : "Resolution", cur, res)) {
                s.width = cur.w;
                s.height = cur.h;
                DisplayChanged(before);
            }
            // breakdown-recomp patch: Breakdown draws 4:3 whatever the console reports: Widescreen widens its camera
            // (src/game/widescreen.cpp), the stretched shapes stretch the 4:3 picture
            if (Choice<uint32_t>("Aspect ratio", s.aspect_ratio,
                                 {{"Widescreen (fits your screen)", kAspectWide},
                                  {"4:3, as on the Xbox", kAspect43},
                                  {"16:9, stretched", kAspect169},
                                  {"Fill the screen, stretched", kAspectFill}}))
                Changed();
            if (s.aspect_ratio == kAspectWide)
            {
                ImGui::PushTextWrapPos(0);
                ImGui::TextDisabled("Shows more of the scene at the sides in your screen's shape, up to 32:9. The HUD, "
                                    "menus and movies stay 4:3. A change can take until the next scene to reach the "
                                    "3D view.");
                ImGui::PopTextWrapPos();
            }
            // breakdown-recomp patch: Fullscreen can switch the display to a rate 60 fps divides evenly
            if (s.display_mode == DisplayMode::Fullscreen) {
                static std::vector<uint32_t> rates;
                static uint64_t rates_for = ~0ull;
                uint64_t key = uint64_t(s.width) << 32 | s.height;
                if (ImGui::IsWindowAppearing() || key != rates_for) {
                    rates = RefreshRates(s.width, s.height);
                    rates_for = key;
                }
                std::vector<std::pair<std::string, uint32_t>> hz{{"Desktop's", 0}};
                for (uint32_t r : rates)
                    hz.push_back({std::to_string(r) + " Hz" + (EvenFor60(r) ? "  (even 60 fps)" : ""), r});
                if (Choice<uint32_t>("Refresh rate", s.refresh_hz, hz)) DisplayChanged(before);
            }
            if (uint32_t now_hz = RefreshHz(); now_hz > 1) {
                ImGui::PushTextWrapPos(0);
                if (VBlankEvery() > 0)
                    ImGui::TextDisabled("60 fps is locked to your display's %u Hz: every frame shows for the same time.",
                                        now_hz);
                else if (!EvenFor60(now_hz))
                    ImGui::TextDisabled("Your display runs at %u Hz, which 60 fps doesn't divide evenly, so motion can "
                                       "judder slightly. For the smoothest play use G-Sync or FreeSync, set Windows to "
                                       "60 or 120 Hz, or use Fullscreen with a refresh rate marked even.",
                                       now_hz);
                ImGui::PopTextWrapPos();
            }
            bool vsync = s.vsync;
            if (ImGui::Checkbox("V-Sync", &vsync)) {
                s.vsync = vsync;
                Changed();
            }
            if (Choice<uint32_t>("Frame-rate limit", s.fps_cap,
                                 {{"Off", 0}, {"Display refresh rate", kFpsCapRefresh}, {"30", 30}, {"60", 60},
                                  {"120", 120}, {"144", 144}}))
                Changed();
            ImGui::TextDisabled("The game keeps its own speed at any frame rate; the limit only caps it. F2 shows the FPS counter.");
        }
        if (ImGui::CollapsingHeader("Rendering", ImGuiTreeNodeFlags_DefaultOpen)) {
            std::vector<std::pair<std::string, uint32_t>> scales{{"Auto (output resolution)", 0}};
            for (uint32_t i = 1; i <= kMaxRenderScale; i++) {
                char name[48];
                std::snprintf(name, sizeof(name), "%ux (%ux%u)", i, 640 * i, 480 * i);
                scales.push_back({name, i});
            }
            if (Choice<uint32_t>("Render scale", s.render_scale, scales)) {
                if (!s.render_scale) s.render_percent = 100;
                Changed();
            }
            if (Choice<uint32_t>("Anisotropic filtering", s.aniso,
                                 {{"As the game asks", 0}, {"2x", 2}, {"4x", 4}, {"8x", 8}, {"16x", 16}}))
                Changed();
            if (Choice<uint32_t>("Anti-aliasing", s.anti_aliasing, {{"Off", 0}, {"FXAA", 1}})) Changed();
            ImGui::TextDisabled("A render scale above your display's resolution also smooths edges (supersampling).");
        }
        if (ImGui::CollapsingHeader("Performance", ImGuiTreeNodeFlags_DefaultOpen)) {
            if (Choice<uint32_t>("Performance overlay", s.perf_overlay,
                                 {{"Off", 0}, {"FPS counter", 1}, {"FPS counter and diagnostics", 2}}))
                Changed();
            bool log = s.diagnostics_log;
            if (ImGui::Checkbox("Frame timing in the log (every 10 s)", &log)) {
                s.diagnostics_log = log;
                Changed();
            }
        }
        if (kGame.menu) kGame.menu();  // breakdown-recomp patch: the game's sections
        ImGui::Spacing();
        if (ImGui::Button("Reset video settings")) {
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
            Changed(true);
        }
        ImGui::SameLine();
        if (ImGui::Button("Close (F1)")) open = false;
        ImGui::PopItemWidth();
        // B closes the menu once nothing inside it (an open list, an item being edited) takes it
        bool popup = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId);
        if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !popup && !popup_before &&
            ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) && !ImGui::IsAnyItemActive())
            open = false;
        popup_before = popup;
    }
    ImGui::End();
    if (!open) {
        g_menu = false;
        LOGR("settings menu closed");
    }
}

}  // namespace

bool MenuOpen() { return g_menu.load(std::memory_order_relaxed); }
void ToggleMenu() {
    g_menu = !g_menu;
    LOGR("settings menu %s", g_menu ? "opened" : "closed");
}

void DrawOverlay() {
    ImGuiIO& io = ImGui::GetIO();
    static bool styled = false;
    if (!styled) {
        Style();
        styled = true;
    }
    input::MenuPad pad = input::ReadMenuPad();
    if (pad.toggle) ToggleMenu();
    FeedControllers(pad);
    // readable from 720p to 4K
    io.FontGlobalScale = std::clamp(io.DisplaySize.y / 720.0f, 1.0f, 3.0f);
    io.MouseDrawCursor = false;
    if (uint32_t o = Get().perf_overlay) FpsCounter(o >= 2);
    if (g_menu) Menu();
    else g_menu_drawn = false;
    RevertPrompt();  // mechassault-recomp patch: also with the menu closed
}

}  // namespace app
