// unit test for sdk/runtime/core/frame_stats (build: see tests/README or run tests/run.sh)
#include "core/frame_stats.h"
#include <cassert>
#include <cmath>
#include <cstdio>
using namespace std::chrono;
static bool near(double a, double b, double e) { return std::fabs(a - b) <= e; }
int main() {
    framestats::Recorder r;
    auto t = steady_clock::time_point{} + seconds(100);
    // 300 frames at 30 fps, with 3 hitches of 100 ms
    r.PresentAt(t, 0);
    for (int i = 1; i <= 300; i++) {
        t += microseconds(i % 100 == 0 ? 100000 : 33333);
        r.PresentAt(t, 500);
        r.Paced(1000);
    }
    auto s = r.Summarize();
    std::printf("frames %u avg %.2f fps %.3f ms low1 %.2f p99 %.2f max %.2f stalls %u jitter %.3f present %.2f pace %.2f\n",
                s.frames, s.avg_fps, s.avg_ms, s.low1_fps, s.p99_ms, s.max_ms, s.stalls, s.jitter_ms, s.present_ms, s.pace_ms);
    assert(s.frames == 300);
    assert(s.stalls == 3);
    assert(near(s.max_ms, 100.0, 0.01));
    assert(near(s.low1_fps, 10.0, 0.01));   // worst 1% = 3 frames of 100 ms
    assert(near(s.present_ms, 0.5, 1e-6) && near(s.pace_ms, 1.0, 1e-6));
    auto w = r.Summarize(50);  // the last 50 frames: one hitch (frame 300)
    assert(w.frames == 50 && w.stalls == 1);
    float g[8];
    assert(r.Intervals(g, 8) == 8 && near(g[7], 100.0, 0.01));
    // ring wraps
    for (int i = 0; i < 2000; i++) { t += microseconds(16667); r.PresentAt(t, 0); }
    auto f = r.Summarize();
    assert(f.frames == framestats::kCapacity && near(f.avg_fps, 60.0, 0.01) && f.stalls == 0);
    std::puts("frame_stats ok");
}
