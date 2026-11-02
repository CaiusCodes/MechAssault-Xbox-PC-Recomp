#pragma once
// breakdown-recomp patch: frame timing statistics shared by the FPS counter, the diagnostics overlay and the log.
// Platform neutral (steady_clock only) so the harness and unit tests use the same code as the Windows runtime.
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>

namespace framestats {

// what one frame cost, in microseconds; zero when not measured
struct Sample {
    uint32_t interval_us = 0;  // previous title present to this one: what the player sees
    uint32_t present_us = 0;   // host work inside the present (submit, swap chain present)
    uint32_t pace_us = 0;      // time slept or waited by the frame limiter after the present
};

struct Summary {
    uint32_t frames = 0;        // samples in the window
    double avg_fps = 0;         // frames / summed intervals
    double avg_ms = 0;          // mean interval
    double low1_fps = 0;        // 1% low: mean of the slowest 1% of intervals, as fps
    double p99_ms = 0;          // 99th percentile interval
    double max_ms = 0;          // slowest interval
    double present_ms = 0;      // mean host present work
    double pace_ms = 0;         // mean limiter wait
    double jitter_ms = 0;       // mean absolute change between consecutive intervals
    uint32_t stalls = 0;        // intervals over twice the median: hitches the player notices
};

constexpr size_t kCapacity = 1024;  // about 17 s at 60 fps, 34 s at 30

class Recorder {
public:
    // the title presented a frame: call once per title present, before the limiter
    void Present(uint32_t present_us);
    // the limiter's wait after that present
    void Paced(uint32_t pace_us);
    // the last `window` frames (all kept frames when 0 or larger than the history)
    Summary Summarize(size_t window = 0) const;
    // the last n intervals in milliseconds, oldest first, for a graph; returns how many were written
    size_t Intervals(float* out, size_t n) const;
    // frames recorded since start
    uint64_t Count() const { return count_.load(std::memory_order_relaxed); }
    void Reset();

    // for tests: a present at an explicit time
    void PresentAt(std::chrono::steady_clock::time_point t, uint32_t present_us);

private:
    mutable std::mutex lock_;
    std::array<Sample, kCapacity> ring_{};
    size_t head_ = 0, size_ = 0;
    std::chrono::steady_clock::time_point last_{};
    bool have_last_ = false;
    std::atomic<uint64_t> count_{0};
};

// the title's frames (D3DDevice_Swap)
Recorder& Title();

}  // namespace framestats
