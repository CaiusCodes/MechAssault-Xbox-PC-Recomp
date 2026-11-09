// breakdown-recomp patch: the release's folders (app.h, Folders)
#include <fstream>
#include <string>
#include <system_error>

#include "app/app.h"

namespace app {

const Folders& Paths() {
    static const Folders f = [] {
        Folders f;
        std::filesystem::path exe = ExeDir(), res = exe / "resources";
        std::error_code ec;
        f.portable = std::filesystem::is_directory(res, ec);
        if (f.portable) {
            f.game = res / "game";
            f.settings = res / "settings";
            f.saves = res / "saves";
            f.logs = res / "logs";
            f.runtime = res / "runtime";
            f.textures = res / "textures";
            for (auto* d : {&f.settings, &f.saves, &f.logs, &f.runtime}) std::filesystem::create_directories(*d, ec);
        } else {
            f.settings = exe;
            f.saves = exe / "hdd";
            f.logs = exe / "logs";
            f.runtime = exe;
            f.textures = exe / "textures";
        }
        return f;
    }();
    return f;
}

// mechassault-recomp patch: the player's save location, one line in saves_location.txt beside the settings file (it
// is read before anything else, so it cannot live in settings.toml, whose folder may be the one being moved)
namespace {
std::filesystem::path LocationFile() { return Paths().settings / "saves_location.txt"; }
}  // namespace

std::filesystem::path SavesDir() {
    std::ifstream in(LocationFile());  // UTF-8
    std::string line;
    if (in && std::getline(in, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        std::error_code ec;
        std::filesystem::path dir = std::filesystem::u8path(line);
        if (!line.empty() && dir.is_absolute() && (std::filesystem::is_directory(dir, ec) ||
                                                   std::filesystem::create_directories(dir, ec)))
            return dir;
    }
    return Paths().saves;
}

void SetSavesDir(const std::filesystem::path& dir) {
    std::error_code ec;
    if (dir.empty() || dir == Paths().saves) {
        std::filesystem::remove(LocationFile(), ec);
        return;
    }
    std::filesystem::create_directories(dir, ec);
    std::ofstream(LocationFile()) << reinterpret_cast<const char*>(dir.u8string().c_str()) << "\n";  // UTF-8
}

}  // namespace app
