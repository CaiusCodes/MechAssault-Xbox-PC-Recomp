// the settings file beside the exe
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <fstream>
#include <mutex>
#include <sstream>

#include <toml++/toml.hpp>

#include "app/app.h"
#include "core/log.h"

namespace app {

namespace {

const char* kModes[] = {"fullscreen", "borderless", "windowed"};

std::filesystem::path File() { return Paths().settings / kGame.settings; }  // breakdown-recomp patch: Paths()

}  // namespace

Settings& Get() {
    static Settings s;
    return s;
}

void Load() {
    Settings& s = Get();
    toml::table t;
    try {
        t = toml::parse_file(File().string());
    } catch (...) {
        return;
    }
    std::string mode = t["display_mode"].value_or(std::string(kModes[uint32_t(s.display_mode)]));
    for (uint32_t i = 0; i < 3; i++)
        if (mode == kModes[i]) s.display_mode = DisplayMode(i);
    s.width = t["width"].value_or(s.width);
    s.height = t["height"].value_or(s.height);
    s.render_percent = t["render_percent"].value_or(s.render_percent);
    s.fps_cap = t["fps_cap"].value_or(s.fps_cap);
    s.vsync = t["vsync"].value_or(s.vsync);
    s.aniso = t["anisotropic_filtering"].value_or(s.aniso);
    s.custom_textures = t["custom_textures"].value_or(s.custom_textures);
    s.dump_textures = t["dump_textures"].value_or(s.dump_textures);
    s.texture_dump_format = std::min(t["texture_dump_format"].value_or(s.texture_dump_format), 1u);
    s.join_ip = t["join_ip"].value_or(s.join_ip);
    // breakdown-recomp patch
    s.render_scale = std::min(t["render_scale"].value_or(s.render_scale), kMaxRenderScale);
    s.perf_overlay = std::min(t["perf_overlay"].value_or(s.perf_overlay), 2u);
    s.diagnostics_log = t["diagnostics_log"].value_or(s.diagnostics_log);
    s.vblank_sync = std::min(t["vblank_sync"].value_or(s.vblank_sync), 1u);
    s.anti_aliasing = std::min(t["anti_aliasing"].value_or(s.anti_aliasing), 1u);
    s.refresh_hz = std::min(t["refresh_rate"].value_or(s.refresh_hz), 1000u);
    s.aspect_ratio = std::min(t["aspect_ratio"].value_or(s.aspect_ratio), kAspectFill);
    for (const Option& o : kGame.options) {
        if (o.flag) *o.flag = t[o.key].value_or(*o.flag);
        else *o.value = t[o.key].value_or(*o.value);
    }
    if (kGame.load) kGame.load(&t);
}

namespace {

toml::table Table() {
    const Settings& s = Get();
    toml::table t{{"display_mode", kModes[uint32_t(s.display_mode)]},
                  {"width", int64_t(s.width)},
                  {"height", int64_t(s.height)},
                  {"render_percent", int64_t(s.render_percent)},
                  {"fps_cap", int64_t(s.fps_cap)},
                  {"vsync", s.vsync},
                  {"anisotropic_filtering", int64_t(s.aniso)},
                  {"custom_textures", s.custom_textures},
                  {"dump_textures", s.dump_textures},
                  {"texture_dump_format", int64_t(s.texture_dump_format)},
                  {"join_ip", s.join_ip},
                  // breakdown-recomp patch
                  {"render_scale", int64_t(s.render_scale)},
                  {"perf_overlay", int64_t(s.perf_overlay)},
                  {"diagnostics_log", s.diagnostics_log},
                  {"vblank_sync", int64_t(s.vblank_sync)},
                  {"anti_aliasing", int64_t(s.anti_aliasing)},
                  {"refresh_rate", int64_t(s.refresh_hz)},
                  {"aspect_ratio", int64_t(s.aspect_ratio)}};
    for (const Option& o : kGame.options) {
        if (o.flag) t.insert(o.key, *o.flag);
        else t.insert(o.key, int64_t(*o.value));
    }
    if (kGame.save) kGame.save(&t);
    return t;
}

}  // namespace

void Save() {
    std::ofstream f(File());
    f << Table() << "\n";
}

void LogSettings() {
    std::ostringstream text;
    text << Table();
    std::istringstream lines(text.str());
    LOGR("Settings:");
    for (std::string line; std::getline(lines, line);)
        if (!line.empty()) LOGR("  %s", line.c_str());
}

Mode Desktop() {
    HMONITOR mon = MonitorFromWindow(static_cast<HWND>(Window()), MONITOR_DEFAULTTOPRIMARY);
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    if (GetMonitorInfoW(mon, &mi) && EnumDisplaySettingsW(mi.szDevice, ENUM_REGISTRY_SETTINGS, &dm))
        return {dm.dmPelsWidth, dm.dmPelsHeight};
    return {uint32_t(GetSystemMetrics(SM_CXSCREEN)), uint32_t(GetSystemMetrics(SM_CYSCREEN))};
}

Mode Output() {
    const Settings& s = Get();
    if (s.width && s.height) return {s.width, s.height};
    return Desktop();
}

bool Widescreen() {
    const Settings& s = Get();
    if (s.aspect_ratio != kAspectFill) return s.aspect_ratio == kAspect169;
    // asked per frame: the desktop mode lookup only runs when the resolution setting changes
    static std::atomic<uint64_t> key{~0ull};
    static std::atomic<bool> wide{true};
    uint64_t k = uint64_t(s.width) << 32 | s.height;
    if (key.exchange(k) != k) {
        Mode m = Output();
        wide = m.width * 2 >= m.height * 3;  // 3:2 or wider
    }
    return wide;
}

float PictureAspect() {
    switch (Get().aspect_ratio) {
        case kAspectWide: return 4.0f / 3.0f * WideView();
        case kAspect169: return 16.0f / 9.0f;
        case kAspectFill: return 0;
        default: return 4.0f / 3.0f;
    }
}

// Output for every frame: the desktop mode lookup reads the registry, so it runs again only when the resolution
// setting or the window's monitor changes
Mode CachedOutput() {
    struct Key {
        uint32_t width;
        uint32_t height;
        HMONITOR monitor;
        bool operator==(const Key&) const = default;
    };
    static std::mutex lock;
    static Key key{};
    static Mode mode{};
    const Settings& s = Get();
    Key now{s.width, s.height, MonitorFromWindow(static_cast<HWND>(Window()), MONITOR_DEFAULTTOPRIMARY)};
    std::lock_guard<std::mutex> guard(lock);
    if (!(now == key) || !mode.height) {
        key = now;
        mode = Output();
    }
    return mode;
}

float WideView() {
    if (Get().aspect_ratio != kAspectWide) return 1;
    Mode m = CachedOutput();
    if (!m.width || !m.height) return 1;
    // up to 32:9; a display narrower than 4:3 (5:4) keeps the 4:3 view
    return std::clamp(float(m.width) / float(m.height) / (4.0f / 3.0f), 1.0f, 8.0f / 3.0f);
}

float RenderScale(uint32_t guest_height) {
    // breakdown-recomp patch: an integer multiple of the title's own resolution
    if (uint32_t n = Get().render_scale) return float(std::min(n, kMaxRenderScale));
    // Auto: as many pixels as the picture gets on screen. Widescreen and the stretched modes show the 4:3 render
    // wider than tall, so the shown width decides (3440x1440 renders 3440x2580 and is shrunk to the height)
    Mode m = CachedOutput();
    float aspect = PictureAspect();
    float shown_w = aspect > 0 ? std::min(float(m.width), m.height * aspect) : float(m.width);
    float h = std::max(float(m.height), shown_w * 0.75f) * (Get().render_percent / 100.0f);
    return guest_height ? std::max(h / guest_height, 0.1f) : 1.0f;
}

}  // namespace app
