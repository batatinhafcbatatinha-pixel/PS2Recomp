// Keep the shared frontend; install this ELF's recovered entries after static initialization.
#define main VcsSharedMain
#include PS2X_SHARED_MAIN_CPP // the PS2Recomp frontend (path set by project/CMakeLists.txt)
#undef main
#include "ps2_syscalls.h"
#include "ps2_stubs.h"
#include "Stubs/SIF.h" // sceSifSearchModuleByName (not in the call list)
#include "Stubs/FileIO.h" // sceDopen/sceDread/sceDclose
#include "Stubs/CD.h"

struct VcsTableEntry { uint32_t address; PS2Runtime::RecompiledFunction function; };
#include "sdk-handlers.inc"
#include "gap-functions.inc"

extern const uint32_t g_ps2RecompiledFunctionTableBase;
extern const uint32_t g_ps2RecompiledFunctionTableSlotCount;
extern PS2Runtime::RecompiledFunction g_ps2RecompiledFunctionTable[];
std::string ps2xDispatchTrail();

// VCS_PROBE=<hex>: log ra/args/trail of the first calls (then powers of two) to one guest address.
static PS2Runtime::RecompiledFunction g_probeOriginal = nullptr;
static void VcsProbe(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime) {
    static int n = 0;
    static const char* raFilter = std::getenv("VCS_PROBE_RA");
    if (raFilter && getRegU32(ctx, 31) != std::strtoul(raFilter, nullptr, 16)) { g_probeOriginal(rdram, ctx, runtime); return; }
    ++n;
    if (n <= 6 || (n & (n - 1)) == 0 || (n <= 64 && std::getenv("VCS_PROBE_RET")))
        std::cerr << "[probe] pc=0x" << std::hex << ctx->pc << " ra=0x" << getRegU32(ctx, 31) << " a0=0x" << getRegU32(ctx, 4)
                  << " a1=0x" << getRegU32(ctx, 5) << " a2=0x" << getRegU32(ctx, 6) << " sp=0x" << getRegU32(ctx, 29) << std::dec
                  << " trail=" << ps2xDispatchTrail() << std::endl;
    const uint32_t a0 = getRegU32(ctx, 4);
    g_probeOriginal(rdram, ctx, runtime);
    // VCS_PROBE_DUMP=<quadwords>: hex dump of the buffer a0 pointed to, after the call (first 4 calls).
    if (const char* dump = std::getenv("VCS_PROBE_DUMP"); dump && n <= 4) {
        const uint32_t base = a0 & 0x01FFFFF0u;
        for (unsigned q = 0; q < std::strtoul(dump, nullptr, 10) && base + q * 16u + 16u <= PS2_RAM_SIZE; ++q) {
            const uint32_t* w = reinterpret_cast<const uint32_t*>(rdram + base + q * 16u);
            std::fprintf(stderr, "[probe:dump] %08x: %08x %08x %08x %08x\n", base + q * 16u, w[0], w[1], w[2], w[3]);
        }
    }
    // VCS_PROBE_DEREF=<ptr>,<off>,<qw>: after the power-of-two calls, dump qw quadwords at *(ptr) + off.
    if (const char* deref = std::getenv("VCS_PROBE_DEREF"); deref && (n & (n - 1)) == 0) {
        char* e = nullptr;
        const uint32_t ptr = std::strtoul(deref, &e, 16), off = std::strtoul(e + 1, &e, 16), qw = std::strtoul(e + 1, nullptr, 10);
        const uint32_t base = (*reinterpret_cast<const uint32_t*>(rdram + (ptr & 0x01FFFFFCu)) + off) & 0x01FFFFF0u;
        for (uint32_t q = 0; q < qw && base + q * 16u + 16u <= PS2_RAM_SIZE; ++q) {
            const uint32_t* w = reinterpret_cast<const uint32_t*>(rdram + base + q * 16u);
            std::fprintf(stderr, "[probe:deref] #%d %08x: %08x %08x %08x %08x\n", n, base + q * 16u, w[0], w[1], w[2], w[3]);
        }
    }
    // VCS_PROBE_RET=1: return value of the first 64 calls; VCS_PROBE_RETALL=1: also of the power-of-two calls after.
    if ((n <= 64 && std::getenv("VCS_PROBE_RET")) || (std::getenv("VCS_PROBE_RETALL") && (n & (n - 1)) == 0))
        std::cerr << "[probe] #" << n << " ret v0=0x" << std::hex << getRegU32(ctx, 2) << " pc=0x" << ctx->pc << std::dec << std::endl;
}

// VCS_HEAPCHECK=1: before each small allocation (0x2BC730, sizes <= 0x100), check the next pointer of the free-list
// head that is about to be popped; log the first corrupt block (the main thread died on new(0x34) returning
// 0xF0000FC2, a value written into a freed block). Then watch that block with PS2X_WATCH=<addr>:4.
static PS2Runtime::RecompiledFunction g_smallAllocOriginal = nullptr;
static void VcsSmallAllocCheck(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime) {
    const uint32_t heap = getRegU32(ctx, 4), size = getRegU32(ctx, 5);
    auto rd = [&](uint32_t a) -> uint32_t {
        a &= 0x1FFFFFFu;
        if (a > PS2_RAM_SIZE - 4) return 0xDEADDEADu;
        uint32_t v; std::memcpy(&v, rdram + a, 4); return v;
    };
    static int reports = 0;
    auto bad = [](uint32_t v) { return v && ((v & 0xE0000000u) == 0xE0000000u || (v & 0x1FFFFFFu) >= PS2_RAM_SIZE); };
    uint32_t bin = 0, head = 0, next = 0;
    if (size && size <= 0x100) {
        bin = rd(heap + ((size + 3) / 4 - 1) * 4 + 0x24);
        head = bin ? rd(bin + 4) : 0;
        next = head && !bad(head) ? rd(head) : 0;
    }
    const uint32_t ra = getRegU32(ctx, 31);
    if ((bad(head) || bad(next)) && reports < 8) {
        ++reports;
        std::cerr << "[heapcheck] before size=0x" << std::hex << size << " heap=0x" << heap << " bin=0x" << bin
                  << " head=0x" << head << " next=0x" << next << " ra=0x" << ra << std::dec
                  << " trail=" << ps2xDispatchTrail() << std::endl;
    }
    g_smallAllocOriginal(rdram, ctx, runtime);
    const uint32_t result = getRegU32(ctx, 2);
    if (bad(result) && reports < 8) {
        ++reports;
        std::cerr << "[heapcheck] returned 0x" << std::hex << result << " size=0x" << size << " bin=0x" << bin
                  << " head(before)=0x" << head << " ra=0x" << ra << " binwords:";
        for (uint32_t i = 0; i < 8; ++i) std::cerr << " " << rd(bin + i * 4);
        std::cerr << std::dec << std::endl;
    }
}

int main(int argc, char** argv) {
    // Defaults for this game (an explicit environment value still wins):
    // - the runtime's arena and callback stacks below the ELF: GTA VCS's own heap runs up to 0x1FE0000 (see NOTES.md);
    // - the native GS renderer: the emulated-GS path draws the screen filter as translucent rectangles.
#ifdef _WIN32
    if (!std::getenv("PS2X_RUNTIME_ARENA")) _putenv_s("PS2X_RUNTIME_ARENA", "80000:100000");
    if (!std::getenv("PS2X_GS")) _putenv_s("PS2X_GS", "hw");
#else
    setenv("PS2X_RUNTIME_ARENA", "80000:100000", 0);
    setenv("PS2X_GS", "hw", 0);
#endif
    for (const auto& entry : kVcsGapFunctions) {
        const uint32_t slot = (entry.address - g_ps2RecompiledFunctionTableBase) / 4;
        if (entry.address >= g_ps2RecompiledFunctionTableBase && slot < g_ps2RecompiledFunctionTableSlotCount) {
            // The AOT table also binds every Ghidra label address to the translated function that *contains* it.
            // A gap start is outside every Ghidra function, so a slot it occupies belongs to a neighbour: replace it.
            g_ps2RecompiledFunctionTable[slot] = entry.function;
        }
    }
    // SDK library matches replace their translated guest code with the runtime's native IOP-side services
    // (SIF RPC, CD, pad, memory card, ...). VCS_GUEST_SDK=1 keeps the guest copies.
    if (!std::getenv("VCS_GUEST_SDK")) {
        for (const auto& entry : kVcsSdkHandlers) {
            const uint32_t slot = (entry.address - g_ps2RecompiledFunctionTableBase) / 4;
            if (entry.address < g_ps2RecompiledFunctionTableBase || slot >= g_ps2RecompiledFunctionTableSlotCount) {
                std::cerr << "VCS SDK handler outside AOT table\n";
                return 2;
            }
            g_ps2RecompiledFunctionTable[slot] = entry.function;
        }
    }
    if (std::getenv("VCS_HEAPCHECK")) {
        const uint32_t slot = (0x2BC730u - g_ps2RecompiledFunctionTableBase) / 4;
        g_smallAllocOriginal = g_ps2RecompiledFunctionTable[slot];
        if (g_smallAllocOriginal) g_ps2RecompiledFunctionTable[slot] = VcsSmallAllocCheck;
    }
    if (const char* probe = std::getenv("VCS_PROBE")) {
        const uint32_t slot = (std::strtoul(probe, nullptr, 16) - g_ps2RecompiledFunctionTableBase) / 4;
        if (slot < g_ps2RecompiledFunctionTableSlotCount && g_ps2RecompiledFunctionTable[slot]) {
            g_probeOriginal = g_ps2RecompiledFunctionTable[slot];
            g_ps2RecompiledFunctionTable[slot] = VcsProbe;
        }
    }
    return VcsSharedMain(argc, argv);
}
