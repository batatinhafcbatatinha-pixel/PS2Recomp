// Keep the shared frontend; install this ELF's recovered entries after static initialization.
#define main LcsSharedMain
#include PS2X_SHARED_MAIN_CPP // the PS2Recomp frontend (path set by project/CMakeLists.txt)
#undef main
#include "ps2_syscalls.h"
#include "ps2_stubs.h"
#include "runtime/ee_scheduler.h"
#include <atomic>
#include <thread>
#include <chrono>
#include <filesystem>
#include <map>
#include <mutex>

struct LcsTableEntry { uint32_t address; PS2Runtime::RecompiledFunction function; };
#include "sdk-handlers.inc"
#include "gap-functions.inc"

static std::string LcsGuestString(const uint8_t* rdram, uint32_t address, uint32_t limit = 256) {
    std::string text;
    address &= 0x1FFFFFFu;
    for (uint32_t i = 0; i < limit && address + i < PS2_RAM_SIZE && rdram[address + i]; ++i) text += static_cast<char>(rdram[address + i]);
    return text;
}

// sceSifSearchModuleByName (0x382E70) binds the IOP loadfile RPC (0x80000006) itself and spins until the server
// answers. The runtime serves module loads natively: report the modules this run already asked for by name
// (the boot code checks again before each load), and "not loaded" otherwise so the game calls sceSifLoadModule.
static void LcsSearchModuleByName(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime) {
    (void)runtime;
    static std::mutex mutex;
    static std::map<std::string, int32_t> seen;
    const std::string name = LcsGuestString(rdram, getRegU32(ctx, 4), 64);
    int32_t result = -1;
    {
        std::lock_guard lock(mutex);
        const auto it = seen.find(name);
        if (it != seen.end()) result = it->second;
        else seen.emplace(name, static_cast<int32_t>(seen.size()) + 1);
    }
    std::cerr << "[lcs] sceSifSearchModuleByName(\"" << name << "\") -> " << result << std::endl;
    setReturnS32(ctx, result);
    ctx->pc = getRegU32(ctx, 31);
}

// sceDopen/sceDread/sceDclose (0x382750/0x382980/0x382818): the game scans the disc directories at boot.
// The guest versions call the fileio RPC through a client the native sceFsInit never binds; list the host folder.
// sce_dirent: sce_stat (mode, attr, size, ctime[8], atime[8], mtime[8], hisize, private[6]) = 0x40, name[256] at 0x40.
struct LcsDirectory { std::vector<std::pair<std::string, PS2VfsStat>> entries; size_t next = 0; };
static std::mutex g_lcsDirMutex;
static std::map<int32_t, LcsDirectory> g_lcsDirs;
static int32_t g_lcsNextDir = 0x40;

static void LcsDopen(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime) {
    const std::string path = LcsGuestString(rdram, getRegU32(ctx, 4));
    const PS2Runtime::IoPaths& io = PS2Runtime::getIoPaths();
    const PS2VfsMounts mounts{io.hostRoot, io.cdRoot, io.mcRoot};
    std::filesystem::path host;
    std::error_code ec;
    int32_t result = -2; // ENOENT
    if (runtime->vfs().resolveHostPath(path, mounts, host) && std::filesystem::is_directory(host, ec)) {
        LcsDirectory dir;
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
            // cdvdman lists ISO9660 file names with their version. The game's stream open (0x2E6208) normalizes the
            // requested path with 0x2E6048, which appends ";1", and compares it with the names this scan registered:
            // without the suffix no stream file (CHK\PS2\GAME.DTZ, MODELS\*.IMG...) ever opens and CDSTREAM gets
            // 0-sector reads (status 0xFE) after the intro movies. sceCdSearchFile strips the suffix again.
            if (!st.directory && path.size() >= 5 && (path.compare(0, 5, "cdrom") == 0 || path.compare(0, 5, "CDROM") == 0))
                name += ";1";
            dir.entries.push_back({name, st});
        }
        std::lock_guard lock(g_lcsDirMutex);
        result = g_lcsNextDir++;
        g_lcsDirs[result] = std::move(dir);
    }
    std::cerr << "[lcs] sceDopen(\"" << path << "\") -> " << result << std::endl;
    setReturnS32(ctx, result);
    ctx->pc = getRegU32(ctx, 31);
}

static void LcsDread(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime) {
    (void)runtime;
    const int32_t fd = static_cast<int32_t>(getRegU32(ctx, 4));
    const uint32_t dirent = getRegU32(ctx, 5) & 0x1FFFFFFu;
    int32_t result = -9; // EBADF
    std::lock_guard lock(g_lcsDirMutex);
    const auto it = g_lcsDirs.find(fd);
    if (it != g_lcsDirs.end() && dirent + 0x140 <= PS2_RAM_SIZE) {
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

static void LcsDclose(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime) {
    (void)rdram; (void)runtime;
    std::lock_guard lock(g_lcsDirMutex);
    setReturnS32(ctx, g_lcsDirs.erase(static_cast<int32_t>(getRegU32(ctx, 4))) ? 0 : -9);
    ctx->pc = getRegU32(ctx, 31);
}
namespace ps2_stubs { void notifyMpegCdStreamEof(PS2Runtime* runtime); }

// 0x2B6B20 ends a movie: once the PSS is demuxed (the loop at 0x35A0C8 stops with < 5 bytes left, so the trailing
// 0x000001B9 is never demuxed) it writes a sequence_end_code (0x000001B7) into the game's video ES ring and sets the
// player state to 2. The native MPEG decoder never reads that ring, so tell it here that the stream is over; without
// this sceMpegIsEnd never turns true and the game waits on the Rockstar logo forever.
static PS2Runtime::RecompiledFunction g_movieEndOriginal = nullptr;
static void LcsMovieDataEnd(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime) {
    g_movieEndOriginal(rdram, ctx, runtime);
    if (getRegU32(ctx, 2) != 0) ps2_stubs::notifyMpegCdStreamEof(runtime);
}

// sceSifFreeIopHeap (0x382C00, RPC 2 to the IOP heap server 0x80000003) was not matched by the analyzer, so the guest
// copy sent an RPC nobody serves and IOP memory was never returned: every movie (logos, legal screen) allocates
// 0x6000 + 0x800 + 0x28010 bytes, and after a few the IOP heap ran out (CDSTREAM then read to address 0 forever).
static void LcsSifFreeIopHeap(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime) {
    ctx->pc = getRegU32(ctx, 31);
    ps2_stubs::sceSifFreeIopHeap(rdram, ctx, runtime);
}

static const LcsTableEntry kLcsLocalHandlers[] = {
    {0x382c00u, LcsSifFreeIopHeap},
    {0x382e70u, LcsSearchModuleByName},
    {0x382750u, LcsDopen},
    {0x382980u, LcsDread},
    {0x382818u, LcsDclose},
};

extern const uint32_t g_ps2RecompiledFunctionTableBase;
extern const uint32_t g_ps2RecompiledFunctionTableSlotCount;
extern PS2Runtime::RecompiledFunction g_ps2RecompiledFunctionTable[];
std::string ps2xDispatchTrail();

// LCS_PROBE=<hex>: log ra/args/trail of the first calls (then powers of two) to one guest address.
static PS2Runtime::RecompiledFunction g_probeOriginal = nullptr;
static void LcsProbe(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime) {
    static int n = 0;
    static const char* raFilter = std::getenv("LCS_PROBE_RA");
    if (raFilter && getRegU32(ctx, 31) != std::strtoul(raFilter, nullptr, 16)) { g_probeOriginal(rdram, ctx, runtime); return; }
    ++n;
    if (n <= 6 || (n & (n - 1)) == 0 || (n <= 64 && std::getenv("LCS_PROBE_RET")))
        std::cerr << "[probe] pc=0x" << std::hex << ctx->pc << " ra=0x" << getRegU32(ctx, 31) << " a0=0x" << getRegU32(ctx, 4)
                  << " a1=0x" << getRegU32(ctx, 5) << " a2=0x" << getRegU32(ctx, 6) << " sp=0x" << getRegU32(ctx, 29)
                  << " csr=0x" << runtime->memory().gs().csr.load() << std::dec << " vsync=" << runtime->eeScheduler().currentVSyncTick()
                  << " trail=" << ps2xDispatchTrail() << std::endl;
    // LCS_RAMDUMP=<file> [LCS_RAMDUMP_N=<call>]: save the 32 MB of guest RAM at that probed call (offline inspection).
    if (const char* dump = std::getenv("LCS_RAMDUMP"); dump && n == (std::getenv("LCS_RAMDUMP_N") ? std::atoi(std::getenv("LCS_RAMDUMP_N")) : 1))
        if (FILE* f = std::fopen(dump, "wb")) { std::fwrite(rdram, 1, PS2_RAM_SIZE, f); std::fclose(f); std::cerr << "[probe] ram saved" << std::endl; }
    // LCS_PROBE_STUCK=<file>: a host watchdog reports a probed call that has not returned after 5 s (ra, args)
    // and saves the guest RAM to <file> (finds endless guest loops such as a circular list).
    static std::atomic<uint64_t> s_enterMs{0};
    static std::atomic<uint32_t> s_ra{0}, s_a0{0}, s_a1{0}, s_a2{0};
    static std::atomic<bool> s_inside{false};
    static std::once_flag s_watch;
    if (const char* stuck = std::getenv("LCS_PROBE_STUCK")) {
        std::call_once(s_watch, [rdram, stuck] {
            std::thread([rdram, stuck] {
                bool reported = false;
                for (;;) {
                    std::this_thread::sleep_for(std::chrono::seconds(1));
                    const uint64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
                    if (!reported && s_inside.load() && now - s_enterMs.load() > 5000) {
                        reported = true;
                        std::cerr << "[probe-stuck] ra=0x" << std::hex << s_ra.load() << " a0=0x" << s_a0.load() << " a1=0x" << s_a1.load()
                                  << " a2=0x" << s_a2.load() << std::dec << std::endl;
                        if (FILE* f = std::fopen(stuck, "wb")) { std::fwrite(rdram, 1, PS2_RAM_SIZE, f); std::fclose(f); }
                    }
                }
            }).detach();
        });
        s_ra = getRegU32(ctx, 31); s_a0 = getRegU32(ctx, 4); s_a1 = getRegU32(ctx, 5); s_a2 = getRegU32(ctx, 6);
        s_enterMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        s_inside = true;
    }
    g_probeOriginal(rdram, ctx, runtime);
    s_inside = false;
    if (n <= 64 && std::getenv("LCS_PROBE_RET"))
        std::cerr << "[probe] #" << n << " ret v0=0x" << std::hex << getRegU32(ctx, 2) << " pc=0x" << ctx->pc << std::dec << std::endl;
}

int main(int argc, char** argv) {
    // Defaults for this game (an explicit environment value still wins), as for GTA VCS (same engine): the runtime's
    // arena and callback stacks below the ELF (the game's heap runs up to its main stack), and the native GS renderer.
#ifdef _WIN32
    if (!std::getenv("PS2X_RUNTIME_ARENA")) _putenv_s("PS2X_RUNTIME_ARENA", "80000:100000");
    if (!std::getenv("PS2X_GS")) _putenv_s("PS2X_GS", "hw");
#else
    setenv("PS2X_RUNTIME_ARENA", "80000:100000", 0);
    setenv("PS2X_GS", "hw", 0);
#endif
    for (const auto& entry : kLcsGapFunctions) {
        const uint32_t slot = (entry.address - g_ps2RecompiledFunctionTableBase) / 4;
        if (entry.address >= g_ps2RecompiledFunctionTableBase && slot < g_ps2RecompiledFunctionTableSlotCount) {
            // The AOT table also binds every Ghidra label address to the translated function that contains it.
            // A gap start is outside every Ghidra function, so such a binding belongs to a neighbour: replace it.
            g_ps2RecompiledFunctionTable[slot] = entry.function;
        }
    }
    // SDK library matches replace their translated guest code with the runtime's native IOP-side services
    // (SIF RPC, CD, pad, memory card, ...): there is no IOP processor answering these requests.
    if (!std::getenv("LCS_GUEST_SDK")) {
        for (const auto& entry : kLcsSdkHandlers) {
            const uint32_t slot = (entry.address - g_ps2RecompiledFunctionTableBase) / 4;
            if (entry.address < g_ps2RecompiledFunctionTableBase || slot >= g_ps2RecompiledFunctionTableSlotCount) {
                std::cerr << "LCS SDK handler outside AOT table\n";
                return 2;
            }
            g_ps2RecompiledFunctionTable[slot] = entry.function;
        }
        for (const auto& entry : kLcsLocalHandlers)
            g_ps2RecompiledFunctionTable[(entry.address - g_ps2RecompiledFunctionTableBase) / 4] = entry.function;
        auto& movieEnd = g_ps2RecompiledFunctionTable[(0x2b6b20u - g_ps2RecompiledFunctionTableBase) / 4];
        g_movieEndOriginal = movieEnd;
        movieEnd = LcsMovieDataEnd;
    }
    if (const char* probe = std::getenv("LCS_PROBE")) {
        const uint32_t slot = (std::strtoul(probe, nullptr, 16) - g_ps2RecompiledFunctionTableBase) / 4;
        if (slot < g_ps2RecompiledFunctionTableSlotCount && g_ps2RecompiledFunctionTable[slot]) {
            g_probeOriginal = g_ps2RecompiledFunctionTable[slot];
            g_ps2RecompiledFunctionTable[slot] = LcsProbe;
        }
    }
    return LcsSharedMain(argc, argv);
}
