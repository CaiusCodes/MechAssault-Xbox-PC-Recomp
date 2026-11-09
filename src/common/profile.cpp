// mechassault-recomp: what the setup screen accepts as the player's copy (GameInfo::profile), shared with the
// profile check (tests/profile_check.cpp): MechAssault (USA) (Rev 1), the release this build is recompiled from
#include <string>

#include "core/game_import.h"

namespace mechassault {

const gameimport::Profile& Profile() {
    static const gameimport::Profile profile = [] {
        gameimport::Profile p;
        p.name = "MechAssault";
        p.title_id = 0x4D530017;  // "MS" 023
        // Rev 1 (2003, the Xbox Live update build), the release this build is made from (config/game.toml)
        p.xbe_sha1 = {"372a01bbe34b0a12c209126b0978784d724d05da"};
        // the same release with a patched header (region-free or HDD-ready packs, re-signed or debug-keyed copies):
        // the code fingerprint of the xbe above (gameimport::XbeCodeSha1). The original 2002 release (xbe
        // 682ddd1ecda1e5110f9339dab0c31d296d51777b, code 919910b4f6fe5ba6b169754088dc61b89e77e2c9) is built from
        // config/2002; one package for both is still to come
        p.xbe_code_sha1 = {"014624358ee6a930246748eebdcc6e174d769e93"};
        p.required = {"default.xbe", "Common.mgf", "text.mgf", "movies.mgf", "Audio/Music.d1Bank",
                      "Audio/SFX.d1Bank", "Audio/SFX_English.d1Bank", "media/ArialUni.Ttf", "media/dlcustom.xpr",
                      "Movie/NTSC/MAIntro.bik"};
        for (const char* m : {"01", "02", "03", "04", "05", "06a", "06b", "07a", "07b", "09", "10", "12", "13", "15a",
                              "15b", "17", "18", "19", "20a", "20B"})
            p.required.push_back(std::string("Mission") + m + ".mgf");
        p.required_dirs = {"Audio", "media", "Movie/NTSC", "Movie/PAL"};
        // never needed here: the Xbox Demos disc launcher (its own xbe; the runtime only starts recompiled images),
        // and the Xbox Live dashboard update and downloader images; about 440 MB less to copy
        p.skip = {"XDemos/", "dashupdate.xbe", "update.xbe", "downloader.xbe"};
        return p;
    }();
    return profile;
}

}  // namespace mechassault
