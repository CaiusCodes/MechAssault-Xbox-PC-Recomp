// mechassault-recomp: what an F8 bug mark records about MechAssault (GameInfo::bug_state, sdk/runtime/app/bug_mark.cpp):
// the release, whether a mission is being played and which campaign node the profile is on, player 1's 'Mech heading
// and view pitch, and pad 1's input over the last five seconds (each change, with how long ago).
#include <chrono>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

#include "common/release.h"
#include "core/input.h"
#include "kernel/kernel.h"

namespace mechassault {
bool InPlay();  // src/game/campaign.cpp

namespace {

using Clock = std::chrono::steady_clock;
struct Seen {
    Clock::time_point at;
    input::Pad pad;
};
std::mutex g_lock;
std::deque<Seen> g_recent;  // pad 1 as the game got it, each change, the last five seconds

bool Same(const input::Pad& a, const input::Pad& b) {
    return a.buttons == b.buttons && std::memcmp(a.analog, b.analog, sizeof(a.analog)) == 0 && a.lx / 4096 == b.lx / 4096 &&
           a.ly / 4096 == b.ly / 4096 && a.rx / 4096 == b.rx / 4096 && a.ry / 4096 == b.ry / 4096;
}

float F(uint32_t va) {
    uint32_t bits = kernel::R32(va);
    float f;
    std::memcpy(&f, &bits, 4);
    return f;
}

std::string Text(uint32_t va, size_t max) {
    std::string s;
    for (size_t i = 0; i < max; i++) {
        uint8_t c = kernel::G<uint8_t>(va + uint32_t(i));
        if (!c) break;
        s += c >= 32 && c < 127 ? char(c) : '?';
    }
    return s;
}

}  // namespace

// the pad hook's last step (src/common/game_info.cpp): pad 1 as the game gets it
void RecordPad(const input::Pad& pad) {
    std::lock_guard l(g_lock);
    auto now = Clock::now();
    if (g_recent.empty() || !Same(g_recent.back().pad, pad)) g_recent.push_back({now, pad});
    while (g_recent.size() > 1 && now - g_recent.front().at > std::chrono::seconds(5)) g_recent.pop_front();
}

std::vector<std::string> BugState() {
    std::vector<std::string> out;
    const GameRelease& r = Current();
    char line[256];
    std::snprintf(line, sizeof(line), "Release: %s", r.name);
    out.push_back(line);
    out.push_back(std::string("In a mission: ") + (InPlay() ? "yes" : "no (menus, a movie or paused)"));
    out.push_back("Campaign node: " + Text(r.progress + 0x14, 64) + " (" + Text(r.progress + 0x54, 64) + ")");
    uint32_t pilot = kernel::R32(r.pilot);
    uint32_t mech = pilot && kernel::R32(pilot) == r.pilot_vtable ? kernel::R32(pilot + 0x10) : 0;
    if (mech && kernel::R32(mech) == r.mech_vtable) {
        uint32_t control = kernel::R32(mech + 0xC);
        for (int hop = 0; hop < 4 && control && kernel::R32(control) != r.control_vtable; hop++)
            control = kernel::R32(control + 0xC);
        bool has_control = control && kernel::R32(control) == r.control_vtable;
        std::snprintf(line, sizeof(line), "'Mech %08X: heading %.1f degrees, view pitch %s", mech, F(mech + 0x2B0),
                      has_control ? (std::to_string(F(control + 0x178)) + " degrees").c_str() : "(no controller)");
        out.push_back(line);
    } else {
        out.push_back("'Mech: none in play");
    }
    std::lock_guard l(g_lock);
    out.push_back("Pad 1, the last 5 seconds (seconds ago: buttons, A B X Y Bk Wh LT RT, sticks):");
    auto now = Clock::now();
    for (const Seen& s : g_recent) {
        const input::Pad& p = s.pad;
        std::snprintf(line, sizeof(line), "  -%4.1f: %04X  %3u %3u %3u %3u %3u %3u %3u %3u  L %6d %6d  R %6d %6d",
                      std::chrono::duration<double>(now - s.at).count(), p.buttons, p.analog[0], p.analog[1],
                      p.analog[2], p.analog[3], p.analog[4], p.analog[5], p.analog[6], p.analog[7], p.lx, p.ly, p.rx,
                      p.ry);
        out.push_back(line);
    }
    return out;
}

}  // namespace mechassault
