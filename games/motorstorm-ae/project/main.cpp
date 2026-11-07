// Keep the shared frontend; install this ELF's recovered entries after static initialization.
#define main MsaeSharedMain
#include PS2X_SHARED_MAIN_CPP // the PS2Recomp frontend (path set by project/CMakeLists.txt)
#undef main
#include "ps2_syscalls.h"
#include "ps2_stubs.h"
#include "ps2_runtime_macros.h"
#include <cstring>

struct MsaeTableEntry { uint32_t address; PS2Runtime::RecompiledFunction function; };
#include "sdk-handlers.inc"
#include "gap-functions.inc"

extern const uint32_t g_ps2RecompiledFunctionTableBase;
extern const uint32_t g_ps2RecompiledFunctionTableSlotCount;
extern PS2Runtime::RecompiledFunction g_ps2RecompiledFunctionTable[];
std::string ps2xDispatchTrail();

// MSAE_PROBE=<hex>: log ra/args/trail of the first calls (then powers of two) to one guest address.
static PS2Runtime::RecompiledFunction g_probeOriginal = nullptr;
static void MsaeProbe(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime) {
    static int n = 0;
    static const char* raFilter = std::getenv("MSAE_PROBE_RA");
    if (raFilter && getRegU32(ctx, 31) != std::strtoul(raFilter, nullptr, 16)) { g_probeOriginal(rdram, ctx, runtime); return; }
    ++n;
    if (n <= 6 || (n & (n - 1)) == 0 || (n <= 64 && std::getenv("MSAE_PROBE_RET")))
        std::cerr << "[probe] pc=0x" << std::hex << ctx->pc << " ra=0x" << getRegU32(ctx, 31) << " a0=0x" << getRegU32(ctx, 4)
                  << " a1=0x" << getRegU32(ctx, 5) << " a2=0x" << getRegU32(ctx, 6) << " sp=0x" << getRegU32(ctx, 29) << std::dec
                  << " trail=" << ps2xDispatchTrail() << std::endl;
    g_probeOriginal(rdram, ctx, runtime);
    if (n <= 64 && std::getenv("MSAE_PROBE_RET"))
        std::cerr << "[probe] #" << n << " ret v0=0x" << std::hex << getRegU32(ctx, 2) << " pc=0x" << ctx->pc << std::dec << std::endl;
}

// The game's memory setup (0x25BAA0) sizes its own allocator as [end of .bss, 0x01FF0000): the real kernel's
// EndOfHeap with the 64 KiB main stack at the top of RAM. It then claims that range with 0x40000-byte mallocs
// (0x25BB18) and ignores failures. The runtime keeps its arena above EndOfHeap = 0x01F00000, so the last mallocs
// failed, later small mallocs landed inside the claimed range and the allocator's memset wiped them (null vtable
// call at 0x13B5D0). Same formula, with the runtime's EndOfHeap as the top.
static PS2Runtime::RecompiledFunction g_memorySetupOriginal = nullptr;
static void MsaeMemorySetup(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime) {
    g_memorySetupOriginal(rdram, ctx, runtime); // base -> gp-0x1EF4, size -> gp-0x1EE4 (also in v0)
    const uint32_t gp = getRegU32(ctx, 28);
    uint32_t base = 0;
    std::memcpy(&base, rdram + ((gp - 0x1EF4u) & 0x1FFFFFFu), 4);
    const uint64_t savedV0 = GPR_U64(ctx, 2);
    ps2_syscalls::EndOfHeap(rdram, ctx, runtime);
    const uint32_t top = getRegU32(ctx, 2);
    SET_GPR_U64(ctx, 2, savedV0);
    uint32_t size = 0;
    std::memcpy(&size, rdram + ((gp - 0x1EE4u) & 0x1FFFFFFu), 4);
    if (top > base + 0x10u && base + size + 0x10u > top) {
        size = top - base - 0x10u;
        std::memcpy(rdram + ((gp - 0x1EE4u) & 0x1FFFFFFu), &size, 4);
        SET_GPR_U32(ctx, 2, size);
        std::cerr << "[msae:memory] arena 0x" << std::hex << base << " size 0x" << size << " (top 0x" << top << ")"
                  << std::dec << std::endl;
    }
}

int main(int argc, char** argv) {
    for (const auto& entry : kMsaeGapFunctions) {
        const uint32_t slot = (entry.address - g_ps2RecompiledFunctionTableBase) / 4;
        if (entry.address >= g_ps2RecompiledFunctionTableBase && slot < g_ps2RecompiledFunctionTableSlotCount) {
            // The AOT table also binds every Ghidra label address to the translated function that contains it.
            // A gap start is outside every Ghidra function, so such a binding belongs to a neighbour: replace it.
            g_ps2RecompiledFunctionTable[slot] = entry.function;
        }
    }
    // SDK library matches replace their translated guest code with the runtime's native IOP-side services
    // (SIF RPC, CD, pad, memory card, ...): there is no IOP processor answering these requests.
    if (!std::getenv("MSAE_GUEST_SDK")) {
        for (const auto& entry : kMsaeSdkHandlers) {
            const uint32_t slot = (entry.address - g_ps2RecompiledFunctionTableBase) / 4;
            if (entry.address < g_ps2RecompiledFunctionTableBase || slot >= g_ps2RecompiledFunctionTableSlotCount) {
                std::cerr << "MSAE SDK handler outside AOT table\n";
                return 2;
            }
            g_ps2RecompiledFunctionTable[slot] = entry.function;
        }
    }
    {
        const uint32_t slot = (0x25BAA0u - g_ps2RecompiledFunctionTableBase) / 4;
        if (slot >= g_ps2RecompiledFunctionTableSlotCount || !g_ps2RecompiledFunctionTable[slot]) {
            std::cerr << "MSAE memory setup 0x25BAA0 missing from the AOT table\n";
            return 2;
        }
        g_memorySetupOriginal = g_ps2RecompiledFunctionTable[slot];
        g_ps2RecompiledFunctionTable[slot] = MsaeMemorySetup;
    }
    if (const char* probe = std::getenv("MSAE_PROBE")) {
        const uint32_t slot = (std::strtoul(probe, nullptr, 16) - g_ps2RecompiledFunctionTableBase) / 4;
        if (slot < g_ps2RecompiledFunctionTableSlotCount && g_ps2RecompiledFunctionTable[slot]) {
            g_probeOriginal = g_ps2RecompiledFunctionTable[slot];
            g_ps2RecompiledFunctionTable[slot] = MsaeProbe;
        }
    }
    return MsaeSharedMain(argc, argv);
}
