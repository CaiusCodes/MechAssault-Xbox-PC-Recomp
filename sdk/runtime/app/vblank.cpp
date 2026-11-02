// breakdown-recomp patch: the console's vertical blank, locked to the monitor's own.
//
// The title paces itself by the vertical blank (Breakdown: every second blank in play, every blank in menus). A host
// timer at 60.000 Hz drifts against a 59.94 Hz or 120 Hz display, and every few seconds a frame stays up one
// refresh too long or too short: the periodic hitch. When the monitor runs at a whole multiple of about 60 Hz, the
// emulated blank instead follows every k-th real blank, so each title frame covers the same number of refreshes.
// Other rates (144, 165 Hz) have no such multiple and keep the timer. On a 59.94 Hz display the title runs 0.1%
// slower than on a 60.000 Hz timer, as on an NTSC console.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dxgi.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <mutex>
#include <string>

#include "app/app.h"
#include "core/log.h"

#pragma comment(lib, "dxgi.lib")

namespace app {

namespace {

using Microsoft::WRL::ComPtr;
using clock = std::chrono::steady_clock;

constexpr double kConsoleMs = 1000.0 / 59.94;  // NTSC field rate; 60.000 is accepted as well
constexpr double kTolerance = 0.02;            // the multiple of the display period may be 2% off 59.94 Hz

struct Output {
    ComPtr<IDXGIOutput> out;
    HMONITOR monitor = nullptr;
};

Output g_out;
ComPtr<IDXGIFactory1> g_factory;
enum class Phase { Measuring, Locked, Timer };
Phase g_phase = Phase::Measuring;
double g_period_ms = 0;     // measured display period (moving average)
int g_every = 0;            // real blanks per console blank while locked
int g_samples = 0;
int g_fast = 0;             // waits that returned far too soon (occluded, minimized, display off)
clock::time_point g_last{};
clock::time_point g_check{};  // next look at the window's monitor and its mode
uint32_t g_hz = 0;            // the mode's nominal rate when last looked at

void Restart(const char* why) {
    g_phase = Phase::Measuring;
    g_period_ms = 0;
    g_samples = g_fast = 0;
    g_last = {};
    LOGR("vblank: measuring the display (%s)", why);
}

bool FindOutput(HMONITOR mon) {
    g_out = {};
    if (!g_factory && FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&g_factory)))) return false;
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT a = 0; g_factory->EnumAdapters1(a, &adapter) != DXGI_ERROR_NOT_FOUND; a++) {
        ComPtr<IDXGIOutput> out;
        for (UINT o = 0; adapter->EnumOutputs(o, &out) != DXGI_ERROR_NOT_FOUND; o++) {
            DXGI_OUTPUT_DESC d{};
            if (SUCCEEDED(out->GetDesc(&d)) && d.Monitor == mon) {
                g_out = {out, mon};
                return true;
            }
        }
    }
    return false;
}

// once a second: the window moved to another monitor or its mode changed, measure again
void Check(clock::time_point now) {
    if (now < g_check) return;
    g_check = now + std::chrono::seconds(1);
    HMONITOR mon = MonitorFromWindow(static_cast<HWND>(Window()), MONITOR_DEFAULTTOPRIMARY);
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    uint32_t hz = GetMonitorInfoW(mon, &mi) && EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm)
                      ? dm.dmDisplayFrequency
                      : 0;
    if (mon == g_out.monitor && hz == g_hz && g_out.out) return;
    g_hz = hz;
    if (!FindOutput(mon)) {
        g_phase = Phase::Timer;
        LOGR("vblank: no DXGI output for the window's monitor, console blank from the 60 Hz timer");
        return;
    }
    // the mode's nominal rate first: no measuring on a display that cannot be a multiple (50, 75, 144, 165 Hz)
    bool maybe = false;
    for (int k = 1; k <= 4; k++)
        if (hz && std::abs(int(hz) - 60 * k) <= 1) maybe = true;
    if (!maybe && hz > 1) {
        g_phase = Phase::Timer;
        LOGR("vblank: display at %u Hz, no whole multiple of 60 Hz, console blank from the 60 Hz timer", hz);
        return;
    }
    Restart("new monitor or mode");
}

void Decide() {
    int every = int(std::lround(kConsoleMs / g_period_ms));
    bool ok = every >= 1 && every <= 4 && std::fabs(every * g_period_ms - kConsoleMs) <= kConsoleMs * kTolerance;
    Phase was = g_phase;
    int was_every = g_every;
    g_phase = ok ? Phase::Locked : Phase::Timer;
    g_every = ok ? every : 0;
    if (g_phase != was || g_every != was_every) {
        if (ok)
            LOGR("vblank: display %.3f Hz, console blank on every %d display blank(s), %.3f Hz", 1000.0 / g_period_ms,
                 every, 1000.0 / (every * g_period_ms));
        else
            LOGR("vblank: display %.3f Hz, no whole multiple of 60 Hz, console blank from the 60 Hz timer",
                 1000.0 / g_period_ms);
    }
}

// one real blank: false when it is not a real one
bool WaitOne() {
    if (FAILED(g_out.out->WaitForVBlank())) {
        g_out = {};
        g_phase = Phase::Timer;
        g_check = clock::now() + std::chrono::seconds(2);
        return false;
    }
    auto t = clock::now();
    if (g_last != clock::time_point{}) {
        double ms = std::chrono::duration<double, std::milli>(t - g_last).count();
        if (ms < 2.0) {  // sooner than any display: no real blank behind it
            if (++g_fast > 8) {
                LOGR("vblank: display blanks unavailable (window hidden or display off), 60 Hz timer for now");
                g_phase = Phase::Timer;
                g_out = {};  // looked up again at the next check, measured again
                g_check = t + std::chrono::seconds(2);
                g_last = {};
                return false;
            }
        } else {
            g_fast = 0;
            // a missed blank is a multiple of the period, not a sample of it
            if (ms < 50.0 && (!g_period_ms || ms < g_period_ms * 1.5)) {
                g_period_ms = g_period_ms ? g_period_ms * 0.95 + ms * 0.05 : ms;
                ++g_samples;
            }
        }
    }
    g_last = t;
    return true;
}

}  // namespace

// waits for the next console blank on the display's own blanks; false without waiting (or after one short
// measuring wait): the caller keeps its timer
bool WaitDisplayVBlank() {
    if (Get().vblank_sync != 0) {
        g_out = {};
        g_hz = 0;
        return false;
    }
    Check(clock::now());
    switch (g_phase) {
        case Phase::Timer:
            return false;
        case Phase::Measuring:
            if (!g_out.out || !WaitOne()) return false;
            if (g_samples >= 30) Decide();
            return false;
        case Phase::Locked:
            for (int i = 0; i < g_every; i++)
                if (!g_out.out || !WaitOne()) return false;
            if (g_samples % 600 == 0) Decide();  // every 10 s or so: still the same rate
            return true;
    }
    return false;
}

// measured display rate in Hz and blanks per console blank (0: timer), for the diagnostics overlay
double DisplayHz() { return g_period_ms ? 1000.0 / g_period_ms : 0; }
int VBlankEvery() { return g_phase == Phase::Locked ? g_every : 0; }

}  // namespace app
