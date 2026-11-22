// mechassault-recomp: MechAssault.exe --check-copy [default.xbe, game folder or disc image]: what release a player's
// copy is, written to MechAssault-copy-check.txt on the Desktop (MECHASSAULT_REPORT_DIR for tests) and opened in
// Notepad. Without a path it asks for one. It only reads. This replaces the PowerShell script the .bat used to carry,
// which antivirus programs flag (a batch file running hidden PowerShell code is how a lot of malware starts).
// Reports: size, SHA-1, build date, title, region, version, media flags, libraries, the code fingerprint the setup
// checks, entry and kernel thunk, sections, which supported release it is, and the 1 KB blocks that differ from that
// release with their bytes.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "app/app.h"
#include "common/check_copy.h"
#include "core/game_import.h"
#include "core/log.h"
#include "kernel/crypto.h"
#include "kernel/disc.h"

namespace fs = std::filesystem;

namespace mechassault {
namespace {

std::string Hex(const uint8_t* d, size_t n) {
    std::string s;
    for (size_t i = 0; i < n; i++) {
        s += "0123456789abcdef"[d[i] >> 4];
        s += "0123456789abcdef"[d[i] & 15];
    }
    return s;
}

std::string Sha1(const uint8_t* d, size_t n) {
    alignas(8) uint8_t ctx[128];
    uint8_t digest[20];
    crypto::ShaInit(ctx);
    crypto::ShaUpdate(ctx, d, n);
    crypto::ShaFinal(ctx, digest);
    return Hex(digest, 20);
}

std::string Utf8(const std::wstring& w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(size_t(std::max(n, 0)), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::string Date(uint32_t t, bool time) {
    std::time_t tt = std::time_t(t);
    std::tm tm{};
    gmtime_s(&tm, &tt);
    char b[32];
    std::strftime(b, sizeof(b), time ? "%Y-%m-%d %H:%M UTC" : "%Y-%m-%d", &tm);
    return b;
}

// default.xbe, a disc image, or a game folder (default.xbe in it or up to two folders down)
fs::path Ask() {
    fs::path out;
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    IFileOpenDialog* d = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d)))) {
        COMDLG_FILTERSPEC types[] = {{L"MechAssault default.xbe or disc image", L"*.xbe;*.iso;*.xiso"},
                                     {L"All files", L"*.*"}};
        d->SetFileTypes(2, types);
        d->SetTitle(L"Choose your MechAssault default.xbe or disc image to check");
        if (SUCCEEDED(d->Show(nullptr))) {
            IShellItem* item = nullptr;
            if (SUCCEEDED(d->GetResult(&item))) {
                PWSTR p = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p))) {
                    out = p;
                    CoTaskMemFree(p);
                }
                item->Release();
            }
        }
        d->Release();
    }
    return out;
}

bool LoadXbe(const fs::path& source, std::vector<uint8_t>& xbe, fs::path& where, std::string& error) {
    std::error_code ec;
    fs::path p = source;
    if (fs::is_directory(p, ec)) {
        for (auto it = fs::recursive_directory_iterator(p, ec); it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (it.depth() > 2) {
                it.disable_recursion_pending();
                continue;
            }
            if (it->is_regular_file(ec) && _wcsicmp(it->path().filename().c_str(), L"default.xbe") == 0 &&
                (where.empty() || it.depth() == 0)) {
                where = it->path();
                if (it.depth() == 0) break;
            }
        }
        if (where.empty()) {
            error = "no default.xbe in that folder";
            return false;
        }
        p = where;
    }
    std::wstring ext = p.extension().wstring();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);
    if (ext == L".iso" || ext == L".xiso") {
        kernel::Disc disc;
        if (!disc.Open(p, error)) return false;
        if (!disc.Load("default.xbe", xbe)) {
            error = "the disc image has no default.xbe";
            return false;
        }
        where = p;
        return true;
    }
    std::ifstream f(p, std::ios::binary);
    if (!f) {
        error = "the file could not be read";
        return false;
    }
    xbe.assign(std::istreambuf_iterator<char>(f), {});
    where = p;
    return true;
}

std::vector<std::string> Report(const std::vector<uint8_t>& b, const fs::path& where) {
    std::vector<std::string> out;
    char line[512];
    auto r32 = [&](size_t o) {
        uint32_t v = 0;
        if (o + 4 <= b.size()) std::memcpy(&v, b.data() + o, 4);
        return v;
    };
    auto r16 = [&](size_t o) {
        uint16_t v = 0;
        if (o + 2 <= b.size()) std::memcpy(&v, b.data() + o, 2);
        return v;
    };
    std::snprintf(line, sizeof(line), "Size: %zu bytes", b.size());
    out.push_back(line);
    out.push_back("SHA-1: " + Sha1(b.data(), b.size()));
    if (b.size() < 0x180 || std::memcmp(b.data(), "XBEH", 4) != 0) {
        out.push_back("Not an Xbox executable (no XBEH header).");
        return out;
    }
    uint32_t base = r32(0x104), cert = r32(0x118) - base;
    out.push_back("Built: " + Date(r32(0x114), true));
    std::string name;
    for (int i = 0; i < 40 && cert + 0xC + i * 2 + 1 < b.size(); i++) {
        uint16_t c = r16(cert + 0xC + i * 2);
        if (!c) break;
        name += c < 0x80 ? char(c) : '?';
    }
    std::snprintf(line, sizeof(line), "Title: %s (title ID %08X)", name.c_str(), r32(cert + 8));
    out.push_back(line);
    uint32_t region = r32(cert + 0xA0);
    std::string parts;
    for (auto [bit, what] : {std::pair{1u, "North America"}, {2u, "Japan"}, {4u, "Europe/rest of world"},
                             {0x80000000u, "manufacturing"}})
        if (region & bit) parts += (parts.empty() ? "" : ", ") + std::string(what);
    std::snprintf(line, sizeof(line), "Region: %08X (%s)", region, parts.c_str());
    out.push_back(line);
    std::snprintf(line, sizeof(line), "Version: %u, disc %u", r32(cert + 0xAC), r32(cert + 0xA8));
    out.push_back(line);
    std::snprintf(line, sizeof(line), "Media flags: %08X, ratings %08X, certificate date %s", r32(cert + 0x9C),
                  r32(cert + 0xA4), Date(r32(cert + 4), false).c_str());
    out.push_back(line);
    std::string libs;
    uint32_t nlib = std::min(r32(0x160), 32u), la = r32(0x164) - base;
    for (uint32_t i = 0; i < nlib && la + i * 16 + 16 <= b.size(); i++) {
        size_t o = la + i * 16;
        char nm[9] = {};
        std::memcpy(nm, b.data() + o, 8);
        std::snprintf(line, sizeof(line), "%s%s %u.%u.%u", libs.empty() ? "" : ", ", nm, r16(o + 8), r16(o + 10),
                      r16(o + 12));
        libs += line;
    }
    out.push_back("Libraries: " + libs);
    std::string code = gameimport::XbeCodeSha1(b);
    out.push_back("Code fingerprint: " + code);
    auto decode = [&](uint32_t v, uint32_t retail, uint32_t debug) {
        uint32_t x = v ^ retail;
        return x >= base && x - base < r32(0x10C) ? x : v ^ debug;
    };
    std::snprintf(line, sizeof(line), "Entry %08X, kernel thunk %08X, base %08X", decode(r32(0x128), 0xA8FC57AB, 0x94859D4B),
                  decode(r32(0x158), 0x5B6D40B6, 0xEFB1F152), base);
    out.push_back(line);
    std::string secs;
    uint32_t count = std::min(r32(0x11C), 256u), hdr = r32(0x120) - base;
    for (uint32_t i = 0; i < count && hdr + i * 0x38 + 0x38 <= b.size(); i++) {
        size_t h = hdr + i * 0x38;
        uint32_t no = r32(h + 0x14) - base;
        std::string sn;
        for (uint32_t k = 0; k < 12 && no + k < b.size() && b[no + k]; k++) sn += char(b[no + k]);
        std::snprintf(line, sizeof(line), "%s%s %X+%X", secs.empty() ? "" : ", ", sn.c_str(), r32(h + 4), r32(h + 8));
        secs += line;
    }
    out.push_back("Sections: " + secs);
    const KnownRelease* same = nullptr;
    for (size_t i = 0; i < kKnownReleaseCount; i++)
        if (code == kKnownReleases[i].code_sha1) same = &kKnownReleases[i];
    if (same == &kKnownReleases[0])
        out.push_back(std::string("=> ") + same->name + ", the release this PC version supports (or a patched copy of it).");
    else if (same)
        out.push_back(std::string("=> ") + same->name + " (or a patched copy of it). This PC version needs Rev 1.");
    else
        out.push_back("=> Different game code from the known releases: another release or a modified file.");
    // against the known release of the same size: the 1 KB blocks that differ, with this copy's bytes in them
    const KnownRelease* ref = nullptr;
    for (size_t i = 0; i < kKnownReleaseCount; i++)
        if (b.size() == kKnownReleases[i].size) ref = &kKnownReleases[i];
    if (ref) {
        std::vector<size_t> diff;
        size_t blocks = (b.size() + 1023) / 1024;
        for (size_t i = 0; i < blocks; i++) {
            size_t n = std::min<size_t>(1024, b.size() - i * 1024);
            if (Sha1(b.data() + i * 1024, n).compare(0, 16, ref->blocks + i * 16, 16) != 0) diff.push_back(i);
        }
        std::snprintf(line, sizeof(line), "Blocks that differ from %s: %zu of %zu", ref->name, diff.size(), blocks);
        out.push_back(line);
        for (size_t k = 0; k < diff.size() && k < 24; k++) {
            size_t o = diff[k] * 1024, n = std::min<size_t>(1024, b.size() - o);
            std::string hex = Hex(b.data() + o, n);
            for (char& c : hex) c = char(std::toupper(uint8_t(c)));
            std::snprintf(line, sizeof(line), "Block %06zX: ", o);
            out.push_back(line + hex);
        }
    } else {
        out.push_back("The file is not the size of a known release, so it was not compared block by block.");
    }
    (void)where;
    return out;
}

}  // namespace

int CheckCopyCommand(const std::wstring& arg, bool open) {
    fs::path source = arg.empty() ? Ask() : fs::path(arg);
    if (source.empty()) return 2;
    std::vector<uint8_t> xbe;
    fs::path where;
    std::string error;
    std::vector<std::string> lines;
    SYSTEMTIME st;
    GetLocalTime(&st);
    char stamp[64];
    std::snprintf(stamp, sizeof(stamp), "MechAssault copy check, %04u-%02u-%02u %02u:%02u", st.wYear, st.wMonth, st.wDay,
                  st.wHour, st.wMinute);
    lines.push_back(stamp);
    if (!LoadXbe(source, xbe, where, error)) {
        lines.push_back("Checked: " + Utf8(source.wstring()));
        lines.push_back("Could not read it: " + error);
    } else {
        lines.push_back("File: " + Utf8(where.wstring()));
        for (auto& l : Report(xbe, where)) lines.push_back(l);
    }
    const char* dir = std::getenv("MECHASSAULT_REPORT_DIR");
    fs::path out;
    if (dir && *dir) {
        out = fs::path(dir);
    } else {
        PWSTR p = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &p))) out = p;
        CoTaskMemFree(p);
    }
    out /= L"MechAssault-copy-check.txt";
    {
        std::ofstream f(out, std::ios::binary);
        for (auto& l : lines) f << l << "\r\n";
    }
    LOGR("check copy: %s -> %s", Utf8(source.wstring()).c_str(), Utf8(out.wstring()).c_str());
    if (open && !(dir && *dir)) ShellExecuteW(nullptr, L"open", out.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    return 0;
}

}  // namespace mechassault
