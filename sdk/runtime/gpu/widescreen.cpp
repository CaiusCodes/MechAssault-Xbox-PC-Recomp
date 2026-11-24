// breakdown-recomp patch: see widescreen.h
#include "gpu/widescreen.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <vector>

namespace gpu::wide {

namespace {
std::atomic<float> g_view{1.0f};
}  // namespace

void SetView(float view) { g_view.store(view > 1.0f ? view : 1.0f, std::memory_order_relaxed); }
float View() { return g_view.load(std::memory_order_relaxed); }

// mechassault-recomp patch: the title's known HUD pieces, set once before play (sorted for the lookup; a function's
// static, so a title can set them from a static initializer of its own)
namespace {
std::vector<uint64_t>& Known() {
    static std::vector<uint64_t> known;
    return known;
}
}  // namespace
void SetKnownHud(const uint64_t* sigs, size_t count) {
    Known().assign(sigs, sigs + count);
    std::sort(Known().begin(), Known().end());
}
bool KnownHud(uint64_t sig) { return sig && std::binary_search(Known().begin(), Known().end(), sig); }

float Factor(uint32_t gw, uint32_t gh, bool reads_target) {
    float view = View();
    if (view <= 1.0f || reads_target || !gw || !gh || gw * 3 != gh * 4) return 1.0f;
    return 1.0f / view;
}

}  // namespace gpu::wide
