#pragma once
// Patches and cheats chosen in the launcher, applied at run time (src/lib/Kernel/ps2_patches.cpp).
#include <cstdint>

class PS2Runtime;

namespace ps2_patches
{
    // After the executable is loaded: memory writes, then the retranslated functions of code patches.
    void load(PS2Runtime &runtime, uint8_t *rdram);
    // At every VBlank: the writes that must keep their value (cheats).
    void everyVblank(uint8_t *rdram);
}
