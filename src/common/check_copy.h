// mechassault-recomp: MechAssault.exe --check-copy (check_copy.cpp), what "Check my MechAssault copy.bat" runs
#pragma once
#include <cstddef>
#include <cstdint>

namespace mechassault {

struct KnownRelease {
    const char* name;
    const char* code_sha1;  // gameimport::XbeCodeSha1
    uint32_t size;          // default.xbe's
    const char* blocks;     // 16 hex digits per 1 KB block: the start of its SHA-1
};
extern const KnownRelease kKnownReleases[];
extern const size_t kKnownReleaseCount;

}  // namespace mechassault
