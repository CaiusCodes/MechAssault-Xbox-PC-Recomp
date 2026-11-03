// Unlock the whole campaign (setup extra / F1 > Gameplay). MechAssault keeps the profile's campaign progress in one
// block at 0x386260 (0x186C bytes, saved as MASave.sav after a 4-byte size and a 20-byte signature, the game signing it
// itself): the node being played at +0x14 (name, 64 bytes), +0x54 (file, 64), +0x94 (done: byte), +0x98 (a count of
// nodes completed so far), and the nodes played before it, oldest first, at +0x178, 0xC4 bytes each (name, file, the
// count at +0x90), their number at +0x170. The campaign itself is \missions\campaign.ini in Common.mgf: StartCampaign
// -> BEGINMOVIE -> ... -> LEVEL20B -> ENDMOVIE -> EndCampaign, one node after the other. The mission select
// (0x8AA40) lists the node being played (continue) and then every node played before (replay).
// A [[hook]] in 0x8B060, which runs when the campaign menu opens, right after it loads the profile (0x895B0, from
// MASave.sav), fills in every node up to the closing movie as played, so the mission select offers all of them and
// Continue plays the ending; the game saves the profile as it always does. Progress already further along is left as
// it is.
#include <cstdint>
#include <cstring>
#include <iterator>

#include "core/log.h"
#include "cpu/x86_context.h"
#include "kernel/kernel.h"

namespace mechassault {
bool g_unlock_campaign = false;  // settings.toml unlock_campaign
}

namespace {

struct Node {
    const char* name;
    const char* file;
};
// campaign.ini's chain after StartCampaign, up to the closing movie
constexpr Node kNodes[] = {
    {"BEGINMOVIE", "OpeningCinema.mgv"},  {"DROPSHIPCRASH", "Dropship_Crash.mgv"},
    {"LEVEL01", "Mission01.wdf"},         {"GRASSLANDMOVIE", "Grassland_Ambush.mgv"},
    {"LEVEL02", "Mission02.wdf"},         {"LEVEL03", "Mission03.wdf"},
    {"LEVEL04", "Mission04.wdf"},         {"LEVEL05", "Mission05.wdf"},
    {"WASTELANDMOVIE", "Wasteland_Tease.mgv"}, {"LEVEL06A", "Mission06a.wdf"},
    {"LEVEL06B", "Mission06b.wdf"},       {"LEVEL07A", "Mission07a.wdf"},
    {"LEVEL07B", "Mission07b.wdf"},       {"LEVEL09", "Mission09.wdf"},
    {"LEVEL10", "Mission10.wdf"},         {"LEVEL12", "Mission12.wdf"},
    {"LEVEL13", "Mission13.wdf"},         {"LEVEL15A", "Mission15a.wdf"},
    {"LEVEL15B", "Mission15b.wdf"},       {"LEVEL17", "Mission17.wdf"},
    {"LEVEL18", "Mission18.wdf"},         {"LEVEL19", "Mission19.wdf"},
    {"LEVEL20A", "Mission20a.wdf"},       {"LEVEL20B", "Mission20b.wdf"},
};
constexpr Node kEnd{"ENDMOVIE", "ClosingCinema.mgv"};
constexpr uint32_t kProgress = 0x386260;
constexpr uint32_t kCurName = kProgress + 0x14, kCurFile = kProgress + 0x54, kCurDone = kProgress + 0x94,
                   kCurCount = kProgress + 0x98, kPlayedCount = kProgress + 0x170, kPlayed = kProgress + 0x178;
constexpr uint32_t kStride = 0xC4, kMaxPlayed = (0x186C - 0x178) / kStride;
static_assert(std::size(kNodes) <= kMaxPlayed);

void Text(uint32_t va, const char* s) {
    std::memset(kernel::g_base + va, 0, 0x40);
    std::memcpy(kernel::g_base + va, s, std::strlen(s));
}
bool Is(uint32_t va, const char* s) { return std::strncmp(reinterpret_cast<const char*>(kernel::g_base + va), s, 0x40) == 0; }

}  // namespace

void MechAssaultUnlockCampaign(X86Context&, uint8_t*) {
    if (!mechassault::g_unlock_campaign) return;
    if (Is(kCurName, kEnd.name) || Is(kCurName, "EndCampaign")) return;  // already at the end
    uint32_t played = kernel::R32(kPlayedCount);
    if (played >= std::size(kNodes) || kernel::G<uint8_t>(kCurDone + 1)) return;  // all played, or replaying one
    for (uint32_t i = 0; i < played; i++)
        if (!Is(kPlayed + i * kStride, kNodes[i].name)) {
            LOGW("mechassault: campaign progress not in campaign.ini's order (%u nodes), left as it is", played);
            return;
        }
    // the nodes already played keep their counts; the rest follow on
    uint32_t count = played ? kernel::R32(kPlayed + (played - 1) * kStride + 0x90) : 0;
    for (uint32_t i = played; i < std::size(kNodes); i++) {
        uint32_t e = kPlayed + i * kStride;
        std::memset(kernel::g_base + e, 0, kStride);
        Text(e, kNodes[i].name);
        Text(e + 0x40, kNodes[i].file);
        kernel::W32(e + 0x90, ++count);
    }
    kernel::W32(kPlayedCount, uint32_t(std::size(kNodes)));
    Text(kCurName, kEnd.name);
    Text(kCurFile, kEnd.file);
    kernel::G<uint8_t>(kCurDone) = 0;
    kernel::W32(kCurCount, count);
    LOGR("mechassault: campaign unlocked (%u nodes were played, now %zu; Continue plays the ending)", played,
         std::size(kNodes));
}
