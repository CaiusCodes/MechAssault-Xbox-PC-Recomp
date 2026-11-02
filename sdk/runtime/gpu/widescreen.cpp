// breakdown-recomp patch: see widescreen.h
#include "gpu/widescreen.h"

#include <atomic>

namespace gpu::wide {

namespace {
std::atomic<float> g_view{1.0f};
}  // namespace

void SetView(float view) { g_view.store(view > 1.0f ? view : 1.0f, std::memory_order_relaxed); }
float View() { return g_view.load(std::memory_order_relaxed); }

float Factor(uint32_t gw, uint32_t gh, bool reads_target) {
    float view = View();
    if (view <= 1.0f || reads_target || !gw || !gh || gw * 3 != gh * 4) return 1.0f;
    return 1.0f / view;
}

}  // namespace gpu::wide
