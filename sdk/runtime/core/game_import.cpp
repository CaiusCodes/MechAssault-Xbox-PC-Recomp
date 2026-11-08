// breakdown-recomp patch: game import (see game_import.h)
#include "core/game_import.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <ctime>
#include <fstream>
#include <functional>
#include <memory>

#include "kernel/crypto.h"
#include "kernel/disc.h"

namespace gameimport {

namespace fs = std::filesystem;

namespace {

constexpr char kXbe[] = "default.xbe";
constexpr char kManifest[] = "import.toml";
constexpr size_t kChunk = 4 << 20;
constexpr uint64_t kFullDiscGamePartition = 0x18300000;  // a whole disc dump's game partition, past the video one

std::string Lower(std::string s) {
    for (char& c : s) c = char(std::tolower(uint8_t(c)));
    return s;
}

bool IsImageName(const fs::path& p) {
    std::string ext = Lower(p.extension().string());
    return ext == ".iso" || ext == ".xiso";
}

bool ReadFile(const fs::path& p, std::vector<uint8_t>& out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), {});
    return true;
}

// a path from the game's root, '/' separated, found in any case (the console's file system ignores case)
fs::path FindCaseless(const fs::path& root, const std::string& rel) {
    fs::path at = root;
    std::error_code ec;
    size_t s = 0;
    while (s <= rel.size()) {
        size_t e = std::min(rel.find('/', s), rel.size());
        std::string part = rel.substr(s, e - s);
        s = e + 1;
        if (part.empty()) continue;
        if (fs::exists(at / part, ec)) {
            at /= part;
            continue;
        }
        bool found = false;
        for (auto& d : fs::directory_iterator(at, ec)) {
            if (Lower(d.path().filename().string()) == Lower(part)) {
                at = d.path();
                found = true;
                break;
            }
        }
        if (!found) return {};
    }
    return at;
}

// a folder of extracted files or a mounted image, behind one interface
struct Source {
    virtual ~Source() = default;
    virtual bool Load(const std::string& rel, std::vector<uint8_t>& out) const = 0;
    virtual bool Has(const std::string& rel, bool dir) const = 0;
    // every file: path from the root ('/' separated) and size
    virtual void Walk(const std::function<void(const std::string&, uint64_t)>& f) const = 0;
    // copies one file's bytes to out, reporting each chunk; false on a read failure
    virtual bool Copy(const std::string& rel, std::ofstream& out, Progress& p) const = 0;
};

struct FolderSource : Source {
    fs::path root;
    bool Load(const std::string& rel, std::vector<uint8_t>& out) const override {
        fs::path p = FindCaseless(root, rel);
        return !p.empty() && ReadFile(p, out);
    }
    bool Has(const std::string& rel, bool dir) const override {
        fs::path p = FindCaseless(root, rel);
        std::error_code ec;
        return !p.empty() && (dir ? fs::is_directory(p, ec) : fs::is_regular_file(p, ec));
    }
    void Walk(const std::function<void(const std::string&, uint64_t)>& f) const override {
        std::error_code ec;
        for (auto it = fs::recursive_directory_iterator(root, ec); !ec && it != fs::recursive_directory_iterator();
             it.increment(ec)) {
            if (!it->is_regular_file(ec)) continue;
            std::string rel = fs::relative(it->path(), root, ec).generic_string();
            if (rel == kManifest) continue;
            f(rel, it->file_size(ec));
        }
    }
    bool Copy(const std::string& rel, std::ofstream& out, Progress& p) const override {
        std::ifstream in(root / fs::path(rel), std::ios::binary);
        if (!in) return false;
        std::vector<char> buf(kChunk);
        while (in && !p.cancel) {
            in.read(buf.data(), std::streamsize(buf.size()));
            std::streamsize n = in.gcount();
            if (n <= 0) break;
            out.write(buf.data(), n);
            p.done += uint64_t(n);
        }
        return !in.bad();
    }
};

struct DiscSource : Source {
    kernel::Disc disc;
    static std::string Win(const std::string& rel) {
        std::string s = rel;
        std::replace(s.begin(), s.end(), '/', '\\');
        return s;
    }
    bool Load(const std::string& rel, std::vector<uint8_t>& out) const override { return disc.Load(Win(rel), out); }
    bool Has(const std::string& rel, bool dir) const override {
        const kernel::Disc::Entry* e = disc.Find(Win(rel));
        return e && e->dir == dir;
    }
    void Walk(const std::function<void(const std::string&, uint64_t)>& f) const override {
        std::function<void(const kernel::Disc::Entry&, const std::string&)> walk = [&](const kernel::Disc::Entry& d,
                                                                                         const std::string& at) {
            for (const auto& c : d.children) {
                std::string rel = at.empty() ? c.name : at + "/" + c.name;
                if (c.dir) walk(c, rel);
                else f(rel, c.size);
            }
        };
        if (const kernel::Disc::Entry* root = disc.Find(std::string_view())) walk(*root, "");
    }
    bool Copy(const std::string& rel, std::ofstream& out, Progress& p) const override {
        const kernel::Disc::Entry* e = disc.Find(Win(rel));
        if (!e || e->dir) return false;
        std::vector<char> buf(kChunk);
        for (uint64_t pos = 0; pos < e->size && !p.cancel;) {
            uint32_t want = uint32_t(std::min<uint64_t>(buf.size(), e->size - pos));
            uint32_t n = disc.Read(*e, pos, buf.data(), want);
            if (n != want) return false;
            out.write(buf.data(), n);
            pos += n;
            p.done += n;
        }
        return true;
    }
};

// a whole disc dump has the game partition far into the file
Kind ImageKind(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    char magic[20] = {};
    f.seekg(std::streamoff(kFullDiscGamePartition + 32 * 2048));
    f.read(magic, 20);
    return f && std::memcmp(magic, "MICROSOFT*XBOX*MEDIA", 20) == 0 ? Kind::DiscImage : Kind::Xiso;
}

// the one disc image in a folder without default.xbe, or empty
fs::path LoneImage(const fs::path& dir) {
    fs::path found;
    std::error_code ec;
    for (auto& e : fs::directory_iterator(dir, ec)) {
        if (e.is_regular_file(ec) && IsImageName(e.path())) {
            if (!found.empty()) return {};
            found = e.path();
        }
    }
    return found;
}

std::unique_ptr<Source> OpenSource(const fs::path& source, Kind& kind, std::string& problem, std::string& fix) {
    std::error_code ec;
    if (fs::is_directory(source, ec)) {
        if (FindCaseless(source, kXbe).empty()) {
            if (fs::path img = LoneImage(source); !img.empty()) return OpenSource(img, kind, problem, fix);
            problem = "This folder has no default.xbe.";
            fix = "Choose the folder that holds default.xbe and the romdata folder, or the disc image (.iso) itself.";
            return nullptr;
        }
        kind = Kind::Folder;
        auto s = std::make_unique<FolderSource>();
        s->root = source;
        return s;
    }
    if (!fs::is_regular_file(source, ec)) {
        problem = "The file or folder could not be found.";
        fix = "Check that the drive is connected and choose it again.";
        return nullptr;
    }
    auto s = std::make_unique<DiscSource>();
    std::string err;
    if (!s->disc.Open(source, err)) {
        problem = "This is not an original Xbox disc image (" + err + ").";
        fix = "Use an image made from the original disc (a full dump or an xiso), or the extracted game folder. "
              "Compressed archives (.zip, .7z) and CSO/CCI images need unpacking or converting to .iso first.";
        return nullptr;
    }
    kind = ImageKind(source);
    return s;
}

}  // namespace

const char* KindName(Kind k) {
    switch (k) {
        case Kind::Folder: return "extracted game folder";
        case Kind::DiscImage: return "full disc image";
        case Kind::Xiso: return "game partition image (xiso)";
        default: return "none";
    }
}

std::string Sha1Hex(const std::vector<uint8_t>& data) {
    alignas(8) uint8_t ctx[128];
    uint8_t digest[20];
    crypto::ShaInit(ctx);
    crypto::ShaUpdate(ctx, data.data(), data.size());
    crypto::ShaFinal(ctx, digest);
    std::string hex;
    for (uint8_t b : digest) {
        hex += "0123456789abcdef"[b >> 4];
        hex += "0123456789abcdef"[b & 15];
    }
    return hex;
}

std::string XbeCodeSha1(const std::vector<uint8_t>& x) {
    auto r32 = [&](size_t o) {
        uint32_t v = 0;
        if (o + 4 <= x.size()) std::memcpy(&v, x.data() + o, 4);
        return v;
    };
    if (x.size() < 0x180 || std::memcmp(x.data(), "XBEH", 4) != 0) return {};
    uint32_t base = r32(0x104), image = r32(0x10C), count = r32(0x11C), headers = r32(0x120);
    auto decode = [&](uint32_t v, uint32_t retail, uint32_t debug) {
        uint32_t r = v ^ retail;
        return r >= base && r - base < image ? r : v ^ debug;
    };
    uint32_t entry = decode(r32(0x128), 0xA8FC57AB, 0x94859D4B), thunk = decode(r32(0x158), 0x5B6D40B6, 0xEFB1F152);
    if (headers < base || count > 256 || size_t(headers - base) + size_t(count) * 0x38 > x.size()) return {};
    alignas(8) uint8_t ctx[128];
    uint8_t digest[20];
    crypto::ShaInit(ctx);
    auto add32 = [&](uint32_t v) { crypto::ShaUpdate(ctx, reinterpret_cast<const uint8_t*>(&v), 4); };
    add32(base);
    add32(entry);
    add32(thunk);
    for (uint32_t i = 0; i < count; i++) {
        size_t h = size_t(headers - base) + size_t(i) * 0x38;
        uint32_t flags = r32(h), va = r32(h + 4), vsize = r32(h + 8), raw = r32(h + 0xC), rsize = r32(h + 0x10);
        if (size_t(raw) + rsize > x.size()) return {};
        add32(flags);
        add32(va);
        add32(vsize);
        add32(rsize);
        if (rsize) crypto::ShaUpdate(ctx, x.data() + raw, rsize);
    }
    crypto::ShaFinal(ctx, digest);
    std::string hex;
    for (uint8_t b : digest) {
        hex += "0123456789abcdef"[b >> 4];
        hex += "0123456789abcdef"[b & 15];
    }
    return hex;
}

bool ReleaseMatch(const std::vector<uint8_t>& xbe, const Profile& profile) {
    auto in = [](const std::vector<std::string>& list, const std::string& v) {
        return !v.empty() && std::find(list.begin(), list.end(), v) != list.end();
    };
    return in(profile.xbe_sha1, Sha1Hex(xbe)) || in(profile.xbe_code_sha1, XbeCodeSha1(xbe));
}

bool ReadCertificate(const std::vector<uint8_t>& x, uint32_t& title_id, std::string& name, uint32_t& version) {
    auto r32 = [&](size_t o) {
        uint32_t v = 0;
        if (o + 4 <= x.size()) std::memcpy(&v, x.data() + o, 4);
        return v;
    };
    if (x.size() < 0x180 || std::memcmp(x.data(), "XBEH", 4) != 0) return false;
    uint32_t base = r32(0x104), cert = r32(0x118);
    if (cert < base || cert - base + 0x1D0 > x.size()) return false;
    size_t c = cert - base;
    title_id = r32(c + 8);
    name.clear();
    for (int i = 0; i < 40; i++) {
        uint16_t ch = uint16_t(x[c + 0xC + i * 2] | x[c + 0xD + i * 2] << 8);
        if (!ch) break;
        name += ch < 0x80 ? char(ch) : '?';
    }
    version = r32(c + 0xAC);
    return true;
}

Check Inspect(const fs::path& source, const Profile& profile) {
    Check c;
    auto src = OpenSource(source, c.kind, c.problem, c.fix);
    if (!src) return c;
    std::vector<uint8_t> xbe;
    if (!src->Load(kXbe, xbe)) {
        c.problem = "default.xbe is missing or unreadable.";
        c.fix = c.kind == Kind::Folder ? "Extract the whole disc again, or choose the disc image instead."
                                       : "The image may be damaged; make it again from the original disc.";
        return c;
    }
    c.xbe_sha1 = Sha1Hex(xbe);
    c.xbe_code_sha1 = XbeCodeSha1(xbe);
    if (!ReadCertificate(xbe, c.title_id, c.title_name, c.version)) {
        c.problem = "default.xbe is not an Xbox executable.";
        c.fix = "The files are damaged; extract or dump the disc again.";
        return c;
    }
    c.release_match = ReleaseMatch(xbe, profile);  // mechassault-recomp patch: or by its code (a patched header)
    for (const auto& f : profile.required)
        if (!src->Has(f, false)) c.missing.push_back(f);
    for (const auto& d : profile.required_dirs)
        if (!src->Has(d, true)) c.missing.push_back(d + "/");
    src->Walk([&](const std::string& rel, uint64_t size) {
        if (Skipped(profile, rel)) return;  // mechassault-recomp patch: what the import will copy
        c.files++;
        c.bytes += size;
    });
    char id[16];
    std::snprintf(id, sizeof(id), "%08X", c.title_id);
    if (profile.title_id && c.title_id != profile.title_id) {
        c.problem = "This is a different game: " + c.title_name + " (title ID " + id + ").";
        c.fix = "Choose your " + profile.name + " disc image or game folder.";
    } else if (!c.release_match) {
        c.problem = "This is " + profile.name + ", but a release this build was not made from (default.xbe SHA-1 " +
                    c.xbe_sha1 + ").";
        c.fix = "This build supports the release whose default.xbe has SHA-1 " +
                (profile.xbe_sha1.empty() ? std::string("(none listed)") : profile.xbe_sha1[0]) +
                ". Other regions and versions are not supported yet. A copy patched by a mod pack (region-free, "
                "HDD-ready) is fine as long as its game code is unchanged; this one's differs, so dump the disc again "
                "without patches.";
    } else if (!c.missing.empty()) {
        c.problem = "The game files are incomplete: " + std::to_string(c.missing.size()) + " required item(s) missing, "
                    "first " + c.missing[0] + ".";
        c.fix = c.kind == Kind::Folder ? "Extract the whole disc again (every folder), or choose the disc image."
                                       : "The image is incomplete or damaged; make it again from the original disc.";
    } else if (profile.min_files && c.files < profile.min_files) {
        c.problem = "The game files are incomplete: " + std::to_string(c.files) + " files, a complete copy has at least " +
                    std::to_string(profile.min_files) + ".";
        c.fix = "Extract the whole disc again, or choose the disc image.";
    } else {
        c.ok = true;
    }
    return c;
}

bool Skipped(const Profile& profile, const std::string& rel) {
    auto lower = [](std::string x) {
        for (char& ch : x) ch = char(std::tolower(static_cast<unsigned char>(ch)));
        return x;
    };
    std::string r = lower(rel);
    for (const std::string& s : profile.skip) {
        std::string k = lower(s);
        if (!k.empty() && k.back() == '/' ? r.compare(0, k.size(), k) == 0 : r == k) return true;
    }
    return false;
}

bool Import(const fs::path& source, const fs::path& dest, const Profile& profile, Progress& p, std::string& error) {
    Kind kind;
    std::string problem, fix;
    auto src = OpenSource(source, kind, problem, fix);
    if (!src) {
        error = problem + " " + fix;
        return false;
    }
    std::vector<std::pair<std::string, uint64_t>> files;
    src->Walk([&](const std::string& rel, uint64_t size) {
        if (!Skipped(profile, rel)) files.push_back({rel, size});  // mechassault-recomp patch
    });
    uint64_t total = 0;
    for (auto& f : files) total += f.second;
    p.total = total;
    p.done = 0;
    p.files_total = uint32_t(files.size());
    p.files_done = 0;

    std::error_code ec;
    fs::path tmp = dest;
    tmp += ".importing";
    fs::remove_all(tmp, ec);  // a previous interrupted import's leftovers, never the player's files
    fs::create_directories(tmp, ec);
    if (ec) {
        error = "Cannot create " + tmp.string() + ": " + ec.message();
        return false;
    }
    auto space = fs::space(tmp.parent_path(), ec);
    if (!ec && space.available < total + (64ull << 20)) {
        error = "Not enough free space: the game needs " + std::to_string(total >> 20) + " MB, the drive has " +
                std::to_string(space.available >> 20) + " MB free.";
        fs::remove_all(tmp, ec);
        return false;
    }
    for (auto& [rel, size] : files) {
        if (p.cancel) break;
        {
            std::lock_guard<std::mutex> g(p.lock);
            p.current = rel;
        }
        fs::path out_path = tmp / fs::path(rel);
        fs::create_directories(out_path.parent_path(), ec);
        std::ofstream out(out_path, std::ios::binary | std::ios::trunc);
        if (!out || !src->Copy(rel, out, p) || !out.good()) {
            if (p.cancel) break;
            error = "Copying " + rel + " failed (read or write error).";
            out.close();
            fs::remove_all(tmp, ec);
            return false;
        }
        p.files_done++;
    }
    if (p.cancel) {
        fs::remove_all(tmp, ec);
        error = "Cancelled.";
        return false;
    }
    // the copy must pass the same checks as the source
    Check c = Inspect(tmp, profile);
    if (!c.ok) {
        error = "The copied files failed the check: " + c.problem;
        fs::remove_all(tmp, ec);
        return false;
    }
    {
        std::ofstream m(tmp / kManifest);
        std::time_t now = std::time(nullptr);
        char when[32];
        std::strftime(when, sizeof(when), "%Y-%m-%dT%H:%M:%SZ", std::gmtime(&now));
        m << "# written by the setup screen once the game files were copied and checked\n";
        m << "source = \"" << source.filename().generic_string() << "\"\n";
        m << "kind = \"" << KindName(kind) << "\"\n";
        m << "xbe_sha1 = \"" << c.xbe_sha1 << "\"\n";
        char id[16];
        std::snprintf(id, sizeof(id), "%08X", c.title_id);
        m << "title_id = \"" << id << "\"\n";
        m << "files = " << c.files << "\nbytes = " << c.bytes << "\nimported = \"" << when << "\"\n";
    }
    fs::remove_all(dest, ec);  // the release's own game folder, replaced only once the new copy is complete
    fs::rename(tmp, dest, ec);
    if (ec) {
        error = "Moving the files into place failed: " + ec.message();
        return false;
    }
    return true;
}

bool Imported(const fs::path& dest, const Profile& profile) {
    std::error_code ec;
    if (!fs::is_regular_file(dest / kManifest, ec)) return false;
    std::vector<uint8_t> xbe;
    fs::path x = FindCaseless(dest, kXbe);
    if (x.empty() || !ReadFile(x, xbe)) return false;
    return ReleaseMatch(xbe, profile);  // mechassault-recomp patch: or by its code
}

std::vector<fs::path> Candidates(const fs::path& dir, const Profile& profile) {
    std::vector<fs::path> found;
    std::error_code ec;
    auto look = [&](const fs::path& d, int depth, auto& self) -> void {
        for (auto& e : fs::directory_iterator(d, fs::directory_options::skip_permission_denied, ec)) {
            if (e.is_regular_file(ec) && IsImageName(e.path())) found.push_back(e.path());
            else if (e.is_directory(ec) && e.path().filename() != "resources") {
                if (!FindCaseless(e.path(), kXbe).empty()) found.push_back(e.path());
                else if (depth > 0) self(e.path(), depth - 1, self);
            }
        }
    };
    look(dir, 1, look);
    if (dir.has_parent_path() && dir.parent_path() != dir) look(dir.parent_path(), 0, look);
    std::sort(found.begin(), found.end());
    found.erase(std::unique(found.begin(), found.end()), found.end());
    // accepted releases first
    std::stable_partition(found.begin(), found.end(), [&](const fs::path& p) { return Inspect(p, profile).ok; });
    return found;
}

}  // namespace gameimport
