// mechassault-recomp: the guest addresses the host code uses, for each release it supports. Rev 1 (the 2003 build
// with the Xbox Live update: MechAssault (USA) (Rev 1), certificate version 8) is the main release; the original
// 2002 one (MechAssault (USA, Europe), version 1) keeps its addresses so either can run. Hooks and symbols are in each
// release's config (config/mechassault.toml); this is what host code reads and writes directly. Rev 1's were placed
// by code matching against the 2002 build (scripts/rev1), each one checked as docs/REV1.md says.
#pragma once
#include <cstdint>

namespace mechassault {

struct GameRelease {
    const char* name;
    uint32_t text_size;     // .text's virtual size, which tells the two apart in the loaded image's header
    uint32_t pilot;         // player 1's pilot pointer (src/common/mouse_aim.cpp)
    uint32_t pilot_vtable;  // its class
    uint32_t mech_vtable;   // the 'Mech it drives (pilot +0x10)
    uint32_t progress;      // the profile's campaign progress block (src/game/campaign.cpp)
    uint32_t in_play;       // a word that is 0 on every front-end screen and in the pause menu (src/game/campaign.cpp)
    uint32_t memcpy;        // the game's CRT memcpy (src/game/selftest.cpp)
};

// the release in memory, from the image's own header (0x10000); Rev 1 until the image is loaded
const GameRelease& Current();

}  // namespace mechassault
