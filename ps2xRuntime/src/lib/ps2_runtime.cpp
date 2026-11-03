#if defined(__linux__) && !defined(__ANDROID__)
#include <pthread.h>
#endif
#include "ps2_runtime.h"
#include "ps2_timeline.h"
#include "host_pc_menu.h"
#include "runtime/gs/gs_shared_present.h"
#include "runtime/gs/gs_gpu_backend.h"
#include "runtime/gs/gs_hw_backend.h"
#include "runtime/gs/gs_threaded_backend.h"
#include "runtime/ps2_pad.h"
#ifndef PS2X_REQUIRE_GPU_RASTERIZER
#define PS2X_REQUIRE_GPU_RASTERIZER 0
#endif
void ps2UpdateHostPadInput();
void ps2ProfilerRegisterCurrentThread(const char *name);
void ps2WatchInstall(uint8_t *rdram, uint32_t (*currentPc)(), void (*dump)());
#if PS2X_ENABLE_CODWAW_SUPPLEMENTS
#include "supplemental/sub_001C93F0_0x1c93f0.cpp"
#include "supplemental-1c90b0/sub_001C90B0_0x1c90b0.cpp"
#include "supplemental-4496b8/sub_004496B8_0x4496b8.cpp"
int32_t codWawMpegDelta();
uint32_t codWawMpegAddress(uint32_t address);
#include "supplemental-mpeg-ctor/integrated.inc"
#include "supplemental-139fb0/integrated.inc"
#include "supplemental-159fc8/integrated.inc"
#include "supplemental-gaps/bindings.inc"
void registerCodWawMpegCtor(PS2Runtime &runtime) { registerMpegCtorSupplement(runtime); }
bool tryBindCodWawMpeg(PS2Runtime &runtime, uint8_t *rdram, uint32_t target);
#endif
#include "ps2_log.h"
#include "ps2_stubs.h"
#include "ps2_syscalls.h"
#include "game_overrides.h"
#include "ps2_runtime_macros.h"
#include "runtime/gs/gs_frontend.h"
#include "runtime/ee_scheduler.h"
#include "ThreadNaming.h"
#include "Kernel/Stubs/Audio.h"
#include "Kernel/Stubs/GS.h"
#include "Kernel/Stubs/MPEG.h"
#include "ps2_host_backend.h"
#include "ps2_iop_host.h"
#include "ps2x/iop/iop_subsystem.h"

#include <iostream>
#include <immintrin.h>
#include <intrin.h>
#include <stdexcept>
#include <fstream>
#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <chrono>
#include <cmath>
#include <atomic>
#include <bit>
#include <cstdio>
#include <memory>
#include <thread>
#include <unordered_map>
#include <sstream>
#if defined(_MSC_VER)
#include <intrin.h>
#define PS2X_TICKS() __rdtsc()
#elif defined(__x86_64__) || defined(__i386__)
#include <x86intrin.h>
#define PS2X_TICKS() __rdtsc()
#else
#include <chrono>
// no time-stamp counter: nanoseconds scaled to the 3.6 GHz ticks the [perf] figures assume
#define PS2X_TICKS() (static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count()) * 18u / 5u)
#endif

namespace ps2_stubs
{
    void resetSifState();
}

#define ELF_MAGIC 0x464C457F // "\x7FELF" in little endian
#define ET_EXEC 2            // Executable file
#define EM_MIPS 8            // MIPS architecture
#define PT_LOAD 1            // Loadable segment

// Host time spent per subsystem, reported with the [perf] line.
std::atomic<uint64_t> g_perfVu1Ns{0}, g_perfGsNs{0}, g_perfIopNs{0}, g_perfIdleNs{0}, g_perfSyncNs{0};

void ps2ProfilerRegisterCurrentThread(const char *name);
std::atomic<bool> g_timelineOn{false};
namespace
{
    struct TimelineRecord { int64_t ns; uint32_t event, a, b, pad; };
    constexpr uint32_t kTimelineCapacity = 1u << 23;
    std::atomic<uint32_t> g_timelineCount{0};
    TimelineRecord *g_timeline = nullptr;
}

void ps2TimelineRecord(uint32_t event, uint32_t a, uint32_t b)
{
    const uint32_t slot = g_timelineCount.fetch_add(1u, std::memory_order_relaxed);
    if (slot < kTimelineCapacity)
        g_timeline[slot] = TimelineRecord{std::chrono::steady_clock::now().time_since_epoch().count(), event, a, b, 0u};
}

void ps2TimelinePoll()
{
    static const char *path = std::getenv("PS2X_TIMELINE");
    if (!path || !*path)
        return;
    static const double openAt = std::getenv("PS2X_TL_AT") ? std::atof(std::getenv("PS2X_TL_AT")) : 120.0;
    static const double span = std::getenv("PS2X_TL_SECS") ? std::atof(std::getenv("PS2X_TL_SECS")) : 6.0;
    static const auto boot = std::chrono::steady_clock::now();
    static int stage = 0;
    const double now = std::chrono::duration<double>(std::chrono::steady_clock::now() - boot).count();
    if (stage == 0 && now >= openAt)
    {
        g_timeline = static_cast<TimelineRecord *>(std::malloc(sizeof(TimelineRecord) * kTimelineCapacity));
        stage = 1;
        g_timelineOn.store(true, std::memory_order_release);
    }
    else if (stage == 1 && now >= openAt + span)
    {
        stage = 2;
        g_timelineOn.store(false, std::memory_order_release);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        const uint32_t count = std::min(g_timelineCount.load(), kTimelineCapacity);
        if (FILE *file = std::fopen(path, "wb"))
        {
            std::fwrite(g_timeline, sizeof(TimelineRecord), count, file);
            std::fclose(file);
        }
        std::cerr << "[timeline] " << count << " records in " << path << std::endl;
    }
}
extern std::atomic<uint32_t> g_gsDebugVu1Pc, g_gsDebugVu1Top;
extern const uint8_t *g_gsDebugVu1Data, *g_gsDebugVu1Code;
static constexpr uint32_t kVu1ProgramCycleCap = 1u << 22;
// On-demand capture (with PS2X_VU_DUMP_TRIGGER set, create analysis/vu-dump.trigger): VU1 data/code memory and
// registers before and after the next 24 microprogram runs (MSCAL or MSCNT).
static int vuDumpBegin()
{
    static const bool allowed = std::getenv("PS2X_VU_DUMP_TRIGGER") != nullptr;
    if (!allowed)
        return -1;
    static uint32_t countdown = 0, left = 0, index = 0;
    if (left == 0u && countdown++ % 256u == 0u && std::filesystem::exists("analysis/vu-dump.trigger"))
    {
        std::filesystem::remove("analysis/vu-dump.trigger");
        left = 24u;
    }
    if (left == 0u)
        return -1;
    --left;
    return static_cast<int>(index++);
}
static void vuDumpWrite(PS2Memory &memory, VU1Interpreter &vu1, int index, const char *phase)
{
    const std::string path = "analysis/vu-dump-" + std::to_string(index) + "-" + phase + ".bin";
    if (FILE *file = std::fopen(path.c_str(), "wb"))
    {
        std::fwrite(memory.getVU1Data(), 1, PS2_VU1_DATA_SIZE, file);
        std::fwrite(memory.getVU1Code(), 1, PS2_VU1_CODE_SIZE, file);
        std::fwrite(&vu1.state(), 1, sizeof(VU1State), file);
        std::fclose(file);
    }
}
static constexpr int FB_WIDTH = 640;
static constexpr int FB_HEIGHT = 512;
static constexpr int DEFAULT_DISPLAY_HEIGHT = 448;
static constexpr uint32_t DEFAULT_FB_SIZE = FB_WIDTH * FB_HEIGHT * 4;
static constexpr uint32_t DEFAULT_FB_ADDR = (PS2_RAM_SIZE - DEFAULT_FB_SIZE - 0x10000u);
#if defined(PLATFORM_VITA)
static constexpr int HOST_WINDOW_WIDTH = 960;
static constexpr int HOST_WINDOW_HEIGHT = 544;
#else
static constexpr int HOST_WINDOW_WIDTH = FB_WIDTH;
static constexpr int HOST_WINDOW_HEIGHT = DEFAULT_DISPLAY_HEIGHT;
#endif
struct ElfHeader
{
    uint32_t magic;
    uint8_t elf_class;
    uint8_t endianness;
    uint8_t version;
    uint8_t os_abi;
    uint8_t abi_version;
    uint8_t padding[7];
    uint16_t type;
    uint16_t machine;
    uint32_t version2;
    uint32_t entry;
    uint32_t phoff;
    uint32_t shoff;
    uint32_t flags;
    uint16_t ehsize;
    uint16_t phentsize;
    uint16_t phnum;
    uint16_t shentsize;
    uint16_t shnum;
    uint16_t shstrndx;
};

struct ProgramHeader
{
    uint32_t type;
    uint32_t offset;
    uint32_t vaddr;
    uint32_t paddr;
    uint32_t filesz;
    uint32_t memsz;
    uint32_t flags;
    uint32_t align;
};

namespace
{
    std::mutex sparseFunctionMutex;
    std::unordered_map<const PS2Runtime *, std::unordered_map<uint32_t, PS2Runtime::RecompiledFunction>> sparseFunctions;

    constexpr uint32_t kGuestHeapDefaultBase = 0x00100000u;
    constexpr uint32_t kGuestHeapDefaultAlignment = 16u;
    constexpr uint32_t kGuestHeapSafetyPad = 0x1000u;
    constexpr uint32_t kGuestHeapHardLimit = 0x01F00000u;
    // Memory the runtime hands out itself (RPC/SIF packets, MPEG callback data, font and GS packets, thread stacks):
    // above the game's heap limit (EndOfHeap = kGuestHeapHardLimit), so the game's own sbrk/malloc, which grows from
    // the end of its .bss, never hands out the same bytes. The interrupt/callback stacks sit above it.
    constexpr uint32_t kRuntimeArenaBase = kGuestHeapHardLimit;
    constexpr uint32_t kRuntimeArenaEnd = 0x01FE0000u;

    constexpr uint32_t COP0_CAUSE_EXCCODE_MASK = 0x0000007Cu;
    constexpr uint32_t COP0_CAUSE_BD = 0x80000000u;
    constexpr uint32_t COP0_STATUS_EXL = 0x00000002u;
    constexpr uint32_t COP0_STATUS_BEV = 0x00400000u;
    constexpr uint32_t EXCEPTION_VECTOR_GENERAL = 0x80000080u;
    constexpr uint32_t EXCEPTION_VECTOR_TLB_REFILL = 0x80000000u;
    constexpr uint32_t EXCEPTION_VECTOR_BOOT = 0xBFC00200u;

    struct DispatchHistory
    {
        std::array<uint32_t, 64> pcs{};
        uint32_t next = 0u;
        bool wrapped = false;
    };

    thread_local DispatchHistory g_dispatchHistory;

    bool computeFileCrc32(const std::string &path, uint32_t &crcOut)
    {
        std::ifstream file(path, std::ios::binary);
        if (!file.is_open())
        {
            return false;
        }

        static const std::array<uint32_t, 256> table = []
        {
            std::array<uint32_t, 256> values{};
            for (uint32_t i = 0; i < values.size(); ++i)
            {
                uint32_t value = i;
                for (uint32_t bit = 0; bit < 8; ++bit)
                {
                    value = (value & 1u) ? (0xEDB88320u ^ (value >> 1u)) : (value >> 1u);
                }
                values[i] = value;
            }
            return values;
        }();

        uint32_t crc = 0xFFFFFFFFu;
        std::array<uint8_t, 16 * 1024> buffer{};
        while (file.good())
        {
            file.read(reinterpret_cast<char *>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
            const std::streamsize count = file.gcount();
            for (std::streamsize i = 0; i < count; ++i)
            {
                crc = table[(crc ^ buffer[static_cast<size_t>(i)]) & 0xFFu] ^ (crc >> 8u);
            }
        }
        if (file.bad())
        {
            return false;
        }
        crcOut = ~crc;
        return true;
    }

    void pushDispatchPc(uint32_t pc)
    {
        DispatchHistory &h = g_dispatchHistory;
        h.pcs[h.next] = pc;
        if (++h.next == static_cast<uint32_t>(h.pcs.size()))
        {
            h.next = 0u;
            h.wrapped = true;
        }
    }

    std::string formatDispatchHistory()
    {
        const DispatchHistory &h = g_dispatchHistory;
        const uint32_t count = h.wrapped ? static_cast<uint32_t>(h.pcs.size()) : h.next;
        if (count == 0u)
        {
            return "(empty)";
        }

        std::ostringstream oss;
        bool first = true;
        for (uint32_t i = 0u; i < count; ++i)
        {
            const uint32_t idx = (h.next + h.pcs.size() - count + i) % static_cast<uint32_t>(h.pcs.size());
            if (!first)
            {
                oss << " -> ";
            }
            first = false;
            oss << "0x" << std::hex << h.pcs[idx];
        }
        return oss.str();
    }

    uint32_t selectExceptionVector(const R5900Context *ctx, bool tlbRefill)
    {
        if (ctx->cop0_status & COP0_STATUS_BEV)
        {
            return EXCEPTION_VECTOR_BOOT;
        }
        return tlbRefill ? EXCEPTION_VECTOR_TLB_REFILL : EXCEPTION_VECTOR_GENERAL;
    }

    void seedVu0IdleSuccess(R5900Context *ctx)
    {
        if (!ctx)
        {
            return;
        }

        ctx->vu0_clip_flags = 0;
        ctx->vu0_clip_flags2 = 0;
        ctx->vu0_mac_flags = 0;
        ctx->vu0_status = 0;
        ctx->vu0_q = 1.0f;
        ctx->vu0_r = _mm_castsi128_ps(_mm_set1_epi32(0x3F800000));
        ctx->vu0_vpu_stat = 0;
        ctx->vu0_vpu_stat2 = 0;
    }

    void copyVu0ContextToState(const R5900Context *ctx, VU1State &state)
    {
        std::memset(&state, 0, sizeof(state));

        for (uint32_t i = 0; i < 32u; ++i)
        {
            _mm_storeu_ps(state.vf[i], ctx->vu0_vf[i]);
        }
        for (uint32_t i = 0; i < 16u; ++i)
        {
            state.vi[i] = static_cast<int16_t>(ctx->vi[i]);
        }

        _mm_storeu_ps(state.acc, ctx->vu0_acc);
        state.q = ctx->vu0_q;
        state.p = ctx->vu0_p;
        state.i = ctx->vu0_i;
        alignas(16) uint32_t rWords[4]{};
        _mm_storeu_si128(reinterpret_cast<__m128i *>(rWords), _mm_castps_si128(ctx->vu0_r));
        state.r = 0x3F800000u | (rWords[0] & 0x007FFFFFu);
        state.pc = ctx->vu0_pc;
        state.mac = ctx->vu0_mac_flags;
        state.clip = ctx->vu0_clip_flags;
        state.status = ctx->vu0_status;
        state.itop = ctx->vu0_itop;
        state.dBitEnabled = (ctx->vu0_fbrst & (1u << 2)) != 0u;
        state.tBitEnabled = (ctx->vu0_fbrst & (1u << 3)) != 0u;

        state.vf[0][0] = 0.0f;
        state.vf[0][1] = 0.0f;
        state.vf[0][2] = 0.0f;
        state.vf[0][3] = 1.0f;
        state.vi[0] = 0;
    }

    void copyVu0StateToContext(const VU1State &state, R5900Context *ctx)
    {
        for (uint32_t i = 0; i < 32u; ++i)
        {
            ctx->vu0_vf[i] = _mm_loadu_ps(state.vf[i]);
        }
        for (uint32_t i = 0; i < 16u; ++i)
        {
            ctx->vi[i] = static_cast<uint16_t>(state.vi[i]);
        }

        ctx->vu0_acc = _mm_loadu_ps(state.acc);
        ctx->vu0_q = state.q;
        ctx->vu0_p = state.p;
        ctx->vu0_i = state.i;
        ctx->vu0_r = _mm_castsi128_ps(_mm_set1_epi32(static_cast<int32_t>(state.r)));
        ctx->vu0_mac_flags = state.mac;
        ctx->vu0_clip_flags = state.clip;
        ctx->vu0_clip_flags2 = state.clip;
        ctx->vu0_status = static_cast<uint16_t>(state.status);
        ctx->vu0_itop = state.itop;
        ctx->vu0_pc = state.pc;
        ctx->vu0_tpc = state.pc;
        ctx->vu0_vpu_stat = (ctx->vu0_vpu_stat & 0xFF00u) | (state.stoppedByD ? (1u << 1) : 0u) | (state.stoppedByT ? (1u << 2) : 0u);
        ctx->vu0_vpu_stat2 = 0;

        ctx->vu0_vf[0] = _mm_set_ps(1.0f, 0.0f, 0.0f, 0.0f);
        ctx->vi[0] = 0;
    }

    void raiseCop0Exception(R5900Context *ctx, uint32_t exceptionCode, bool tlbRefill = false)
    {
        if (ctx->in_delay_slot)
        {
            ctx->cop0_epc = ctx->branch_pc;
            ctx->cop0_cause = (ctx->cop0_cause & ~COP0_CAUSE_EXCCODE_MASK) |
                              ((exceptionCode << 2) & COP0_CAUSE_EXCCODE_MASK) |
                              COP0_CAUSE_BD;
        }
        else
        {
            ctx->cop0_epc = ctx->pc;
            ctx->cop0_cause = (ctx->cop0_cause & ~(COP0_CAUSE_EXCCODE_MASK | COP0_CAUSE_BD)) |
                              ((exceptionCode << 2) & COP0_CAUSE_EXCCODE_MASK);
        }

        ctx->cop0_status |= COP0_STATUS_EXL;
        ctx->pc = selectExceptionVector(ctx, tlbRefill);
        ctx->in_delay_slot = false;
    }

    std::filesystem::path normalizeAbsolutePath(const std::filesystem::path &path)
    {
        if (path.empty())
        {
            return {};
        }

#if defined(PLATFORM_VITA)
        const std::string generic = path.generic_string();
        const std::size_t colon = generic.find(':');
        if (colon != std::string::npos && colon != 0u)
        {
            const std::size_t slash = generic.find_first_of("/\\");
            if (slash == std::string::npos || colon < slash)
            {
                return path.lexically_normal();
            }
        }
#endif

        std::error_code ec;
        const std::filesystem::path absolute = std::filesystem::absolute(path, ec);
        if (ec)
        {
            return path.lexically_normal();
        }
        return absolute.lexically_normal();
    }

    PS2Runtime::IoPaths &runtimeIoPaths()
    {
        static PS2Runtime::IoPaths paths = []()
        {
            PS2Runtime::IoPaths defaults;
            std::error_code ec;
            const std::filesystem::path cwd = std::filesystem::current_path(ec);
            defaults.elfDirectory = ec ? std::filesystem::path(".") : cwd.lexically_normal();
            defaults.hostRoot = defaults.elfDirectory;
            defaults.cdRoot = defaults.elfDirectory;
            defaults.mcRoot = defaults.elfDirectory / "mc0";
            return defaults;
        }();

        return paths;
    }

    std::string readGuestPrintableString(const uint8_t *rdram, uint32_t addr, size_t maxLen)
    {
        std::string out;
        if (!rdram || maxLen == 0)
        {
            return out;
        }

        out.reserve(std::min<size_t>(maxLen, 64));
        for (size_t i = 0; i < maxLen; ++i)
        {
            const char ch = static_cast<char>(rdram[(addr + static_cast<uint32_t>(i)) & PS2_RAM_MASK]);
            if (ch == '\0')
            {
                break;
            }
            if (ch >= 0x20 && ch < 0x7F)
            {
                out.push_back(ch);
            }
            else
            {
                out.push_back('.');
            }
        }
        return out;
    }
}

extern std::atomic<bool> g_gsForceReadback;

static void UploadFrame(Texture2D &tex, PS2Runtime *rt, uint32_t &outWidth, uint32_t &outHeight)
{
    static uint64_t s_lastPresentationTick = std::numeric_limits<uint64_t>::max();
    static bool s_hasLatchedInitialFrame = false;
    static uint32_t s_lastDisplayFbp = std::numeric_limits<uint32_t>::max();
    static uint32_t s_lastSourceFbp = std::numeric_limits<uint32_t>::max();
    static bool s_lastPreferred = false;
    static uint32_t s_lastWidth = 0u;
    static uint32_t s_lastHeight = 0u;
    static bool s_hasUploadedFrame = false;
    static std::vector<uint8_t> s_scratch;
    static std::vector<uint8_t> s_uploadBuffer(DEFAULT_FB_SIZE, 0u);

    const uint64_t currentTick = rt->eeScheduler().currentVSyncTick();
    const char *frameDump = std::getenv("PS2X_FRAME_DUMP");
    static uint64_t lastFrameDumpTick = std::numeric_limits<uint64_t>::max();
    // PS2X_FRAME_DUMP_TICKS: a dump reads the frame back from the GPU, which disturbs measurements.
    static const uint64_t dumpTicks = std::getenv("PS2X_FRAME_DUMP_TICKS") ? std::strtoull(std::getenv("PS2X_FRAME_DUMP_TICKS"), nullptr, 0) : 120u;
    const bool dumpDue = frameDump && *frameDump &&
                         (lastFrameDumpTick == std::numeric_limits<uint64_t>::max() || currentTick >= lastFrameDumpTick + dumpTicks);
    const bool sharedFrames = GSSharedPresent::Active();
    if (sharedFrames && !dumpDue)
    {
        if (currentTick != s_lastPresentationTick)
        {
            rt->gs().latchHostPresentationFrame();
            s_lastPresentationTick = currentTick;
            s_hasLatchedInitialFrame = true;
        }
        GSSharedPresent::Frame frame;
        if (GSSharedPresent::Acquire(frame) && !frame.blank && frame.texture != 0u)
        {
            tex.id = frame.texture;
            tex.width = static_cast<int>(frame.width);
            tex.height = static_cast<int>(frame.height);
            s_lastWidth = frame.width;
            s_lastHeight = frame.height;
            s_hasUploadedFrame = true;
        }
        outWidth = s_lastWidth != 0u ? s_lastWidth : FB_WIDTH;
        outHeight = s_lastHeight != 0u ? s_lastHeight : DEFAULT_DISPLAY_HEIGHT;
        return;
    }
    if (dumpDue && sharedFrames)
        g_gsForceReadback.store(true, std::memory_order_relaxed);
    const bool needsLatch = !s_hasLatchedInitialFrame || currentTick != s_lastPresentationTick || dumpDue;
    if (needsLatch)
    {
        rt->gs().latchHostPresentationFrame();
        s_lastPresentationTick = currentTick;
        s_hasLatchedInitialFrame = true;
    }
    else if (s_hasUploadedFrame)
    {
        outWidth = (s_lastWidth != 0u) ? s_lastWidth : FB_WIDTH;
        outHeight = (s_lastHeight != 0u) ? s_lastHeight : DEFAULT_DISPLAY_HEIGHT;
        return;
    }

    s_scratch.clear();
    uint32_t width = 0u;
    uint32_t height = 0u;
    uint32_t displayFbp = 0u;
    uint32_t sourceFbp = 0u;
    bool usedPreferredDisplaySource = false;
    if (!rt->gs().copyLatchedHostPresentationFrame(s_scratch,
                                                   width,
                                                   height,
                                                   &displayFbp,
                                                   &sourceFbp,
                                                   &usedPreferredDisplaySource))
    {
        Image blank = GenImageColor(tex.width, tex.height, MAGENTA);
        UpdateTexture(tex, blank.data);
        UnloadImage(blank);
        outWidth = FB_WIDTH;
        outHeight = DEFAULT_DISPLAY_HEIGHT;
        s_lastWidth = outWidth;
        s_lastHeight = outHeight;
        s_hasUploadedFrame = true;
        return;
    }

    PS2_IF_AGRESSIVE_LOGS({
        static uint32_t s_uploadDebugCount = 0u;
        if (s_uploadDebugCount < 128u ||
            displayFbp != s_lastDisplayFbp ||
            sourceFbp != s_lastSourceFbp ||
            usedPreferredDisplaySource != s_lastPreferred ||
            width != s_lastWidth ||
            height != s_lastHeight)
        {
            std::cout << "[frame:upload] idx=" << s_uploadDebugCount
                      << " tick=" << currentTick
                      << " displayFbp=" << displayFbp
                      << " sourceFbp=" << sourceFbp
                      << " size=" << width << "x" << height
                      << " preferred=" << static_cast<uint32_t>(usedPreferredDisplaySource ? 1u : 0u)
                      << std::endl;
        }
        ++s_uploadDebugCount;
    });
    s_lastDisplayFbp = displayFbp;
    s_lastSourceFbp = sourceFbp;
    s_lastPreferred = usedPreferredDisplaySource;
    s_lastWidth = width;
    s_lastHeight = height;

    // The render-target GS renderer (PS2X_GS=hw) presents at its internal scale (1024x896 at 2x): the texture grows
    // to the frame instead of cropping it to 640x512.
    const int texW = std::max<int>(FB_WIDTH, static_cast<int>(width)), texH = std::max<int>(FB_HEIGHT, static_cast<int>(height));
    if (tex.width != texW || tex.height != texH)
    {
        UnloadTexture(tex);
        Image blank = GenImageColor(texW, texH, BLANK);
        tex = LoadTextureFromImage(blank);
        UnloadImage(blank);
    }
    s_uploadBuffer.assign(static_cast<size_t>(texW) * static_cast<size_t>(texH) * 4u, 0u);
    if (!s_scratch.empty() && width != 0u && height != 0u)
    {
        const uint32_t copyWidth = std::min<uint32_t>(width, static_cast<uint32_t>(texW));
        const uint32_t copyHeight = std::min<uint32_t>(height, static_cast<uint32_t>(texH));
        const size_t srcRowBytes = static_cast<size_t>(width) * 4u;
        const size_t dstRowBytes = static_cast<size_t>(texW) * 4u;
        const size_t copyRowBytes = static_cast<size_t>(copyWidth) * 4u;
        for (uint32_t y = 0; y < copyHeight; ++y)
        {
            const size_t srcOffset = static_cast<size_t>(y) * srcRowBytes;
            const size_t dstOffset = static_cast<size_t>(y) * dstRowBytes;
            if (srcOffset + copyRowBytes > s_scratch.size() ||
                dstOffset + copyRowBytes > s_uploadBuffer.size())
            {
                break;
            }
            std::memcpy(s_uploadBuffer.data() + dstOffset, s_scratch.data() + srcOffset, copyRowBytes);
        }
    }

    if (frameDump && *frameDump && width && height && !s_scratch.empty() &&
        (lastFrameDumpTick == std::numeric_limits<uint64_t>::max() || currentTick >= lastFrameDumpTick + 120u)) {
        Image frame{s_scratch.data(), static_cast<int>(width), static_cast<int>(height), 1,
                    PIXELFORMAT_UNCOMPRESSED_R8G8B8A8};
        ExportImage(frame, frameDump);
        lastFrameDumpTick = currentTick;
    }
    if (sharedFrames)
    {
        outWidth = s_lastWidth != 0u ? s_lastWidth : width;
        outHeight = s_lastHeight != 0u ? s_lastHeight : height;
        return;
    }
    UpdateTexture(tex, s_uploadBuffer.data());
    outWidth = width;
    outHeight = height;
    s_hasUploadedFrame = true;
}

PS2Runtime::PS2Runtime()
{
    m_iopHost = std::make_unique<PS2IopHostAdapter>(*this);
    m_iopSubsystem = std::make_unique<ps2x::iop::IopSubsystem>(*m_iopHost);

    m_eeScheduler = std::make_unique<EeScheduler>(*this);

    // Assign rather than memset: R5900Context's constructor zeroes itself and
    // then applies the COP0 reset values, which a memset here would discard.
    m_cpuContext = R5900Context{};

    // R0 is always zero in MIPS
    m_cpuContext.r[0] = _mm_set1_epi32(0);
    m_cpuContext.vu0_vf[0] = _mm_set_ps(1.0f, 0.0f, 0.0f, 0.0f);
    m_cpuContext.vu0_q = 1.0f;
    m_cpuContext.vu0_r = _mm_castsi128_ps(_mm_set1_epi32(0x3F800000));

    // Stack pointer (SP) and global pointer (GP) will be set by the loaded ELF

    m_loadedModules.clear();
    m_guestHeapBlocks.clear();
    m_guestHeapBase = kGuestHeapDefaultBase;
    m_guestHeapEnd = kGuestHeapDefaultBase;
    m_guestHeapLimit = std::min(kGuestHeapHardLimit, PS2_RAM_SIZE);
    m_guestHeapSuggestedBase = kGuestHeapDefaultBase;
    m_guestHeapConfigured = false;
    m_asyncCallbackStackFloor = std::min(kRuntimeArenaEnd, PS2_RAM_SIZE);
    m_asyncCallbackStackTop = PS2_RAM_SIZE;
    m_runtimeArenaEnd = kRuntimeArenaEnd;
}

void PS2Runtime::setDebugUiCallbacks(DebugUiCallback initCallback,
                                     DebugUiCallback drawCallback,
                                     DebugUiCallback shutdownCallback,
                                     void *userData)
{
    if (m_debugUiInitialized && m_debugUiShutdownCallback)
    {
        m_debugUiShutdownCallback(*this, m_debugUiUserData);
        m_debugUiInitialized = false;
    }

    m_debugUiInitCallback = initCallback;
    m_debugUiDrawCallback = drawCallback;
    m_debugUiShutdownCallback = shutdownCallback;
    m_debugUiUserData = userData;
}

PS2Runtime::~PS2Runtime()
{
    try
    {
        requestStop();
        m_iopSubsystem.reset();
        m_iopHost.reset();
#if defined(PLATFORM_VITA)
        m_audioBackend.stopAll();
        m_audioBackend.setAudioReady(false);
#else
        if (IsAudioDeviceReady())
        {
            CloseAudioDevice();
            m_audioBackend.setAudioReady(false);
        }
#endif
        if (m_debugUiInitialized && m_debugUiShutdownCallback)
        {
            m_debugUiShutdownCallback(*this, m_debugUiUserData);
            m_debugUiInitialized = false;
        }

        if (IsWindowReady())
        {
            CloseWindow();
        }

        m_loadedModules.clear();
    }
    catch (const std::exception &e)
    {
        std::cerr << "[~PS2Runtime] cleanup exception: " << e.what() << std::endl;
    }
    catch (...)
    {
        std::cerr << "[~PS2Runtime] cleanup exception: unknown" << std::endl;
    }
    std::lock_guard lock(sparseFunctionMutex);
    sparseFunctions.erase(this);
}

ps2x::iop::ModuleLoadResult PS2Runtime::loadIopModule(std::string_view path, const void *arguments, uint32_t argumentSize)
{
    auto scope = m_iopHost->enterCall(nullptr, m_memory.getRDRAM());
    return m_iopSubsystem->loadModule(path, arguments, argumentSize);
}

ps2x::iop::ModuleLoadResult PS2Runtime::loadIopModuleBuffer(uint32_t guestAddress, const void *arguments, uint32_t argumentSize)
{
    auto scope = m_iopHost->enterCall(nullptr, m_memory.getRDRAM());
    return m_iopSubsystem->loadModuleBuffer(guestAddress, arguments, argumentSize);
}

bool PS2Runtime::stopIopModule(int32_t moduleId, int32_t *result)
{
    auto scope = m_iopHost->enterCall(nullptr, m_memory.getRDRAM());
    return m_iopSubsystem->stopModule(moduleId, result);
}

ps2x::iop::RpcAbi PS2Runtime::selectIopRpcAbi(const ps2x::iop::RpcAbiRequest &request) const
{
    return m_iopSubsystem->selectRpcAbi(request);
}

bool PS2Runtime::canBindIopRpc(uint32_t sid) const noexcept
{
    return m_iopSubsystem->canBindRpc(sid);
}

ps2x::iop::RpcResult PS2Runtime::handleIopRpc(uint8_t *rdram, R5900Context *ctx, ps2x::iop::RpcRequest request)
{
    auto scope = m_iopHost->enterCall(ctx, rdram);
    request.callToken = scope.token();
    return m_iopSubsystem->handleRpc(request);
}

void PS2Runtime::notifyIopSifTransfer(uint8_t *rdram, const ps2x::iop::SifTransfer &transfer)
{
    auto scope = m_iopHost->enterCall(nullptr, rdram);
    m_iopSubsystem->onSifTransfer(transfer);
}

void PS2Runtime::advanceIopEeCycles(uint64_t eeCycles) noexcept
{
    const auto iopStart = std::chrono::steady_clock::now();
    m_iopSubsystem->runEeCycles(eeCycles);
    g_perfIopNs += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - iopStart).count();
}

void PS2Runtime::resetIop()
{
    m_iopSubsystem->reset();
}

ps2x::iop::DebugSnapshot PS2Runtime::iopDebugSnapshot() const
{
    return m_iopSubsystem->debugSnapshot();
}

uint32_t PS2Runtime::allocateIopMemory(uint32_t size, uint32_t alignment)
{
    return m_iopSubsystem ? m_iopSubsystem->allocateMemory(size, alignment) : 0u;
}

bool PS2Runtime::freeIopMemory(uint32_t address)
{
    return m_iopSubsystem && m_iopSubsystem->freeMemory(address);
}

bool PS2Runtime::readIopMemory(uint32_t address, void *destination, size_t size) const
{
    return m_iopSubsystem && m_iopSubsystem->readMemory(address, destination, size);
}

bool PS2Runtime::writeIopMemory(uint32_t address, const void *source, size_t size)
{
    return m_iopSubsystem && m_iopSubsystem->writeMemory(address, source, size);
}

bool PS2Runtime::zeroIopMemory(uint32_t address, size_t size)
{
    return m_iopSubsystem && m_iopSubsystem->zeroMemory(address, size);
}

bool PS2Runtime::isIopMemoryRange(uint32_t address, size_t size) const
{
    return m_iopSubsystem && m_iopSubsystem->isMemoryRange(address, size);
}

bool PS2Runtime::syncCoreSubsystems()
{
    uint8_t *const rdram = m_memory.getRDRAM();
    uint8_t *const gsVram = m_memory.getGSVRAM();
    if (!rdram || !gsVram)
    {
        return false;
    }

    if (m_boundRdram == rdram && m_boundGSVram == gsVram)
    {
        return true;
    }

    m_gs.init(gsVram, static_cast<uint32_t>(PS2_GS_VRAM_SIZE), &m_memory.gs());
    m_gifArbiter.setProcessPacketFn([this](const uint8_t *data, uint32_t size)
                                    {
                                        if (m_memory.queueGsPacket(data, size))
                                            return;
                                        const auto gsStart = std::chrono::steady_clock::now();
                                        m_gs.processGIFPacket(data, size);
                                        g_perfGsNs += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - gsStart).count(); });
    m_memory.setGifArbiter(&m_gifArbiter);
    m_memory.setVu1MscalCallback([this](uint32_t startPC, uint32_t top, uint32_t itop)
                                 {
                                     // On the VIF worker the EE context is not ours to touch: FBRST comes with the job.
                                     const bool onWorker = PS2Memory::onVifWorker();
                                     R5900Context *cpuContext = onWorker ? nullptr : m_eeScheduler ? m_eeScheduler->currentContext() : nullptr;
                                     if (!cpuContext && !onWorker)
                                     {
                                         cpuContext = &m_cpuContext;
                                     }
                                     const uint32_t fbrst = onWorker ? PS2Memory::vifJobFbrst() : cpuContext->vu0_fbrst;
                                     m_vu1.state().dBitEnabled = (fbrst & (1u << 10)) != 0u;
                                     m_vu1.state().tBitEnabled = (fbrst & (1u << 11)) != 0u;
                                     const int dumpIndex = vuDumpBegin();
                                     if (dumpIndex >= 0)
                                     {
                                         vuDumpWrite(m_memory, m_vu1, dumpIndex, "before");
                                         std::cerr << "[vu1:dump] index=" << dumpIndex << " mscal=0x" << std::hex << startPC << " top=0x" << top
                                                   << " itop=0x" << itop << std::dec << std::endl;
                                     }
                                     g_gsDebugVu1Pc.store(startPC, std::memory_order_relaxed);
                                     g_gsDebugVu1Top.store(top, std::memory_order_relaxed);
                                     g_gsDebugVu1Data = m_memory.getVU1Data();
                                     g_gsDebugVu1Code = m_memory.getVU1Code();
                                     // VU1 runs a microprogram to its E bit; the cap only guards runaway code.
                                     const uint64_t vuStart = PS2X_TICKS();
                                     m_vu1.execute(m_memory.getVU1Code(), PS2_VU1_CODE_SIZE,
                                                   m_memory.getVU1Data(), PS2_VU1_DATA_SIZE,
                                                   m_gs, &m_memory, startPC, top, itop, kVu1ProgramCycleCap);
                                     g_perfVu1Ns += (PS2X_TICKS() - vuStart) * 5u / 18u; // about 3.6 GHz: a figure for the [perf] line
                                     if (dumpIndex >= 0)
                                         vuDumpWrite(m_memory, m_vu1, dumpIndex, "after");
                                     if (cpuContext)
                                         cpuContext->vu0_vpu_stat =
                                             (cpuContext->vu0_vpu_stat & ~0x0600u) |
                                             (m_vu1.state().stoppedByD ? 0x0200u : 0u) |
                                             (m_vu1.state().stoppedByT ? 0x0400u : 0u); });
    m_memory.setVu1MscntCallback([this](uint32_t top, uint32_t itop)
                                 {
                                     // On the VIF worker the EE context is not ours to touch: FBRST comes with the job.
                                     const bool onWorker = PS2Memory::onVifWorker();
                                     R5900Context *cpuContext = onWorker ? nullptr : m_eeScheduler ? m_eeScheduler->currentContext() : nullptr;
                                     if (!cpuContext && !onWorker)
                                     {
                                         cpuContext = &m_cpuContext;
                                     }
                                     const uint32_t fbrst = onWorker ? PS2Memory::vifJobFbrst() : cpuContext->vu0_fbrst;
                                     m_vu1.state().dBitEnabled = (fbrst & (1u << 10)) != 0u;
                                     m_vu1.state().tBitEnabled = (fbrst & (1u << 11)) != 0u;
                                     const int dumpIndex = vuDumpBegin();
                                     if (dumpIndex >= 0)
                                     {
                                         vuDumpWrite(m_memory, m_vu1, dumpIndex, "before");
                                         std::cerr << "[vu1:dump] index=" << dumpIndex << " mscnt pc=0x" << std::hex << m_vu1.state().pc
                                                   << " top=0x" << top << " itop=0x" << itop << std::dec << std::endl;
                                     }
                                     const uint64_t vuStart = PS2X_TICKS();
                                     g_gsDebugVu1Pc.store(0x10000u | m_vu1.state().pc, std::memory_order_relaxed);
                                     g_gsDebugVu1Top.store(top, std::memory_order_relaxed);
                                     m_vu1.resume(m_memory.getVU1Code(), PS2_VU1_CODE_SIZE,
                                                  m_memory.getVU1Data(), PS2_VU1_DATA_SIZE,
                                                  m_gs, &m_memory, top, itop, kVu1ProgramCycleCap);
                                     g_perfVu1Ns += (PS2X_TICKS() - vuStart) * 5u / 18u; // about 3.6 GHz: a figure for the [perf] line
                                     if (dumpIndex >= 0)
                                         vuDumpWrite(m_memory, m_vu1, dumpIndex, "after");
                                     if (cpuContext)
                                         cpuContext->vu0_vpu_stat =
                                             (cpuContext->vu0_vpu_stat & ~0x0600u) |
                                             (m_vu1.state().stoppedByD ? 0x0200u : 0u) |
                                             (m_vu1.state().stoppedByT ? 0x0400u : 0u); });
    resetIop();
    m_vu0.reset();
    m_vu1.reset();
    if (!std::getenv("PS2X_VIF_THREAD") || std::getenv("PS2X_VIF_THREAD")[0] != '0')
        m_memory.startVifWorker([this]
        {
            const R5900Context *context = m_eeScheduler ? m_eeScheduler->currentContext() : nullptr;
            return context ? context->vu0_fbrst : m_cpuContext.vu0_fbrst;
        },
        [this](const uint8_t *data, uint32_t size)
        {
            const auto gsStart = std::chrono::steady_clock::now();
            m_gs.processGIFPacket(data, size);
            g_perfGsNs += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - gsStart).count();
        });
    // VU1 writes VF/VI/ACC at issue (readers interlock on them); PS2X_VU1=ref keeps the queued reference model.
    // Native blocks generated from captured microcode (tools/vu1n_gen.py) run when linked; the exact engine covers the rest.
    {
        const char *mode = std::getenv("PS2X_VU1");
        const bool reference = mode && std::strcmp(mode, "ref") == 0;
        m_vu1.setDirectRegisterWrites(!reference);
        std::cerr << "[vu1] mode=" << (reference ? "ref" : "direct") << std::endl;
    }

    m_boundRdram = rdram;
    m_boundGSVram = gsVram;
    return true;
}

bool PS2Runtime::initialize(const char *title)
{
    try
    {
        if (!m_memory.initialize())
        {
            std::cerr << "Failed to initialize PS2 memory" << std::endl;
            return false;
        }

        if (!syncCoreSubsystems())
        {
            std::cerr << "Failed to bind runtime core subsystems" << std::endl;
            return false;
        }
#if defined(PLATFORM_VITA)
        InitWindow(HOST_WINDOW_WIDTH, HOST_WINDOW_HEIGHT, title); // raylib vita does not support audio
#else
        SetConfigFlags(FLAG_WINDOW_RESIZABLE | (std::getenv("PS2X_PC_MENU_TEST") ? FLAG_WINDOW_UNFOCUSED : 0));
        InitWindow(1920, 1080, title);
        host_video::initialize();
        host_video::selfTest();
        host_menu::initialize(title);
        {
            // GS rasterizer: GPU by default, PS2X_GS=cpu selects the software path.
            const char *gsBackend = std::getenv("PS2X_GS");
#if PS2X_REQUIRE_GPU_RASTERIZER
            if (gsBackend && std::string(gsBackend) == "cpu")
                throw std::runtime_error("This build requires the host GPU rasterizer; PS2X_GS=cpu is incompatible");
#endif
            if (gsBackend && std::string(gsBackend) == "hw")
            {
                // Native renderer (render targets, texture cache) on its own GS thread. Its context shares objects
                // with the window's, so finished frames reach the window as textures, without a GPU readback.
                GSSharedPresent::CaptureHostContext();
                m_gs.setRasterBackend(std::make_unique<GSThreadedBackend>(std::make_unique<GSHwBackend>()));
                GSSharedPresent::RestoreHostContext();
                std::cerr << "[gs] backend=hw" << std::endl;
            }
            else if (!gsBackend || std::string(gsBackend) != "cpu")
            {
                if (auto gpu = GSGpuBackend::Create(PS2X_REQUIRE_GPU_RASTERIZER != 0))
                {
                    m_gs.setRasterBackend(std::move(gpu));
                    std::cerr << "[gs] backend=gpu" << std::endl;
                }
                else
                {
#if PS2X_REQUIRE_GPU_RASTERIZER
                    throw std::runtime_error("Required OpenGL GS rasterizer unavailable; software fallback is disabled");
#else
                    std::cerr << "[gs] backend=cpu (GPU backend unavailable)" << std::endl;
#endif
                }
            }
            else
            {
                std::cerr << "[gs] backend=cpu" << std::endl;
            }
        }
        ps2UpdateHostPadInput();
        InitAudioDevice();
        m_audioBackend.setAudioReady(IsAudioDeviceReady());
#endif
        {
            // Presentation only; the guest vblank stays at 60 Hz. 0 removes the limit.
#if defined(PLATFORM_VITA)
            const char *fpsLimit = std::getenv("PS2X_FPS_LIMIT");
            SetTargetFPS(fpsLimit ? std::atoi(fpsLimit) : 60);
#else
            host_menu::applyFrameLimit();  // the F1 menu's limit, else PS2X_FPS_LIMIT, else 60
#endif
        }
        if (m_debugUiInitCallback)
        {
            m_debugUiInitCallback(*this, m_debugUiUserData);
            m_debugUiInitialized = true;
        }

        return true;
    }
    catch (const std::exception &e)
    {
        std::cerr << "Failed to initialize PS2 runtime: " << e.what() << std::endl;
    }
    catch (...)
    {
        std::cerr << "Failed to initialize PS2 runtime: unknown exception" << std::endl;
    }

    return false;
}

bool PS2Runtime::loadELF(const std::string &elfPath)
{
    configureIoPathsFromElf(elfPath);

    std::ifstream file(elfPath, std::ios::binary);
    if (!file)
    {
        std::cerr << "Failed to open ELF file: " << elfPath << std::endl;
        return false;
    }

    file.seekg(0, std::ios::end);
    const std::streamoff fileSize = file.tellg();
    if (fileSize < static_cast<std::streamoff>(sizeof(ElfHeader)))
    {
        std::cerr << "ELF file is too small: " << elfPath << std::endl;
        return false;
    }
    file.seekg(0, std::ios::beg);

    ElfHeader header{};
    if (!file.read(reinterpret_cast<char *>(&header), sizeof(header)))
    {
        std::cerr << "Failed to read ELF header from: " << elfPath << std::endl;
        return false;
    }

    if (header.magic != ELF_MAGIC)
    {
        std::cerr << "Invalid ELF magic number" << std::endl;
        return false;
    }

    if (header.elf_class != 1u || header.endianness != 1u)
    {
        std::cerr << "Unsupported ELF format (expected 32-bit little-endian)." << std::endl;
        return false;
    }

    if (header.machine != EM_MIPS || header.type != ET_EXEC)
    {
        std::cerr << "Not a MIPS executable ELF file" << std::endl;
        return false;
    }

    if (header.phnum != 0u && header.phentsize < sizeof(ProgramHeader))
    {
        std::cerr << "Unsupported ELF program-header entry size: " << header.phentsize << std::endl;
        return false;
    }

    const uint64_t programHeaderTableEnd =
        static_cast<uint64_t>(header.phoff) +
        static_cast<uint64_t>(header.phnum) * static_cast<uint64_t>(header.phentsize);
    if (programHeaderTableEnd > static_cast<uint64_t>(fileSize))
    {
        std::cerr << "ELF program-header table is out of range." << std::endl;
        return false;
    }

    m_cpuContext.pc = header.entry;
    m_debugPc.store(m_cpuContext.pc, std::memory_order_relaxed);

    uint32_t maxLoadedRdramEnd = kGuestHeapDefaultBase;
    uint32_t moduleBase = std::numeric_limits<uint32_t>::max();
    uint32_t moduleEnd = 0u;
    bool loadedAnySegment = false;

    for (uint16_t i = 0; i < header.phnum; i++)
    {
        const uint64_t phOffset =
            static_cast<uint64_t>(header.phoff) +
            static_cast<uint64_t>(i) * static_cast<uint64_t>(header.phentsize);
        if (phOffset + sizeof(ProgramHeader) > static_cast<uint64_t>(fileSize))
        {
            std::cerr << "ELF program header " << i << " is out of range." << std::endl;
            return false;
        }

        ProgramHeader ph{};
        file.seekg(static_cast<std::streamoff>(phOffset), std::ios::beg);
        if (!file.read(reinterpret_cast<char *>(&ph), sizeof(ph)))
        {
            std::cerr << "Failed to read ELF program header " << i << std::endl;
            return false;
        }

        if (ph.type != PT_LOAD || ph.memsz == 0u)
        {
            continue;
        }

        if (ph.filesz > ph.memsz)
        {
            std::cerr << "ELF segment " << i << " has filesz > memsz." << std::endl;
            return false;
        }

        const uint64_t segmentFileEnd = static_cast<uint64_t>(ph.offset) + static_cast<uint64_t>(ph.filesz);
        if (segmentFileEnd > static_cast<uint64_t>(fileSize))
        {
            std::cerr << "ELF segment " << i << " exceeds file bounds." << std::endl;
            return false;
        }

        const bool scratch =
            ph.vaddr >= PS2_SCRATCHPAD_BASE &&
            ph.vaddr < (PS2_SCRATCHPAD_BASE + PS2_SCRATCHPAD_SIZE);

        uint32_t physAddr = 0u;
        try
        {
            physAddr = m_memory.translateAddress(ph.vaddr);
        }
        catch (const std::exception &e)
        {
            std::cerr << "Failed to translate ELF segment " << i
                      << " virtual address 0x" << std::hex << ph.vaddr
                      << std::dec << ": " << e.what() << std::endl;
            return false;
        }
        const uint64_t regionSize = scratch ? static_cast<uint64_t>(PS2_SCRATCHPAD_SIZE)
                                            : static_cast<uint64_t>(PS2_RAM_SIZE);
        const uint64_t segmentMemEnd = static_cast<uint64_t>(physAddr) + static_cast<uint64_t>(ph.memsz);
        if (segmentMemEnd > regionSize)
        {
            std::cerr << "ELF segment " << i << " exceeds "
                      << (scratch ? "scratchpad" : "RDRAM")
                      << " bounds (vaddr=0x" << std::hex << ph.vaddr
                      << " memsz=0x" << ph.memsz << std::dec << ")." << std::endl;
            return false;
        }

        uint8_t *destBase = scratch ? m_memory.getScratchpad() : m_memory.getRDRAM();
        if (!destBase)
        {
            std::cerr << "ELF segment " << i << " has no destination memory backing." << std::endl;
            return false;
        }

        uint8_t *dest = destBase + physAddr;
        if (ph.filesz > 0u)
        {
            file.seekg(static_cast<std::streamoff>(ph.offset), std::ios::beg);
            if (!file.read(reinterpret_cast<char *>(dest), ph.filesz))
            {
                std::cerr << "Failed to read ELF segment " << i << " payload." << std::endl;
                return false;
            }
        }

        if (ph.memsz > ph.filesz)
        {
            std::memset(dest + ph.filesz, 0, ph.memsz - ph.filesz);
        }

        RUNTIME_LOG("Loading segment: 0x" << std::hex << ph.vaddr
                                          << " - 0x" << (static_cast<uint64_t>(ph.vaddr) + static_cast<uint64_t>(ph.memsz))
                                          << " (filesz: 0x" << ph.filesz
                                          << ", memsz: 0x" << ph.memsz << ")"
                                          << std::dec << std::endl);

        if (!scratch)
        {
            maxLoadedRdramEnd = std::max(maxLoadedRdramEnd, static_cast<uint32_t>(segmentMemEnd));
        }

        if (ph.flags & 0x1u) // PF_X
        {
            const uint64_t execEnd = static_cast<uint64_t>(ph.vaddr) + static_cast<uint64_t>(ph.filesz);
            if (execEnd <= std::numeric_limits<uint32_t>::max())
            {
                m_memory.registerCodeRegion(ph.vaddr, static_cast<uint32_t>(execEnd));
            }
        }

        loadedAnySegment = true;
        moduleBase = std::min(moduleBase, ph.vaddr);
        const uint64_t segmentVirtualEnd = static_cast<uint64_t>(ph.vaddr) + static_cast<uint64_t>(ph.memsz);
        const uint32_t clampedVirtualEnd =
            (segmentVirtualEnd > std::numeric_limits<uint32_t>::max())
                ? std::numeric_limits<uint32_t>::max()
                : static_cast<uint32_t>(segmentVirtualEnd);
        moduleEnd = std::max(moduleEnd, clampedVirtualEnd);
    }

    if (!loadedAnySegment)
    {
        std::cerr << "ELF contains no loadable PT_LOAD segments." << std::endl;
        return false;
    }

    if (maxLoadedRdramEnd > PS2_RAM_SIZE)
    {
        maxLoadedRdramEnd = PS2_RAM_SIZE;
    }

    const uint32_t paddedEnd = (maxLoadedRdramEnd > (PS2_RAM_SIZE - kGuestHeapSafetyPad))
                                   ? PS2_RAM_SIZE
                                   : (maxLoadedRdramEnd + kGuestHeapSafetyPad);
    const uint32_t suggestedHeapBase = alignGuestHeapValue(paddedEnd, kGuestHeapDefaultAlignment);
    {
        std::lock_guard<std::mutex> lock(m_guestHeapMutex);
        if (!m_guestHeapConfigured)
        {
            const uint32_t hardLimit = std::min(kGuestHeapHardLimit, PS2_RAM_SIZE);
            m_guestHeapSuggestedBase = std::min(suggestedHeapBase, hardLimit);
            m_guestHeapBase = m_guestHeapSuggestedBase;
            m_guestHeapEnd = m_guestHeapSuggestedBase;
            m_guestHeapLimit = hardLimit;
        }
    }
    {
        std::lock_guard<std::mutex> lock(m_asyncCallbackStackMutex);
        m_asyncCallbackStackFloor = std::min(std::max(kRuntimeArenaEnd, suggestedHeapBase), PS2_RAM_SIZE);
        m_asyncCallbackStackTop = PS2_RAM_SIZE;
    }

    LoadedModule module;
    module.name = elfPath.substr(elfPath.find_last_of("/\\") + 1);
    module.baseAddress = (moduleBase == std::numeric_limits<uint32_t>::max()) ? 0x00100000u : moduleBase;
    module.size = (moduleEnd > module.baseAddress) ? static_cast<size_t>(moduleEnd - module.baseAddress) : 0u;
    module.active = true;

    m_loadedModules.push_back(module);

    uint32_t elfCrc32 = 0u;
    const bool elfCrc32Valid = computeFileCrc32(elfPath, elfCrc32);
    if (!elfCrc32Valid)
    {
        std::cerr << "[ps2xIOP] failed to compute ELF CRC32 for '" << elfPath << "'" << std::endl;
    }
    ps2x::iop::GameIdentity identity;
    identity.elfName = module.name;
    identity.entryPoint = m_cpuContext.pc;
    identity.crc32 = elfCrc32;
    std::string romError;
    if (!m_romDevice.configure(identity, &romError))
    {
        std::cerr << "[ROM0] failed to configure profile: " << romError << std::endl;
        return false;
    }

    m_iopSubsystem->reset();

    {
        std::lock_guard lock(sparseFunctionMutex);
        sparseFunctions.erase(this);
    }
    ps2_game_overrides::applyMatching(*this,
                                      elfPath,
                                      m_cpuContext.pc,
                                      elfCrc32,
                                      elfCrc32Valid);

    RUNTIME_LOG("ELF file loaded successfully. Entry point: 0x" << std::hex << m_cpuContext.pc << std::dec);
    return true;
}

const PS2Runtime::IoPaths &PS2Runtime::getIoPaths()
{
    return runtimeIoPaths();
}

void PS2Runtime::setIoPaths(const IoPaths &paths)
{
    IoPaths normalized = paths;
    normalized.elfPath = normalizeAbsolutePath(normalized.elfPath);
    normalized.elfDirectory = normalizeAbsolutePath(normalized.elfDirectory);
    normalized.hostRoot = normalizeAbsolutePath(normalized.hostRoot);
    normalized.cdRoot = normalizeAbsolutePath(normalized.cdRoot);
    normalized.mcRoot = normalizeAbsolutePath(normalized.mcRoot);
    normalized.cdImage = normalizeAbsolutePath(normalized.cdImage);

    if (normalized.elfDirectory.empty() && !normalized.elfPath.empty())
    {
        normalized.elfDirectory = normalized.elfPath.parent_path();
    }

    if (normalized.hostRoot.empty())
    {
        normalized.hostRoot = normalized.elfDirectory;
    }
    if (normalized.cdRoot.empty())
    {
        normalized.cdRoot = normalized.elfDirectory;
    }
    if (normalized.mcRoot.empty())
    {
        normalized.mcRoot = normalized.elfDirectory / "mc0";
    }

    runtimeIoPaths() = normalized;
}

void PS2Runtime::configureIoPathsFromElf(const std::string &elfPath)
{
    IoPaths paths = runtimeIoPaths();
    paths.elfPath = normalizeAbsolutePath(std::filesystem::path(elfPath));
    if (!paths.elfPath.empty())
    {
        paths.elfDirectory = paths.elfPath.parent_path();
    }

    if (!paths.elfDirectory.empty())
    {
        paths.hostRoot = paths.elfDirectory;
        paths.cdRoot = paths.elfDirectory;
        paths.mcRoot = paths.elfDirectory / "mc0";
    }

    setIoPaths(paths);
}

namespace
{
    bool generatedFunctionTableSlot(uint32_t address, uint32_t &slot)
    {
        if ((address & 3u) != 0u || g_ps2RecompiledFunctionTableSlotCount == 0u)
        {
            return false;
        }

        if (address < g_ps2RecompiledFunctionTableBase || address >= g_ps2RecompiledFunctionTableEnd)
        {
            return false;
        }

        const uint32_t offset = address - g_ps2RecompiledFunctionTableBase;
        slot = offset >> 2;
        return slot < g_ps2RecompiledFunctionTableSlotCount;
    }
}

bool PS2Runtime::replaceFunction(uint32_t address, RecompiledFunction func)
{
    uint32_t slot = 0u;
    if (!generatedFunctionTableSlot(address, slot))
    {
        if (address == 0u || (address & 3u) != 0u || address >= 0x02000000u)
            return false;
        std::lock_guard lock(sparseFunctionMutex);
        if (func) sparseFunctions[this][address] = func;
        else if (auto owner = sparseFunctions.find(this); owner != sparseFunctions.end())
            owner->second.erase(address);
        return true;
    }

    g_ps2RecompiledFunctionTable[slot] = func;
    return true;
}

bool PS2Runtime::registerFunction(uint32_t address, RecompiledFunction func)
{
    return replaceFunction(address, func);
}

bool PS2Runtime::hasFunction(uint32_t address) const
{
    uint32_t slot = 0u;
    if (generatedFunctionTableSlot(address, slot))
        return g_ps2RecompiledFunctionTable[slot] != nullptr;
    std::lock_guard lock(sparseFunctionMutex);
    const auto owner = sparseFunctions.find(this);
    return owner != sparseFunctions.end() && owner->second.contains(address);
}

const char *describeGuestBranchKind(PS2Runtime::GuestBranchKind kind)
{
    switch (kind)
    {
    case PS2Runtime::GuestBranchKind::DirectJump:
        return "DirectJump";
    case PS2Runtime::GuestBranchKind::DirectCall:
        return "DirectCall";
    case PS2Runtime::GuestBranchKind::IndirectJump:
        return "IndirectJump";
    case PS2Runtime::GuestBranchKind::IndirectCall:
        return "IndirectCall";
    case PS2Runtime::GuestBranchKind::Return:
        return "Return";
    default:
        return "Unknown";
    }
}

PS2Runtime::RecompiledFunction PS2Runtime::lookupFunction(uint32_t address)
{
    pushDispatchPc(address);

    uint32_t slot = 0u;
    if (generatedFunctionTableSlot(address, slot))
    {
        RecompiledFunction fn = g_ps2RecompiledFunctionTable[slot];
        if (fn != nullptr)
        {
            return fn;
        }
    }

    {
        std::lock_guard lock(sparseFunctionMutex);
        if (auto owner = sparseFunctions.find(this); owner != sparseFunctions.end()) {
            if (auto entry = owner->second.find(address); entry != owner->second.end())
                return entry->second;
        }
    }
    std::cerr << "Error: No exact recompiled function for guest PC 0x" << std::hex << address
              << " tableBase=0x" << g_ps2RecompiledFunctionTableBase
              << " tableEnd=0x" << g_ps2RecompiledFunctionTableEnd
              << " codeRegion=" << (m_memory.isCodeAddress(address) ? "yes" : "no")
              << " trace=" << formatDispatchHistory()
              << std::dec << std::endl;

    static RecompiledFunction missingFunction = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t badPc = ctx->pc;
        runtime->reportMissingFunction(rdram,
                                       ctx,
                                       badPc,
                                       0u,
                                       PS2Runtime::GuestBranchKind::IndirectJump,
                                       "dispatch");
    };

    return missingFunction;
}

void PS2Runtime::setMissingFunctionPolicy(MissingFunctionPolicy policy)
{
    m_missingFunctionPolicy.store(static_cast<uint32_t>(policy), std::memory_order_release);
}

PS2Runtime::MissingFunctionPolicy PS2Runtime::missingFunctionPolicy() const
{
    return static_cast<MissingFunctionPolicy>(m_missingFunctionPolicy.load(std::memory_order_acquire));
}

void PS2Runtime::resetMissingFunctionReportOnce()
{
    m_missingFunctionReported.store(false, std::memory_order_release);
}

// last dispatched guest PCs of the calling host thread (EE scheduler diagnostics)
std::string ps2xDispatchTrail()
{
    return formatDispatchHistory();
}

void PS2Runtime::reportMissingFunction(uint8_t *rdram,
                                       R5900Context *ctx,
                                       uint32_t targetPc,
                                       uint32_t sourcePc,
                                       GuestBranchKind kind,
                                       const char *debugName)
{
    const MissingFunctionPolicy policy = missingFunctionPolicy();
    const bool firstReport = !m_missingFunctionReported.exchange(true, std::memory_order_acq_rel);

    const uint32_t pc = ctx->pc;
    const uint32_t ra = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[31], 0));
    const uint32_t sp = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[29], 0));
    const uint32_t gp = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[28], 0));
    const uint32_t a0 = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[4], 0));
    const uint32_t a1 = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[5], 0));
    const uint32_t a2 = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[6], 0));
    const uint32_t a3 = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[7], 0));
    const uint32_t s0 = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[16], 0));
    const uint32_t s1 = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[17], 0));
    const uint32_t v0 = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[2], 0));
    const uint32_t v1 = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[3], 0));

    auto readGuestU32At = [rdram](uint32_t addr, uint32_t &out) -> bool
    {
        // TODO this !rdram exist only because of test fix those test later
        if (!rdram || addr > PS2_RAM_SIZE - sizeof(uint32_t))
        {
            out = 0u;
            return false;
        }

        std::memcpy(&out, rdram + addr, sizeof(uint32_t));
        return true;
    };

    auto readGuestU32Offset = [&readGuestU32At](uint32_t base, uint32_t offset, uint32_t &out) -> bool
    {
        if (base > PS2_RAM_SIZE - sizeof(uint32_t) || offset > PS2_RAM_SIZE - sizeof(uint32_t) - base)
        {
            out = 0u;
            return false;
        }

        return readGuestU32At(base + offset, out);
    };

    uint32_t a0Word0 = 0u;
    uint32_t a0Word4 = 0u;
    uint32_t a0Word8 = 0u;
    uint32_t a0WordC = 0u;
    const bool a0Readable =
        readGuestU32Offset(a0, 0x00u, a0Word0) &&
        readGuestU32Offset(a0, 0x04u, a0Word4) &&
        readGuestU32Offset(a0, 0x08u, a0Word8) &&
        readGuestU32Offset(a0, 0x0cu, a0WordC);

    uint32_t s0Word0 = 0u;
    uint32_t s0Word4 = 0u;
    uint32_t s0Word8 = 0u;
    uint32_t s0WordC = 0u;
    const bool s0Readable =
        readGuestU32Offset(s0, 0x00u, s0Word0) &&
        readGuestU32Offset(s0, 0x04u, s0Word4) &&
        readGuestU32Offset(s0, 0x08u, s0Word8) &&
        readGuestU32Offset(s0, 0x0cu, s0WordC);

    uint32_t recordWord0 = 0u;
    uint32_t recordWord4 = 0u;
    uint32_t recordWord8 = 0u;
    uint32_t recordWordC = 0u;
    const bool recordReadable =
        s0Readable && s0Word4 != 0u &&
        readGuestU32Offset(s0Word4, 0x00u, recordWord0) &&
        readGuestU32Offset(s0Word4, 0x04u, recordWord4) &&
        readGuestU32Offset(s0Word4, 0x08u, recordWord8) &&
        readGuestU32Offset(s0Word4, 0x0cu, recordWordC);

    uint32_t vtableSlot0 = 0u;
    uint32_t vtableSlot4 = 0u;
    uint32_t vtableSlot8 = 0u;
    uint32_t vtableSlotC = 0u;
    const bool vtableReadable =
        a0Readable && a0Word0 != 0u &&
        readGuestU32Offset(a0Word0, 0x00u, vtableSlot0) &&
        readGuestU32Offset(a0Word0, 0x04u, vtableSlot4) &&
        readGuestU32Offset(a0Word0, 0x08u, vtableSlot8) &&
        readGuestU32Offset(a0Word0, 0x0cu, vtableSlotC);

    if (firstReport)
    {
        const char *dumpMissing = std::getenv("PS2X_DUMP_MISSING_CODE");
        if (dumpMissing && std::string(dumpMissing) == "1" && rdram &&
            targetPc >= 0x100000u && targetPc < 0x1fe0000u) {
            const uint32_t base = targetPc & ~0xffffu;
            std::ostringstream path;
            path << "analysis/missing-code-" << std::hex << base << ".bin";
            std::ofstream dump(path.str(), std::ios::binary);
            dump.write(reinterpret_cast<const char *>(rdram + base), 0x20000u);
            std::cerr << "[guest-code-dump] base=0x" << std::hex << base
                      << std::dec << " size=131072 path=" << path.str() << std::endl;
        }
        std::ostringstream oss;
        oss << "[guest-branch:missing-target] kind=" << describeGuestBranchKind(kind)
            << " op=" << (debugName ? debugName : "<unknown>")
            << " source=0x" << std::hex << sourcePc
            << " target=0x" << targetPc
            << " pc=0x" << pc
            << " ra=0x" << ra
            << " sp=0x" << sp
            << " gp=0x" << gp
            << " a0=0x" << a0
            << " a1=0x" << a1
            << " a2=0x" << a2
            << " a3=0x" << a3
            << " s0=0x" << s0
            << " s1=0x" << s1
            << " v0=0x" << v0
            << " v1=0x" << v1
            << " a0Readable=" << (a0Readable ? "yes" : "no")
            << " a0[0]=0x" << a0Word0
            << " a0[4]=0x" << a0Word4
            << " a0[8]=0x" << a0Word8
            << " a0[c]=0x" << a0WordC
            << " s0Readable=" << (s0Readable ? "yes" : "no")
            << " s0[0]=0x" << s0Word0
            << " s0[4]=0x" << s0Word4
            << " s0[8]=0x" << s0Word8
            << " s0[c]=0x" << s0WordC
            << " recordReadable=" << (recordReadable ? "yes" : "no")
            << " record[0]=0x" << recordWord0
            << " record[4]=0x" << recordWord4
            << " record[8]=0x" << recordWord8
            << " record[c]=0x" << recordWordC
            << " vtableReadable=" << (vtableReadable ? "yes" : "no")
            << " vtbl[0]=0x" << vtableSlot0
            << " vtbl[4]=0x" << vtableSlot4
            << " vtbl[8]=0x" << vtableSlot8
            << " vtbl[c]=0x" << vtableSlotC
            << " codeRegion=" << (m_memory.isCodeAddress(targetPc) ? "yes" : "no")
            << " policy=" << static_cast<uint32_t>(policy)
            << " trace=" << formatDispatchHistory()
            << std::dec;

        static std::mutex s_missingFunctionLogMutex;
        {
            std::lock_guard<std::mutex> lock(s_missingFunctionLogMutex);
            std::cerr << oss.str() << std::endl;
        }
    }

    if (firstReport && policy == MissingFunctionPolicy::BreakOnce)
    {
#if defined(_MSC_VER)
        __debugbreak();
#endif // TODO others breakpoints
    }

    if (ctx)
    {
        ctx->pc = targetPc;
    }

    if (policy == MissingFunctionPolicy::Stop)
    {
        requestStop();
    }
}

// A PC alone cannot distinguish a return from a yield in recursive guest code.
// Every checkpoint yield changes this token; all native callers propagate it to
// the scheduler without executing guest continuations against an inner frame.
static thread_local uint64_t s_guestYieldGeneration = 0;

bool PS2Runtime::dispatchGuestBranch(uint8_t *rdram,
                                     R5900Context *ctx,
                                     uint32_t targetPc,
                                     uint32_t sourcePc,
                                     uint32_t fallthroughPc,
                                     GuestBranchKind kind,
                                     const char *debugName)
{
    ctx->pc = targetPc;
    // Asura resolves module imports by rewriting JAL 0 instructions in EE RAM.
    // Generated code retains the original placeholder; honor the resolved JAL.
    if (kind == GuestBranchKind::DirectCall && targetPc == 0u && rdram &&
        sourcePc >= 0x100000u && sourcePc < PS2_RAM_SIZE - 4u) {
        const uint32_t instruction = Ps2FastRead32(rdram, sourcePc);
        const uint32_t resolved = ((sourcePc + 4u) & 0xf0000000u) | ((instruction & 0x03ffffffu) << 2u);
        if ((instruction >> 26u) == 3u && resolved != 0u) {
            targetPc = resolved;
            ctx->pc = resolved;
            static uint32_t resolvedImportLogs = 0;
            if (resolvedImportLogs++ < 16u)
                std::cerr << "[guest-import-jal] source=0x" << std::hex << sourcePc
                          << " resolved=0x" << resolved << std::dec << std::endl;
        }
    }
#if PS2X_ENABLE_CODWAW_SUPPLEMENTS
    if (sourcePc == 0x424378u && !hasFunction(targetPc))
        tryBindCodWawMpeg(*this, rdram, targetPc);
#endif
    const bool isCall = (kind == GuestBranchKind::DirectCall || kind == GuestBranchKind::IndirectCall);

    // Every inter-function transfer is also a deterministic EE safe point.
    // Backward edges inside generated functions use eeCheckpointDue(), while
    // this charge bounds straight-line call chains that have no local loop.
    if (m_eeScheduler && m_eeScheduler->checkpointDue(EeScheduler::kGuestDispatchCycles))
    {
        ++s_guestYieldGeneration;
        return false;
    }

    if (!isCall)
    {
        if (!hasFunction(targetPc))
        {
            reportMissingFunction(rdram, ctx, targetPc, sourcePc, kind, debugName);
        }

        ctx->pc = targetPc;
        return false;
    }

    // One table read for the common case: this runs on every guest call.
    RecompiledFunction targetFn = nullptr;
    uint32_t tableSlot = 0u;
    if (generatedFunctionTableSlot(targetPc, tableSlot))
        targetFn = g_ps2RecompiledFunctionTable[tableSlot];
    if (targetFn != nullptr)
        pushDispatchPc(targetPc);
    else if (!hasFunction(targetPc))
    {
        reportMissingFunction(rdram, ctx, targetPc, sourcePc, kind, debugName);

        const MissingFunctionPolicy policy = missingFunctionPolicy();

        if (policy == MissingFunctionPolicy::SkipCallDebug && isCall)
        {
            ctx->pc = fallthroughPc;
            return true;
        }

        if (policy == MissingFunctionPolicy::ContinueToTarget)
        {
            ctx->pc = targetPc;
            // if you need the app to keep open to open debug pannel change this to false
            return false;
        }

        return false;
    }

    if (targetFn == nullptr)
        targetFn = lookupFunction(targetPc);
    const uint32_t entryPc = ctx->pc;
    const uint64_t yieldGeneration = s_guestYieldGeneration;
    const uint32_t entrySp = getRegU32(ctx, 29);
    targetFn(rdram, ctx, this);

    if (yieldGeneration != s_guestYieldGeneration)
    {
        // The callee yielded somewhere. It only counts as a completed call when the guest is back in this frame:
        // at the return address *and* with the caller's stack pointer (a yield taken right at the return). A
        // recursive inner frame can stop at the same PC with a lower sp; continuing here ran the outer
        // continuation on the inner frame and loaded locals as RA (Ben10 deaths with ra=0xa / 0xd).
        const bool returned = ctx->pc == fallthroughPc && getRegU32(ctx, 29) == entrySp;
        static const bool traceYields = std::getenv("PS2X_DISPATCH_YIELD_TRACE") != nullptr;
        static thread_local unsigned collisionLogs = 0;
        if (traceYields && !returned && (ctx->pc == fallthroughPc || ctx->pc == entryPc) && collisionLogs++ < 16u)
            std::fprintf(stderr, "[dispatch:yield-collision] source=%x target=%x return=%x yielded=%x sp=%x entrySp=%x\n",
                         sourcePc, targetPc, fallthroughPc, ctx->pc, getRegU32(ctx, 29), entrySp);
        if (!returned)
            return false;
    }

    if (isStopRequested() || ctx->pc == 0u)
    {
        return false;
    }

    if (ctx->pc == entryPc)
    {
        ctx->pc = fallthroughPc;
    }

    return ctx->pc == fallthroughPc;
}

void PS2Runtime::SignalException(R5900Context *ctx, PS2Exception exception)
{
    if (exception == EXCEPTION_INTEGER_OVERFLOW)
    {
        HandleIntegerOverflow(ctx);
        return;
    }

    raiseCop0Exception(ctx, static_cast<uint32_t>(exception),
                       exception == EXCEPTION_TLB_REFILL);
}

void PS2Runtime::executeVU0Microprogram(uint8_t *rdram, R5900Context *ctx, uint32_t address)
{
    (void)rdram;

    uint8_t *const vu0Code = m_memory.getVU0Code();
    uint8_t *const vu0Data = m_memory.getVU0Data();
    const uint32_t startPC = address & ~0x7u;

    if (!vu0Code || !vu0Data || startPC + 8u > PS2_VU0_CODE_SIZE)
    {
        seedVu0IdleSuccess(ctx);
        return;
    }

    // PS2X_VU0_TRACE=1 logs vf1 in / vf2 out of the first calls of each microprogram
    // and dumps VU0 code memory once (analysis/vu0-code.bin).
    static const bool vu0Trace = std::getenv("PS2X_VU0_TRACE") != nullptr;
    alignas(16) float in1[4] = {};
    if (vu0Trace)
        _mm_store_ps(in1, ctx->vu0_vf[1]);

    // PS2X_VU0_FULL=<n>: the first n calls of each microprogram log every VF register the call changed (in -> out)
    // and the VF/VI inputs, to check the results against the arithmetic they implement.
    static const int vu0Full = std::getenv("PS2X_VU0_FULL") ? std::atoi(std::getenv("PS2X_VU0_FULL")) : 0;
    alignas(16) float fullIn[32][4];
    if (vu0Full > 0)
        for (int r = 0; r < 32; ++r)
            _mm_store_ps(fullIn[r], ctx->vu0_vf[r]);
    const uint16_t fullVi[4] = {ctx->vi[1], ctx->vi[2], ctx->vi[3], ctx->vi[4]};

    m_vu0.reset();
    copyVu0ContextToState(ctx, m_vu0.state());
    m_vu0.execute(vu0Code, PS2_VU0_CODE_SIZE,
                  vu0Data, PS2_VU0_DATA_SIZE,
                  m_gs, &m_memory,
                  startPC, 0u, ctx->vu0_itop, 4096);
    copyVu0StateToContext(m_vu0.state(), ctx);

    if (vu0Full > 0)
    {
        static std::unordered_map<uint32_t, int> fullCalls;
        if (fullCalls[startPC]++ < vu0Full)
        {
            std::fprintf(stderr, "[vu0:full] pc=0x%x vi1-4=%u,%u,%u,%u\n", startPC, fullVi[0], fullVi[1], fullVi[2], fullVi[3]);
            for (int r = 1; r < 32; ++r)
            {
                alignas(16) float out[4];
                _mm_store_ps(out, ctx->vu0_vf[r]);
                const bool changed = std::memcmp(out, fullIn[r], sizeof(out)) != 0;
                const bool used = fullIn[r][0] != 0.0f || fullIn[r][1] != 0.0f || fullIn[r][2] != 0.0f || fullIn[r][3] != 0.0f;
                if (changed || used)
                    std::fprintf(stderr, "[vu0:full]   vf%02d in=(%g,%g,%g,%g)%s out=(%g,%g,%g,%g)\n", r, fullIn[r][0], fullIn[r][1],
                                 fullIn[r][2], fullIn[r][3], changed ? " CHANGED" : "", out[0], out[1], out[2], out[3]);
            }
        }
    }

    if (vu0Trace)
    {
        static uint32_t callsPerPc[PS2_VU0_CODE_SIZE / 8u] = {};
        static bool codeDumped = false;
        if (!codeDumped)
        {
            codeDumped = true;
            if (FILE *f = std::fopen("analysis/vu0-code.bin", "wb"))
            {
                std::fwrite(vu0Code, 1, PS2_VU0_CODE_SIZE, f);
                std::fclose(f);
            }
        }
        uint32_t &n = callsPerPc[startPC / 8u];
        if (++n <= 24u || (n & (n - 1u)) == 0u)
        {
            alignas(16) float out2[4];
            _mm_store_ps(out2, ctx->vu0_vf[2]);
            std::fprintf(stderr,
                         "[vu0:call] pc=0x%x n=%u in=(%g,%g,%g,%g) out=(%g,%g,%g,%g) sin=%g cos=%g sinz=%g cosz=%g\n",
                         startPC, n, in1[0], in1[1], in1[2], in1[3], out2[0], out2[1], out2[2], out2[3],
                         std::sin(in1[0]), std::cos(in1[0]), std::sin(in1[2]), std::cos(in1[2]));
        }
    }
}

void PS2Runtime::vu0StartMicroProgram(uint8_t *rdram, R5900Context *ctx, uint32_t address)
{
    // VCALLMS and VCALLMSR both route here.
    executeVU0Microprogram(rdram, ctx, address);
}

void PS2Runtime::handleSyscall(uint8_t *rdram, R5900Context *ctx)
{
    handleSyscall(rdram, ctx, 0);
}

void PS2Runtime::handleSyscall(uint8_t *rdram, R5900Context *ctx, uint32_t encodedSyscallId)
{
    if (ctx->in_delay_slot)
    {
        throw std::runtime_error("Attempted to execute a syscall inside a branch delay slot! "
                                 "This breaks the atomic basic block model and is structurally unsupported by the emulator.");
    }

    const uint32_t syscallId = (encodedSyscallId != 0u)
                                   ? encodedSyscallId
                                   : getRegU32(ctx, 3); // $v1 / $3 is the EE kernel syscall number

    if (ps2_syscalls::dispatchNumericSyscall(syscallId, rdram, ctx, this))
    {
        return;
    }

    // God help you
    ps2_syscalls::TODO(rdram, ctx, this, encodedSyscallId);
}

void PS2Runtime::handleBreak(uint8_t *rdram, R5900Context *ctx)
{
    raiseCop0Exception(ctx, EXCEPTION_BREAKPOINT);
}

void PS2Runtime::drainCompletedDmacHandlers(uint8_t *rdram)
{
    for (uint32_t cause : m_memory.consumeCompletedDmacCauses())
    {
        ps2_syscalls::dispatchDmacHandlersForCause(rdram, this, cause);
    }
}

void PS2Runtime::handleTrap(uint8_t *rdram, R5900Context *ctx)
{
    raiseCop0Exception(ctx, EXCEPTION_TRAP);
}

void PS2Runtime::handleTLBR(uint8_t *rdram, R5900Context *ctx)
{
    uint32_t vpn = 0;
    uint32_t pfn = 0;
    uint32_t mask = 0;
    bool valid = false;

    const uint32_t index = ctx->cop0_index & 0x3Fu;
    if (!m_memory.tlbRead(index, vpn, pfn, mask, valid))
    {
        raiseCop0Exception(ctx, EXCEPTION_RESERVED_INSTRUCTION);
        return;
    }

    // Preserve low ASID bits in EntryHi.
    ctx->cop0_entryhi = (ctx->cop0_entryhi & 0x00000FFFu) | (vpn & 0xFFFFF000u);
    ctx->cop0_entrylo0 = (ctx->cop0_entrylo0 & ~0x03FFFFC2u) |
                         ((pfn & 0x000FFFFFu) << 6) |
                         (valid ? 0x2u : 0u);
    ctx->cop0_pagemask = mask & 0x01FFE000u;
}

void PS2Runtime::handleTLBWI(uint8_t *rdram, R5900Context *ctx)
{
    const uint32_t index = ctx->cop0_index & 0x3Fu;
    const uint32_t vpn = ctx->cop0_entryhi & 0xFFFFF000u;
    const uint32_t pfn = (ctx->cop0_entrylo0 >> 6) & 0x000FFFFFu;
    const uint32_t mask = ctx->cop0_pagemask & 0x01FFE000u;
    const bool valid = (ctx->cop0_entrylo0 & 0x2u) != 0u;

    if (!m_memory.tlbWrite(index, vpn, pfn, mask, valid))
    {
        raiseCop0Exception(ctx, EXCEPTION_RESERVED_INSTRUCTION);
    }
}

void PS2Runtime::handleTLBWR(uint8_t *rdram, R5900Context *ctx)
{
    const uint32_t entryCount = static_cast<uint32_t>(m_memory.tlbEntryCount());
    if (entryCount == 0)
    {
        raiseCop0Exception(ctx, EXCEPTION_RESERVED_INSTRUCTION);
        return;
    }

    const uint32_t wired = std::min(ctx->cop0_wired, entryCount - 1);
    uint32_t random = ctx->cop0_random % entryCount;
    if (random < wired)
    {
        random = wired;
    }

    const uint32_t vpn = ctx->cop0_entryhi & 0xFFFFF000u;
    const uint32_t pfn = (ctx->cop0_entrylo0 >> 6) & 0x000FFFFFu;
    const uint32_t mask = ctx->cop0_pagemask & 0x01FFE000u;
    const bool valid = (ctx->cop0_entrylo0 & 0x2u) != 0u;

    if (!m_memory.tlbWrite(random, vpn, pfn, mask, valid))
    {
        raiseCop0Exception(ctx, EXCEPTION_RESERVED_INSTRUCTION);
        return;
    }

    // Keep COP0 bookkeeping in sync with the selected slot.
    ctx->cop0_index = (ctx->cop0_index & ~0x3Fu) | (random & 0x3Fu);
    ctx->cop0_random = (random <= wired) ? (entryCount - 1) : (random - 1);
}

void PS2Runtime::handleTLBP(uint8_t *rdram, R5900Context *ctx)
{
    const int32_t index = m_memory.tlbProbe(ctx->cop0_entryhi & 0xFFFFF000u);
    if (index >= 0)
    {
        ctx->cop0_index = (ctx->cop0_index & ~0x8000003Fu) |
                          (static_cast<uint32_t>(index) & 0x3Fu);
    }
    else
    {
        // MIPS sets probe failure bit (P) in Index[31].
        ctx->cop0_index |= 0x80000000u;
    }
}

void PS2Runtime::clearLLBit(R5900Context *ctx)
{
    // LL/SC reservation is tracked separately from COP0 Status.
    ctx->llbit = 0;
    ctx->lladdr = 0;
}

uint32_t PS2Runtime::alignGuestHeapValue(uint32_t value, uint32_t alignment)
{
    if (alignment == 0)
    {
        return value;
    }

    const uint32_t mask = alignment - 1u;
    if (value > (std::numeric_limits<uint32_t>::max() - mask))
    {
        return std::numeric_limits<uint32_t>::max();
    }
    return (value + mask) & ~mask;
}

bool PS2Runtime::isGuestHeapAlignmentValid(uint32_t alignment)
{
    return alignment != 0u && (alignment & (alignment - 1u)) == 0u;
}

uint32_t PS2Runtime::normalizeGuestHeapAlignment(uint32_t alignment)
{
    if (!isGuestHeapAlignmentValid(alignment))
    {
        return kGuestHeapDefaultAlignment;
    }
    return std::max(alignment, kGuestHeapDefaultAlignment);
}

uint32_t PS2Runtime::clampGuestHeapBase(uint32_t guestBase) const
{
    uint32_t normalized = guestBase;
    if (normalized >= PS2_RAM_SIZE)
    {
        normalized &= PS2_RAM_MASK;
    }
    const uint32_t hardLimit = std::min(kGuestHeapHardLimit, PS2_RAM_SIZE);
    return std::min(normalized, hardLimit);
}

uint32_t PS2Runtime::clampGuestHeapLimit(uint32_t guestLimit) const
{
    const uint32_t hardLimit = std::min(kGuestHeapHardLimit, PS2_RAM_SIZE);
    if (guestLimit == 0u || guestLimit > hardLimit)
    {
        return hardLimit;
    }
    return guestLimit;
}

void PS2Runtime::resetGuestHeapLocked(uint32_t guestBase, uint32_t guestLimit)
{
    // The game's heap range (guestBase..guestLimit) stays the game's: the runtime allocates from its own arena.
    (void)guestBase;
    (void)guestLimit;
    uint32_t base = kRuntimeArenaBase;
    uint32_t limit = std::min(m_runtimeArenaEnd, PS2_RAM_SIZE);

    m_guestHeapBlocks.clear();
    if (limit > base)
    {
        m_guestHeapBlocks.push_back({base, limit - base, true});
    }

    m_guestHeapBase = base;
    m_guestHeapEnd = base;
    m_guestHeapLimit = limit;
    m_guestHeapConfigured = true;
}

void PS2Runtime::ensureGuestHeapInitializedLocked()
{
    if (m_guestHeapConfigured)
    {
        return;
    }

    const uint32_t suggested = (m_guestHeapSuggestedBase == 0u) ? kGuestHeapDefaultBase : m_guestHeapSuggestedBase;
    resetGuestHeapLocked(suggested, clampGuestHeapLimit(0u));
}

int32_t PS2Runtime::findGuestHeapBlockIndexLocked(uint32_t guestAddr) const
{
    const uint32_t normalizedAddr = guestAddr & PS2_RAM_MASK;
    for (size_t i = 0; i < m_guestHeapBlocks.size(); ++i)
    {
        const GuestHeapBlock &block = m_guestHeapBlocks[i];
        if (!block.free && block.addr == normalizedAddr)
        {
            return static_cast<int32_t>(i);
        }
    }
    return -1;
}

uint32_t PS2Runtime::allocateGuestBlockLocked(uint32_t size, uint32_t alignment)
{
    if (size == 0u)
    {
        return 0u;
    }

    const uint32_t normalizedAlignment = normalizeGuestHeapAlignment(alignment);
    if (size > (std::numeric_limits<uint32_t>::max() - (kGuestHeapDefaultAlignment - 1u)))
    {
        return 0u;
    }

    const uint32_t allocSize = alignGuestHeapValue(size, kGuestHeapDefaultAlignment);
    if (allocSize == 0u)
    {
        return 0u;
    }

    for (size_t i = 0; i < m_guestHeapBlocks.size(); ++i)
    {
        const GuestHeapBlock block = m_guestHeapBlocks[i];
        if (!block.free)
        {
            continue;
        }

        const uint64_t blockStart = block.addr;
        const uint64_t blockEnd = blockStart + static_cast<uint64_t>(block.size);
        const uint32_t alignedAddr = alignGuestHeapValue(block.addr, normalizedAlignment);
        if (alignedAddr < block.addr)
        {
            continue;
        }

        const uint64_t alignedStart = alignedAddr;
        if (alignedStart > blockEnd)
        {
            continue;
        }

        const uint64_t allocEnd = alignedStart + static_cast<uint64_t>(allocSize);
        if (allocEnd > blockEnd)
        {
            continue;
        }

        const uint32_t prefixSize = static_cast<uint32_t>(alignedStart - blockStart);
        const uint32_t suffixSize = static_cast<uint32_t>(blockEnd - allocEnd);

        std::vector<GuestHeapBlock> replacement;
        replacement.reserve(3);
        if (prefixSize > 0u)
        {
            replacement.push_back({block.addr, prefixSize, true});
        }
        replacement.push_back({alignedAddr, allocSize, false});
        if (suffixSize > 0u)
        {
            replacement.push_back({static_cast<uint32_t>(allocEnd), suffixSize, true});
        }

        m_guestHeapBlocks.erase(m_guestHeapBlocks.begin() + static_cast<std::ptrdiff_t>(i));
        m_guestHeapBlocks.insert(m_guestHeapBlocks.begin() + static_cast<std::ptrdiff_t>(i),
                                 replacement.begin(),
                                 replacement.end());

        m_guestHeapEnd = std::max(m_guestHeapEnd, static_cast<uint32_t>(allocEnd));
        return alignedAddr;
    }

    return 0u;
}

void PS2Runtime::coalesceGuestHeapLocked()
{
    if (m_guestHeapBlocks.empty())
    {
        return;
    }

    size_t i = 1;
    while (i < m_guestHeapBlocks.size())
    {
        GuestHeapBlock &prev = m_guestHeapBlocks[i - 1];
        GuestHeapBlock &curr = m_guestHeapBlocks[i];
        const uint64_t prevEnd = static_cast<uint64_t>(prev.addr) + static_cast<uint64_t>(prev.size);
        if (prev.free && curr.free && prevEnd == curr.addr)
        {
            prev.size += curr.size;
            m_guestHeapBlocks.erase(m_guestHeapBlocks.begin() + static_cast<std::ptrdiff_t>(i));
            continue;
        }
        ++i;
    }
}

void PS2Runtime::freeGuestBlockLocked(uint32_t guestAddr)
{
    const int32_t index = findGuestHeapBlockIndexLocked(guestAddr);
    if (index < 0)
    {
        return;
    }

    m_guestHeapBlocks[static_cast<size_t>(index)].free = true;
    coalesceGuestHeapLocked();
}

void PS2Runtime::configureGuestHeap(uint32_t guestBase, uint32_t guestLimit)
{
    std::lock_guard<std::mutex> lock(m_guestHeapMutex);
    uint32_t normalizedBase = alignGuestHeapValue(clampGuestHeapBase(guestBase), kGuestHeapDefaultAlignment);
    if (normalizedBase == 0u)
    {
        normalizedBase = (m_guestHeapSuggestedBase != 0u) ? m_guestHeapSuggestedBase : kGuestHeapDefaultBase;
    }
    m_guestHeapSuggestedBase = normalizedBase;
    resetGuestHeapLocked(normalizedBase, guestLimit);
}

uint32_t PS2Runtime::guestMalloc(uint32_t size, uint32_t alignment)
{
    std::lock_guard<std::mutex> lock(m_guestHeapMutex);
    ensureGuestHeapInitializedLocked();
    return allocateGuestBlockLocked(size, alignment);
}

uint32_t PS2Runtime::guestCalloc(uint32_t count, uint32_t size, uint32_t alignment)
{
    if (count == 0u || size == 0u)
    {
        return 0u;
    }
    if (count > (std::numeric_limits<uint32_t>::max() / size))
    {
        return 0u;
    }

    const uint32_t totalSize = count * size;
    const uint32_t guestAddr = guestMalloc(totalSize, alignment);
    if (guestAddr != 0u)
    {
        uint8_t *rdram = m_memory.getRDRAM();
        if (rdram)
        {
            uint32_t physAddr = guestAddr & PS2_RAM_MASK;
            if (physAddr + totalSize <= PS2_RAM_SIZE)
                std::memset(rdram + physAddr, 0, totalSize);
        }
    }

    return guestAddr;
}

uint32_t PS2Runtime::guestRealloc(uint32_t guestAddr, uint32_t newSize, uint32_t alignment)
{
    if (guestAddr == 0u)
    {
        return guestMalloc(newSize, alignment);
    }
    if (newSize == 0u)
    {
        guestFree(guestAddr);
        return 0u;
    }

    if (newSize > (std::numeric_limits<uint32_t>::max() - (kGuestHeapDefaultAlignment - 1u)))
    {
        return 0u;
    }

    const uint32_t normalizedAlignment = normalizeGuestHeapAlignment(alignment);
    const uint32_t requestedSize = alignGuestHeapValue(newSize, kGuestHeapDefaultAlignment);

    std::lock_guard<std::mutex> lock(m_guestHeapMutex);
    ensureGuestHeapInitializedLocked();

    const int32_t index = findGuestHeapBlockIndexLocked(guestAddr);
    if (index < 0)
    {
        return 0u;
    }

    const size_t blockIndex = static_cast<size_t>(index);
    const uint32_t oldAddr = m_guestHeapBlocks[blockIndex].addr;
    const uint32_t oldSize = m_guestHeapBlocks[blockIndex].size;

    if (requestedSize <= oldSize)
    {
        if (requestedSize < oldSize)
        {
            const uint32_t tailAddr = oldAddr + requestedSize;
            const uint32_t tailSize = oldSize - requestedSize;
            m_guestHeapBlocks[blockIndex].size = requestedSize;
            m_guestHeapBlocks.insert(m_guestHeapBlocks.begin() + static_cast<std::ptrdiff_t>(blockIndex + 1u),
                                     GuestHeapBlock{tailAddr, tailSize, true});
            coalesceGuestHeapLocked();
        }
        return oldAddr;
    }

    if (blockIndex + 1u < m_guestHeapBlocks.size())
    {
        GuestHeapBlock &next = m_guestHeapBlocks[blockIndex + 1u];
        const uint64_t blockEnd = static_cast<uint64_t>(m_guestHeapBlocks[blockIndex].addr) +
                                  static_cast<uint64_t>(m_guestHeapBlocks[blockIndex].size);
        if (next.free && blockEnd == next.addr)
        {
            const uint64_t combined = static_cast<uint64_t>(m_guestHeapBlocks[blockIndex].size) +
                                      static_cast<uint64_t>(next.size);
            if (combined >= requestedSize)
            {
                const uint32_t extraNeeded = requestedSize - m_guestHeapBlocks[blockIndex].size;
                m_guestHeapBlocks[blockIndex].size = requestedSize;
                if (next.size == extraNeeded)
                {
                    m_guestHeapBlocks.erase(m_guestHeapBlocks.begin() + static_cast<std::ptrdiff_t>(blockIndex + 1u));
                }
                else
                {
                    next.addr += extraNeeded;
                    next.size -= extraNeeded;
                }
                m_guestHeapEnd = std::max(m_guestHeapEnd, oldAddr + requestedSize);
                return oldAddr;
            }
        }
    }

    const uint32_t newAddr = allocateGuestBlockLocked(newSize, normalizedAlignment);
    if (newAddr == 0u)
    {
        return 0u;
    }

    uint8_t *rdram = m_memory.getRDRAM();
    if (rdram)
    {
        const uint32_t copyBytes = std::min(oldSize, newSize);
        uint32_t dstPhys = newAddr & PS2_RAM_MASK;
        uint32_t srcPhys = oldAddr & PS2_RAM_MASK;
        if (dstPhys + copyBytes <= PS2_RAM_SIZE && srcPhys + copyBytes <= PS2_RAM_SIZE)
            std::memmove(rdram + dstPhys, rdram + srcPhys, copyBytes);
    }

    freeGuestBlockLocked(oldAddr);
    return newAddr;
}

void PS2Runtime::guestFree(uint32_t guestAddr)
{
    if (guestAddr == 0u)
    {
        return;
    }

    std::lock_guard<std::mutex> lock(m_guestHeapMutex);
    ensureGuestHeapInitializedLocked();
    freeGuestBlockLocked(guestAddr);
}

uint32_t PS2Runtime::guestHeapBase() const
{
    std::lock_guard<std::mutex> lock(m_guestHeapMutex);
    return m_guestHeapConfigured ? m_guestHeapBase : m_guestHeapSuggestedBase;
}

uint32_t PS2Runtime::guestHeapEnd() const
{
    std::lock_guard<std::mutex> lock(m_guestHeapMutex);
    return m_guestHeapConfigured ? m_guestHeapEnd : m_guestHeapSuggestedBase;
}

uint32_t PS2Runtime::guestHeapLimit() const
{
    std::lock_guard<std::mutex> lock(m_guestHeapMutex);
    return m_guestHeapConfigured ? m_guestHeapLimit : m_guestHeapSuggestedBase;
}

uint32_t PS2Runtime::reserveAsyncCallbackStack(uint32_t size, uint32_t alignment)
{
    if (size == 0u)
    {
        return 0u;
    }

    const uint32_t normalizedAlignment = normalizeGuestHeapAlignment(alignment);
    const uint32_t allocSize = alignGuestHeapValue(size, kGuestHeapDefaultAlignment);
    if (allocSize == 0u)
    {
        return 0u;
    }

    std::lock_guard<std::mutex> lock(m_asyncCallbackStackMutex);
    uint32_t top = m_asyncCallbackStackTop;
    if (top > PS2_RAM_SIZE)
    {
        top = PS2_RAM_SIZE;
    }
    top &= ~(kGuestHeapDefaultAlignment - 1u);

    if (top <= allocSize)
    {
        return 0u;
    }

    uint32_t base = top - allocSize;
    base &= ~(normalizedAlignment - 1u);
    if (base < m_asyncCallbackStackFloor || base >= top)
    {
        return 0u;
    }

    m_asyncCallbackStackTop = base;
    return top - 0x10u;
}

bool PS2Runtime::shrinkRuntimeArena(uint32_t limit)
{
    std::lock_guard<std::mutex> lock(m_guestHeapMutex);
    if (!m_guestHeapConfigured)
    {
        m_runtimeArenaEnd = limit;
        return true;
    }
    for (const GuestHeapBlock &block : m_guestHeapBlocks)
    {
        if (!block.free && block.addr + block.size > limit)
        {
            return false;
        }
    }
    std::vector<GuestHeapBlock> kept;
    for (GuestHeapBlock block : m_guestHeapBlocks)
    {
        if (block.addr >= limit)
        {
            continue;
        }
        block.size = std::min(block.size, limit - block.addr);
        kept.push_back(block);
    }
    m_guestHeapBlocks = std::move(kept);
    m_runtimeArenaEnd = limit;
    m_guestHeapLimit = std::min(m_guestHeapLimit, limit);
    m_guestHeapEnd = std::min(m_guestHeapEnd, limit);
    return true;
}

void PS2Runtime::reserveGuestStackRegion(uint32_t stackBase, uint32_t stackTop)
{
    if (stackBase >= stackTop || stackBase >= PS2_RAM_SIZE)
    {
        return;
    }

    std::lock_guard<std::mutex> lock(m_asyncCallbackStackMutex);
    // Only relocate when the guest stack overlaps the callback stack area.
    if (stackTop <= m_asyncCallbackStackFloor || stackBase >= m_asyncCallbackStackTop)
    {
        return;
    }
    const uint32_t newTop = stackBase & ~(kGuestHeapDefaultAlignment - 1u);
    if (newTop > m_asyncCallbackStackFloor)
    {
        m_asyncCallbackStackTop = newTop;
    }
    else if (newTop > kRuntimeArenaBase + (PS2_RAM_SIZE - kRuntimeArenaEnd) &&
             shrinkRuntimeArena(newTop - (PS2_RAM_SIZE - kRuntimeArenaEnd)))
    {
        // Top-of-RAM stack (SetupThread stack=-1): the callback stacks and the runtime arena's end move below it.
        m_asyncCallbackStackTop = newTop;
        m_asyncCallbackStackFloor = newTop - (PS2_RAM_SIZE - kRuntimeArenaEnd);
        std::cerr << "[runtime] guest stack 0x" << std::hex << stackBase << "-0x" << stackTop
                  << ": callback stacks 0x" << m_asyncCallbackStackFloor << "-0x" << m_asyncCallbackStackTop
                  << ", arena end 0x" << m_asyncCallbackStackFloor << std::dec << std::endl;
    }
    else if (stackTop <= PS2_RAM_SIZE)
    {
        // No room above the floor: fall back to the region above the guest stack, if any.
        m_asyncCallbackStackFloor = std::min(stackTop, PS2_RAM_SIZE);
        m_asyncCallbackStackTop = PS2_RAM_SIZE;
    }
}

uint8_t PS2Runtime::Load8(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr)
{
    try
    {
        return m_memory.read8(vaddr);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_LOAD);
        return 0;
    }
}

uint16_t PS2Runtime::Load16(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr)
{
    try
    {
        return m_memory.read16(vaddr);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_LOAD);
        return 0;
    }
}

uint32_t PS2Runtime::Load32(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr)
{
    try
    {
        return m_memory.read32(vaddr);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_LOAD);
        return 0;
    }
}

uint64_t PS2Runtime::Load64(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr)
{
    try
    {
        return m_memory.read64(vaddr);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_LOAD);
        return 0;
    }
}

__m128i PS2Runtime::Load128(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr)
{
    vaddr &= ~0xFu; // quadword accesses are always aligned
    try
    {
        return m_memory.read128(vaddr);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_LOAD);
        return _mm_setzero_si128();
    }
}

void PS2Runtime::Store8(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr, uint8_t value)
{
    ps2TraceGuestWrite(rdram, vaddr, 1u, value, 0u, "WRITE8", ctx);
    try
    {
        m_memory.write8(vaddr, value);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_STORE);
    }
}

void PS2Runtime::Store16(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr, uint16_t value)
{
    ps2TraceGuestWrite(rdram, vaddr, 2u, value, 0u, "WRITE16", ctx);
    try
    {
        m_memory.write16(vaddr, value);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_STORE);
    }
}

void PS2Runtime::Store32(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr, uint32_t value)
{
    ps2TraceGuestWrite(rdram, vaddr, 4u, value, 0u, "WRITE32", ctx);
    try
    {
        m_memory.write32(vaddr, value);
        drainCompletedDmacHandlers(rdram);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_STORE);
    }
}

void PS2Runtime::Store64(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr, uint64_t value)
{
    ps2TraceGuestWrite(rdram, vaddr, 8u, value, 0u, "WRITE64", ctx);
    try
    {
        m_memory.write64(vaddr, value);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_STORE);
    }
}

void PS2Runtime::Store128(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr, __m128i value)
{
    vaddr &= ~0xFu; // quadword accesses are always aligned
    alignas(16) uint64_t _parts[2];
    _mm_storeu_si128(reinterpret_cast<__m128i *>(_parts), value);
    ps2TraceGuestWrite(rdram, vaddr, 16u, _parts[0], _parts[1], "WRITE128", ctx);
    try
    {
        m_memory.write128(vaddr, value);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_STORE);
    }
}

void PS2Runtime::kickGifDmaChainFromMMIO(uint8_t *rdram,
                                         R5900Context *ctx,
                                         uint32_t dPcrValue,
                                         uint32_t dStatValue,
                                         uint32_t tadr,
                                         uint32_t chcr)
{
    constexpr uint32_t D_PCR = 0x1000E020u;
    constexpr uint32_t D_STAT = 0x1000E010u;
    constexpr uint32_t GIF_TADR = 0x1000A030u;
    constexpr uint32_t GIF_CHCR = 0x1000A000u;

    ps2TraceGuestWrite(rdram, D_PCR, 4u, dPcrValue, 0u, "WRITE32", ctx);
    m_memory.writeIORegister(D_PCR, dPcrValue);
    ps2TraceGuestWrite(rdram, D_STAT, 4u, dStatValue, 0u, "WRITE32", ctx);
    m_memory.writeIORegister(D_STAT, dStatValue);
    ps2TraceGuestWrite(rdram, GIF_TADR, 4u, tadr, 0u, "WRITE32", ctx);
    m_memory.writeIORegister(GIF_TADR, tadr);
    ps2TraceGuestWrite(rdram, GIF_CHCR, 4u, chcr, 0u, "WRITE32", ctx);
    if (m_memory.tryProcessNativeGifImageUploadChain(m_gs, tadr, chcr))
    {
        drainCompletedDmacHandlers(rdram);
        return;
    }
    if (m_memory.tryProcessNativeGifPackedChain(m_gs, tadr, chcr))
    {
        drainCompletedDmacHandlers(rdram);
        return;
    }
    m_memory.writeIORegister(GIF_CHCR, chcr);
    m_memory.processPendingTransfers();
    drainCompletedDmacHandlers(rdram);
}

void PS2Runtime::requestStop()
{
    m_stopRequested.store(true, std::memory_order_relaxed);
    if (m_eeScheduler)
    {
        m_eeScheduler->requestStop();
    }
}

bool PS2Runtime::isStopRequested() const
{
    return m_stopRequested.load(std::memory_order_relaxed);
}

EeScheduler &PS2Runtime::eeScheduler()
{
    return *m_eeScheduler;
}

const EeScheduler &PS2Runtime::eeScheduler() const
{
    return *m_eeScheduler;
}

void PS2Runtime::postEeEvent(EeEvent event)
{
    m_eeScheduler->postEvent(event);
}

bool PS2Runtime::eeCheckpointDue(uint32_t cycles) noexcept
{
    const bool due = m_eeScheduler->checkpointDue(cycles);
    if (due)
        ++s_guestYieldGeneration;
    return due;
}

[[noreturn]] void PS2Runtime::eeWaitVSyncTicks(uint32_t ticks, uint32_t resumePc)
{
    const uint64_t currentTick = m_eeScheduler->currentVSyncTick();
    const uint64_t waitTicks = std::max<uint64_t>(1u, ticks);
    m_eeScheduler->waitVSync(currentTick + waitTicks - 1u,
                             0,
                             [resumePc](R5900Context &context)
                             {
                                 context.pc = resumePc;
                             });
}

void PS2Runtime::addEeExitHandler(int threadId, uint32_t function, uint32_t argument)
{
    std::lock_guard lock(m_eeKernelStateMutex);
    m_eeExitHandlers[threadId].push_back({function, argument});
}

std::vector<PS2Runtime::EeExitHandlerRegistration> PS2Runtime::takeEeExitHandlers(int threadId)
{
    std::lock_guard lock(m_eeKernelStateMutex);
    auto it = m_eeExitHandlers.find(threadId);
    if (it == m_eeExitHandlers.end())
    {
        return {};
    }
    auto handlers = std::move(it->second);
    m_eeExitHandlers.erase(it);
    return handlers;
}

void PS2Runtime::removeEeExitHandlers(int threadId)
{
    std::lock_guard lock(m_eeKernelStateMutex);
    m_eeExitHandlers.erase(threadId);
}

bool PS2Runtime::findEeSyscallOverride(uint32_t syscallNumber, uint32_t &handler) const
{
    std::lock_guard lock(m_eeKernelStateMutex);
    const auto it = m_eeSyscallOverrides.find(syscallNumber);
    if (it == m_eeSyscallOverrides.end())
    {
        return false;
    }
    handler = it->second;
    return true;
}

void PS2Runtime::setEeSyscallOverride(uint8_t *rdram, uint32_t syscallNumber, uint32_t handler)
{
    constexpr uint32_t kTableBase = 0x80011F80u & 0x1FFFFFFFu;
    constexpr uint32_t kMirrorLimit = 0x00080000u;
    const int64_t offset = static_cast<int64_t>(static_cast<int32_t>(syscallNumber)) * 4;
    const int64_t address = static_cast<int64_t>(kTableBase) + offset;

    std::lock_guard lock(m_eeKernelStateMutex);
    if (handler == 0u)
    {
        m_eeSyscallOverrides.erase(syscallNumber);
    }
    else
    {
        m_eeSyscallOverrides[syscallNumber] = handler;
    }
    if (!rdram || address < 0 || address + 4 > kMirrorLimit)
    {
        return;
    }
    const uint32_t guestAddress = static_cast<uint32_t>(address);
    std::memcpy(rdram + guestAddress, &handler, sizeof(handler));
    if (handler == 0u)
    {
        m_eeSyscallMirrorAddresses.erase(guestAddress);
    }
    else
    {
        m_eeSyscallMirrorAddresses.insert(guestAddress);
    }
}

void PS2Runtime::initializeEeKernelState(uint8_t *rdram)
{
    if (!rdram)
    {
        return;
    }
    constexpr uint32_t kTableGuestBase = 0x80011F80u;
    constexpr uint32_t kTableBase = kTableGuestBase & 0x1FFFFFFFu;
    constexpr uint32_t kMirrorLimit = 0x00080000u;
    constexpr uint32_t kProbeBase = 0x000002F0u;

    std::lock_guard lock(m_eeKernelStateMutex);
    for (const uint32_t address : m_eeSyscallMirrorAddresses)
    {
        const uint32_t zero = 0u;
        std::memcpy(rdram + address, &zero, sizeof(zero));
    }
    m_eeSyscallMirrorAddresses.clear();
    const uint32_t high = kTableGuestBase >> 16;
    const uint32_t low = kTableGuestBase & 0xFFFFu;
    std::memcpy(rdram + kProbeBase, &high, sizeof(high));
    std::memcpy(rdram + kProbeBase + 8u, &low, sizeof(low));
    m_eeSyscallMirrorAddresses.insert(kProbeBase);
    m_eeSyscallMirrorAddresses.insert(kProbeBase + 8u);

    for (const auto &[syscallNumber, handler] : m_eeSyscallOverrides)
    {
        const int64_t offset = static_cast<int64_t>(static_cast<int32_t>(syscallNumber)) * 4;
        const int64_t address = static_cast<int64_t>(kTableBase) + offset;
        if (address < 0 || address + 4 > kMirrorLimit)
        {
            continue;
        }
        const uint32_t guestAddress = static_cast<uint32_t>(address);
        std::memcpy(rdram + guestAddress, &handler, sizeof(handler));
        m_eeSyscallMirrorAddresses.insert(guestAddress);
    }
}

void PS2Runtime::HandleIntegerOverflow(R5900Context *ctx)
{
    raiseCop0Exception(ctx, EXCEPTION_INTEGER_OVERFLOW);
}

void PS2Runtime::run()
{
#if PS2X_ENABLE_CODWAW_SUPPLEMENTS
    // Final Fronts: a leaf routine omitted by the original function discovery.
    // Bind only when its original instruction fingerprint matches the loaded ELF.
    uint8_t *bootRam = m_memory.getRDRAM();
    if (!hasFunction(0x447328u) &&
        Ps2FastRead32(bootRam, 0x447328u) == 0x0080302du &&
        Ps2FastRead32(bootRam, 0x44732cu) == 0x8cc30008u &&
        Ps2FastRead32(bootRam, 0x447330u) == 0x18600008u)
    {
        registerFunction(0x447328u, [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            SET_GPR_U64(ctx, 6, GPR_U64(ctx, 4));
            SET_GPR_S32(ctx, 3, static_cast<int32_t>(READ32(GPR_U32(ctx, 6) + 8u)));
            SET_GPR_S32(ctx, 2, -1);
            if (GPR_S64(ctx, 3) > 0)
            {
                const bool skip = GPR_S64(ctx, 5) == -1;
                SET_GPR_S32(ctx, 2, static_cast<int32_t>(GPR_U32(ctx, 3) - 1u));
                if (!skip)
                {
                    SET_GPR_S32(ctx, 4, static_cast<int32_t>(READ32(GPR_U32(ctx, 6))));
                    SET_GPR_S32(ctx, 3, static_cast<int32_t>(GPR_U32(ctx, 2) << 2u));
                    WRITE32(GPR_U32(ctx, 6) + 8u, GPR_U32(ctx, 2));
                    SET_GPR_S32(ctx, 3, static_cast<int32_t>(GPR_U32(ctx, 3) + GPR_U32(ctx, 4)));
                    WRITE32(GPR_U32(ctx, 3), GPR_U32(ctx, 5));
                }
            }
            ctx->pc = GPR_U32(ctx, 31);
        });
    }
    if (!hasFunction(0x1d03f0u) &&
        Ps2FastRead32(bootRam, 0x1d03f0u) == 0x8c820018u &&
        Ps2FastRead32(bootRam, 0x1d03f4u) == 0x2403ffffu &&
        Ps2FastRead32(bootRam, 0x1d0414u) == 0xac800008u)
    {
        registerFunction(0x1d03f0u, [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t object = GPR_U32(ctx, 4);
            SET_GPR_S32(ctx, 2, static_cast<int32_t>(READ32(object + 24u)));
            SET_GPR_S32(ctx, 3, -1);
            const bool invalid = GPR_S64(ctx, 2) == -1;
            SET_GPR_S32(ctx, 2, -2);
            if (!invalid) WRITE32(object + 24u, GPR_U32(ctx, 2));
            WRITE32(object + 32u, GPR_U32(ctx, 3));
            WRITE32(object + 12u, 0u);
            WRITE8(object + 4u, 0u);
            WRITE32(object + 8u, 0u);
            ctx->pc = GPR_U32(ctx, 31);
        });
    }
    if (!hasFunction(0x1b5cc8u) &&
        Ps2FastRead32(bootRam, 0x1b5cc8u) == 0x3c013f00u &&
        Ps2FastRead32(bootRam, 0x1b5cf4u) == 0x00a0302du)
    {
        // Missing prologue; the rest of this routine is already translated.
        registerFunction(0x1b5cc8u, [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            SET_GPR_S32(ctx, 1, 0x3f000000);
            ctx->f[2] = 0.5f;
            SET_GPR_S32(ctx, 4, static_cast<int32_t>(GPR_U32(ctx, 4) + 28u));
            WRITE32(GPR_U32(ctx, 5) + 4u, 0u);
            SET_GPR_U64(ctx, 7, 0u);
            WRITE32(GPR_U32(ctx, 5), 0u);
            SET_GPR_S32(ctx, 10, 2);
            SET_GPR_S32(ctx, 1, 0x3f800000);
            ctx->f[1] = 1.0f;
            SET_GPR_S32(ctx, 9, 1);
            SET_GPR_S32(ctx, 8, 3);
            SET_GPR_U64(ctx, 6, GPR_U64(ctx, 5));
            ctx->pc = 0x1b5cf8u;
        });
    }
    if (!hasFunction(0x1d05d8u) &&
        Ps2FastRead32(bootRam, 0x1d05d8u) == 0x3c020058u &&
        Ps2FastRead32(bootRam, 0x1d05ecu) == 0x46010069u)
    {
        registerFunction(0x1d05d8u, [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t object = GPR_U32(ctx, 4);
            const auto loadFloat = [&](uint32_t address) { const uint32_t bits = READ32(address); float value; std::memcpy(&value, &bits, 4); return value; };
            const auto storeFloat = [&](uint32_t address, float value) { uint32_t bits; std::memcpy(&bits, &value, 4); WRITE32(address, bits); };
            SET_GPR_S32(ctx, 2, 0x580000);
            ctx->f[0] = loadFloat(object + 12u);
            ctx->f[1] = FPU_CVT_S_W(static_cast<int32_t>(READ32(0x583160u)));
            ctx->f[2] = loadFloat(object + 8u);
            ctx->f[1] = std::min(ctx->f[0], ctx->f[1]);
            const bool less = FPU_C_OLT_S(ctx->f[1], ctx->f[2]);
            ctx->fcr31 = less ? (ctx->fcr31 | 0x800000u) : (ctx->fcr31 & ~0x800000u);
            ctx->f[3] = loadFloat(0x583b30u);
            storeFloat(object + 12u, ctx->f[1]);
            if (!less) { ctx->pc = 0x1d0624u; return; }
            SET_GPR_S32(ctx, 1, 0x44480000);
            ctx->f[0] = 800.0f;
            ctx->f[0] = FPU_MUL_S(ctx->f[3], ctx->f[0]);
            ctx->f[0] = FPU_SUB_S(ctx->f[2], ctx->f[0]);
            ctx->f[0] = std::max(ctx->f[1], ctx->f[0]);
            storeFloat(object + 8u, ctx->f[0]);
            ctx->pc = 0x1d0650u;
        });
    }
    if (!hasFunction(0x1c98e8u) &&
        Ps2FastRead32(bootRam, 0x1c98e8u) == 0x8c820008u &&
        Ps2FastRead32(bootRam, 0x1c9900u) == 0xac820008u)
    {
        registerFunction(0x1c98e8u, [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t object = GPR_U32(ctx, 4);
            SET_GPR_S32(ctx, 2, static_cast<int32_t>(READ32(object + 8u)));
            SET_GPR_S32(ctx, 3, -2);
            WRITE32(object + 24u, 0u);
            SET_GPR_U64(ctx, 2, GPR_U64(ctx, 2) & GPR_U64(ctx, 3));
            WRITE32(object + 28u, 0u);
            WRITE32(object + 8u, GPR_U32(ctx, 2));
            ctx->pc = GPR_U32(ctx, 31);
        });
    }
    if (!hasFunction(0x1c9688u) &&
        Ps2FastRead32(bootRam, 0x1c9688u) == 0x8c820008u &&
        Ps2FastRead32(bootRam, 0x1c96a0u) == 0xac820008u)
    {
        registerFunction(0x1c9688u, [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t object = GPR_U32(ctx, 4);
            SET_GPR_S32(ctx, 2, static_cast<int32_t>(READ32(object + 8u)));
            SET_GPR_S32(ctx, 3, -2);
            WRITE32(object + 36u, 0u);
            SET_GPR_U64(ctx, 2, GPR_U64(ctx, 2) & GPR_U64(ctx, 3));
            WRITE32(object + 24u, 0u);
            WRITE32(object + 8u, GPR_U32(ctx, 2));
            ctx->pc = GPR_U32(ctx, 31);
        });
    }
    if (!hasFunction(0x1c9388u) &&
        Ps2FastRead32(bootRam, 0x1c9388u) == 0x8c820008u &&
        Ps2FastRead32(bootRam, 0x1c939cu) == 0xac820008u)
    {
        registerFunction(0x1c9388u, [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t object = GPR_U32(ctx, 4);
            SET_GPR_S32(ctx, 2, static_cast<int32_t>(READ32(object + 8u)));
            SET_GPR_S32(ctx, 3, -2);
            WRITE32(object + 20u, 0u);
            SET_GPR_U64(ctx, 2, GPR_U64(ctx, 2) & GPR_U64(ctx, 3));
            WRITE32(object + 8u, GPR_U32(ctx, 2));
            ctx->pc = GPR_U32(ctx, 31);
        });
    }
    if (!hasFunction(0x1c93f0u) &&
        Ps2FastRead32(bootRam, 0x1c93f0u) == 0x0080402du &&
        Ps2FastRead32(bootRam, 0x1c93f4u) == 0x3c020058u)
        registerFunction(0x1c93f0u, sub_001C93F0_0x1c93f0);
    if (!hasFunction(0x1c90b0u) &&
        Ps2FastRead32(bootRam, 0x1c90b0u) == 0x27bdffd0u &&
        Ps2FastRead32(bootRam, 0x1c90b4u) == 0x2403fffeu) {
        registerFunction(0x1c90b0u, sub_001C90B0_0x1c90b0);
        registerFunction(0x1c9364u, sub_001C90B0_0x1c90b0);
    }
    if (!hasFunction(0x455418u) &&
        Ps2FastRead32(bootRam, 0x455418u) == 0x03e00008u &&
        Ps2FastRead32(bootRam, 0x45541cu) == 0x24020001u) {
        registerFunction(0x455418u, [](uint8_t *, R5900Context *ctx, PS2Runtime *) {
            SET_GPR_S32(ctx, 2, 1);
            ctx->pc = GPR_U32(ctx, 31);
        });
    }
    for (uint32_t address : {0x455410u, 0x455428u}) {
        if (!hasFunction(address) &&
            Ps2FastRead32(bootRam, address) == 0x03e00008u &&
            Ps2FastRead32(bootRam, address + 4u) == 0x0000102du)
            registerFunction(address, [](uint8_t *, R5900Context *ctx, PS2Runtime *) {
                SET_GPR_U64(ctx, 2, 0);
                ctx->pc = GPR_U32(ctx, 31);
            });
    }
    if (!hasFunction(0x455420u) &&
        Ps2FastRead32(bootRam, 0x455420u) == 0x03e00008u &&
        Ps2FastRead32(bootRam, 0x455424u) == 0x9082000cu)
        registerFunction(0x455420u, [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            const uint32_t target = GPR_U32(ctx, 31);
            SET_GPR_U64(ctx, 2, READ8(GPR_U32(ctx, 4) + 12u));
            ctx->pc = target;
        });
    if (!hasFunction(0x4553e8u) &&
        Ps2FastRead32(bootRam, 0x4553e8u) == 0xc4810008u &&
        Ps2FastRead32(bootRam, 0x455408u) == 0xe4a20004u)
        registerFunction(0x4553e8u, [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            uint32_t bits = READ32(GPR_U32(ctx, 4) + 8u);
            std::memcpy(&ctx->f[1], &bits, 4);
            bits = READ32(GPR_U32(ctx, 5));
            std::memcpy(&ctx->f[0], &bits, 4);
            bits = READ32(GPR_U32(ctx, 5) + 4u);
            std::memcpy(&ctx->f[2], &bits, 4);
            ctx->f[0] = FPU_MUL_S(ctx->f[0], ctx->f[1]);
            std::memcpy(&bits, &ctx->f[0], 4);
            WRITE32(GPR_U32(ctx, 5), bits);
            bits = READ32(GPR_U32(ctx, 4) + 12u);
            std::memcpy(&ctx->f[1], &bits, 4);
            ctx->f[2] = FPU_MUL_S(ctx->f[2], ctx->f[1]);
            const uint32_t target = GPR_U32(ctx, 31);
            std::memcpy(&bits, &ctx->f[2], 4);
            WRITE32(GPR_U32(ctx, 5) + 4u, bits);
            ctx->pc = target;
        });
    if (!hasFunction(0x4496b8u) &&
        Ps2FastRead32(bootRam, 0x4496b8u) == 0x8c870008u &&
        Ps2FastRead32(bootRam, 0x4496bcu) == 0x18e00017u) {
        registerFunction(0x4496b8u, sub_004496B8_0x4496b8);
        registerFunction(0x4496c8u, sub_004496B8_0x4496b8);
    }
    if (Ps2FastRead32(bootRam, 0x139fb0u) == 0x8c870020u &&
        Ps2FastRead32(bootRam, 0x139fd8u) == 0x03e00008u)
        register139fb0Supplement(*this);
    if (Ps2FastRead32(bootRam, 0x159fc8u) == 0x27bdffc0u &&
        Ps2FastRead32(bootRam, 0x15a218u) == 0x03e00008u)
        register159fc8Supplement(*this);
    // Every .text range the function map missed, recompiled from the original ELF.
    if (Ps2FastRead32(bootRam, 0x4532e0u) == 0x03e00008u &&
        Ps2FastRead32(bootRam, 0x4532e4u) == 0x0080102du)
        registerCodeGapsSupplement(*this);
    // Diagnostic timing experiment, not an instruction-accurate replacement.
    // Keep normal execution unchanged unless explicitly selected for a boot test.
    // The engine runs on measured frame time (com_maxfps at 0x583B50): only this cap keeps it at 30 fps.
    if (Ps2FastRead32(bootRam, 0x14b6c8u) == 0x3c020058u && Ps2FastRead32(bootRam, 0x14b6d0u) == 0xc4403b40u)
    {
        registerFunction(0x14b6c8u, [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *) {
            PS2TL(TL_GAME_FRAME, getRegU32(ctx, 31));
            static const char *setting = std::getenv("PS2X_COD_MAXFPS");
            static const float period = 1.0f / static_cast<float>(setting ? std::atof(setting) : 60.0);
            float fixed;
            std::memcpy(&fixed, rdram + 0x583b40u, 4);
            ctx->f[0] = fixed > 0.0f ? fixed : period;
            ctx->pc = getRegU32(ctx, 31);
        });
    }
    // sceGsSyncV at the end of the frame (0x259650) rounds every frame up to a multiple of the VBlank: a 20 ms frame took
    // 33 ms. The engine's own limiter above paces it; the wrapper keeps reading the field and the swap follows as usual.
    static RecompiledFunction codSyncV = nullptr;
    if (!std::getenv("PS2X_COD_VSYNC"))
    {
        codSyncV = lookupFunction(0x424fd8u);
        registerFunction(0x424fd8u, [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            if (getRegU32(ctx, 31) != 0x259658u)
            {
                codSyncV(rdram, ctx, runtime);
                return;
            }
            setReturnS32(ctx, static_cast<int32_t>(runtime->eeScheduler().currentVSyncTick() & 1u));
            ctx->pc = 0x259658u;
        });
    }
    const char *fastFrameWait = std::getenv("PS2X_FAST_FRAME_WAIT");
    if (fastFrameWait && std::string(fastFrameWait) == "1" &&
        Ps2FastRead32(bootRam, 0x14b380u) == 0x0c0908a2u &&
        Ps2FastRead32(bootRam, 0x14b398u) == 0x4501fff9u) {
        registerFunction(0x14b380u, [](uint8_t *, R5900Context *ctx, PS2Runtime *runtime) {
            runtime->eeScheduler().accountCycles(294912u);
            SET_GPR_U32(ctx, 31, 0x14b388u);
            ctx->pc = 0x242288u;
        });
    }
#endif
#if AGRESSIVE_LOGS
    // Per-function flushes can dominate boot time. Retain explicit opt-in tracing.
    const char *functionTrace = std::getenv("PS2X_FUNCTION_TRACE");
    if (!functionTrace || std::string(functionTrace) != "1")
        ps2_log::log_stream().setstate(std::ios::badbit);
#endif
    m_stopRequested.store(false, std::memory_order_relaxed);
    ps2_stubs::resetSifState();
    resetIop();
    ps2_stubs::resetAudioStubState();
    ps2_stubs::resetMpegStubState();
    initializeEeKernelState(m_memory.getRDRAM());
    m_cpuContext.r[4] = _mm_setzero_si128();
    m_cpuContext.r[5] = _mm_setzero_si128();
    m_cpuContext.r[29] = _mm_set_epi64x(0, static_cast<int64_t>(PS2_RAM_SIZE - 0x10u));
    m_debugPc.store(m_cpuContext.pc, std::memory_order_relaxed);
    m_debugRa.store(static_cast<uint32_t>(_mm_extract_epi32(m_cpuContext.r[31], 0)), std::memory_order_relaxed);
    m_debugSp.store(static_cast<uint32_t>(_mm_extract_epi32(m_cpuContext.r[29], 0)), std::memory_order_relaxed);
    m_debugGp.store(static_cast<uint32_t>(_mm_extract_epi32(m_cpuContext.r[28], 0)), std::memory_order_relaxed);

    RUNTIME_LOG("Starting execution at address 0x" << std::hex << m_cpuContext.pc << std::dec);

    // A blank image to use as a framebuffer
    Image blank = GenImageColor(FB_WIDTH, FB_HEIGHT, BLANK);
    Texture2D frameTex = LoadTextureFromImage(blank);
    const unsigned ownFrameTexture = frameTex.id;
    UnloadImage(blank);

    std::atomic<bool> gameThreadFinished{false};

#if defined(__linux__) && !defined(__ANDROID__)
    // Windows builds link the runner with a large /STACK (unoptimized generated code uses big native frames and
    // deep guest call chains). glibc sizes std::thread stacks from RLIMIT_STACK (usually 8 MiB), so raise the
    // process default before the game thread starts. PS2X_THREAD_STACK_MB overrides it (default 64).
    {
        size_t stackMb = 64u;
        if (const char *env = std::getenv("PS2X_THREAD_STACK_MB"))
            stackMb = std::max<size_t>(8u, static_cast<size_t>(std::strtoul(env, nullptr, 10)));
        pthread_attr_t attr;
        if (pthread_attr_init(&attr) == 0)
        {
            pthread_attr_setstacksize(&attr, stackMb * 1024u * 1024u);
            pthread_setattr_default_np(&attr);
            pthread_attr_destroy(&attr);
        }
    }
#endif

    std::thread gameThread([&]()
                           {
        ThreadNaming::SetCurrentThreadName("GameThread");
        ps2ProfilerRegisterCurrentThread("game");
        {
            static PS2Runtime *watchRuntime = nullptr;
            watchRuntime = this;
            ps2WatchInstall(m_memory.getRDRAM(), []() -> uint32_t {
                R5900Context *context = watchRuntime->eeScheduler().currentContext();
                return context ? context->pc : 0u;
            }, []() {
                R5900Context *context = watchRuntime->eeScheduler().currentContext();
                if (!context)
                    return;
                uint8_t *rdram = watchRuntime->memory().getRDRAM();
                auto word = [&](uint32_t address) { return Ps2FastRead32(rdram, address); };
                auto real = [&](uint32_t address) { float f; uint32_t b = word(address); std::memcpy(&f, &b, 4); return f; };
                const uint32_t s0 = GPR_U32(context, 16), s4 = GPR_U32(context, 20), sp = GPR_U32(context, 29);
                std::fprintf(stderr, "[watch:ctx] s0=0x%x s1=0x%x s4=0x%x sp=0x%x ra=0x%x f20=%g f22=%g\n", s0,
                             GPR_U32(context, 17), s4, sp, GPR_U32(context, 31), context->f[20], context->f[22]);
                std::fprintf(stderr, "[watch:ctx] table[s0]=%g,%g,%g dir[sp]=%g,%g,%g pt[sp+10]=%g,%g,%g table=0x%x count=%d dt=%g\n",
                             real(s0), real(s0 + 4u), real(s0 + 8u), real(sp), real(sp + 4u), real(sp + 8u),
                             real(sp + 0x10u), real(sp + 0x14u), real(sp + 0x18u), word(s4 + 0xDCu), static_cast<int>(word(s4 + 0xD4u)), real(0x584380u));
                std::fprintf(stderr, "[watch:ctx] f0-12=");
                for (int reg = 0; reg <= 12; ++reg)
                    std::fprintf(stderr, "%g%s", context->f[reg], reg == 12 ? "" : ",");
                std::fprintf(stderr, " a0=0x%x a1=0x%x a2=0x%x s2=0x%x\n", GPR_U32(context, 4), GPR_U32(context, 5),
                             GPR_U32(context, 6), GPR_U32(context, 18));
            });
        }
        try
        {
            m_eeScheduler->reset(m_memory.getRDRAM(), m_cpuContext);
            m_eeScheduler->run();
            uint32_t pc = m_debugPc.load(std::memory_order_relaxed);
            RUNTIME_LOG("Game thread returned. PC=0x" << std::hex << pc
                      << " RA=0x" << static_cast<uint32_t>(_mm_extract_epi32(m_cpuContext.r[31], 0)) << std::dec << std::endl);
        }
        catch (const std::exception &e)
        {
            std::cerr << "Error during program execution: " << e.what() << std::endl;
        }
        catch (...)
        {
            std::cerr << "Error during program execution: unknown exception" << std::endl;
        }
        gameThreadFinished.store(true, std::memory_order_release); });

    uint64_t tick = 0;
    while (!isStopRequested() && !gameThreadFinished.load(std::memory_order_acquire))
    {
        PS2_IF_AGRESSIVE_LOGS({
            tick++;
            if ((tick % 120) == 0)
            {
                uint64_t curDma = m_memory.dmaStartCount();
                uint64_t curGif = m_memory.gifCopyCount();
                uint64_t curGs = m_memory.gsWriteCount();
                uint64_t curVif = m_memory.vifWriteCount();
                const GSRegisters &gs = m_memory.gs();
                const uint32_t dbgPc = m_debugPc.load(std::memory_order_relaxed);
                const uint32_t dbgRa = m_debugRa.load(std::memory_order_relaxed);
                const uint32_t dbgSp = m_debugSp.load(std::memory_order_relaxed);
                const uint32_t dbgGp = m_debugGp.load(std::memory_order_relaxed);
                const auto eeSnapshot = m_eeScheduler->snapshot();

                RUNTIME_LOG("[run:tick] tick=" << tick
                                               << " pc=0x" << std::hex << dbgPc
                                               << " ra=0x" << dbgRa
                                               << " sp=0x" << dbgSp
                                               << " gp=0x" << dbgGp
                                               << " dispfb1=0x" << gs.dispfb1
                                               << " display1=0x" << gs.display1
                                               << std::dec
                                               << " activeThreads=" << eeSnapshot.threads.size()
                                               << " dma=" << curDma
                                               << " gif=" << curGif
                                               << " gsw=" << curGs
                                               << " vif=" << curVif
                                               << std::endl);

            }
        });
        host_video::update();
        host_menu::update();
        ps2UpdateHostPadInput();
        uint32_t presentWidth = FB_WIDTH;
        uint32_t presentHeight = DEFAULT_DISPLAY_HEIGHT;
        UploadFrame(frameTex, this, presentWidth, presentHeight);

        BeginDrawing();
        ClearBackground(BLACK);
        const float srcWidth = static_cast<float>(std::max<uint32_t>(1u, presentWidth));
        const float srcHeight = static_cast<float>(std::max<uint32_t>(1u, presentHeight));
        const float screenWidth = static_cast<float>(GetScreenWidth());
        const float screenHeight = static_cast<float>(GetScreenHeight());
        // The PS2 output is shown at 4:3 like a TV; F7 stretches it over the whole window.
        float dstWidth = screenWidth;
        float dstHeight = screenHeight;
        if (!host_video::stretch)
        {
            dstWidth = std::min(screenWidth, screenHeight * 4.0f / 3.0f);
            dstHeight = dstWidth * 3.0f / 4.0f;
        }
        else if (host_video::resolutionWidth > 0)
        {
            // an ultrawide choice on a narrower screen keeps its shape
            const float shape = static_cast<float>(host_video::resolutionWidth) / static_cast<float>(host_video::resolutionHeight);
            dstWidth = std::min(screenWidth, screenHeight * shape);
            dstHeight = dstWidth / shape;
        }
        const Rectangle srcRect{0.0f, 0.0f, srcWidth, srcHeight};
        const Rectangle dstRect{
            (screenWidth - dstWidth) * 0.5f,
            (screenHeight - dstHeight) * 0.5f,
            dstWidth,
            dstHeight};
        {
            static int textureFilter = -1;
            const int wanted = host_menu::filter ? TEXTURE_FILTER_BILINEAR : TEXTURE_FILTER_POINT;
            static unsigned filteredTexture = 0u; // shared frames arrive in several textures
            if (textureFilter != wanted || filteredTexture != frameTex.id)
            {
                SetTextureFilter(frameTex, wanted);
                textureFilter = wanted;
                filteredTexture = frameTex.id;
            }
        }
        host_menu::drawPicture(frameTex, srcRect, dstRect);
        if (m_debugUiInitialized && m_debugUiDrawCallback)
        {
            m_debugUiDrawCallback(*this, m_debugUiUserData);
        }
        host_menu::drawFps(host_video::fps);
        host_menu::draw();
        host_video::drawHelp();
        PS2TL(TL_SWAP_BEGIN);
        EndDrawing();
        PS2TL(TL_SWAP_END);
        ps2TimelinePoll();
        host_video::countFrame(static_cast<uint32_t>(m_memory.gs().dispfb1));
        {
            // Stall diagnostic: when the game stops flipping frames for 8 s, log the
            // EE thread and semaphore states once per stall.
            static uint32_t lastFb = 0;
            static double lastFlip = GetTime();
            static bool reported = false;
            const uint32_t fb = static_cast<uint32_t>(m_memory.gs().dispfb1);
            if (fb != lastFb) { lastFb = fb; lastFlip = GetTime(); reported = false; }
            else if (!reported && GetTime() - lastFlip > 8.0)
            {
                reported = true;
                const auto snap = m_eeScheduler->snapshot();
                std::cerr << "[stall] running=" << snap.runningThreadId << " cycle=" << snap.eeCycle
                          << " nextEvent=" << snap.nextEventCycle << std::endl;
                for (const auto &t : snap.threads)
                    std::cerr << "[stall] thread=" << t.id << " status=" << static_cast<int>(t.status)
                              << " wait=" << static_cast<int>(t.waitReason) << " waitId=" << t.waitId
                              << std::hex << " pc=0x" << t.pc << " ra=0x" << t.ra << " entry=0x" << t.entry
                              << std::dec << " prio=" << t.currentPriority << " depth=" << t.invocationDepth << std::endl;
                for (const auto &s : snap.semaphores)
                    if (s.waiters)
                        std::cerr << "[stall] sema=" << s.id << " count=" << s.count << " waiters=" << s.waiters << std::endl;
                for (const auto &e : snap.eventFlags)
                    if (e.waiters)
                        std::cerr << "[stall] evf=" << e.id << " bits=0x" << std::hex << e.bits << std::dec
                                  << " waiters=" << e.waiters << std::endl;
            }
        }

        if (WindowShouldClose())
        {
            RUNTIME_LOG("[run] window close requested, breaking out of loop");
            requestStop();
            break;
        }
    }

    requestStop();
    if (gameThread.joinable())
    {
        gameThread.join();
    }

    if (m_debugUiInitialized && m_debugUiShutdownCallback)
    {
        m_debugUiShutdownCallback(*this, m_debugUiUserData);
        m_debugUiInitialized = false;
    }
    frameTex.id = ownFrameTexture;
    GSSharedPresent::ShutdownHost();
    UnloadTexture(frameTex);
    CloseWindow();

    RUNTIME_LOG("[run] exiting loop");
}
