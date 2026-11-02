// mixer output through XAudio2
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <xaudio2.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

#include "audio/audio.h"
#include "core/diag.h"
#include "core/log.h"

namespace audio {

namespace {

// mechassault-recomp patch: XBE_AUDIO_WAV=<file> records what is played into a 16-bit stereo WAV, from second
// XBE_AUDIO_WAV_FROM (default 0) for XBE_AUDIO_WAV_SECONDS (default 60), so a test run's sound can be listened to later
struct WavRecorder {
    FILE* f = nullptr;
    uint64_t skip = 0, left = 0, frames = 0;
    WavRecorder() {
        const char* path = std::getenv("XBE_AUDIO_WAV");
        if (!path) return;
        const char* from = std::getenv("XBE_AUDIO_WAV_FROM");
        const char* secs = std::getenv("XBE_AUDIO_WAV_SECONDS");
        skip = uint64_t(from ? std::atof(from) * kRate : 0);
        left = uint64_t((secs ? std::atof(secs) : 60.0) * kRate);
        f = std::fopen(path, "wb");
        if (!f) return;
        uint8_t header[44] = {};
        std::fwrite(header, 1, sizeof(header), f);  // written properly once the length is known
        LOGR("audio: recording to %s", path);
    }
    void Add(const float* b, uint32_t n) {
        if (!f || !left) return;
        uint32_t start = 0;
        if (skip) {
            uint32_t s = uint32_t(std::min<uint64_t>(skip, n));
            skip -= s;
            start = s;
        }
        for (uint32_t i = start; i < n && left; i++, left--, frames++) {
            int16_t s[2];
            for (int c = 0; c < 2; c++) s[c] = int16_t(std::lround(std::clamp(b[i * 2 + c], -1.0f, 1.0f) * 32767.0f));
            std::fwrite(s, sizeof(s), 1, f);
        }
        if (!left) Finish();
    }
    void Finish() {
        if (!f) return;
        auto u32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
        auto u16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };
        uint32_t data = uint32_t(frames * 4);
        std::fseek(f, 0, SEEK_SET);
        std::fwrite("RIFF", 1, 4, f), u32(36 + data), std::fwrite("WAVEfmt ", 1, 8, f);
        u32(16), u16(1), u16(2), u32(kRate), u32(kRate * 4), u16(4), u16(16);
        std::fwrite("data", 1, 4, f), u32(data);
        std::fclose(f);
        f = nullptr;
        LOGR("audio: recording finished, %.1f s", double(frames) / kRate);
    }
};

}  // namespace

void StartOutput(std::function<void(float*, uint32_t)> render) {
    std::thread([render] {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        IXAudio2* xa = nullptr;
        IXAudio2MasteringVoice* master = nullptr;
        IXAudio2SourceVoice* voice = nullptr;
        WAVEFORMATEX wf{};
        wf.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
        wf.nChannels = 2;
        wf.nSamplesPerSec = kRate;
        wf.wBitsPerSample = 32;
        wf.nBlockAlign = 8;
        wf.nAvgBytesPerSec = kRate * 8;
        HRESULT hr = XAudio2Create(&xa, 0, XAUDIO2_DEFAULT_PROCESSOR);
        if (SUCCEEDED(hr)) hr = xa->CreateMasteringVoice(&master);
        if (SUCCEEDED(hr)) hr = xa->CreateSourceVoice(&voice, &wf);
        if (FAILED(hr)) {
            LOGE("audio: XAudio2 unavailable (%08X), no sound", unsigned(hr));
            return;
        }
        voice->Start();
        constexpr uint32_t kFrames = kRate / 100;
        constexpr uint32_t kMinQueued = 3;
        constexpr uint32_t kMaxQueued = 10;
        // one slot more than can be queued, so the one rendered is never one XAudio2 still reads
        constexpr uint32_t kSlots = kMaxQueued + 1;
        std::vector<float> bufs(kSlots * kFrames * 2);
        uint32_t depth = kMinQueued;
        for (uint32_t n = 0;;) {
            XAUDIO2_VOICE_STATE st;
            voice->GetState(&st, XAUDIO2_VOICE_NOSAMPLESPLAYED);
            // a starved voice crackles: queue one buffer more each time, up to kMaxQueued
            if (n && !st.BuffersQueued && depth < kMaxQueued) {
                depth++;
                LOGI("audio: output starved, queueing %u ms", depth * 10);
            }
            if (st.BuffersQueued >= depth) {
                Sleep(2);
                continue;
            }
            float* b = &bufs[(n % kSlots) * kFrames * 2];
            render(b, kFrames);
            static WavRecorder recorder;
            recorder.Add(b, kFrames);
            // mechassault-recomp patch: with XBE_PERF, the output's peak and RMS every 10 s (is anything playing)
            if (diag::Perf()) {
                static double sum = 0;
                static float peak = 0;
                static uint64_t count = 0;
                static auto next = std::chrono::steady_clock::now() + std::chrono::seconds(10);
                for (uint32_t i = 0; i < kFrames * 2; i++) {
                    sum += double(b[i]) * b[i];
                    peak = std::max(peak, std::fabs(b[i]));
                }
                count += kFrames * 2;
                if (std::chrono::steady_clock::now() >= next) {
                    LOGR("audio: peak %.3f, rms %.4f over 10 s", peak, std::sqrt(sum / double(count ? count : 1)));
                    sum = 0, peak = 0, count = 0;
                    next += std::chrono::seconds(10);
                }
            }
            XAUDIO2_BUFFER xb{};
            xb.AudioBytes = kFrames * 8;
            xb.pAudioData = reinterpret_cast<const BYTE*>(b);
            voice->SubmitSourceBuffer(&xb);
            n++;  // only a submitted buffer takes a slot, XAudio2 still reads the queued ones
        }
    }).detach();
}

}  // namespace audio
