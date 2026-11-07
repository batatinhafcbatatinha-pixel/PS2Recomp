#pragma once

#include <cstdint>

namespace ps2x::iop
{
    // Native SPU2 output: interleaved stereo s16 at 48 kHz. Returns frames copied; the rest is zero-filled.
    // Called from the host audio thread. PS2X_SPU2=0 disables the SPU2 model.
    uint32_t spu2ReadOutput(int16_t *interleavedStereo, uint32_t frames);
    bool spu2Enabled();
}
