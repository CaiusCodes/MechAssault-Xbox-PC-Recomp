// mechassault-recomp: see release.h
#include "common/release.h"

#include "core/log.h"
#include "kernel/kernel.h"

namespace mechassault {

namespace {

// Rev 1 (2003-08-08, xbe SHA-1 372a01bb..., code 01462435...); the data moved up 0x29040 (front-end globals) and
// 0x2BD98 (the player and profile) against 2002, the code by section
constexpr GameRelease kRev1{"Rev 1 (2003)", 0x247130, 0x3B3A24, 0x31C708, 0x31FCA0, 0x3B1FF8, 0x380100, 0x1ED440};
// the original release (2002-10-13, xbe SHA-1 682ddd1e..., code 919910b4...)
constexpr GameRelease kOriginal{"original (2002)", 0x227460, 0x387C8C, 0x2FA4C0, 0x2FDA50, 0x386260, 0x3570C0, 0x1D0580};

}  // namespace

const GameRelease& Current() {
    static const GameRelease* known = nullptr;
    if (known) return *known;
    if (!kernel::g_base || kernel::R32(0x10000) != 0x48454258) return kRev1;  // "XBEH": not loaded yet
    uint32_t base = kernel::R32(0x10104), headers = kernel::R32(0x10120);
    uint32_t text = kernel::R32(headers + 8);  // the first section, .text: its virtual size
    known = text == kOriginal.text_size ? &kOriginal : &kRev1;
    if (text != kOriginal.text_size && text != kRev1.text_size)
        LOGW("mechassault: unknown release (.text %X at base %X), using Rev 1's addresses", text, base);
    LOGR("mechassault: %s release", known->name);
    return *known;
}

}  // namespace mechassault
