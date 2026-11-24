// mechassault-recomp patch (after breakdown-recomp's): F8 marks a bug for a play tester. The moment goes to the log
// ("bug mark N") and to logs\bugs\bug-<date>-<time>.txt with the version and what the game says about it
// (GameInfo::bug_state: the mission, the 'Mech's heading, the last seconds of pad input), and the next frame drawn is
// saved beside it as a .png. A short note at the top of the window says it was saved. The newest 30 are kept;
// --save-report zips the folder with the logs.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <imgui.h>

#include "app/app.h"
#include "app/update.h"
#include "core/log.h"

#pragma comment(lib, "windowscodecs.lib")

namespace fs = std::filesystem;
using Microsoft::WRL::ComPtr;

namespace app {

namespace {

constexpr size_t kKeepMarks = 30;  // the newest bug marks kept, each a .txt and a .png

std::mutex g_lock;
fs::path g_shot;  // the picture the next frame is saved to, empty when none is due
int g_marks = 0;  // this session's
std::atomic<int64_t> g_toast_until{0};
std::string g_toast;

int64_t NowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

std::string Narrow(const std::wstring& w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(size_t(std::max(n, 0)), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

void Prune(const fs::path& dir) {
    std::error_code ec;
    std::vector<std::pair<fs::file_time_type, fs::path>> txt;
    for (const auto& e : fs::directory_iterator(dir, ec))
        if (e.path().extension() == L".txt") txt.emplace_back(e.last_write_time(ec), e.path());
    if (txt.size() <= kKeepMarks) return;
    std::sort(txt.begin(), txt.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    for (size_t i = kKeepMarks; i < txt.size(); i++) {
        fs::path png = txt[i].second;
        png.replace_extension(L".png");
        fs::remove(txt[i].second, ec);
        fs::remove(png, ec);
    }
}

// BGRA rows, top down, to a .png (WIC), on a thread of its own so the frame does not wait
void WritePng(fs::path file, std::vector<uint8_t> px, uint32_t w, uint32_t h) {
    std::thread([file = std::move(file), px = std::move(px), w, h] {
        HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        {
            ComPtr<IWICImagingFactory> wic;
            ComPtr<IWICStream> stream;
            ComPtr<IWICBitmapEncoder> enc;
            ComPtr<IWICBitmapFrameEncode> frame;
            WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
            std::vector<uint8_t> rgb(px);  // opaque: the frame's alpha is the game's, not a picture's
            for (size_t i = 3; i < rgb.size(); i += 4) rgb[i] = 255;
            bool ok = SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) &&
                      SUCCEEDED(wic->CreateStream(&stream)) &&
                      SUCCEEDED(stream->InitializeFromFilename(file.c_str(), GENERIC_WRITE)) &&
                      SUCCEEDED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) &&
                      SUCCEEDED(enc->Initialize(stream.Get(), WICBitmapEncoderNoCache)) &&
                      SUCCEEDED(enc->CreateNewFrame(&frame, nullptr)) && SUCCEEDED(frame->Initialize(nullptr)) &&
                      SUCCEEDED(frame->SetSize(w, h)) && SUCCEEDED(frame->SetPixelFormat(&fmt)) &&
                      SUCCEEDED(frame->WritePixels(h, w * 4, UINT(rgb.size()), rgb.data())) &&
                      SUCCEEDED(frame->Commit()) && SUCCEEDED(enc->Commit());
            if (!ok) LOGR("bug mark: the picture could not be saved (%s)", Narrow(file.wstring()).c_str());
        }
        if (SUCCEEDED(co)) CoUninitialize();
    }).detach();
}

}  // namespace

fs::path BugDir() { return Paths().logs / "bugs"; }

void MarkBug() {
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t stamp[40];
    swprintf(stamp, 40, L"bug-%04u%02u%02u-%02u%02u%02u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    fs::path dir = BugDir();
    std::error_code ec;
    fs::create_directories(dir, ec);
    std::vector<std::string> state;
    if (kGame.bug_state) state = kGame.bug_state();
    int n;
    {
        std::lock_guard l(g_lock);
        n = ++g_marks;
        g_shot = dir / (std::wstring(stamp) + L".png");
    }
    {
        std::ofstream f(dir / (std::wstring(stamp) + L".txt"));
        f << "Bug mark " << n << " (F8), " << Narrow(kGame.title) << " "
          << (update::Version().empty() ? std::string("(no version)") : update::Version()) << "\n";
        f << "Log: " << logx::Path() << "\n";
        for (const std::string& line : state) f << line << "\n";
    }
    LOGR("bug mark %d: %s (F8)", n, Narrow(stamp).c_str());
    for (const std::string& line : state) LOGR("bug mark %d:   %s", n, line.c_str());
    logx::Flush();
    Prune(dir);
    {
        std::lock_guard l(g_lock);
        g_toast = "Bug " + std::to_string(n) + " marked: saved in logs\\bugs";
    }
    g_toast_until = NowMs() + 3500;
}

bool BugShotDue(fs::path& file) {
    std::lock_guard l(g_lock);
    if (g_shot.empty()) return false;
    file = std::move(g_shot);
    g_shot.clear();
    return true;
}

void SaveBugPicture(fs::path file, std::vector<uint8_t> bgra, uint32_t w, uint32_t h) {
    WritePng(std::move(file), std::move(bgra), w, h);
}

// each frame inside the ImGui frame: the note that a mark was saved, at the top of the window
void DrawBugToast() {
    if (NowMs() > g_toast_until.load()) return;
    std::string text;
    {
        std::lock_guard l(g_lock);
        text = g_toast;
    }
    ImGuiIO& io = ImGui::GetIO();
    ImDrawList* d = ImGui::GetForegroundDrawList();
    ImVec2 size = ImGui::CalcTextSize(text.c_str());
    float pad = size.y * 0.6f;
    ImVec2 a(io.DisplaySize.x / 2 - size.x / 2 - pad, size.y * 1.5f);
    ImVec2 b(io.DisplaySize.x / 2 + size.x / 2 + pad, a.y + size.y + pad * 2);
    d->AddRectFilled(a, b, IM_COL32(13, 9, 10, 220), pad * 0.5f);
    d->AddRect(a, b, IM_COL32(196, 30, 40, 255), pad * 0.5f, 0, 2.0f);
    d->AddText(ImVec2(a.x + pad, a.y + pad), IM_COL32(238, 232, 230, 255), text.c_str());
}

}  // namespace app
