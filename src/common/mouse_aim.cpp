// Mouse aiming for MechAssault, first version: the mouse drives pad 1's right stick, which the game reads as Look
// up/down and Rotate left/right (the torso; its Normal thumbstick layout). The game turns the torso at a rate set by
// the stick, so each poll the mouse counts since the last one are turned into the deflection that would turn that far
// in the time since the last poll, past the stick's dead zone; what does not fit under full deflection carries into
// the next polls, so a quick flick finishes at the stick's top rate instead of being lost. A real pad's right stick
// wins. The window, raw input, cursor capture and the menu come from the Breakdown port's mouse look; Breakdown's
// version wrote into the game's own controller object, which MechAssault does not have mapped yet
// (KNOWN_ISSUES.md: mouse aiming is a stick emulation).
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include "core/input.h"
#include "core/log.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <imgui.h>
#include "app/app.h"
#endif

namespace mechassault {

void OnWheel(int delta);  // keyboard_pad.cpp: the wheel as d-pad presses
void BindingsMenu();      // keyboard_pad.cpp

// settings.toml: mouse_aim, mouse_sensitivity (percent; 100 = 0.05 degrees per count), mouse_invert_y
bool g_mouse_aim = true;
uint32_t g_mouse_sensitivity = 100;
bool g_mouse_invert_y = false;

namespace {

std::atomic<int32_t> g_dx{0}, g_dy{0};  // raw counts the game has not taken yet

// The stick model, from measurements: the game's pad code (0x276C0) scales each axis by 1/32768, zeroes it inside a
// dead zone of 0.30 (0x33D90C) and stretches the rest back to 0..1 ((|v| - 0.3) / 0.7). The torso then turns at the
// stick's share of a top rate, reached through a ramp: holding the right stick fully right from rest turned it about
// 45 degrees in 0.5 s and 120-125 in 1 s (HUD compass, Mission 1, 60 fps), which fits a top rate of 150 degrees a
// second reached after 0.4 s (375 deg/s^2), and it stopped when the stick was let go. Mouse turns of 45 and 90
// degrees driven without the ramp came out at about 20 and 60, so the model below follows it.
constexpr float kFullRate = 150.0f;   // degrees a second at full deflection
constexpr float kRamp = 375.0f;       // degrees a second, per second
constexpr float kDeadZone = 0.302f;
constexpr float kMaxPending = 180.0f;  // degrees the mouse may run ahead of the torso

struct Axis {
    float pending = 0;  // degrees the mouse asked for that the torso has not turned yet
    float rate = 0;     // the torso's rate as the model predicts it, degrees a second, signed
};

// the deflection (signed) for this poll. The game ramps the torso's rate toward the stick's share of the top rate by
// itself, so the stick asks for the rate wanted now (all of the pending turn this poll, at most the top rate) and the
// model follows the game's ramp to know how much it really turns; letting go stops it at once, as measured.
int16_t Deflection(Axis& a, float dt) {
    if (std::fabs(a.pending) < 0.05f || dt <= 0) {
        a.rate = 0;
        return 0;
    }
    float want = std::min(std::fabs(a.pending) / dt, kFullRate);
    float from = (a.rate > 0) == (a.pending > 0) ? std::fabs(a.rate) : 0.0f;
    float rate = std::min(want, from + kRamp * dt);  // what the torso turns at this poll
    a.rate = std::copysign(rate, a.pending);
    a.pending -= a.rate * dt;
    float share = want / kFullRate;
    return int16_t(std::copysign((kDeadZone + share * (1.0f - kDeadZone)) * 32767.0f, a.rate));
}

#ifdef _WIN32
bool g_captured = false;

void Capture(HWND hwnd) {
    if (g_captured) return;
    RECT r;
    GetClientRect(hwnd, &r);
    MapWindowPoints(hwnd, nullptr, reinterpret_cast<POINT*>(&r), 2);
    ClipCursor(&r);
    while (ShowCursor(FALSE) >= 0) {
    }
    g_captured = true;
}

void Release() {
    if (!g_captured) return;
    ClipCursor(nullptr);
    while (ShowCursor(TRUE) < 0) {
    }
    g_captured = false;
    g_dx = 0;
    g_dy = 0;
}
#endif

}  // namespace

// pad 1, after the keyboard: the mouse's share of the right stick
void MouseAimPad(int port, input::Pad& pad) {
    if (port != 0) return;
    using clock = std::chrono::steady_clock;
    static clock::time_point last = clock::now();
    static Axis ax, ay;
    auto now = clock::now();
    float dt = std::clamp(std::chrono::duration<float>(now - last).count(), 0.0f, 0.1f);
    last = now;
    int32_t dx = g_dx.exchange(0), dy = g_dy.exchange(0);
    // test aid: XBE_MOUSE_TEST="dx,dy,from,to" adds those counts at every poll from second from to second to (since
    // the first poll), as if the mouse moved, for unattended runs (no real mouse movement is made)
    static const char* test = std::getenv("XBE_MOUSE_TEST");
    static float t_args[4] = {};
    static bool t_on = test && std::sscanf(test, "%f,%f,%f,%f", &t_args[0], &t_args[1], &t_args[2], &t_args[3]) == 4;
    static const clock::time_point t0 = now;
    if (t_on) {
        float t = std::chrono::duration<float>(now - t0).count();
        if (t >= t_args[2] && t < t_args[3]) {
            dx += int32_t(t_args[0]);
            dy += int32_t(t_args[1]);
        }
    }
    if (!g_mouse_aim) {
        ax = ay = {};
        return;
    }
    float k = 0.05f * float(g_mouse_sensitivity) / 100.0f;
    ax.pending = std::clamp(ax.pending + float(dx) * k, -kMaxPending, kMaxPending);
    // the stick's up is positive: mouse up (negative counts) looks up unless inverted
    ay.pending = std::clamp(ay.pending - float(dy) * k * (g_mouse_invert_y ? -1.0f : 1.0f), -kMaxPending, kMaxPending);
    // the pad's own right stick wins; the mouse's backlog is dropped
    if (pad.rx == 0) pad.rx = Deflection(ax, dt);
    else ax = {};
    if (pad.ry == 0) pad.ry = Deflection(ay, dt);
    else ay = {};
}

// the game window is up: raw mouse input to it
void MouseWindow(void* hwnd) {
#ifdef _WIN32
    RAWINPUTDEVICE dev{0x01, 0x02, 0, static_cast<HWND>(hwnd)};  // generic desktop, mouse
    if (!RegisterRawInputDevices(&dev, 1, sizeof(dev))) LOGR("mouse: raw input not available (%lu)", GetLastError());
#else
    (void)hwnd;
#endif
}

// window thread: raw mouse counts while the window has the cursor; the cursor is held in the window while mouse aim is
// on, the window is in front and the F1 menu is closed
bool MouseMessage(void* hwnd_, uint32_t msg, uintptr_t wp, intptr_t lp, intptr_t* result) {
#ifdef _WIN32
    HWND hwnd = static_cast<HWND>(hwnd_);
    (void)result;
    switch (msg) {
        case WM_INPUT: {
            bool want = g_mouse_aim && !app::MenuOpen() && GetForegroundWindow() == hwnd && !IsIconic(hwnd);
            if (!want) {
                Release();
                break;
            }
            RAWINPUT in;
            UINT size = sizeof(in);
            if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lp), RID_INPUT, &in, &size, sizeof(RAWINPUTHEADER)) == UINT(-1))
                break;
            if (in.header.dwType != RIM_TYPEMOUSE || (in.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE)) break;
            if (!g_captured) {
                Capture(hwnd);  // the first movement takes the cursor; that one is not a turn
                break;
            }
            g_dx += in.data.mouse.lLastX;
            g_dy += in.data.mouse.lLastY;
            break;
        }
        case WM_ACTIVATE:
            if (LOWORD(wp) == WA_INACTIVE) Release();
            break;
        case WM_KEYDOWN:
            if (wp == VK_F1) Release();  // the settings menu opens: the cursor is the menu's
            break;
        case WM_MOUSEWHEEL:
            OnWheel(GET_WHEEL_DELTA_WPARAM(wp));  // d-pad presses (keyboard_pad.cpp); ImGui has had it already
            break;
        case WM_KILLFOCUS:
        case WM_DESTROY:
            Release();
            break;
        case WM_SIZE:
        case WM_MOVE:
            if (g_captured) {
                Release();
                Capture(hwnd);
            }
            break;
    }
#else
    (void)hwnd_, (void)msg, (void)wp, (void)lp, (void)result;
#endif
    return false;
}

// the F1 menu's Controls section
void MouseMenu() {
#ifdef _WIN32
    if (!ImGui::CollapsingHeader("Controls", ImGuiTreeNodeFlags_DefaultOpen)) return;
    bool changed = ImGui::Checkbox("Mouse aim", &g_mouse_aim);
    int sens = int(g_mouse_sensitivity);
    if (ImGui::SliderInt("Mouse sensitivity", &sens, 10, 400, "%d%%")) {
        g_mouse_sensitivity = uint32_t(sens);
        changed = true;
    }
    changed |= ImGui::Checkbox("Invert mouse Y", &g_mouse_invert_y);
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("The mouse turns the torso and looks up and down through the game's right stick. The cursor "
                       "is held while the game is in front.");
    ImGui::PopStyleColor();
    if (changed) app::Save();
    BindingsMenu();
#endif
}

}  // namespace mechassault
