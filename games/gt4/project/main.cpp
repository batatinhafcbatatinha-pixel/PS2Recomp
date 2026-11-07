// Keep the shared frontend; install this ELF's recovered entries after static initialization.
#define main Gt4SharedMain
#include PS2X_SHARED_MAIN_CPP // the PS2Recomp frontend (path set by project/CMakeLists.txt)
#undef main
#include "ps2_syscalls.h"
#include "ps2_stubs.h"

struct Gt4TableEntry { uint32_t address; PS2Runtime::RecompiledFunction function; };
#include "sdk-handlers.inc"
#include "gap-functions.inc"

extern const uint32_t g_ps2RecompiledFunctionTableBase;
extern const uint32_t g_ps2RecompiledFunctionTableSlotCount;
extern PS2Runtime::RecompiledFunction g_ps2RecompiledFunctionTable[];
std::string ps2xDispatchTrail();

// GT4_PROBE=<hex>: log ra/args/trail of the first calls (then powers of two) to one guest address.
static PS2Runtime::RecompiledFunction g_probeOriginal = nullptr;
static void Gt4ProbeState(uint8_t* rdram, R5900Context* ctx) {
    if (!std::getenv("GT4_PROBE_STATE")) return;
    uint64_t tick = 0;
    uint32_t waiter = 0;
    std::memcpy(&tick, getConstMemPtr(rdram, 0x70002050u), sizeof(tick));
    std::memcpy(&waiter, getConstMemPtr(rdram, 0x70002090u), sizeof(waiter));
    std::cerr << "[probe-state] tick=" << tick << " waiter=0x" << std::hex << waiter
              << " s0=0x" << getRegU32(ctx, 16) << " s1=0x" << getRegU32(ctx, 17)
              << " status=0x" << ctx->cop0_status << std::dec << std::endl;
}
static void Gt4Probe(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime) {
    static int n = 0;
    static const char* raFilter = std::getenv("GT4_PROBE_RA");
    if (raFilter && getRegU32(ctx, 31) != std::strtoul(raFilter, nullptr, 16)) { g_probeOriginal(rdram, ctx, runtime); return; }
    ++n;
    if (n <= 6 || (n & (n - 1)) == 0 || (n <= 64 && std::getenv("GT4_PROBE_RET")))
        std::cerr << "[probe] pc=0x" << std::hex << ctx->pc << " ra=0x" << getRegU32(ctx, 31) << " a0=0x" << getRegU32(ctx, 4)
                  << " a1=0x" << getRegU32(ctx, 5) << " a2=0x" << getRegU32(ctx, 6) << " sp=0x" << getRegU32(ctx, 29) << std::dec
                  << " trail=" << ps2xDispatchTrail() << std::endl;
    if (n <= 64 || (n & (n - 1)) == 0) Gt4ProbeState(rdram, ctx);
    g_probeOriginal(rdram, ctx, runtime);
    if (n <= 64 && std::getenv("GT4_PROBE_RET"))
        std::cerr << "[probe] #" << n << " ret v0=0x" << std::hex << getRegU32(ctx, 2) << " pc=0x" << ctx->pc << std::dec << std::endl;
}

// libfs (EE side of the IOP file server). The runtime serves sceOpen/sceRead/... natively, so the guest sceFsInit
// never binds its RPC client: the remaining libfs entries would call the server with sid 0 and wait forever.
// They get native answers too: stat/mkdir through the runtime's file system, and "no such device" for the
// devices this PC build does not have (dev9 network adapter, hdd/pfs). Addresses: analyzer matches in CORE.ELF.
template <int Result>
static void Gt4FsReturn(uint8_t*, R5900Context* ctx, PS2Runtime*) {
    setReturnS32(ctx, Result);
    ctx->pc = getRegU32(ctx, 31);
}
static void Gt4FsGetstat(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime) {
    ctx->pc = getRegU32(ctx, 31);
    ps2_syscalls::fioGetstat(rdram, ctx, runtime);
}
static void Gt4FsMkdir(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime) {
    ctx->pc = getRegU32(ctx, 31);
    ps2_syscalls::fioMkdir(rdram, ctx, runtime);
}
static void Gt4FsDevctl(uint8_t* rdram, R5900Context* ctx, PS2Runtime*) {
    static int logged = 0;
    const char* name = reinterpret_cast<const char*>(getConstMemPtr(rdram, getRegU32(ctx, 4)));
    if (logged++ < 8)
        std::cerr << "[gt4:fs] sceDevctl(" << (name ? name : "?") << ", 0x" << std::hex << getRegU32(ctx, 5) << std::dec
                  << ") -> no device" << std::endl;
    setReturnS32(ctx, -19); // ENODEV
    ctx->pc = getRegU32(ctx, 31);
}
static const Gt4TableEntry kGt4FsHandlers[] = {
    {0x5B2BC0u, Gt4FsReturn<0>},   // sceFsSetIopBuf
    {0x5B2D00u, Gt4FsReturn<0>},   // sceFsSetIopPrio
    {0x5B40A8u, Gt4FsMkdir},       // sceMkdir
    {0x5B4280u, Gt4FsReturn<-19>}, // sceFormat
    {0x5B4650u, Gt4FsReturn<-2>},  // sceDopen
    {0x5B4718u, Gt4FsReturn<-9>},  // sceDclose
    {0x5B4880u, Gt4FsReturn<-9>},  // sceDread
    {0x5B49D8u, Gt4FsGetstat},     // sceGetstat
    {0x5B4B90u, Gt4FsReturn<-1>},  // sceChstat
    {0x5B4DE0u, Gt4FsReturn<-1>},  // sceRename
    {0x5B5008u, Gt4FsReturn<0>},   // sceSync
    {0x5B51B8u, Gt4FsReturn<-19>}, // sceMount
    {0x5B5698u, Gt4FsDevctl},      // sceDevctl
    {0x5B5940u, Gt4FsReturn<-19>}, // sceSymlink
    {0x5B5B38u, Gt4FsReturn<-19>}, // sceReadlink
};

int main(int argc, char** argv) {
    // GT4 needs the IOP's USB host controller (USBD unloads without it and the game waits on USB storage);
    // the runtime only emulates it on request. An explicit PS2X_IOP_USB still wins.
    if (!std::getenv("PS2X_IOP_USB")) {
#ifdef _WIN32
        _putenv_s("PS2X_IOP_USB", "1");
#else
        setenv("PS2X_IOP_USB", "1", 0);
#endif
    }
    for (const auto& entry : kGt4GapFunctions) {
        const uint32_t slot = (entry.address - g_ps2RecompiledFunctionTableBase) / 4;
        if (entry.address >= g_ps2RecompiledFunctionTableBase && slot < g_ps2RecompiledFunctionTableSlotCount) {
            // The AOT table also binds every Ghidra label address to the translated function that contains it.
            // A gap start is outside every Ghidra function, so such a binding belongs to a neighbour: replace it.
            g_ps2RecompiledFunctionTable[slot] = entry.function;
        }
    }
    // SDK library matches replace their translated guest code with the runtime's native IOP-side services
    // (SIF RPC, CD, pad, memory card, ...): there is no IOP processor answering these requests.
    if (!std::getenv("GT4_GUEST_SDK")) {
        for (const auto& entry : kGt4SdkHandlers) {
            const uint32_t slot = (entry.address - g_ps2RecompiledFunctionTableBase) / 4;
            if (entry.address < g_ps2RecompiledFunctionTableBase || slot >= g_ps2RecompiledFunctionTableSlotCount) {
                std::cerr << "GT4 SDK handler outside AOT table\n";
                return 2;
            }
            g_ps2RecompiledFunctionTable[slot] = entry.function;
        }
        for (const auto& entry : kGt4FsHandlers)
            g_ps2RecompiledFunctionTable[(entry.address - g_ps2RecompiledFunctionTableBase) / 4] = entry.function;
    }
    if (const char* probe = std::getenv("GT4_PROBE")) {
        const uint32_t slot = (std::strtoul(probe, nullptr, 16) - g_ps2RecompiledFunctionTableBase) / 4;
        if (slot < g_ps2RecompiledFunctionTableSlotCount && g_ps2RecompiledFunctionTable[slot]) {
            g_probeOriginal = g_ps2RecompiledFunctionTable[slot];
            g_ps2RecompiledFunctionTable[slot] = Gt4Probe;
        }
    }
    return Gt4SharedMain(argc, argv);
}
