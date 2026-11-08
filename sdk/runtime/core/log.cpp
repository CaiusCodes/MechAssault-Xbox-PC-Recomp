#include "core/log.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <mutex>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace logx {

static std::mutex g_mutex;
static FILE* g_file = nullptr;
static std::string g_path;
static auto g_start = std::chrono::steady_clock::now();

constexpr int kKeep = 30;

// the run number of prefix_NNN.log, 0 for another name
static int RunNumber(const fs::path& file, const std::string& prefix) {
    std::string name = file.filename().string();
    if (file.extension() != ".log" || name.size() <= prefix.size() + 1 || name.compare(0, prefix.size() + 1, prefix + "_"))
        return 0;
    return std::atoi(name.c_str() + prefix.size() + 1);
}

void Init(const std::string& dir, const std::string& prefix, bool append) {
    std::lock_guard<std::mutex> lock(g_mutex);
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (!kVerbose) {
        g_path = (fs::path(dir) / (prefix + ".log")).string();
        g_file = std::fopen(g_path.c_str(), append ? "a" : "w");
        return;
    }
    std::vector<std::pair<int, fs::path>> runs;
    for (const auto& e : fs::directory_iterator(dir, ec))
        if (int n = RunNumber(e.path(), prefix)) runs.emplace_back(n, e.path());
    std::sort(runs.begin(), runs.end());
    for (size_t i = 0; i + kKeep <= runs.size(); i++) fs::remove(runs[i].second, ec);
    char name[256];
    std::snprintf(name, sizeof(name), "%s_%03d.log", prefix.c_str(), runs.empty() ? 1 : runs.back().first + 1);
    g_path = (fs::path(dir) / name).string();
    g_file = std::fopen(g_path.c_str(), "w");
}

const std::string& Path() { return g_path; }

void Write(Level lv, const char* fmt, ...) {
    // mechassault-recomp patch: XBE_LOG_LEVEL=info or warn lets a release build log those levels too (to find the
    // first unimplemented call without a debug build); the file name and line format stay the release ones
    static const Level min = [] {
        const char* v = std::getenv("XBE_LOG_LEVEL");
        // mechassault-recomp patch (from breakdown-recomp): a release keeps warnings as well (each a real problem)
        if (kVerbose || !v) return kVerbose ? Level::Debug : Level::Warn;
        return std::string(v) == "info" ? Level::Info : std::string(v) == "warn" ? Level::Warn : Level::Error;
    }();
    if (lv < min) return;
    // mechassault-recomp patch: at most kWarnings warnings a session in a release, so a loop of one cannot fill the disk
    if (!kVerbose && lv == Level::Warn) {
        static std::atomic<int> warnings{0};
        constexpr int kWarnings = 500;
        int n = warnings.fetch_add(1, std::memory_order_relaxed);
        if (n > kWarnings) return;
        if (n == kWarnings) fmt = "(more warnings are not logged)";
    }
    char msg[4096];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - g_start).count();
    static const char* names[] = {"debug", "info", "warn", "error", "info"};
    static const char* prefixes[] = {"", "", "warning: ", "error: ", ""};
    auto tid = std::hash<std::thread::id>()(std::this_thread::get_id()) & 0xFFFF;
    std::lock_guard<std::mutex> lock(g_mutex);
    char line[4300];
    int n = kVerbose ? std::snprintf(line, sizeof(line), "[%9.3f] [%s] [t%04zx] %s\n", t, names[int(lv)], size_t(tid), msg)
                     : std::snprintf(line, sizeof(line), "[%9.3f] %s%s\n", t, prefixes[int(lv)], msg);
    n = std::min(n, int(sizeof(line)) - 1);
    if (g_file) {
        std::fwrite(line, 1, size_t(n), g_file);
        // mechassault-recomp patch: a release writes few lines; each goes to disk at once, so a hard crash, a driver
        // reset or a killed process still leaves the log complete
        if (lv >= Level::Warn || !kVerbose) std::fflush(g_file);
    }
    std::fwrite(line, 1, size_t(n), stderr);
}

void Flush() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_file) std::fflush(g_file);
}

}  // namespace logx
