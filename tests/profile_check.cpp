// mechassault-recomp: the setup screen's check (core/game_import Inspect) of a real game folder or disc image with
// MechAssault's profile. Read only. scripts\b.bat clang++ -std=c++20 -O1 -Isdk/runtime -Isrc tests/profile_check.cpp
//   src/common/profile.cpp sdk/runtime/core/game_import.cpp sdk/runtime/kernel/disc.cpp sdk/runtime/kernel/crypto.cpp
//   -o scratch/profile_check.exe   then   scratch\profile_check.exe <folder or image> [another folder ...]
#include <cstdio>

#include "core/game_import.h"

namespace mechassault {
const gameimport::Profile& Profile();
}

int main(int argc, char** argv) {
    int bad = 0;
    for (int i = 1; i < argc; i++) {
        gameimport::Check c = gameimport::Inspect(argv[i], mechassault::Profile());
        std::printf("%s: %s ok=%d release=%d title %08X '%s' v%u, %u files, %llu bytes\n", argv[i],
                    gameimport::KindName(c.kind), c.ok, c.release_match, c.title_id, c.title_name.c_str(), c.version,
                    c.files, (unsigned long long)c.bytes);
        if (!c.ok) std::printf("  problem: %s\n  fix: %s\n", c.problem.c_str(), c.fix.c_str());
        for (auto& m : c.missing) std::printf("  missing: %s\n", m.c_str());
        bad += !c.ok;
    }
    return bad;
}
