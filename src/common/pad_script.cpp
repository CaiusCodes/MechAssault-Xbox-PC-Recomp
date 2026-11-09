// mechassault-recomp: scripted pad 1 for unattended Windows test runs (the Linux harness has its own). XBE_PAD_SCRIPT
// names a text file of lines "<seconds> <control> [hold ms, default 150] [value]", seconds from the first poll;
// controls: a b x y black white lt rt start back l3 r3 up down left right, lx ly rx ry (value -32768..32767, held
// for the hold time); "dump" logs every guest thread's stack and starts an F3 frame capture (XBE_CAPTURE_DIR).
// "peek" logs the XBE_PEEK values only; "addf <chain> <amount>" / "logf <chain>" change / log a float through a pointer
// chain ("387C8C>10>26C"); "memdump <tag> [1]" writes the image's data (1: all committed memory) for
// scripts/memdiff.py.
// "menu" opens or closes the F1 settings menu; "set <key> <value>" changes a video setting live as the F1 menu would
// (render_scale, aspect_ratio, anti_aliasing, upscaler, fsr_sharpness, brightness, fps_cap, vsync, width, height,
// display_mode 0 fullscreen 1 borderless 2 windowed). '#' starts a comment. Merged into whatever the keyboard and controllers give.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "core/input.h"
#include "core/log.h"
#include "gpu/backend.h"
#include "gpu/prompts.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"

#include "app/app.h"

namespace app {
void ToggleMenu();  // app/overlay.cpp
}

namespace mechassault {

extern uint32_t g_fov_percent;  // src/game/widescreen.cpp

namespace {

struct Step {
    double at, until;
    std::string control;
    int value;
    std::string key;  // "set": the setting
    int port = 0;     // a control written "2:a" goes to controller port 2 (index 1)
    float add = 0;    // "addf": the amount
};

// "387C8C>10>26C": 0x387C8C, then for each '>' the pointer there plus that offset; 0 when a pointer is null
uint32_t Chain(const std::string& text) {
    uint32_t at = uint32_t(std::strtoul(text.c_str(), nullptr, 16));
    for (size_t p = text.find('>'); p != std::string::npos && at; p = text.find('>', p + 1)) {
        uint32_t ptr = kernel::R32(at);
        at = ptr ? ptr + uint32_t(std::strtoul(text.c_str() + p + 1, nullptr, 16)) : 0;
    }
    return at;
}

std::vector<Step> Load() {
    std::vector<Step> steps;
    const char* path = std::getenv("XBE_PAD_SCRIPT");
    if (!path) return steps;
    std::ifstream f(path);
    std::string line;
    while (std::getline(f, line)) {
        if (auto h = line.find('#'); h != std::string::npos) line.resize(h);
        std::istringstream s(line);
        Step st{};
        double hold = 150;
        if (!(s >> st.at >> st.control)) continue;
        if (st.control == "addf" || st.control == "logf") {
            s >> st.key >> st.add;
            st.until = st.at;
            steps.push_back(st);
            continue;
        }
        if (st.control == "set" || st.control == "memdump") {
            s >> st.key >> st.value;
            st.until = st.at;
            steps.push_back(st);
            continue;
        }
        s >> hold >> st.value;
        if (st.control.rfind("2:", 0) == 0) {
            st.port = 1;
            st.control.erase(0, 2);
        }
        st.until = st.at + hold / 1000.0;
        steps.push_back(st);
    }
    LOGR("pad script: %zu steps from %s", steps.size(), path);
    return steps;
}

}  // namespace

void ScriptPad(int port, input::Pad& pad) {
    static const std::vector<Step> steps = Load();
    static const bool second = [] {
        for (const Step& st : steps)
            if (st.port == 1) return true;
        return false;
    }();
    if (steps.empty() || port > 1 || (port == 1 && !second)) return;
    using clock = std::chrono::steady_clock;
    static const clock::time_point start = clock::now();
    double now = std::chrono::duration<double>(clock::now() - start).count();
    static const char* kButtons[] = {"up", "down", "left", "right", "start", "back", "l3", "r3"};
    static const char* kAnalog[] = {"a", "b", "x", "y", "black", "white", "lt", "rt"};
    pad.connected = true;
    static size_t dumped = 0;
    for (size_t i = 0; i < steps.size(); i++) {
        const Step& st = steps[i];
        if (st.port != port) continue;
        // "prompts": the last frame's button prompt rectangles, in the window's client pixels
        if (st.control == "prompts" && now >= st.at && i >= dumped) {
            dumped = i + 1;
            gpu::PromptFrame f = gpu::Prompts();
            for (const gpu::PromptRect& r : f.rects)
                LOGR("prompt %016llX at %.0f,%.0f to %.0f,%.0f", (unsigned long long)r.key, f.x + r.x0 * f.w,
                     f.y + r.y0 * f.h, f.x + r.x1 * f.w, f.y + r.y1 * f.h);
        }
        if ((st.control == "dump" || st.control == "peek") && now >= st.at && i >= dumped) {
            dumped = i + 1;
            if (st.control == "dump") gpu::g_capture = true;
            // XBE_PEEK="366F1C*+0,366F1C*+4,33DAC0": guest dwords to log; '*' reads the pointer there first
            if (const char* peek = std::getenv("XBE_PEEK")) {
                std::string list = peek;
                for (size_t p = 0; p < list.size();) {
                    size_t e = list.find(',', p);
                    std::string item = list.substr(p, e == std::string::npos ? std::string::npos : e - p);
                    p = e == std::string::npos ? list.size() : e + 1;
                    uint32_t addr = uint32_t(std::strtoul(item.c_str(), nullptr, 16)), off = 0;
                    if (auto plus = item.find('+'); plus != std::string::npos)
                        off = uint32_t(std::strtoul(item.c_str() + plus + 1, nullptr, 16));
                    uint32_t ptr = item.find('*') != std::string::npos ? kernel::R32(addr) : addr;
                    uint32_t at = ptr ? ptr + off : 0;
                    LOGR("peek %s: [%08X] = %08X", item.c_str(), at, at ? kernel::R32(at) : 0);
                }
            }
            for (auto& t : kernel::Threads())
                if (st.control == "dump" && t->ctx) {
                    LOGR("thread %X", t->id);
                    kernel::LogGuestStack(*t->ctx, 32);
                }
        }
        // "addf <chain> <amount>" adds to the float there, "logf <chain>" logs it (chains as Chain reads them)
        if ((st.control == "addf" || st.control == "logf") && now >= st.at && i >= dumped) {
            dumped = i + 1;
            if (uint32_t at = Chain(st.key)) {
                float v;
                uint32_t bits = kernel::R32(at);
                std::memcpy(&v, &bits, 4);
                if (st.control == "addf") {
                    float w = v + st.add;
                    std::memcpy(&bits, &w, 4);
                    kernel::W32(at, bits);
                }
                LOGR("pad script: %s %s [%08X] %.3f%s", st.control.c_str(), st.key.c_str(), at, v,
                     st.control == "addf" ? (" -> " + std::to_string(v + st.add)).c_str() : "");
            } else {
                LOGR("pad script: %s %s: null pointer", st.control.c_str(), st.key.c_str());
            }
        }
        // "memdump <tag>": the image's data, 0x2E0000 up to its end, into <XBE_CAPTURE_DIR or captures>\mem_<tag>.bin
        // as (va, size, bytes) blocks, for scripts/memdiff.py and finding the game's variables
        if (st.control == "memdump" && now >= st.at && i >= dumped) {
            dumped = i + 1;
            const char* dir = std::getenv("XBE_CAPTURE_DIR");
            std::string path = std::string(dir ? dir : "captures") + "\\mem_" + st.key + ".bin";
            if (FILE* f = std::fopen(path.c_str(), "wb")) {
                // "memdump <tag> 1": every committed page of the 64 MB instead, the heap's objects too
                // "memdump <tag> 2": the contiguous memory window (0x80000000 up), where textures and targets live
                const uint32_t lo = st.value == 2 ? 0x80000000u : st.value == 1 ? 0x10000 : 0x2E0000,
                               hi = st.value == 2 ? 0x84000000u : st.value == 1 ? 0x4000000
                                                     : 0x10000 + kernel::R32(0x1010C);  // the image's end (header)
                for (uint32_t va = lo; va < hi;) {
                    if (st.value && !mem::IsCommitted(va)) {
                        va += 0x1000;
                        continue;
                    }
                    uint32_t end = va;
                    while (end < hi && (!st.value || mem::IsCommitted(end))) end = std::min(hi, end + 0x1000);
                    uint32_t size = end - va;
                    std::fwrite(&va, 4, 1, f);
                    std::fwrite(&size, 4, 1, f);
                    std::fwrite(kernel::g_base + va, 1, size, f);
                    va = end;
                }
                std::fclose(f);
                LOGR("pad script: memory to %s", path.c_str());
            }
        }
        if (st.control == "set" && now >= st.at && i >= dumped) {
            dumped = i + 1;
            app::Settings& g = app::Get();
            uint32_t v = uint32_t(st.value);
            bool display = false;
            if (st.key == "render_scale") g.render_scale = v;
            else if (st.key == "aspect_ratio") g.aspect_ratio = v;
            else if (st.key == "anti_aliasing") g.anti_aliasing = v;
            else if (st.key == "upscaler") g.upscaler = v;
            else if (st.key == "fsr_sharpness") g.fsr_sharpness = v;
            else if (st.key == "sharpening") g.sharpen = v;
            else if (st.key == "texture_detail") g.texture_detail = v;
            else if (st.key == "brightness") g.brightness = v;
            else if (st.key == "fps_cap") g.fps_cap = v;
            else if (st.key == "vsync") g.vsync = v != 0;
            else if (st.key == "width") g.width = v, display = true;
            else if (st.key == "height") g.height = v, display = true;
            else if (st.key == "display_mode") g.display_mode = app::DisplayMode(v), display = true;
            else if (st.key == "fov_percent") g_fov_percent = v;
            LOGR("pad script: set %s %u", st.key.c_str(), v);
            if (display) app::ApplyDisplay();
        }
        if (st.control == "menu" && now >= st.at && i >= dumped) {
            dumped = i + 1;
            app::ToggleMenu();
        }
        if (now < st.at || now >= st.until) continue;
        for (int b = 0; b < 8; b++) {
            if (st.control == kButtons[b]) pad.buttons |= uint16_t(1u << b);
            if (st.control == kAnalog[b]) pad.analog[b] = 0xFF;
        }
        int16_t v = int16_t(st.value);
        if (st.control == "lx") pad.lx = v;
        if (st.control == "ly") pad.ly = v;
        if (st.control == "rx") pad.rx = v;
        if (st.control == "ry") pad.ry = v;
    }
}

}  // namespace mechassault
