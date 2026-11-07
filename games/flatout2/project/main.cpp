// Keep the shared frontend; install this ELF's recovered entries after static initialization.
#define main Fo2SharedMain
#include PS2X_SHARED_MAIN_CPP // the PS2Recomp frontend (path set by project/CMakeLists.txt)
#undef main
#include "ps2_syscalls.h"
#include "ps2_stubs.h"

#include <filesystem>
#include <map>
#include <mutex>

struct Fo2TableEntry { uint32_t address; PS2Runtime::RecompiledFunction function; };
#include "sdk-handlers.inc"
#include "gap-functions.inc"

static std::string Fo2GuestString(const uint8_t* rdram, uint32_t address, uint32_t limit = 256) {
    std::string text;
    address &= 0x1FFFFFFu;
    for (uint32_t i = 0; i < limit && address + i < PS2_RAM_SIZE && rdram[address + i]; ++i) text += static_cast<char>(rdram[address + i]);
    return text;
}

// The libsif loadfile client (sceSifSearchModuleByName/StopModule/UnloadModule) calls _lf_bind (0x3F9308), which
// spins until the IOP loadfile RPC server (0x80000006) answers; the runtime serves module loads natively instead.
// Search: report modules this run already asked for by name, else "not loaded" so the game calls sceSifLoadModule.
static void Fo2SearchModuleByName(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime) {
    (void)runtime;
    static std::mutex mutex;
    static std::map<std::string, int32_t> seen;
    const std::string name = Fo2GuestString(rdram, getRegU32(ctx, 4), 64);
    int32_t result = -1;
    {
        std::lock_guard lock(mutex);
        const auto it = seen.find(name);
        if (it != seen.end()) result = it->second;
        else seen.emplace(name, static_cast<int32_t>(seen.size()) + 1);
    }
    std::cerr << "[fo2] sceSifSearchModuleByName(\"" << name << "\") -> " << result << std::endl;
    setReturnS32(ctx, result);
    ctx->pc = getRegU32(ctx, 31);
}

// sceSifStopModule(id, args, argp, &result) / sceSifUnloadModule(id): nothing runs on a real IOP to stop.
static void Fo2StopModule(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime) {
    (void)runtime;
    const uint32_t resultAddr = getRegU32(ctx, 7) & 0x1FFFFFFu;
    if (resultAddr && resultAddr + 4 <= PS2_RAM_SIZE) std::memset(rdram + resultAddr, 0, 4);
    std::cerr << "[fo2] sceSifStopModule(" << static_cast<int32_t>(getRegU32(ctx, 4)) << ")" << std::endl;
    setReturnS32(ctx, static_cast<int32_t>(getRegU32(ctx, 4)));
    ctx->pc = getRegU32(ctx, 31);
}
static void Fo2UnloadModule(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime) {
    (void)rdram; (void)runtime;
    std::cerr << "[fo2] sceSifUnloadModule(" << static_cast<int32_t>(getRegU32(ctx, 4)) << ")" << std::endl;
    setReturnS32(ctx, static_cast<int32_t>(getRegU32(ctx, 4)));
    ctx->pc = getRegU32(ctx, 31);
}

// sceGetstat(name, sce_stat*) (0x3F8C60): the guest version calls the fileio RPC through a client the native
// sceFsInit never binds. sce_stat: mode, attr, size, ctime[8], atime[8], mtime[8], hisize, private[6] (0x40 bytes).
static void Fo2Getstat(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime) {
    const std::string path = Fo2GuestString(rdram, getRegU32(ctx, 4));
    const uint32_t out = getRegU32(ctx, 5) & 0x1FFFFFFu;
    const PS2Runtime::IoPaths& io = PS2Runtime::getIoPaths();
    const PS2VfsMounts mounts{io.hostRoot, io.cdRoot, io.mcRoot};
    std::filesystem::path host;
    std::error_code ec;
    int32_t result = -2; // ENOENT
    if (runtime->vfs().resolveHostPath(path, mounts, host) && std::filesystem::exists(host, ec) && out + 0x40 <= PS2_RAM_SIZE) {
        const bool dir = std::filesystem::is_directory(host, ec);
        const uint64_t size = dir ? 0u : static_cast<uint64_t>(std::filesystem::file_size(host, ec));
        uint8_t* st = rdram + out;
        std::memset(st, 0, 0x40);
        const uint32_t mode = dir ? 0x1000u | 0x16Du : 0x2000u | 0x124u; // SCE_STM_FDIR / FREG, read+exec
        const uint32_t lo = static_cast<uint32_t>(size), hi = static_cast<uint32_t>(size >> 32);
        std::memcpy(st + 0x00, &mode, 4);
        std::memcpy(st + 0x08, &lo, 4);
        std::memcpy(st + 0x24, &hi, 4);
        result = 0;
    }
    std::cerr << "[fo2] sceGetstat(\"" << path << "\") -> " << result << std::endl;
    setReturnS32(ctx, result);
    ctx->pc = getRegU32(ctx, 31);
}

// sceDevctl(name, cmd, arg, arglen, buf, buflen) (0x3F8E40, fileio fno 0x17): same unbound client as sceGetstat.
// Before the intro movie the game sends "dev9x:" DDIOC_OFF (0x4402); there is no network adapter to power down.
static void Fo2Devctl(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime) {
    (void)runtime;
    const std::string name = Fo2GuestString(rdram, getRegU32(ctx, 4), 64);
    const uint32_t cmd = getRegU32(ctx, 5);
    const uint32_t buf = getRegU32(ctx, 8) & 0x1FFFFFFu, buflen = getRegU32(ctx, 9);
    if (buf && buflen && buflen <= 0x400 && buf + buflen <= PS2_RAM_SIZE) std::memset(rdram + buf, 0, buflen);
    const int32_t result = name.rfind("dev9", 0) == 0 ? 0 : -19; // ENODEV for devices nobody serves
    std::cerr << "[fo2] sceDevctl(\"" << name << "\", 0x" << std::hex << cmd << std::dec << ") -> " << result << std::endl;
    setReturnS32(ctx, result);
    ctx->pc = getRegU32(ctx, 31);
}

// scePowerOffHandler(func, arg) (0x3F7D00): the guest version waits on the fileio client (_sceFsWaitS) and hooks a
// SIF power-off command; a PC has no power button event, so record nothing and return "no previous handler".
static void Fo2PowerOffHandler(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime) {
    (void)rdram; (void)runtime;
    std::cerr << "[fo2] scePowerOffHandler(0x" << std::hex << getRegU32(ctx, 4) << ", 0x" << getRegU32(ctx, 5) << std::dec << ")" << std::endl;
    setReturnS32(ctx, 0);
    ctx->pc = getRegU32(ctx, 31);
}

// libpad2: the IOP side (DBCMAN/DS2U) is a stub in ps2xIOP, so the EE library is replaced by the runtime's native
// scePad2* (Kernel/Stubs/Pad.cpp). Without it the controller never links and the autosave notice waits for X forever.
namespace ps2_stubs {
void scePad2Init(uint8_t*, R5900Context*, PS2Runtime*);
void scePad2End(uint8_t*, R5900Context*, PS2Runtime*);
void scePad2CreateSocket(uint8_t*, R5900Context*, PS2Runtime*);
void scePad2DeleteSocket(uint8_t*, R5900Context*, PS2Runtime*);
void scePad2GetState(uint8_t*, R5900Context*, PS2Runtime*);
void scePad2Read(uint8_t*, R5900Context*, PS2Runtime*);
void scePad2GetButtonInfo(uint8_t*, R5900Context*, PS2Runtime*);
void scePad2NoInfo(uint8_t*, R5900Context*, PS2Runtime*);
}
#define FO2_PAD2(name)                                                                  \
    static void Fo2_##name(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime) {    \
        ctx->pc = getRegU32(ctx, 31);                                                   \
        ps2_stubs::name(rdram, ctx, runtime);                                           \
    }
FO2_PAD2(scePad2Init)
FO2_PAD2(scePad2End)
FO2_PAD2(scePad2CreateSocket)
FO2_PAD2(scePad2DeleteSocket)
FO2_PAD2(scePad2GetState)
FO2_PAD2(scePad2Read)
FO2_PAD2(scePad2GetButtonInfo)
FO2_PAD2(scePad2NoInfo)
#undef FO2_PAD2

static const Fo2TableEntry kFo2LocalHandlers[] = {
    {0x2bffa0u, Fo2_scePad2Init},
    {0x2bffe0u, Fo2_scePad2End},
    {0x2c0048u, Fo2_scePad2CreateSocket},
    {0x2c0150u, Fo2_scePad2DeleteSocket},
    {0x2c01a8u, Fo2_scePad2Read},
    {0x2c0288u, Fo2_scePad2GetState},
    {0x2c03c8u, Fo2_scePad2GetButtonInfo},
    {0x3e5c90u, Fo2_scePad2NoInfo}, // DBCMAN query (cmd 3) after the link: actuator/profile info

    {0x3f9970u, Fo2SearchModuleByName},
    {0x3f96d8u, Fo2StopModule},
    {0x3f98e0u, Fo2UnloadModule},
    {0x3f8c60u, Fo2Getstat},
    {0x3f8e40u, Fo2Devctl},
    {0x3f7d00u, Fo2PowerOffHandler},
};

extern const uint32_t g_ps2RecompiledFunctionTableBase;
extern const uint32_t g_ps2RecompiledFunctionTableSlotCount;
extern PS2Runtime::RecompiledFunction g_ps2RecompiledFunctionTable[];
std::string ps2xDispatchTrail();

// FO2_PROBE=<hex>: log ra/args/trail of the first calls (then powers of two) to one guest address.
static PS2Runtime::RecompiledFunction g_probeOriginal = nullptr;
static void Fo2Probe(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime) {
    static int n = 0;
    static const char* raFilter = std::getenv("FO2_PROBE_RA");
    if (raFilter && getRegU32(ctx, 31) != std::strtoul(raFilter, nullptr, 16)) { g_probeOriginal(rdram, ctx, runtime); return; }
    ++n;
    if (n <= 6 || (n & (n - 1)) == 0 || (n <= 64 && std::getenv("FO2_PROBE_RET")))
        std::cerr << "[probe] pc=0x" << std::hex << ctx->pc << " ra=0x" << getRegU32(ctx, 31) << " a0=0x" << getRegU32(ctx, 4)
                  << " a1=0x" << getRegU32(ctx, 5) << " a2=0x" << getRegU32(ctx, 6) << " sp=0x" << getRegU32(ctx, 29) << std::dec
                  << " trail=" << ps2xDispatchTrail() << std::endl;
    g_probeOriginal(rdram, ctx, runtime);
    if (n <= 64 && std::getenv("FO2_PROBE_RET"))
        std::cerr << "[probe] #" << n << " ret v0=0x" << std::hex << getRegU32(ctx, 2) << " pc=0x" << ctx->pc << std::dec << std::endl;
}

int main(int argc, char** argv) {
    for (const auto& entry : kFo2GapFunctions) {
        const uint32_t slot = (entry.address - g_ps2RecompiledFunctionTableBase) / 4;
        if (entry.address >= g_ps2RecompiledFunctionTableBase && slot < g_ps2RecompiledFunctionTableSlotCount) {
            // The AOT table also binds every Ghidra label address to the translated function that *contains* it.
            // A gap start is outside every Ghidra function, so a slot it occupies belongs to a neighbour: replace it.
            g_ps2RecompiledFunctionTable[slot] = entry.function;
        }
    }
    // SDK library matches replace their translated guest code with the runtime's native IOP-side services
    // (SIF RPC, CD, pad, memory card, ...). FO2_GUEST_SDK=1 keeps the guest copies.
    if (!std::getenv("FO2_GUEST_SDK")) {
        for (const auto& entry : kFo2SdkHandlers) {
            const uint32_t slot = (entry.address - g_ps2RecompiledFunctionTableBase) / 4;
            if (entry.address < g_ps2RecompiledFunctionTableBase || slot >= g_ps2RecompiledFunctionTableSlotCount) {
                std::cerr << "FO2 SDK handler outside AOT table\n";
                return 2;
            }
            g_ps2RecompiledFunctionTable[slot] = entry.function;
        }
        for (const auto& entry : kFo2LocalHandlers)
            g_ps2RecompiledFunctionTable[(entry.address - g_ps2RecompiledFunctionTableBase) / 4] = entry.function;
    }
    if (const char* probe = std::getenv("FO2_PROBE")) {
        const uint32_t slot = (std::strtoul(probe, nullptr, 16) - g_ps2RecompiledFunctionTableBase) / 4;
        if (slot < g_ps2RecompiledFunctionTableSlotCount && g_ps2RecompiledFunctionTable[slot]) {
            g_probeOriginal = g_ps2RecompiledFunctionTable[slot];
            g_ps2RecompiledFunctionTable[slot] = Fo2Probe;
        }
    }
    return Fo2SharedMain(argc, argv);
}
