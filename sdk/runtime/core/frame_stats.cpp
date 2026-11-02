// breakdown-recomp patch: frame timing statistics (see frame_stats.h)
#include "core/frame_stats.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace framestats {

void Recorder::Present(uint32_t present_us) { PresentAt(std::chrono::steady_clock::now(), present_us); }

void Recorder::PresentAt(std::chrono::steady_clock::time_point t, uint32_t present_us) {
    std::lock_guard<std::mutex> g(lock_);
    count_.fetch_add(1, std::memory_order_relaxed);
    if (!have_last_) {
        have_last_ = true;
        last_ = t;
        return;  // the first present has no interval
    }
    auto us = std::chrono::duration_cast<std::chrono::microseconds>(t - last_).count();
    last_ = t;
    Sample s;
    s.interval_us = uint32_t(std::clamp<int64_t>(us, 0, 0xFFFFFFFF));
    s.present_us = present_us;
    ring_[head_] = s;
    head_ = (head_ + 1) % kCapacity;
    size_ = std::min(size_ + 1, kCapacity);
}

void Recorder::Paced(uint32_t pace_us) {
    std::lock_guard<std::mutex> g(lock_);
    if (!size_) return;
    ring_[(head_ + kCapacity - 1) % kCapacity].pace_us = pace_us;
}

void Recorder::Reset() {
    std::lock_guard<std::mutex> g(lock_);
    head_ = size_ = 0;
    have_last_ = false;
}

size_t Recorder::Intervals(float* out, size_t n) const {
    std::lock_guard<std::mutex> g(lock_);
    n = std::min(n, size_);
    size_t start = (head_ + kCapacity - n) % kCapacity;
    for (size_t i = 0; i < n; i++) out[i] = ring_[(start + i) % kCapacity].interval_us / 1000.0f;
    return n;
}

Summary Recorder::Summarize(size_t window) const {
    std::vector<Sample> v;
    {
        std::lock_guard<std::mutex> g(lock_);
        size_t n = (window && window < size_) ? window : size_;
        v.reserve(n);
        size_t start = (head_ + kCapacity - n) % kCapacity;
        for (size_t i = 0; i < n; i++) v.push_back(ring_[(start + i) % kCapacity]);
    }
    Summary r;
    r.frames = uint32_t(v.size());
    if (v.empty()) return r;
    double total = 0, present = 0, pace = 0, jitter = 0;
    std::vector<uint32_t> iv;
    iv.reserve(v.size());
    for (size_t i = 0; i < v.size(); i++) {
        total += v[i].interval_us;
        present += v[i].present_us;
        pace += v[i].pace_us;
        if (i) jitter += std::fabs(double(v[i].interval_us) - double(v[i - 1].interval_us));
        iv.push_back(v[i].interval_us);
    }
    double n = double(v.size());
    r.avg_ms = total / n / 1000.0;
    r.avg_fps = total > 0 ? n * 1e6 / total : 0;
    r.present_ms = present / n / 1000.0;
    r.pace_ms = pace / n / 1000.0;
    r.jitter_ms = v.size() > 1 ? jitter / (n - 1) / 1000.0 : 0;
    std::sort(iv.begin(), iv.end());
    r.max_ms = iv.back() / 1000.0;
    r.p99_ms = iv[std::min(iv.size() - 1, size_t(std::ceil(n * 0.99)) - 1)] / 1000.0;
    // 1% low: the slowest 1% of frames (at least one), averaged, as a rate
    size_t worst = std::max<size_t>(1, size_t(n / 100));
    double slow = 0;
    for (size_t i = iv.size() - worst; i < iv.size(); i++) slow += iv[i];
    r.low1_fps = slow > 0 ? worst * 1e6 / slow : 0;
    uint32_t median = iv[iv.size() / 2];
    for (uint32_t x : iv)
        if (median && x > 2 * median) r.stalls++;
    return r;
}

Recorder& Title() {
    static Recorder r;
    return r;
}

}  // namespace framestats
