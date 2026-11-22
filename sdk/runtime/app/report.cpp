// mechassault-recomp patch (from breakdown-recomp's report.cpp): --save-report, run by "Make a problem report.bat" in
// the release folder. A zip on the Desktop, MechAssault-report-YYYYMMDD-HHMMSS.zip, holds what a bug report needs: the
// logs (this session's and the ones before), the newest crash dumps, settings.toml and an info.txt with the version.
// The log itself lists the machine (system_info.cpp), the GPU and its driver, the controllers and any crash.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <objbase.h>
#include <shellapi.h>
#include <shlobj.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

#include "app/app.h"
#include "core/log.h"

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

namespace fs = std::filesystem;

namespace app {

namespace {

constexpr size_t kDumps = 3;  // the newest crash dumps in a report

std::string Utf8(const std::wstring& w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(size_t(std::max(n, 0)), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

fs::path DesktopFolder() {
    PWSTR p = nullptr;
    fs::path out;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &p))) out = p;
    CoTaskMemFree(p);
    return out;
}

// the files of dir with this extension, newest first
std::vector<fs::path> Newest(const fs::path& dir, const wchar_t* ext) {
    std::vector<std::pair<fs::file_time_type, fs::path>> found;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec))
        if (e.is_regular_file(ec) && e.path().extension() == ext) found.emplace_back(e.last_write_time(ec), e.path());
    std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    std::vector<fs::path> out;
    for (auto& f : found) out.push_back(f.second);
    return out;
}

// Windows' own tar.exe (System32), without a window
int RunTar(const std::wstring& args, unsigned timeout_ms) {
    wchar_t sys[MAX_PATH];
    UINT n = GetSystemDirectoryW(sys, MAX_PATH);
    if (!n || n >= MAX_PATH) return -1;
    fs::path tar = fs::path(sys) / L"tar.exe";
    std::wstring cmd = L"\"" + tar.wstring() + L"\" " + args;
    STARTUPINFOW si{sizeof(si)};
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(tar.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS,
                        nullptr, nullptr, &si, &pi)) {
        LOGR("report: tar.exe did not start (error %lu)", GetLastError());
        return -1;
    }
    DWORD code = DWORD(-1);
    if (WaitForSingleObject(pi.hProcess, timeout_ms) == WAIT_OBJECT_0) GetExitCodeProcess(pi.hProcess, &code);
    else TerminateProcess(pi.hProcess, 1);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return int(code);
}

}  // namespace

int ReportCommand() {
    LOGR("report: making a problem report");
    logx::Flush();
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t stamp[32];
    swprintf(stamp, 32, L"%04u%02u%02u-%02u%02u%02u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    // MECHASSAULT_REPORT_DIR: another folder than the Desktop, for tests
    const char* dir = std::getenv("MECHASSAULT_REPORT_DIR");
    fs::path desktop = dir && *dir ? fs::path(dir) : DesktopFolder();
    if (desktop.empty()) {
        LOGR("report: the Desktop folder was not found");
        return 1;
    }
    std::wstring base = std::wstring(kGame.name) + L"-report-" + stamp;
    fs::path zip = desktop / (base + L".zip");

    // everything is gathered in a temporary folder first, then zipped from there in one go
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    fs::path stage = fs::path(tmp) / base;
    std::error_code ec;
    fs::remove_all(stage, ec);
    fs::create_directories(stage / "logs", ec);
    std::vector<std::wstring> entries;
    size_t logs = 0, dumps = 0;
    for (const fs::path& f : Newest(Paths().logs, L".log"))
        if (fs::copy_file(f, stage / "logs" / f.filename(), fs::copy_options::overwrite_existing, ec)) logs++;
    if (logs) entries.push_back(L"logs");
    std::vector<fs::path> dmp = Newest(Paths().logs / "CrashDumps", L".dmp");
    if (!dmp.empty()) {
        fs::create_directories(stage / "CrashDumps", ec);
        for (size_t i = 0; i < dmp.size() && i < kDumps; i++)
            if (fs::copy_file(dmp[i], stage / "CrashDumps" / dmp[i].filename(), fs::copy_options::overwrite_existing, ec))
                dumps++;
        entries.push_back(L"CrashDumps");
    }
    if (fs::copy_file(Paths().settings / kGame.settings, stage / kGame.settings, fs::copy_options::overwrite_existing, ec))
        entries.push_back(fs::path(kGame.settings).wstring());
    {
        std::ofstream info(stage / "info.txt");
        info << Utf8(kGame.title) << " " << (*kGame.version ? kGame.version : "(no version)") << "\n";
        info << "Folder: " << Utf8(ExeDir().wstring()) << "\n";
        info << "Logs: " << logs << ", crash dumps: " << dumps << " of " << dmp.size() << "\n";
        info << "The machine, graphics card, driver and controllers are listed at the top of each log.\n";
    }
    entries.push_back(L"info.txt");

    std::wstring args = L"-a -c -f \"" + zip.wstring() + L"\" -C \"" + stage.wstring() + L"\"";
    for (const std::wstring& e : entries) args += L" \"" + e + L"\"";
    int rc = RunTar(args, 120000);
    fs::remove_all(stage, ec);
    if (rc != 0 || !fs::exists(zip, ec)) {
        LOGR("report: tar.exe failed (%d) writing %s", rc, Utf8(zip.wstring()).c_str());
        logx::Flush();
        return 1;
    }
    LOGR("report: %s, %zu logs, %zu crash dumps", Utf8(zip.wstring()).c_str(), logs, dumps);
    logx::Flush();
    // the zip selected in Explorer, ready to attach (not for tests)
    if (!(dir && *dir)) {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        std::wstring select = L"/select,\"" + zip.wstring() + L"\"";
        ShellExecuteW(nullptr, L"open", L"explorer.exe", select.c_str(), nullptr, SW_SHOWNORMAL);
        CoUninitialize();
    }
    return 0;
}

}  // namespace app
