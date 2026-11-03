// mechassault-recomp patch: where the frame drew the images the game layer provides for button prompts
// (texturedumper's provider), so a click on one can press its button. Positions are fractions of the picture, which
// sits at (x, y) with size (w, h) in a backbuffer of bw x bh pixels, the window's client area.
#pragma once
#include <cstdint>
#include <vector>

namespace gpu {

// a menu's highlight bar (the chosen item), recorded among the prompts under this key
constexpr uint64_t kPromptHighlight = 1;

struct PromptRect {
    uint64_t key;
    float x0, y0, x1, y1;
};
struct PromptFrame {
    std::vector<PromptRect> rects;
    float x = 0, y = 0, w = 0, h = 0, bw = 0, bh = 0;
};
PromptFrame Prompts();  // the last presented frame's

}  // namespace gpu
