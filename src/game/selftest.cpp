// XBE_SELFTEST=1: checks of the recompiled code that ordinary play may not reach, run once on a game thread from the
// first camera build (widescreen.cpp) and logged as "selftest: ...".
//
// memcpy: the game's own CRT memcpy (0x1D0580) for every length 0..64 at every source and destination alignment.
// It copies the tail through a jump table of small blocks; the xboxrecomp toolkit, run on this game, once returned
// from the 28..31 byte tails with the stack pointer a byte off (its fixes #133..#135). Each call must copy exactly
// the bytes asked for, return the destination, pop only its return address, and keep ebx, esi, edi and ebp.
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "common/release.h"
#include "core/log.h"
#include "cpu/x86_context.h"
#include "cpu/x86_ops.h"
#include "kernel/kernel.h"

namespace {

#define kMemcpy (mechassault::Current().memcpy)  // 2002: 0x1D0580

int TestMemcpy(X86Context& ctx, uint8_t* base) {
    // buffers in the free stack below the hook's own frame
    uint32_t esp0 = ctx.esp;
    uint32_t src = (esp0 - 0x1000) & ~0xFFu, dst = src - 0x200;
    ctx.esp = dst - 0x100;
    int bad = 0;
    for (uint32_t len = 0; len <= 64; len++)
        for (uint32_t sa = 0; sa < 4; sa++)
            for (uint32_t da = 0; da < 4; da++) {
                uint8_t* s = static_cast<uint8_t*>(kernel::HostPtr(src));
                uint8_t* d = static_cast<uint8_t*>(kernel::HostPtr(dst));
                for (int i = 0; i < 0x100; i++) {
                    s[i] = uint8_t(i * 7 + 1);
                    d[i] = 0xEE;
                }
                X86Context keep = ctx;
                uint32_t sp = ctx.esp;
                ctx.esp -= 4;
                kernel::W32(ctx.esp, len);
                ctx.esp -= 4;
                kernel::W32(ctx.esp, src + sa);
                ctx.esp -= 4;
                kernel::W32(ctx.esp, dst + 0x40 + da);
                ctx.esp -= 4;
                kernel::W32(ctx.esp, 0xFFFFFFF0u);
                uint32_t want_esp = ctx.esp + 4;  // cdecl: the callee pops its return address only
                x86_icall(ctx, base, kMemcpy);
                bool copied = std::memcmp(d + 0x40 + da, s + sa, len) == 0;
                bool clean = true;
                for (uint32_t i = 0; i < 0x100; i++)
                    if ((i < 0x40 + da || i >= 0x40 + da + len) && d[i] != 0xEE) clean = false;
                bool regs = ctx.ebx == keep.ebx && ctx.esi == keep.esi && ctx.edi == keep.edi && ctx.ebp == keep.ebp;
                if (!copied || !clean || ctx.eax != dst + 0x40 + da || ctx.esp != want_esp || !regs) {
                    if (bad++ < 10)
                        LOGE("selftest: memcpy len %u src+%u dst+%u: copied %d, outside untouched %d, eax %08X, "
                             "esp %+d, saved registers %d",
                             len, sa, da, copied, clean, ctx.eax, int(ctx.esp - want_esp), regs);
                }
                ctx = keep;
                ctx.esp = sp;
            }
    ctx.esp = esp0;
    return bad;
}

}  // namespace

void MechAssaultSelfTest(X86Context& ctx, uint8_t* base) {
    static const bool on = std::getenv("XBE_SELFTEST") != nullptr;
    static bool done = false;
    if (!on || done) return;
    done = true;
    X86Context keep = ctx;
    int bad = TestMemcpy(ctx, base);
    ctx = keep;
    LOGR("selftest: memcpy, 65 lengths x 16 alignments: %s (%d failed)", bad ? "FAILED" : "ok", bad);
}
