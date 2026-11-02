#pragma once
#include <cstdint>

namespace input {

// the console controller's report, analog buttons are 0-255
struct Pad {
    bool connected = false;
    uint16_t buttons = 0;  // dpad up/down/left/right, start, back, left thumb, right thumb
    uint8_t analog[8] = {};  // A B X Y black white left trigger right trigger
    int16_t lx = 0, ly = 0, rx = 0, ry = 0;
};

void Init();  // window thread, before boot; the game code window starts it earlier
uint16_t HeldOnAnyController();  // face buttons as XINPUT_GAMEPAD_A/B/X/Y masks, GameInfo::pad left out
// the controller on that port, then GameInfo::pad
Pad Poll(int port);
void Rumble(int port, uint16_t left, uint16_t right);

// breakdown-recomp patch: every controller at once, for the PC settings menu (overlay.cpp). Holding Back and pressing
// Start opens or closes it; that press, and whatever is still held when the menu closes, is kept from the game.
enum MenuButton : uint32_t {
    kMenuA = 1, kMenuB = 2, kMenuX = 4, kMenuY = 8, kMenuUp = 16, kMenuDown = 32, kMenuLeft = 64, kMenuRight = 128,
    kMenuLB = 256, kMenuRB = 512,
};
struct MenuPad {
    uint32_t buttons = 0;  // MenuButton bits
    float lx = 0, ly = 0;  // the left sticks, -1 to 1, y up
    bool toggle = false;   // Back and Start pressed together since the last read
};
MenuPad ReadMenuPad();

}  // namespace input
