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
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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

// a setting's label in the left column; the control drawn next fills the right one (MenuRow, overlay.h)
float g_label_w = 200;
float g_page_footer = 0;  // the room a page leaves for the menu's buttons

// XBE_MENU_TAB=<name> (tests, screenshots): that tab is chosen as the menu opens
ImGuiTabItemFlags TabFlags(const char* name) {
    static const char* want = std::getenv("XBE_MENU_TAB");
    return want && ImGui::IsWindowAppearing() && !std::strcmp(want, name) ? ImGuiTabItemFlags_SetSelected : 0;
}

void Row(const char* label) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::SameLine(g_label_w);
    ImGui::SetNextItemWidth(-FLT_MIN);
}

// a hover note for the control just drawn
void Hint(const char* text) {
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort | ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 26);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

// a combo over (label, value) pairs in a row; true when the value changed
template <typename T>
bool Choice(const char* label, T& value, const std::vector<std::pair<std::string, T>>& items) {
    std::string preview = "Custom";
    for (auto& [name, v] : items)
        if (v == value) preview = name;
    bool changed = false;
    Row(label);
    if (ImGui::BeginCombo((std::string("##") + label).c_str(), preview.c_str())) {
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

bool Slider(const char* label, uint32_t& value, int lo, int hi, const char* format) {
    int v = int(value);
    Row(label);
    if (!ImGui::SliderInt((std::string("##") + label).c_str(), &v, lo, hi, format)) return false;
    value = uint32_t(v);
    return true;
}

bool Toggle(const char* label, bool& value) {
    Row(label);
    return ImGui::Checkbox((std::string("##") + label).c_str(), &value);
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

// breakdown-recomp patch, mechassault-recomp patch (minimal): near black, quiet greys, the game's red only as the
// accent (the chosen tab, sliders, checks), soft corners
void Style() {
    ImGuiStyle& st = ImGui::GetStyle();
    st.WindowRounding = 6;
    st.ChildRounding = 4;
    st.FrameRounding = 4;
    st.PopupRounding = 4;
    st.GrabRounding = 4;
    st.TabRounding = 4;
    st.WindowBorderSize = 1;
    st.FrameBorderSize = 0;
    st.PopupBorderSize = 1;
    st.TabBorderSize = 0;
    st.WindowPadding = ImVec2(18, 16);
    st.FramePadding = ImVec2(10, 6);
    st.ItemSpacing = ImVec2(10, 9);
    st.ItemInnerSpacing = ImVec2(8, 6);
    st.GrabMinSize = 12;
    st.ScrollbarSize = 10;
    st.ScrollbarRounding = 4;
    st.SeparatorTextBorderSize = 1;
    auto rgb = [](int r, int g, int b, float a = 1.0f) { return ImVec4(r / 255.0f, g / 255.0f, b / 255.0f, a); };
    const ImVec4 accent = rgb(196, 30, 40), accent_hi = rgb(224, 58, 62);
    ImVec4* c = st.Colors;
    c[ImGuiCol_Text] = rgb(232, 230, 228);
    c[ImGuiCol_TextDisabled] = rgb(128, 124, 122);
    c[ImGuiCol_WindowBg] = rgb(12, 12, 13, 0.97f);
    c[ImGuiCol_ChildBg] = rgb(0, 0, 0, 0);
    c[ImGuiCol_PopupBg] = rgb(20, 20, 22, 0.98f);
    c[ImGuiCol_Border] = rgb(48, 46, 48);
    c[ImGuiCol_FrameBg] = rgb(30, 30, 32);
    c[ImGuiCol_FrameBgHovered] = rgb(42, 40, 42);
    c[ImGuiCol_FrameBgActive] = rgb(52, 46, 48);
    c[ImGuiCol_TitleBg] = rgb(12, 12, 13);
    c[ImGuiCol_TitleBgActive] = rgb(12, 12, 13);
    c[ImGuiCol_TitleBgCollapsed] = rgb(12, 12, 13);
    c[ImGuiCol_ScrollbarBg] = rgb(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarGrab] = rgb(56, 54, 56);
    c[ImGuiCol_ScrollbarGrabHovered] = rgb(80, 76, 78);
    c[ImGuiCol_ScrollbarGrabActive] = accent;
    c[ImGuiCol_CheckMark] = accent_hi;
    c[ImGuiCol_SliderGrab] = accent;
    c[ImGuiCol_SliderGrabActive] = accent_hi;
    c[ImGuiCol_Button] = rgb(34, 33, 35);
    c[ImGuiCol_ButtonHovered] = rgb(52, 48, 50);
    c[ImGuiCol_ButtonActive] = accent;
    c[ImGuiCol_Header] = rgb(44, 40, 42);
    c[ImGuiCol_HeaderHovered] = rgb(56, 48, 50);
    c[ImGuiCol_HeaderActive] = accent;
    c[ImGuiCol_Separator] = rgb(40, 38, 40);
    c[ImGuiCol_SeparatorHovered] = accent;
    c[ImGuiCol_SeparatorActive] = accent_hi;
    c[ImGuiCol_ResizeGrip] = rgb(0, 0, 0, 0);
    c[ImGuiCol_ResizeGripHovered] = rgb(56, 54, 56);
    c[ImGuiCol_ResizeGripActive] = accent;
    c[ImGuiCol_Tab] = rgb(0, 0, 0, 0);
    c[ImGuiCol_TabHovered] = rgb(36, 34, 36);
    c[ImGuiCol_TabSelected] = rgb(30, 30, 32);
    c[ImGuiCol_TabSelectedOverline] = accent;
    c[ImGuiCol_TabDimmed] = rgb(0, 0, 0, 0);
    c[ImGuiCol_TabDimmedSelected] = rgb(30, 30, 32);
    c[ImGuiCol_TabDimmedSelectedOverline] = accent;
    c[ImGuiCol_TableHeaderBg] = rgb(24, 23, 25);
    c[ImGuiCol_TableBorderStrong] = rgb(40, 38, 40);
    c[ImGuiCol_TableBorderLight] = rgb(32, 30, 32);
    c[ImGuiCol_TableRowBgAlt] = rgb(255, 255, 255, 0.025f);
    c[ImGuiCol_PlotLines] = accent_hi;
    c[ImGuiCol_PlotLinesHovered] = rgb(255, 120, 90);
    c[ImGuiCol_TextSelectedBg] = rgb(196, 30, 40, 0.45f);
    c[ImGuiCol_NavHighlight] = rgb(224, 200, 196);
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

void DisplayTab(Settings& s, const Settings& before) {
    if (Choice<DisplayMode>("Display mode", s.display_mode,
                            {{"Fullscreen", DisplayMode::Fullscreen},
                             {"Borderless", DisplayMode::Borderless},
                             {"Windowed", DisplayMode::Windowed}}))
        DisplayChanged(before);
    // mechassault-recomp patch: which display; a new one starts at its desktop resolution and rate
    static std::vector<Monitor> monitors;
    bool new_monitor = false;
    if (ImGui::IsWindowAppearing() || monitors.empty()) monitors = Monitors();
    if (monitors.size() > 1) {
        std::vector<std::pair<std::string, std::string>> list{{"Automatic", ""}};
        for (const Monitor& m : monitors)
            list.push_back({"Display " + std::to_string(m.number) + "  (" + std::to_string(m.width) + " x " +
                                std::to_string(m.height) + (m.primary ? ", main)" : ")"),
                            m.device});
        if (Choice<std::string>("Monitor", s.monitor, list)) {
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
    if (Choice<Resolution>(s.display_mode == DisplayMode::Windowed ? "Window size" : "Resolution", cur, res)) {
        s.width = cur.w;
        s.height = cur.h;
        DisplayChanged(before);
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
        for (uint32_t r : rates) hz.push_back({std::to_string(r) + " Hz" + (EvenFor60(r) ? "  (even)" : ""), r});
        if (Choice<uint32_t>("Refresh rate", s.refresh_hz, hz)) DisplayChanged(before);
        Hint("Rates marked even show every frame for the same time: the smoothest motion.");
    }
    // breakdown-recomp patch: Breakdown draws 4:3 whatever the console reports: Widescreen widens its camera
    // (src/game/widescreen.cpp), the stretched shapes stretch the 4:3 picture
    if (Choice<uint32_t>("Aspect ratio", s.aspect_ratio,
                         {{"Widescreen (fits your screen)", kAspectWide},
                          {"16:9", kAspectWide169},
                          {"21:9", kAspectWide219},
                          {"4:3, as on the Xbox", kAspect43},
                          {"16:9, stretched", kAspect169},
                          {"Fill, stretched", kAspectFill}}))
        Changed();
    Hint("Widescreen shows more of the scene at the sides, nothing stretched. Menus and movies stay 4:3.");
    if (s.aspect_ratio == kAspectWide || s.aspect_ratio == kAspectWide169 || s.aspect_ratio == kAspectWide219) {
        // mechassault-recomp patch: where the HUD goes (d3d12 HudShift)
        bool edges = s.hud_edges;
        if (Choice<bool>("HUD", edges, {{"At the screen's edges", true}, {"Around the middle", false}})) {
            s.hud_edges = edges;
            Changed();
        }
    }
    // mechassault-recomp patch: brightness as a gamma curve on the finished picture
    if (Slider("Brightness", s.brightness, 50, 200, "%d%%")) Changed();
    ImGui::Spacing();
    bool vsync = s.vsync;
    if (Toggle("V-Sync", vsync)) {
        s.vsync = vsync;
        Changed();
    }
    if (Choice<uint32_t>("Frame rate", s.fps_cap,
                         {{"60", 60}, {"120", 120}, {"144", 144}, {"30", 30}, {"Display refresh rate", kFpsCapRefresh},
                          {"Unlimited", 0}}))
        Changed();
    Hint("The game keeps its own speed at any frame rate. On a display that 60 fps does not divide evenly, use G-Sync "
         "or FreeSync, or a refresh rate marked even, for the smoothest motion.");
    if (Choice<uint32_t>("FPS counter", s.perf_overlay, {{"Off", 0}, {"On", 1}, {"With details", 2}})) Changed();
    Hint("F2 switches it.");
}

void GraphicsTab(Settings& s) {
    std::vector<std::pair<std::string, uint32_t>> scales{{LowEndGpu() ? "Auto (up to 2x)" : "Auto", 0}};
    for (uint32_t i = 1; i <= kMaxRenderScale; i++) {
        char name[48];
        std::snprintf(name, sizeof(name), "%ux  (%u x %u)", i, 640 * i, 480 * i);
        scales.push_back({name, i});
    }
    if (Choice<uint32_t>("Render scale", s.render_scale, scales)) {
        if (!s.render_scale) s.render_percent = 100;
        Changed();
    }
    Hint("Auto draws as many pixels as your screen shows. 1x is the Xbox's own 640 x 480; above your screen's "
         "resolution it also smooths edges.");
    if (Choice<uint32_t>("Anti-aliasing", s.anti_aliasing, {{"Off", 0}, {"FXAA", 1}, {"SMAA", 2}})) Changed();
    Hint("SMAA smooths edges and keeps textures sharp; FXAA is softer.");
    if (Choice<uint32_t>("Texture filtering", s.aniso,
                         {{"As the game", 0}, {"2x anisotropic", 2}, {"4x anisotropic", 4}, {"8x anisotropic", 8},
                          {"16x anisotropic", 16}}))
        Changed();
    if (Slider("Texture sharpness", s.texture_detail, 0, 15, s.texture_detail ? "+%d" : "As the game")) Changed();
    Hint("Sharper texture levels at a distance. High values can shimmer while moving.");
    ImGui::Spacing();
    // mechassault-recomp patch: FSR 1 (own implementation of its algorithm) for a render smaller than the picture
    if (Choice<uint32_t>("Upscaling", s.upscaler, {{"FSR 1", 1}, {"Bilinear", 0}})) Changed();
    Hint("Only when the render scale is below your screen's resolution.");
    if (s.upscaler == 1)
        if (Slider("FSR sharpening", s.fsr_sharpness, 0, 20, "%d")) Changed();
    if (Slider("Sharpening", s.sharpen, 0, 20, s.sharpen ? "%d" : "Off")) Changed();
    Hint("Contrast-adaptive sharpening (RCAS) on the finished picture, when FSR is not upscaling.");
}

// mechassault-recomp patch: System Link's network (app/network.cpp, xdk/xnet.cpp)
void NetworkTab(Settings& s) {
    static std::vector<NetAdapter> adapters;
    if (ImGui::IsWindowAppearing() || adapters.empty()) adapters = NetworkAdapters();
    std::vector<std::pair<std::string, std::string>> list{{"Automatic", ""}};
    for (const NetAdapter& n : adapters) list.push_back({n.name + "  (" + n.ip + ")", n.ip});
    if (Choice<std::string>("Network", s.lan_adapter, list)) Changed();
    Hint("For a virtual LAN (Radmin VPN, ZeroTier, Hamachi, Tailscale) pick its adapter on every PC. Applies the next "
         "time you open System Link.");
    static char join[128] = {};
    static bool loaded = false;
    if (!loaded || ImGui::IsWindowAppearing()) {
        std::snprintf(join, sizeof(join), "%s", s.join_ip.c_str());
        loaded = true;
    }
    Row("Join address");
    if (ImGui::InputTextWithHint("##join", "a host's internet address", join, sizeof(join))) {
        s.join_ip = join;
        Changed();
    }
    Hint("To join a host over the internet without a virtual LAN. The host forwards UDP ports 1000 to 1002.");
    ImGui::Spacing();
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("Host and join from the game's own Multiplayer > System Link.");
    ImGui::PopTextWrapPos();
}

void Recommended(Settings& s) {
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

void Menu() {
    Settings& s = Get();
    const Settings before = s;  // what a display change can go back to
    ImGuiIO& io = ImGui::GetIO();
    float u = ImGui::GetFontSize();
    g_label_w = u * 13;
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Appearing,
                            ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(u * 40, std::min(u * 38, io.DisplaySize.y * 0.9f)), ImGuiCond_Appearing);
    if (!g_menu_drawn) ImGui::SetNextWindowFocus();  // a controller can move in it at once
    g_menu_drawn = true;
    static bool popup_before = false;  // B that closed a list this frame must not close the menu too
    bool open = true;
    if (ImGui::Begin("##settings", &open, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoSavedSettings)) {
        // mechassault-recomp patch: the title's own name, not Breakdown's (the shared layer never names a title)
        std::string title;
        for (const wchar_t* c = kGame.short_title; *c; c++) title += *c < 0x80 ? char(*c) : '?';
        for (char& ch : title) ch = char(std::toupper(static_cast<unsigned char>(ch)));
        ImGui::TextDisabled("%s", title.c_str());
        ImGui::SameLine();
        ImGui::TextUnformatted("Settings");
        ImGui::SameLine(ImGui::GetContentRegionMax().x - ImGui::GetFrameHeight());
        if (ImGui::Button("x", ImVec2(ImGui::GetFrameHeight(), 0))) open = false;
        ImGui::Spacing();
        float footer = ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y;
        if (ImGui::BeginTabBar("##tabs", ImGuiTabBarFlags_NoTooltip)) {
            auto tab = [&](const char* name, auto draw) {
                if (!ImGui::BeginTabItem(name, nullptr, TabFlags(name))) return;
                ImGui::BeginChild("##page", ImVec2(0, -footer), ImGuiChildFlags_None, ImGuiWindowFlags_NavFlattened);
                ImGui::Spacing();
                draw();
                ImGui::EndChild();
                ImGui::EndTabItem();
            };
            tab("Display", [&] { DisplayTab(s, before); });
            tab("Graphics", [&] { GraphicsTab(s); });
            if (kGame.menu) {
                // breakdown-recomp patch: the game's own tabs (MenuPage, overlay.h)
                g_page_footer = footer;
                kGame.menu();
            }
            tab("Network", [&] { NetworkTab(s); });
            ImGui::EndTabBar();
        }
        ImGui::Separator();
        if (ImGui::Button("Recommended")) Recommended(s);
        Hint(LowEndGpu() ? "Borderless at your desktop resolution, render scale Auto up to 2x, FXAA, FSR 1, V-Sync, "
                           "60 fps, widescreen (integrated graphics found)."
                         : "Borderless at your desktop resolution, render scale Auto, FXAA, FSR 1, V-Sync, 60 fps, "
                           "widescreen.");
        ImGui::SameLine();
        ImGui::TextDisabled("  F1 or Back + Start closes");
        float close_w = ImGui::CalcTextSize("Close").x + ImGui::GetStyle().FramePadding.x * 2;
        ImGui::SameLine(ImGui::GetContentRegionMax().x - close_w);
        if (ImGui::Button("Close")) open = false;
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

void MenuRow(const char* label) { Row(label); }
void MenuHint(const char* text) { Hint(text); }
bool MenuPage(const char* name) {
    if (!ImGui::BeginTabItem(name, nullptr, TabFlags(name))) return false;
    ImGui::BeginChild("##page", ImVec2(0, -g_page_footer), ImGuiChildFlags_None, ImGuiWindowFlags_NavFlattened);
    ImGui::Spacing();
    return true;
}
void EndMenuPage() {
    ImGui::EndChild();
    ImGui::EndTabItem();
}

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
