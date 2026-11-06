// mechassault-recomp patch: updates from the game's GitHub releases (update.h)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

#include "app/app.h"
#include "app/update.h"
#include "core/log.h"

#pragma comment(lib, "winhttp.lib")

namespace fs = std::filesystem;

namespace app::update {
namespace {

std::mutex g_lock;
Status g_status;
std::string g_url;  // the newer release's <name>.zip
std::atomic<bool> g_busy{false};

void Set(State s, const std::string& error = {}) {
    std::lock_guard<std::mutex> l(g_lock);
    g_status.state = s;
    g_status.error = error;
}

std::string Narrow(const wchar_t* w) {
    std::string s;
    for (; *w; w++) s += *w < 0x80 ? char(*w) : '?';
    return s;
}

std::wstring Wide(const std::string& s) { return std::wstring(s.begin(), s.end()); }

std::string Name() { return Narrow(kGame.name); }

// the releases API, or <LOG>_UPDATE_URL (tests: a local server's JSON)
std::string ApiUrl() {
    std::string var = kGame.log;
    for (char& c : var) c = char(std::toupper(static_cast<unsigned char>(c)));
    var += "_UPDATE_URL";
    if (const char* e = std::getenv(var.c_str()); e && *e) return e;
    if (!kGame.update_repo || !*kGame.update_repo) return {};
    return std::string("https://api.github.com/repos/") + kGame.update_repo + "/releases/latest";
}

// a GET: the status code, and the body to `sink` (with the length when the server gives one); 0 when it failed
DWORD Get(const std::string& url, const std::function<bool(const char*, size_t, uint64_t)>& sink, std::string& error) {
    std::wstring w = Wide(url);
    URL_COMPONENTS c{sizeof(c)};
    wchar_t host[256] = {}, path[2048] = {};
    c.lpszHostName = host;
    c.dwHostNameLength = 256;
    c.lpszUrlPath = path;
    c.dwUrlPathLength = 2048;
    if (!WinHttpCrackUrl(w.c_str(), 0, 0, &c)) {
        error = "bad address " + url;
        return 0;
    }
    std::wstring agent = Wide(Name() + "-PC-updater");
    HINTERNET s = WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, nullptr, nullptr, 0);
    HINTERNET h = s ? WinHttpConnect(s, host, c.nPort, 0) : nullptr;
    HINTERNET r = h ? WinHttpOpenRequest(h, L"GET", path, nullptr, nullptr, nullptr,
                                         c.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0)
                    : nullptr;
    if (s) WinHttpSetTimeouts(s, 10000, 10000, 15000, 30000);
    DWORD status = 0, size = sizeof(status);
    const wchar_t* headers = L"Accept: application/vnd.github+json\r\n";
    if (r && WinHttpSendRequest(r, headers, DWORD(-1), nullptr, 0, 0, 0) && WinHttpReceiveResponse(r, nullptr) &&
        WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &status, &size, nullptr)) {
        wchar_t len[32] = {};
        DWORD len_size = sizeof(len);
        uint64_t total = WinHttpQueryHeaders(r, WINHTTP_QUERY_CONTENT_LENGTH, nullptr, len, &len_size, nullptr)
                             ? std::wcstoull(len, nullptr, 10)
                             : 0;
        std::vector<char> buf(65536);
        DWORD n = 0;
        while (WinHttpReadData(r, buf.data(), DWORD(buf.size()), &n) && n)
            if (!sink(buf.data(), n, total)) {
                status = 0;
                error = "stopped";
                break;
            }
    } else {
        error = "request failed (" + std::to_string(GetLastError()) + ")";
        status = 0;
    }
    if (r) WinHttpCloseHandle(r);
    if (h) WinHttpCloseHandle(h);
    if (s) WinHttpCloseHandle(s);
    return status;
}

// the numbers of a version: "v0.5.1" and "v0.4+21 (abc1234)" give 0.5.1 and 0.4
std::vector<int> Numbers(const std::string& v) {
    std::vector<int> out;
    size_t i = v.find_first_of("0123456789");
    while (i != std::string::npos && i < v.size() && std::isdigit(static_cast<unsigned char>(v[i]))) {
        out.push_back(std::atoi(v.c_str() + i));
        while (i < v.size() && std::isdigit(static_cast<unsigned char>(v[i]))) i++;
        if (i >= v.size() || v[i] != '.') break;
        i++;
    }
    while (!out.empty() && out.back() == 0) out.pop_back();  // 0.5.0 is 0.5
    return out;
}

// a JSON string value after "key": the first one at or after `from`
std::string JsonString(const std::string& json, const std::string& key, size_t from = 0, size_t* at = nullptr) {
    size_t k = json.find("\"" + key + "\"", from);
    if (k == std::string::npos) return {};
    size_t q = json.find('"', json.find(':', k) + 1);
    if (q == std::string::npos) return {};
    std::string out;
    for (size_t i = q + 1; i < json.size() && json[i] != '"'; i++) {
        if (json[i] == '\\' && i + 1 < json.size()) i++;
        out += json[i];
    }
    if (at) *at = q;
    return out;
}

State DoCheck(std::string& latest, std::string& error) {
    std::string url = ApiUrl();
    if (url.empty()) {
        error = "no release source";
        return State::Failed;
    }
    std::string body;
    DWORD status = Get(url, [&](const char* p, size_t n, uint64_t) { body.append(p, n); return body.size() < (8u << 20); },
                       error);
    if (status == 404) return State::NoReleases;
    if (status != 200) {
        if (error.empty()) error = "the server answered " + std::to_string(status);
        LOGW("update: %s: %s", url.c_str(), error.c_str());
        return State::Failed;
    }
    latest = JsonString(body, "tag_name");
    std::string zip, want = "/" + Name() + ".zip";
    for (size_t at = 0;;) {
        std::string u = JsonString(body, "browser_download_url", at, &at);
        if (u.empty()) break;
        if (u.size() >= want.size() && u.compare(u.size() - want.size(), want.size(), want) == 0) zip = u;
        at++;
    }
    if (latest.empty()) {
        error = "no tag_name in the answer";
        return State::Failed;
    }
    LOGR("update: latest release %s (this build %s)%s", latest.c_str(), Version().c_str(),
         (zip.empty() ? ", without " + Name() + ".zip" : std::string()).c_str());
    if (Numbers(latest) <= Numbers(Version())) return State::UpToDate;
    if (zip.empty()) {
        error = "release " + latest + " has no " + Name() + ".zip";
        return State::Failed;
    }
    {
        std::lock_guard<std::mutex> l(g_lock);
        g_url = zip;
    }
    return State::Available;
}

// a program run hidden, its exit code (-1 when it could not start)
int RunHidden(const std::wstring& cmd) {
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    std::wstring line = cmd;
    if (!CreateProcessW(nullptr, line.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
        return -1;
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return int(code);
}

fs::path ExePath() {
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(nullptr, buf, DWORD(std::size(buf)));
    return fs::path(std::wstring(buf, n));
}

fs::path UpdateDir() { return Paths().runtime / "update"; }

void Progress(float p) {
    std::lock_guard<std::mutex> l(g_lock);
    g_status.progress = p;
}

// downloads, unpacks and copies the release over this install; true when the new program is in place
bool DoInstall(std::string& error) {
    std::string url;
    {
        std::lock_guard<std::mutex> l(g_lock);
        url = g_url;
    }
    std::error_code ec;
    fs::path dir = UpdateDir(), zip = dir / (Name() + ".zip"), out = dir / "unpacked";
    fs::remove_all(dir, ec);
    fs::create_directories(out, ec);
    {
        std::ofstream f(zip, std::ios::binary);
        uint64_t got = 0;
        DWORD status = Get(url,
                           [&](const char* p, size_t n, uint64_t total) {
                               f.write(p, std::streamsize(n));
                               got += n;
                               if (total) Progress(0.8f * float(got) / float(total));
                               return bool(f);
                           },
                           error);
        if (status != 200 || !f) {
            if (error.empty()) error = "download failed (" + std::to_string(status) + ")";
            return false;
        }
        LOGR("update: downloaded %llu bytes from %s", (unsigned long long)got, url.c_str());
    }
    Progress(0.85f);
    wchar_t sys[MAX_PATH];
    GetSystemDirectoryW(sys, MAX_PATH);
    std::wstring tar = std::wstring(sys) + L"\\tar.exe";
    int code = RunHidden(L"\"" + tar + L"\" -xf \"" + zip.wstring() + L"\" -C \"" + out.wstring() + L"\"");
    if (code != 0) {
        error = "unpacking failed (tar " + std::to_string(code) + ")";
        return false;
    }
    std::wstring name = kGame.name;
    fs::path from = out / name, new_exe = from / (name + L".exe");
    if (!fs::exists(new_exe, ec)) {
        error = "the package has no " + Name() + "\\" + Name() + ".exe";
        return false;
    }
    Progress(0.9f);
    fs::path exe = ExePath(), root = exe.parent_path(), old = root / (name + L".old.exe");
    fs::remove(old, ec);
    fs::rename(exe, old, ec);
    if (ec) {
        error = "the running program could not be moved aside (" + ec.message() + ")";
        return false;
    }
    fs::copy_file(new_exe, exe, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        std::error_code back;
        fs::rename(old, exe, back);
        error = "the new program could not be copied (" + ec.message() + ")";
        return false;
    }
    // the readme, licences and compiler; never resources\game, saves, settings or logs
    auto copy = [&](const fs::path& rel) {
        std::error_code e;
        if (!fs::exists(from / rel, e)) return;
        if (fs::is_directory(from / rel, e)) {
            fs::create_directories(root / rel, e);
            fs::copy(from / rel, root / rel, fs::copy_options::recursive | fs::copy_options::overwrite_existing, e);
        } else {
            fs::copy_file(from / rel, root / rel, fs::copy_options::overwrite_existing, e);
        }
        if (e) LOGW("update: %s not copied (%s)", rel.string().c_str(), e.message().c_str());
    };
    copy("README.txt");
    copy("licenses");
    copy(fs::path("resources") / "runtime" / "compiler");
    Progress(1);
    LOGR("update: installed %s over %s", url.c_str(), root.string().c_str());
    return true;
}

// the new program, into its setup, told which process to wait for; false when it could not start
bool Restart() {
    Save();
    std::wstring line = L"\"" + ExePath().wstring() + L"\" --setup --updated=" + std::to_wstring(GetCurrentProcessId());
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, line.data(), nullptr, nullptr, FALSE, DETACHED_PROCESS, nullptr,
                        ExePath().parent_path().wstring().c_str(), &si, &pi))
        return false;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

}  // namespace

const std::string& Version() {
    static const std::string v = kGame.version ? kGame.version : "";
    return v;
}

bool DevelopmentLayout() { return !Paths().portable; }

Status Current() {
    std::lock_guard<std::mutex> l(g_lock);
    return g_status;
}

void Check() {
    if (g_busy.exchange(true)) return;
    Set(State::Checking);
    std::thread([] {
        std::string latest, error;
        State s = DoCheck(latest, error);
        {
            std::lock_guard<std::mutex> l(g_lock);
            g_status.state = s;
            g_status.latest = latest;
            g_status.error = error;
        }
        g_busy = false;
    }).detach();
}

void Install() {
    if (DevelopmentLayout() || Current().state != State::Available || g_busy.exchange(true)) return;
    Set(State::Installing);
    Progress(0);
    std::thread([] {
        std::string error;
        if (!DoInstall(error)) {
            LOGW("update: %s", error.c_str());
            {
                std::lock_guard<std::mutex> l(g_lock);
                g_status.state = State::Available;
                g_status.error = error;
            }
            g_busy = false;
            return;
        }
        Set(State::Installed);
        logx::Flush();
        if (Restart()) {
            ChangeDisplaySettingsW(nullptr, 0);  // a fullscreen mode back to the desktop's
            logx::Flush();
            ExitProcess(0);
        }
        Set(State::Failed, "the new program did not start: start it again by hand");
        g_busy = false;
    }).detach();
}

void Cleanup() {
    std::error_code ec;
    if (const wchar_t* p = std::wcsstr(GetCommandLineW(), L"--updated=")) {
        DWORD pid = DWORD(std::wcstoul(p + 10, nullptr, 10));
        if (HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, pid)) {
            WaitForSingleObject(h, 10000);
            CloseHandle(h);
        }
        LOGR("update: started after an update");
    }
    fs::path old = ExePath().parent_path() / (std::wstring(kGame.name) + L".old.exe");
    if (fs::exists(old, ec)) {
        for (int i = 0; i < 20 && fs::exists(old, ec); i++) {
            fs::remove(old, ec);
            if (fs::exists(old, ec)) Sleep(250);
        }
        LOGR("update: %s %s", old.filename().string().c_str(), fs::exists(old, ec) ? "could not be removed" : "removed");
    }
    if (Paths().portable && fs::exists(UpdateDir(), ec)) {
        fs::remove_all(UpdateDir(), ec);
        LOGR("update: download folder removed");
    }
}

int CommandLine(bool install) {
    std::string latest, error;
    State s = DoCheck(latest, error);
    const char* what = s == State::UpToDate ? "up to date" : s == State::NoReleases ? "no releases yet"
                       : s == State::Available ? "newer release available" : "could not check";
    LOGR("update: %s%s%s", what, latest.empty() ? "" : (" (" + latest + ")").c_str(),
         error.empty() ? "" : (": " + error).c_str());
    if (s == State::Failed) return 1;
    if (s != State::Available) return 0;
    if (!install) return 10;
    if (DevelopmentLayout()) {
        LOGR("update: a development build folder updates through git");
        return 1;
    }
    if (!DoInstall(error)) {
        LOGR("update: install failed: %s", error.c_str());
        return 1;
    }
    return 0;
}

}  // namespace app::update
