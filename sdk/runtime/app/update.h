// mechassault-recomp patch: updates from the game's GitHub releases (GameInfo::update_repo). The latest release's
// asset <name>.zip (a release package) is downloaded, unpacked with Windows' tar, and its program, readme, licences
// and compiler copied over a portable install; resources\game, saves, settings and logs are never touched. The new
// program starts with --setup --updated=<pid>, waits for this one to end, and its setup builds the game code again.
#pragma once
#include <string>

namespace app::update {

enum class State { Idle, Checking, UpToDate, NoReleases, Failed, Available, Installing, Installed };

struct Status {
    State state = State::Idle;
    std::string latest;  // the newer release's tag (Available, Installing)
    float progress = 0;  // Installing: 0..1
    std::string error;   // Failed, or an install that failed
};

// the build's version as the menu shows it ("v0.5.1+3 (abc1234)"), empty outside git
const std::string& Version();
// starts a check on its own thread; Current() follows it
void Check();
// Available in a portable install: downloads and installs on its own thread, then restarts into the setup
void Install();
Status Current();
// a development build folder: updates come through git, not the releases
bool DevelopmentLayout();

// at startup, before anything else: removes what an update left behind (<name>.old.exe, resources\runtime\update),
// after waiting for the previous program when started with --updated=<pid>
void Cleanup();
// --check-updates / --install-update: the whole run, logged to <log>-update.log; the process exit code (0 up to date,
// no releases or installed; 10 newer; 1 failed)
int CommandLine(bool install);

}  // namespace app::update
