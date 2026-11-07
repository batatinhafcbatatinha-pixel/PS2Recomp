#pragma once
// Run-time recompilation ("hot" code): when the game reaches guest code that has no translation, the runtime runs
// tools/ps2port/hotrecomp.py, which translates that function with ps2_recomp and compiles it into a DLL with the
// compiler already installed for the build. The DLL calls back into the executable only through this table (the
// PS2Runtime services the generated code uses), so the executable needs no exported symbols.
#include "ps2_runtime.h"

struct PS2HotApi
{
    uint32_t version;
    uint8_t (PS2Runtime::*load8)(uint8_t *, R5900Context *, uint32_t);
    uint16_t (PS2Runtime::*load16)(uint8_t *, R5900Context *, uint32_t);
    uint32_t (PS2Runtime::*load32)(uint8_t *, R5900Context *, uint32_t);
    uint64_t (PS2Runtime::*load64)(uint8_t *, R5900Context *, uint32_t);
    __m128i (PS2Runtime::*load128)(uint8_t *, R5900Context *, uint32_t);
    void (PS2Runtime::*store8)(uint8_t *, R5900Context *, uint32_t, uint8_t);
    void (PS2Runtime::*store16)(uint8_t *, R5900Context *, uint32_t, uint16_t);
    void (PS2Runtime::*store32)(uint8_t *, R5900Context *, uint32_t, uint32_t);
    void (PS2Runtime::*store64)(uint8_t *, R5900Context *, uint32_t, uint64_t);
    void (PS2Runtime::*store128)(uint8_t *, R5900Context *, uint32_t, __m128i);
    bool (PS2Runtime::*dispatchGuestBranch)(uint8_t *, R5900Context *, uint32_t, uint32_t, uint32_t,
                                            PS2Runtime::GuestBranchKind, const char *);
    bool (PS2Runtime::*eeCheckpointDue)(uint32_t) noexcept;
    void (PS2Runtime::*handleBreak)(uint8_t *, R5900Context *);
    void (PS2Runtime::*handleTrap)(uint8_t *, R5900Context *);
    void (PS2Runtime::*signalException)(R5900Context *, PS2Exception);
    void (PS2Runtime::*handleSyscall2)(uint8_t *, R5900Context *);
    void (PS2Runtime::*handleSyscall3)(uint8_t *, R5900Context *, uint32_t);
    PS2Runtime::RecompiledFunction (PS2Runtime::*lookupFunction)(uint32_t);
    // The guest code at the address changed (another overlay): translate the code that is there now.
    PS2Runtime::RecompiledFunction (*retranslate)(PS2Runtime *, uint32_t);
};

constexpr uint32_t kPS2HotApiVersion = 1u;

// In the DLL: ps2x_hot_bind(api) stores the table, ps2x_hot_entry() returns the translated function.
using PS2HotBindFn = void (*)(const PS2HotApi *);
using PS2HotEntryFn = PS2Runtime::RecompiledFunction (*)();
