// Keep the shared frontend; install this ELF's recovered entries after static initialization.
#define main SopSharedMain
#include PS2X_SHARED_MAIN_CPP // the PS2Recomp frontend (path set by project/CMakeLists.txt)
#undef main
#include "ps2_syscalls.h"
#include "ps2_stubs.h"
#include "ps2_runtime_macros.h"
#include "runtime/ee_scheduler.h"
#include <filesystem>
#include <map>
#include <mutex>

struct SopTableEntry { uint32_t address; PS2Runtime::RecompiledFunction function; };
#include "sdk-handlers.inc"
#include "gap-functions.inc"

static std::string SopGuestString(const uint8_t* rdram, uint32_t address, uint32_t limit = 256) {
    std::string text;
    address &= 0x1FFFFFFu;
    for (uint32_t i = 0; i < limit && address + i < PS2_RAM_SIZE && rdram[address + i]; ++i) text += static_cast<char>(rdram[address + i]);
    return text;
}

// sceDopen/sceDread/sceDclose (0x3C13C0/0x3C15F0/0x3C1488): the game scans the disc directories at boot.
// The guest versions call the fileio RPC through a client the native sceFsInit never binds; list the host folder.
// sce_dirent: sce_stat (mode, attr, size, ctime[8], atime[8], mtime[8], hisize, private[6]) = 0x40, name[256] at 0x40.
struct SopDirectory { std::vector<std::pair<std::string, PS2VfsStat>> entries; size_t next = 0; };
static std::mutex g_sopDirMutex;
static std::map<int32_t, SopDirectory> g_sopDirs;
static int32_t g_sopNextDir = 0x40;

static void SopDopen(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime) {
    const std::string path = SopGuestString(rdram, getRegU32(ctx, 4));
    const PS2Runtime::IoPaths& io = PS2Runtime::getIoPaths();
    const PS2VfsMounts mounts{io.hostRoot, io.cdRoot, io.mcRoot};
    std::filesystem::path host;
    std::error_code ec;
    int32_t result = -2; // ENOENT
    if (runtime->vfs().resolveHostPath(path, mounts, host) && std::filesystem::is_directory(host, ec)) {
        SopDirectory dir;
        PS2VfsStat dot;
        dot.directory = true;
        dir.entries.push_back({".", dot});
        dir.entries.push_back({"..", dot});
        for (const auto& item : std::filesystem::directory_iterator(host, ec)) {
            std::string name = item.path().filename().string();
            if (name.empty() || name[0] == '.') continue; // host-side metadata (.disc)
            PS2VfsStat st;
            st.directory = item.is_directory(ec);
            st.size = st.directory ? 0u : static_cast<uint64_t>(item.file_size(ec));
            // cdvdman lists ISO9660 names with their version: the pack scan (0x1CFA80) matches "_P.fs;1".
            if (!st.directory && path.rfind("cdrom", 0) == 0) name += ";1";
            dir.entries.push_back({name, st});
        }
        std::lock_guard lock(g_sopDirMutex);
        result = g_sopNextDir++;
        g_sopDirs[result] = std::move(dir);
    }
    std::cerr << "[sop] sceDopen(\"" << path << "\") -> " << result << std::endl;
    setReturnS32(ctx, result);
    ctx->pc = getRegU32(ctx, 31);
}

static void SopDread(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime) {
    (void)runtime;
    const int32_t fd = static_cast<int32_t>(getRegU32(ctx, 4));
    const uint32_t dirent = getRegU32(ctx, 5) & 0x1FFFFFFu;
    int32_t result = -9; // EBADF
    std::lock_guard lock(g_sopDirMutex);
    const auto it = g_sopDirs.find(fd);
    if (it != g_sopDirs.end() && dirent + 0x140 <= PS2_RAM_SIZE) {
        result = 0;
        if (it->second.next < it->second.entries.size()) {
            const auto& [name, st] = it->second.entries[it->second.next++];
            uint8_t* out = rdram + dirent;
            std::memset(out, 0, 0x140);
            const uint32_t mode = st.directory ? 0x1000u | 0x16Du : 0x2000u | 0x124u; // SCE_STM_FDIR / FREG, read+exec
            const uint32_t size = static_cast<uint32_t>(st.size), hisize = static_cast<uint32_t>(st.size >> 32);
            std::memcpy(out + 0x00, &mode, 4);
            std::memcpy(out + 0x08, &size, 4);
            std::memcpy(out + 0x24, &hisize, 4);
            std::memcpy(out + 0x40, name.data(), std::min<size_t>(name.size(), 255));
            result = static_cast<int32_t>(std::min<size_t>(name.size(), 255));
        }
    }
    setReturnS32(ctx, result);
    ctx->pc = getRegU32(ctx, 31);
}

static void SopDclose(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime) {
    (void)rdram; (void)runtime;
    std::lock_guard lock(g_sopDirMutex);
    setReturnS32(ctx, g_sopDirs.erase(static_cast<int32_t>(getRegU32(ctx, 4))) ? 0 : -9);
    ctx->pc = getRegU32(ctx, 31);
}
static const SopTableEntry kSopLocalHandlers[] = {
    {0x3c13c0u, SopDopen},
    {0x3c15f0u, SopDread},
    {0x3c1488u, SopDclose},
};

extern const uint32_t g_ps2RecompiledFunctionTableBase;
extern const uint32_t g_ps2RecompiledFunctionTableSlotCount;
extern PS2Runtime::RecompiledFunction g_ps2RecompiledFunctionTable[];
std::string ps2xDispatchTrail();

// SOP_PROBE=<hex>: log ra/args/trail of the first calls (then powers of two) to one guest address.
static PS2Runtime::RecompiledFunction g_probeOriginal = nullptr;
static void SopProbe(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime) {
    static int n = 0;
    static const char* raFilter = std::getenv("SOP_PROBE_RA");
    if (raFilter && getRegU32(ctx, 31) != std::strtoul(raFilter, nullptr, 16)) { g_probeOriginal(rdram, ctx, runtime); return; }
    ++n;
    if (n <= 6 || (n & (n - 1)) == 0 || (n <= 64 && std::getenv("SOP_PROBE_RET")))
        std::cerr << "[probe] pc=0x" << std::hex << ctx->pc << " ra=0x" << getRegU32(ctx, 31) << " a0=0x" << getRegU32(ctx, 4)
                  << " a1=0x" << getRegU32(ctx, 5) << " a2=0x" << getRegU32(ctx, 6) << " sp=0x" << getRegU32(ctx, 29) << std::dec
                  << " a0str=\"" << SopGuestString(rdram, getRegU32(ctx, 4), 96) << "\""
                  << " trail=" << ps2xDispatchTrail() << std::endl;
    g_probeOriginal(rdram, ctx, runtime);
    if (n <= 64 && std::getenv("SOP_PROBE_RET"))
        std::cerr << "[probe] #" << n << " ret v0=0x" << std::hex << getRegU32(ctx, 2) << " pc=0x" << ctx->pc << std::dec << std::endl;
}

// Stream mutex (sema id in 0x1CA3A80). The IOP's STREAM.IRX notifies the EE through the EE RPC server 0x332588
// (sid 0x12344321), whose handler runs the stream callback 0x329EC8, which takes the mutex. On the PS2 that server
// runs in its own thread; here it is dispatched inside whatever EE thread is running. When that is the thread that
// already holds the mutex (inside one of the lock holders below), WaitSema blocked it forever (New Game -> black
// screen, [idle] t1 why=2 obj=3 pc=0x329f1c). Such a callback is deferred until the holder returns.
static const uint32_t kSopStreamLockHolders[] = {0x328550u, 0x328978u, 0x328bc8u, 0x32ad68u, 0x32af00u};
static PS2Runtime::RecompiledFunction g_sopHolderOriginal[5] = {};
static PS2Runtime::RecompiledFunction g_sopStreamCbOriginal = nullptr;
static std::map<int, int> g_sopHolderDepth;   // EE thread id -> nesting inside lock holders
static std::map<int, int> g_sopDeferredCb;    // EE thread id -> deferred callbacks
static std::mutex g_sopStreamMutex;

static void SopRunDeferred(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime, int tid) {
    for (;;) {
        {
            std::lock_guard<std::mutex> lock(g_sopStreamMutex);
            auto it = g_sopDeferredCb.find(tid);
            if (it == g_sopDeferredCb.end() || it->second == 0) return;
            --it->second;
        }
        const uint32_t pc = ctx->pc, ra = getRegU32(ctx, 31), v0 = getRegU32(ctx, 2), v1 = getRegU32(ctx, 3);
        ctx->pc = 0x329ec8u;
        g_sopStreamCbOriginal(rdram, ctx, runtime);
        ctx->pc = pc;
        SET_GPR_U32(ctx, 31, ra); SET_GPR_U32(ctx, 2, v0); SET_GPR_U32(ctx, 3, v1);
    }
}

template <int I>
static void SopStreamHolder(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime) {
    const int tid = runtime->eeScheduler().currentThreadId();
    { std::lock_guard<std::mutex> lock(g_sopStreamMutex); ++g_sopHolderDepth[tid]; }
    g_sopHolderOriginal[I](rdram, ctx, runtime);
    bool outermost;
    { std::lock_guard<std::mutex> lock(g_sopStreamMutex); outermost = --g_sopHolderDepth[tid] == 0; }
    if (outermost) SopRunDeferred(rdram, ctx, runtime, tid);
}

static void SopStreamCallback(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime) {
    EeScheduler& ee = runtime->eeScheduler();
    const int tid = ee.currentThreadId();
    bool reentrant;
    { std::lock_guard<std::mutex> lock(g_sopStreamMutex); reentrant = g_sopHolderDepth[tid] > 0; }
    uint32_t semaId = 0;
    std::memcpy(&semaId, rdram + 0x1ca3a80u, 4);
    EeSemaphore* sema = reentrant ? ee.semaphore(static_cast<int>(semaId)) : nullptr;
    const bool lend = sema && sema->count == 0 && sema->waiters.empty();
    if (lend) {
        // Re-entrant: the holder is this thread, so the callback borrows the mutex instead of waiting on itself.
        static int logs = 0;
        if (logs++ < 8) std::cerr << "[sop] stream callback re-entered by its holder (thread " << tid << ")" << std::endl;
        sema->count = 1;
    }
    g_sopStreamCbOriginal(rdram, ctx, runtime);
    if (lend) {
        if (EeSemaphore* again = ee.semaphore(static_cast<int>(semaId)); again && again->count > 0) again->count = 0;
    }
}

static void SopInstallStreamGuards() {
    if (std::getenv("SOP_NO_STREAM_GUARD")) return;
    auto slotOf = [](uint32_t a) { return (a - g_ps2RecompiledFunctionTableBase) / 4; };
    PS2Runtime::RecompiledFunction wrappers[5] = {SopStreamHolder<0>, SopStreamHolder<1>, SopStreamHolder<2>,
                                                 SopStreamHolder<3>, SopStreamHolder<4>};
    for (int i = 0; i < 5; ++i) {
        auto& slot = g_ps2RecompiledFunctionTable[slotOf(kSopStreamLockHolders[i])];
        if (!slot) { std::cerr << "[sop] stream lock holder missing\n"; return; }
        g_sopHolderOriginal[i] = slot;
        slot = wrappers[i];
    }
    auto& cb = g_ps2RecompiledFunctionTable[slotOf(0x329ec8u)];
    g_sopStreamCbOriginal = cb;
    cb = SopStreamCallback;
}

int main(int argc, char** argv) {
    for (const auto& entry : kSopGapFunctions) {
        const uint32_t slot = (entry.address - g_ps2RecompiledFunctionTableBase) / 4;
        if (entry.address >= g_ps2RecompiledFunctionTableBase && slot < g_ps2RecompiledFunctionTableSlotCount) {
            // The AOT table also binds every Ghidra label address to the translated function that contains it.
            // A gap start is outside every Ghidra function, so such a binding belongs to a neighbour: replace it.
            g_ps2RecompiledFunctionTable[slot] = entry.function;
        }
    }
    // SDK library matches replace their translated guest code with the runtime's native IOP-side services
    // (SIF RPC, CD, pad, memory card, ...): there is no IOP processor answering these requests.
    if (!std::getenv("SOP_GUEST_SDK")) {
        for (const auto& entry : kSopSdkHandlers) {
            const uint32_t slot = (entry.address - g_ps2RecompiledFunctionTableBase) / 4;
            if (entry.address < g_ps2RecompiledFunctionTableBase || slot >= g_ps2RecompiledFunctionTableSlotCount) {
                std::cerr << "Sopranos SDK handler outside AOT table\n";
                return 2;
            }
            g_ps2RecompiledFunctionTable[slot] = entry.function;
        }
        for (const auto& entry : kSopLocalHandlers)
            g_ps2RecompiledFunctionTable[(entry.address - g_ps2RecompiledFunctionTableBase) / 4] = entry.function;
    }
    SopInstallStreamGuards();
    if (const char* probe = std::getenv("SOP_PROBE")) {
        const uint32_t slot = (std::strtoul(probe, nullptr, 16) - g_ps2RecompiledFunctionTableBase) / 4;
        if (slot < g_ps2RecompiledFunctionTableSlotCount && g_ps2RecompiledFunctionTable[slot]) {
            g_probeOriginal = g_ps2RecompiledFunctionTable[slot];
            g_ps2RecompiledFunctionTable[slot] = SopProbe;
        }
    }
    return SopSharedMain(argc, argv);
}
