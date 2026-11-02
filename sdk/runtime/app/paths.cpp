// breakdown-recomp patch: the release's folders (app.h, Folders)
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

}  // namespace app
