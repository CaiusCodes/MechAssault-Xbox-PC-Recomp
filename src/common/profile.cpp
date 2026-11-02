// mechassault-recomp: what the setup screen accepts as the player's copy (GameInfo::profile), shared with the
// profile check (tests/profile_check.cpp): MechAssault (USA, Europe), the release this build was recompiled from
#include <string>

#include "core/game_import.h"

namespace mechassault {

const gameimport::Profile& Profile() {
    static const gameimport::Profile profile = [] {
        gameimport::Profile p;
        p.name = "MechAssault";
        p.title_id = 0x4D530017;  // "MS" 023
        p.xbe_sha1 = {"682ddd1ecda1e5110f9339dab0c31d296d51777b"};
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
