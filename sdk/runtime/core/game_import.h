#pragma once
// breakdown-recomp patch: the player's game, checked and brought into the release's game folder: from an original
// Xbox disc image (a whole disc dump or an xiso) or from files someone extracted before. Platform neutral, so the
// harness and the unit tests run the same code as the Windows setup screen. Never writes to the source.
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

namespace gameimport {

// what a title's game layer says its game looks like (GameInfo::profile)
struct Profile {
    std::string name;                       // "Breakdown"
    uint32_t title_id = 0;                  // the xbe certificate's
    std::vector<std::string> xbe_sha1;      // releases this build runs, lower case hex
    // mechassault-recomp patch: the same releases by their code (XbeCodeSha1), so a copy whose xbe header was patched
    // by a dump or region tool (region, media flags, signature, keys) is accepted: its code is the one recompiled
    std::vector<std::string> xbe_code_sha1;
    std::vector<std::string> required;      // files the game needs, '/' separated, from the game's root
    std::vector<std::string> required_dirs; // folders, the same way
    uint32_t min_files = 0;                 // a complete copy has at least this many files
    // mechassault-recomp patch: files and folders ("dir/") the game never reads, left out of the import (and of the
    // counts the check reports), '/' separated, any case
    std::vector<std::string> skip;
};

enum class Kind { None, Folder, DiscImage, Xiso };
const char* KindName(Kind k);

// what a source holds; ok means the import can go ahead
struct Check {
    Kind kind = Kind::None;
    bool ok = false;
    std::string problem;              // for the player: what is wrong
    std::string fix;                  // and what to do about it
    std::string xbe_sha1;
    std::string xbe_code_sha1;        // mechassault-recomp patch: XbeCodeSha1
    uint32_t title_id = 0;
    std::string title_name;           // from the certificate
    uint32_t version = 0;             // the certificate's
    bool release_match = false;       // xbe_sha1 is one of the profile's
    std::vector<std::string> missing; // required files and folders that are not there
    uint32_t files = 0;
    uint64_t bytes = 0;
};

// a disc image file, or a folder with default.xbe (or a folder holding exactly one disc image)
Check Inspect(const std::filesystem::path& source, const Profile& profile);

struct Progress {
    std::atomic<uint64_t> done{0}, total{0};  // bytes
    std::atomic<uint32_t> files_done{0}, files_total{0};
    std::atomic<bool> cancel{false};
    std::mutex lock;
    std::string current;  // the file being written, under lock
};

// extracts an image's or copies a folder's files to dest (made fresh through a temporary folder beside it, so an
// interrupted import leaves dest as it was), then checks the copy; false with the reason
bool Import(const std::filesystem::path& source, const std::filesystem::path& dest, const Profile& profile,
            Progress& progress, std::string& error);

// dest holds a finished import of a release the profile accepts (import.toml written last, xbe hash matching)
bool Imported(const std::filesystem::path& dest, const Profile& profile);

// likely sources without searching the computer: disc images and game folders in dir and its parent folder, one
// level down at most; best first (an accepted release before others, images and folders by name)
std::vector<std::filesystem::path> Candidates(const std::filesystem::path& dir, const Profile& profile);

// the xbe's certificate: false when data is no xbe
bool ReadCertificate(const std::vector<uint8_t>& xbe, uint32_t& title_id, std::string& name, uint32_t& version);

std::string Sha1Hex(const std::vector<uint8_t>& data);

// mechassault-recomp patch: the SHA-1 of what the recompiled code depends on: the base address, the entry point and
// kernel thunk (decoded with the retail or debug key), and each section's flags, address, sizes and bytes; the rest of
// the header (certificate, signature, logo) is left out. Empty when data is no xbe.
std::string XbeCodeSha1(const std::vector<uint8_t>& xbe);
// the xbe is one of the profile's releases, exactly or by its code
bool ReleaseMatch(const std::vector<uint8_t>& xbe, const Profile& profile);

// mechassault-recomp patch: rel (a '/' separated path from the game's root) is one of profile.skip or inside one
bool Skipped(const Profile& profile, const std::string& rel);

}  // namespace gameimport
