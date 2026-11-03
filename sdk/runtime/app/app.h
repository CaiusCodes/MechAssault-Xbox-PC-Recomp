#pragma once
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace input {
struct Pad;
}
namespace gameimport {
struct Profile;
}

// the host side: window, settings
namespace app {

enum class DisplayMode : uint32_t { Fullscreen, Borderless, Windowed };
inline constexpr uint32_t kFpsCapRefresh = 1;  // fps_cap: the monitor's refresh rate
// breakdown-recomp patch: aspect_ratio values
inline constexpr uint32_t kAspect43 = 0, kAspectWide = 1, kAspect169 = 2, kAspectFill = 3;
// mechassault-recomp patch: true widescreen in a fixed shape whatever the display's (bars on the other one)
inline constexpr uint32_t kAspectWide169 = 4, kAspectWide219 = 5;

struct Settings {
    DisplayMode display_mode = DisplayMode::Borderless;
    uint32_t width = 0, height = 0;  // 0 is the desktop mode
    uint32_t render_percent = 100;   // of the output height
    uint32_t fps_cap = 60;           // 0 unlimited, kFpsCapRefresh the monitor's refresh rate
    bool vsync = true;
    uint32_t aniso = 0;              // 0 as the game asks
    bool custom_textures = true;     // images in textures\replacement\ beside the exe replace the game's
    bool dump_textures = false;      // each texture once into textures\dumps\ beside the exe
    uint32_t texture_dump_format = 0;  // 0 PNG, 1 DDS
    std::string join_ip;                   // co-op: a host past the LAN the system link search also asks
    // breakdown-recomp patch: PC settings shared by every title
    uint32_t render_scale = 0;       // 0: render_percent of the output height; N: N times the title's own height
    uint32_t perf_overlay = 0;       // 0 off, 1 FPS counter, 2 FPS counter and frame timing diagnostics
    bool diagnostics_log = false;    // a frame timing summary in the log every 10 seconds
    uint32_t vblank_sync = 0;        // 0: the console's blank follows the display's where it can (vblank.cpp); 1: timer
    uint32_t anti_aliasing = 1;      // 0 off, 1 FXAA on the finished frame (render scale above the display supersamples)
    uint32_t refresh_hz = 0;         // Fullscreen mode's refresh rate, 0 the desktop's (60 or 120 Hz shows 60 fps evenly)
    // mechassault-recomp patch: 0 bilinear, 1 FSR 1 (EASU + RCAS) when the picture is shown larger than rendered;
    // sharpness 0 (off) .. 20 (strongest), in tenths of FSR's stops
    uint32_t upscaler = 1;
    uint32_t brightness = 100;  // mechassault-recomp patch: a gamma curve over the picture, percent (50..200)
    uint32_t fsr_sharpness = 18;
    uint32_t aspect_ratio = kAspectWide;  // the picture: kAspect43, kAspectWide (a wider view in the display's shape,
                                          // WideView), kAspect169 or kAspectFill (the 4:3 picture stretched; they
                                          // also tell the title it is on a widescreen TV)
    std::string monitor;  // mechassault-recomp patch: the display the game is shown on (its device name), empty: main
};
// mechassault-recomp patch: the displays to choose from (Settings::monitor), and the chosen one (an HMONITOR: the
// setting's display if it is connected, else the window's)
struct Monitor {
    std::string device;  // \\.\DISPLAYn
    uint32_t number, width, height;
    bool primary;
};
std::vector<Monitor> Monitors();
void* ChosenMonitor();
inline constexpr uint32_t kMaxRenderScale = 4;
// breakdown-recomp patch: the PC settings menu (overlay.cpp) is open: the title gets no pad input
bool MenuOpen();
// breakdown-recomp patch: the console's vertical blank on the display's own (vblank.cpp); false: use the timer
bool WaitDisplayVBlank();
double DisplayHz();   // measured, 0 before
int VBlankEvery();    // display blanks per console blank, 0 on the timer

Settings& Get();
void Load();
void Save();
void LogSettings();  // every setting, as the settings file has them
void LogSystem();    // the machine and the settings, at a run's start
void ApplyDisplay();  // any thread
struct Mode {
    uint32_t width, height;
};
Mode Desktop();
Mode Output();
float RenderScale(uint32_t guest_height);
// mechassault-recomp patch: the renderer found an integrated GPU (little dedicated memory): render scale Auto stays at
// or below 2x there (measured: an AMD Radeon iGPU held 60 fps at 2x with room to spare, 3x had stalls uncapped)
void SetLowEndGpu(bool low);
bool LowEndGpu();
// breakdown-recomp patch: the console reports a widescreen TV (aspect_ratio 16:9, or the display's shape when it is
// 3:2 or wider), else a 4:3 one
bool Widescreen();
// breakdown-recomp patch: the shape the frame is shown in, width over height; 0 fills the window
float PictureAspect();
// breakdown-recomp patch: true widescreen for a title that only draws 4:3 (aspect_ratio kAspectWide): how many times
// wider than 4:3 the display is, 1 when off. The title's camera is widened by that much (its hooks), the frame is
// shown in the display's shape and flat draws (HUD, menus) are squeezed back to 4:3 (gpu/widescreen.h).
float WideView();

std::filesystem::path ExeDir();
// breakdown-recomp patch: where things go. A release with a resources folder beside the exe keeps everything under
// it (portable layout); otherwise the development layout: settings, logs, hdd and the game DLL beside the exe and the
// game in an assets folder found upwards.
struct Folders {
    bool portable = false;
    std::filesystem::path game;      // resources\game (portable); empty: search for assets
    std::filesystem::path settings;  // folder of the settings file
    std::filesystem::path saves;     // the console hard disk (T:, U:, Z:)
    std::filesystem::path logs;
    std::filesystem::path runtime;   // the game DLL and the compiler folder
    std::filesystem::path textures;  // replacements and dumps
};
const Folders& Paths();
struct GameFiles;
// first-run setup (setup.cpp), an installer window: imports the game into Paths().game, finds it (FindGame) into game
// unless have_game, builds the game code, applies the extras the player ticked; false when the player quit
bool RunSetup(const gameimport::Profile& profile, GameFiles& game, bool have_game);
// the game in assets (game_files.cpp): its folder and this executable's xbe, from the extracted files or a disc
// image (.iso), which it mounts as D:; false after telling the player what is missing
struct GameFiles {
    std::filesystem::path dir;
    std::vector<uint8_t> xbe;
};
bool FindGame(GameFiles& game);
std::string Sha1(const std::vector<uint8_t>& data);  // lower case hex
// the recompiled code, <exe>.dll, built from the game in assets when missing or out of date (game_code.cpp) and
// loaded; false after telling the player why the game cannot start, or when they chose not to start it yet
bool PrepareGameCode(const GameFiles& game);
// breakdown-recomp patch: the installer's build of the game code (game_code.cpp, shown by setup.cpp)
bool GameCodeStale(const GameFiles& game);  // a DLL to build or rebuild
// starts the build on its own thread; msg is posted to hwnd with wp 1 (built) or 0 (failed or cancelled). Returns
// what stops it from starting, for the player; empty when it started (or there was nothing to build)
std::wstring StartGameCodeBuild(const GameFiles& game, void* hwnd, uint32_t msg);
void CancelGameCodeBuild();
bool GameCodeBuildCancelled();
void JoinGameCodeBuild();
struct GameCodeStatus {
    std::wstring stage, time_left;
    float fraction = 0;
    bool ready = false;
};
GameCodeStatus GameCodeProgress();
std::wstring GameCodeFailure();  // what failed, with the end of logs\compile.log
uint64_t CompilerFolderBytes();
void DeleteCompilerFolder();
int Instance();  // --instance N on the command line, 0 without: a second copy on this PC for system link tests
void* Window();  // HWND
// boot splash: GameInfo::splash .bmp or .png beside the exe, up before the game window
bool OpenSplash();        // window thread, at launch; false without one, and the game window opens at once
void SplashFirstFrame();  // the game's first Swap: waits out the splash's minimum, then opens the game window
void CloseSplash();       // window thread
void OpenGameWindow(bool apply = true);  // any thread but the window thread's; returns once the window is up
// relaunched: the previous instance keeps its window up until this one has shown its first frames (handover.cpp)
bool StartHandover();   // window thread, at launch; true: the game window starts hidden
bool HandingOver();
void FinishHandover();  // any thread but the window thread's: opens the game window, lets the old instance go

// held around ImGui: the window thread's messages and the renderer's frame
std::recursive_mutex& UiLock();

// what the game repo defines once
struct Option {  // a setting in the settings file, flag or value
    const char* key;
    bool* flag;
    uint32_t* value;
};
struct GameInfo {
    const wchar_t* title;        // window and splash
    const wchar_t* short_title;  // message boxes
    const wchar_t* name;         // window classes, crash dumps, handover event
    const char* log;             // log prefix
    const char* settings;        // settings file beside the exe
    const wchar_t* splash;       // .bmp or .png beside the exe
    const wchar_t* missing;      // message when neither the game files nor a disc image are found
    std::vector<std::pair<const char*, const char*>> images;  // xbe file name, executable name
    std::vector<Option> options;
    // the settings file's toml::table, for what options cannot hold
    void (*load)(const void* table);
    void (*save)(void* table);
    void (*init)();                // after the settings load, before the window and boot
    void (*loaded)();              // image in memory, before its entry runs
    void (*window)(void* hwnd);    // the game window is up, before boot
    // window thread, each message after ImGui's; true returns result instead of the host's handling
    bool (*message)(void* hwnd, uint32_t msg, uintptr_t wp, intptr_t lp, intptr_t* result);
    void (*ui)(double fps);        // each frame inside the ImGui frame, UiLock held
    // breakdown-recomp patch: the game's own sections of the F1 settings menu (ImGui, inside its window); app::Save()
    // keeps what changed
    void (*menu)() = nullptr;
    void (*pad)(int port, input::Pad& pad);  // a polled pad before the game gets it
    void (*rumble)(int port, uint16_t& left, uint16_t& right);
    // breakdown-recomp patch: what the setup screen accepts (core/game_import.h); null: no setup screen
    const gameimport::Profile* profile = nullptr;
    // breakdown-recomp patch: the installer's optional extras beyond its own (fullscreen, shortcut, compiler), each a
    // settings flag ticked or not; and the controls summary on its last page
    struct Extra {
        const wchar_t* label;
        const wchar_t* detail;
        bool* flag;
    };
    std::vector<Extra> extras = {};
    const wchar_t* controls = nullptr;
    const wchar_t* platform_line = nullptr;  // under the installer's buttons, e.g. the title ID
};
extern const GameInfo kGame;

// sleeps out the rest of the frame for the fps cap
void Pace();

}  // namespace app
