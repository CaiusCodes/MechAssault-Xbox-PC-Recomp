// the host side without a window: settings at their defaults, the run folder as the exe folder
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <thread>

#include "app/app.h"

namespace app {

Settings& Get() {
    static Settings s;
    return s;
}
void Load() {}
void Save() {}
void ApplyDisplay() {}
Mode Desktop() { return {1280, 720}; }
Mode Output() { return Desktop(); }
float RenderScale(uint32_t) { return 1.0f; }
// WIDE=<display aspect>, e.g. 1.7778: true widescreen as on a display of that shape
float WideView() {
    static const float k = [] {
        const char* e = std::getenv("WIDE");
        float a = e ? float(std::atof(e)) : 0.0f;
        return a > 4.0f / 3.0f ? a / (4.0f / 3.0f) : 1.0f;
    }();
    return k;
}
float PictureAspect() { return 4.0f / 3.0f * WideView(); }
// WIDESCREEN=0: the console reports a 4:3 TV
bool Widescreen() {
    static const bool wide = [] {
        const char* e = std::getenv("WIDESCREEN");
        return !e || std::atoi(e) != 0;
    }();
    return wide;
}

std::filesystem::path ExeDir() {
    static const std::filesystem::path dir = std::filesystem::current_path();
    return dir;
}
int Instance() {
    static const int n = [] {
        const char* e = std::getenv("INSTANCE");
        return e ? std::atoi(e) : 0;
    }();
    return n;
}
void* Window() { return nullptr; }
// breakdown-recomp patch: the development layout in the run folder
const Folders& Paths() {
    static const Folders f = [] {
        Folders f;
        f.settings = ExeDir();
        f.saves = ExeDir() / "hdd";
        f.logs = ExeDir() / "logs";
        f.runtime = ExeDir();
        f.textures = ExeDir() / "textures";
        return f;
    }();
    return f;
}

bool OpenSplash() { return false; }
void SplashFirstFrame() {}
void CloseSplash() {}
void OpenGameWindow(bool) {}
bool StartHandover() { return false; }
bool HandingOver() { return false; }
void FinishHandover() {}

std::recursive_mutex& UiLock() {
    static std::recursive_mutex m;
    return m;
}

void Pace() {}
bool MenuOpen() { return false; }  // breakdown-recomp patch
bool WaitDisplayVBlank() { return false; }
double DisplayHz() { return 0; }
int VBlankEvery() { return 0; }

}  // namespace app
