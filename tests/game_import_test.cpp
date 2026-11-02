// unit test for sdk/runtime/core/game_import: run by tests/run.sh with a game folder (GAME_SUBSET) and
// images made from it by make_xiso.py
#include "core/game_import.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <fstream>
namespace fs = std::filesystem;
using namespace gameimport;
int main(int argc, char** argv) {
    fs::path folder = argv[1], xiso = argv[2], full = argv[3], work = argv[4];
    std::vector<uint8_t> x;
    { std::ifstream f(folder / "default.xbe", std::ios::binary); x.assign(std::istreambuf_iterator<char>(f), {}); }
    Profile p;
    p.name = "Breakdown";
    p.title_id = 0x4E4D0009;
    p.xbe_sha1 = {Sha1Hex(x)};
    p.required = {"default.xbe", "stream/bgm.sth", "romdata/common/usa.pak"};
    p.required_dirs = {"romdata/area/opening"};
    uint32_t id; std::string name; uint32_t ver;
    assert(ReadCertificate(x, id, name, ver));
    std::printf("cert: %08X '%s' version %u sha1 %s\n", id, name.c_str(), ver, p.xbe_sha1[0].c_str());
    assert(id == 0x4E4D0009);
    for (fs::path src : {folder, xiso, full}) {
        Check c = Inspect(src, p);
        std::printf("%s: %s ok=%d files=%u bytes=%llu problem='%s'\n", src.filename().string().c_str(), KindName(c.kind),
                    c.ok, c.files, (unsigned long long)c.bytes, c.problem.c_str());
        assert(c.ok && c.release_match && c.missing.empty());
    }
    assert(Inspect(folder, p).kind == Kind::Folder && Inspect(xiso, p).kind == Kind::Xiso &&
           Inspect(full, p).kind == Kind::DiscImage);
    // a missing required file, another release, another game, not an image
    Profile q = p; q.required.push_back("romdata/common/missing.pak");
    Check m = Inspect(xiso, q); assert(!m.ok && m.missing.size() == 1); std::printf("missing: %s / %s\n", m.problem.c_str(), m.fix.c_str());
    q = p; q.xbe_sha1 = {"00"}; Check r = Inspect(xiso, q); assert(!r.ok && !r.release_match); std::printf("release: %s\n", r.problem.c_str());
    q = p; q.title_id = 0x12345678; Check g = Inspect(full, q); assert(!g.ok); std::printf("game: %s\n", g.problem.c_str());
    Check n = Inspect(folder / "stream" / "bgm.sth", p); assert(!n.ok && n.kind == Kind::None); std::printf("not image: %s\n", n.problem.c_str());
    // imports from each kind; the result matches the folder byte for byte in size and passes Imported
    for (fs::path src : {xiso, full, folder}) {
        fs::path dest = work / "game";
        Progress pr;
        std::string err;
        bool ok = Import(src, dest, p, pr, err);
        std::printf("import %s: %d %s (%llu/%llu bytes, %u files)\n", src.filename().string().c_str(), ok, err.c_str(),
                    (unsigned long long)pr.done.load(), (unsigned long long)pr.total.load(), pr.files_done.load());
        assert(ok && pr.done == pr.total && Imported(dest, p));
        Check c = Inspect(dest, p);
        assert(c.ok && c.bytes == Inspect(folder, p).bytes);
        assert(!fs::exists(work / "game.importing"));
    }
    // cancel leaves the previous import in place
    { Progress pr; pr.cancel = true; std::string err; assert(!Import(xiso, work / "game", p, pr, err)); assert(Imported(work / "game", p)); }
    // candidates beside a fake exe folder
    fs::create_directories(work / "rel");
    auto cand = Candidates(work / "rel", p);
    for (auto& c : cand) std::printf("candidate: %s\n", c.string().c_str());
    std::puts("game_import ok");
}
