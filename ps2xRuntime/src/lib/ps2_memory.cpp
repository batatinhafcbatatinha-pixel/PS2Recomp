#include "ps2_timed_wait.h"
#include "runtime/ps2_memory.h"
#include "ps2_timeline.h"
#include <intrin.h>
#include <condition_variable>
#include <deque>
#include <thread>
#include "runtime/ps2_address.h"
#include "runtime/gs/gs_frontend.h"
#include "ps2_log.h"
#include <atomic>
#include <filesystem>
#include <cstring>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <stdexcept>
#include <algorithm>
#include <string>
#include <vector>
#if defined(_MSC_VER)
#include <intrin.h>
#define PS2X_RETURN_ADDRESS() _ReturnAddress()
#else
#define PS2X_RETURN_ADDRESS() __builtin_return_address(0)
#endif

bool Ps2GifOrderTraceEnabled()
{
    static const char *path = std::getenv("PS2X_GIF_ORDER_TRACE");
    return path && *path;
}

// Shared with ps2_gif_arbiter.cpp so submission and dispatch use one ordered
// diagnostic stream. No guest state, packet bytes or scheduling are changed.
void Ps2TraceGifOrder(const char *stage, unsigned path, const uint8_t *data, uint32_t size,
                      const void *owner, size_t pendingGif, size_t pendingVif, const char *extra)
{
    static const char *tracePath = std::getenv("PS2X_GIF_ORDER_TRACE");
    if (!tracePath || !*tracePath) return;
    static const auto start = std::chrono::steady_clock::now();
    static const double captureAt = std::getenv("PS2X_GIF_ORDER_AT") ? std::atof(std::getenv("PS2X_GIF_ORDER_AT")) : 90.0;
    const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    if (elapsed < captureAt) return;
    static std::mutex mutex;
    std::lock_guard<std::mutex> lock(mutex);
    static uint64_t count = 0;
    if (count >= 16000u) return;
    static FILE *file = std::fopen(tracePath, "w");
    if (!file) return;
    const uint64_t id = ++count;
    uint64_t hash = 14695981039346656037ull;
    for (uint32_t i = 0; data && i < size; ++i) { hash ^= data[i]; hash *= 1099511628211ull; }
    auto word = [&](uint32_t offset) { uint64_t value = 0; std::memcpy(&value, data + offset, 8); return value; };
    const uint64_t headerLo = data && size >= 16u ? word(0) : 0u;
    const uint64_t headerHi = data && size >= 16u ? word(8) : 0u;
    uint64_t tex0[2]{};
    unsigned tex0Writes = 0;
    // Scan only complete GIF packets. Native image payload is raw pixels.
    if (data && std::strcmp(stage, "native-image") != 0)
    {
        uint64_t offset = 0;
        auto reg = [&](unsigned address, uint64_t value) {
            if (address == GS_REG_TEX0_1 || address == GS_REG_TEX0_2) {
                tex0[address == GS_REG_TEX0_2] = value; ++tex0Writes;
            }
        };
        while (offset + 16u <= size)
        {
            const uint64_t lo = word(static_cast<uint32_t>(offset)), hi = word(static_cast<uint32_t>(offset + 8u));
            offset += 16u;
            const unsigned loops = static_cast<unsigned>(lo & 0x7fffu), format = static_cast<unsigned>((lo >> 58u) & 3u);
            unsigned nreg = static_cast<unsigned>(lo >> 60u); if (!nreg) nreg = 16u;
            if (format >= 2u) { offset += static_cast<uint64_t>(loops) * 16u; continue; }
            const unsigned stride = format == 0u ? 16u : 8u;
            const uint64_t elements = static_cast<uint64_t>(loops) * nreg;
            if (elements * stride > size - offset) break;
            for (uint64_t i = 0; i < elements; ++i, offset += stride)
            {
                const unsigned address = static_cast<unsigned>((hi >> ((i % nreg) * 4u)) & 15u);
                const uint64_t value = word(static_cast<uint32_t>(offset));
                if (format == 0u && address == 14u)
                    reg(static_cast<unsigned>(word(static_cast<uint32_t>(offset + 8u)) & 255u), value);
                else reg(address, value);
            }
            if (format == 1u && (elements & 1u)) offset += 8u;
        }
    }
    std::fprintf(file,
        "{\"id\":%llu,\"elapsed\":%.9f,\"stage\":\"%s\",\"path\":%u,\"owner\":\"%p\","
        "\"size\":%u,\"fnv1a64\":\"%016llx\",\"header_lo\":\"%016llx\",\"header_hi\":\"%016llx\","
        "\"tex0_writes\":%u,\"tex0_1\":\"%016llx\",\"tex0_2\":\"%016llx\",\"pending_gif\":%zu,\"pending_vif\":%zu,\"extra\":%s}\n",
        static_cast<unsigned long long>(id), elapsed, stage, path, owner, size, static_cast<unsigned long long>(hash),
        static_cast<unsigned long long>(headerLo), static_cast<unsigned long long>(headerHi), tex0Writes,
        static_cast<unsigned long long>(tex0[0]), static_cast<unsigned long long>(tex0[1]), pendingGif, pendingVif, extra);
    std::fflush(file);
    static const char *dumpDir = std::getenv("PS2X_GIF_ORDER_DUMP_DIR");
    static unsigned dumps = 0;
    static const unsigned maxDumps = std::getenv("PS2X_GIF_ORDER_MAX_DUMPS") ? static_cast<unsigned>(std::atoi(std::getenv("PS2X_GIF_ORDER_MAX_DUMPS"))) : 256u;
    if (dumpDir && data && size && dumps < maxDumps &&
        (std::strcmp(stage, "submit") == 0 || std::strcmp(stage, "native-image") == 0 || std::strcmp(stage, "native-packed-accepted") == 0))
    {
        ++dumps;
        char name[96]; std::snprintf(name, sizeof(name), "/packet-%llu-%s.bin", static_cast<unsigned long long>(id), stage);
        if (FILE *packet = std::fopen((std::string(dumpDir) + name).c_str(), "wb")) {
            std::fwrite(data, 1, size, packet); std::fclose(packet);
        }
    }
}

namespace
{
    inline void inRange(uint32_t offset, size_t bytes, size_t regionSize, const char *op, uint32_t address)
    {
        if (static_cast<uint64_t>(offset) + static_cast<uint64_t>(bytes) > static_cast<uint64_t>(regionSize))
        {
            throw std::runtime_error(std::string(op) + " out-of-bounds at address: 0x" + std::to_string(address));
        }
    }

    template <typename T>
    inline T loadScalar(const uint8_t *base, uint32_t offset, size_t regionSize, const char *op, uint32_t address)
    {
        inRange(offset, sizeof(T), regionSize, op, address);
        T value{};
        std::memcpy(&value, base + offset, sizeof(T));
        return value;
    }

    template <typename T>
    inline void storeScalar(uint8_t *base, uint32_t offset, size_t regionSize, T value, const char *op, uint32_t address)
    {
        inRange(offset, sizeof(T), regionSize, op, address);
        std::memcpy(base + offset, &value, sizeof(T));
    }

    inline bool isGsPrivReg(uint32_t addr)
    {
        return Ps2AddressInRange(addr, PS2_GS_PRIV_REG_BASE, PS2_GS_PRIV_REG_SIZE);
    }

    inline bool isIoRegister(uint32_t addr)
    {
        return Ps2AddressInRange(addr, PS2_IO_BASE, PS2_IO_SIZE);
    }

    inline uint64_t *gsRegPtr(GSRegisters &gs, uint32_t addr)
    {
        // Support both 64-bit base offsets and +4 dword aliases.
        uint32_t off = (addr - PS2_GS_PRIV_REG_BASE) & ~0x7u;
        switch (off)
        {
        case 0x0000:
            return &gs.pmode;
        case 0x0010:
            return &gs.smode1;
        case 0x0020:
            return &gs.smode2;
        case 0x0030:
            return &gs.srfsh;
        case 0x0040:
            return &gs.synch1;
        case 0x0050:
            return &gs.synch2;
        case 0x0060:
            return &gs.syncv;
        case 0x0070:
            return &gs.dispfb1;
        case 0x0080:
            return &gs.display1;
        case 0x0090:
            return &gs.dispfb2;
        case 0x00A0:
            return &gs.display2;
        case 0x00B0:
            return &gs.extbuf;
        case 0x00C0:
            return &gs.extdata;
        case 0x00D0:
            return &gs.extwrite;
        case 0x00E0:
            return &gs.bgcolor;
        // CSR (offset 0x1000) is intentionally not handled here: it is
        // std::atomic<uint64_t> and no longer converts to uint64_t*. Callers must
        // check for offset 0x1000 themselves and go through writeCsrHalf/
        // writeCsrFull/gs.csr.load() instead of gsRegPtr().
        case 0x1010:
            return &gs.imr;
        case 0x1040:
            return &gs.busdir;
        case 0x1080:
            return &gs.siglblid;
        default:
            return nullptr;
        }
    }

    constexpr uint32_t kGsCsrRegOffset = 0x1000u;

    // Atomically apply a 32-bit write to one half (off=0 low dword, off=4 high
    // dword) of the GS CSR register. Bits 0..1 of the low dword (SIGNAL/FINISH) are
    // write-one-to-clear; everything else is a plain merge. Uses compare_exchange
    // so the whole read-modify-write is a single atomic step -- this register is
    // also touched by the vsync worker (FIELD bit) and the GIF (SIGNAL/FINISH) on
    // other threads, so a load-then-store here would race with them.
    inline void writeCsrHalf(std::atomic<uint64_t> &csr, uint32_t off, uint32_t value)
    {
        constexpr uint32_t kW1cMask = 0x1Fu; // SIGNAL, FINISH, HSINT, VSINT, EDWINT
        uint64_t expected = csr.load();
        uint64_t desired;
        do
        {
            if (off == 0u)
            {
                uint32_t oldLow = static_cast<uint32_t>(expected & 0xFFFFFFFFull);
                uint32_t mergedLow = (oldLow & kW1cMask) | (value & ~kW1cMask);
                desired = (expected & 0xFFFFFFFF00000000ull) | static_cast<uint64_t>(mergedLow);
                desired &= ~static_cast<uint64_t>(value & kW1cMask);
            }
            else
            {
                uint64_t mask = 0xFFFFFFFFull << (off * 8u);
                desired = (expected & ~mask) | (static_cast<uint64_t>(value) << (off * 8u));
            }
        } while (!csr.compare_exchange_weak(expected, desired));
    }

    // Same as writeCsrHalf but for a full 64-bit CSR write (bits 0..1 are still
    // write-one-to-clear against the current value).
    inline void writeCsrFull(std::atomic<uint64_t> &csr, uint64_t value)
    {
        constexpr uint64_t kW1cMask = 0x1Full;
        uint64_t expected = csr.load();
        uint64_t desired;
        do
        {
            desired = (expected & kW1cMask) | (value & ~kW1cMask);
            desired &= ~(value & kW1cMask);
        } while (!csr.compare_exchange_weak(expected, desired));
    }

    constexpr std::array<uint32_t, 4> kEeTimerBases = {
        0x10000000u,
        0x10000800u,
        0x10001000u,
        0x10001800u,
    };
    constexpr uint32_t kEeTimerCountOffset = 0x00u;
    constexpr uint32_t kEeTimerModeOffset = 0x10u;
    constexpr uint32_t kEeTimerCompareOffset = 0x20u;
    constexpr uint32_t kEeTimerHoldOffset = 0x30u;
    constexpr uint32_t kEeTimerModeClksMask = 0x3u;
    constexpr uint32_t kEeTimerModeConfigMask = 0x3FFu;
    constexpr uint32_t kEeTimerModeStatusMask = 0xC00u;
    constexpr uint32_t kEeTimerModeZret = 1u << 6;
    constexpr uint32_t kEeTimerModeCue = 1u << 7;
    constexpr uint32_t kEeTimerModeCmpe = 1u << 8;
    constexpr uint32_t kEeTimerModeOvfe = 1u << 9;
    constexpr uint32_t kEeTimerModeEquf = 1u << 10;
    constexpr uint32_t kEeTimerModeOvff = 1u << 11;
    constexpr uint64_t kEeClockHz = 294912000ull;
    constexpr std::array<uint64_t, 4> kEeTimerClockHz = {
        147456000ull,
        9216000ull,
        576000ull,
        15734ull,
    };

    inline bool decodeEeTimerRegister(uint32_t address, size_t &timerIndex, uint32_t &offset)
    {
        for (size_t index = 0; index < kEeTimerBases.size(); ++index)
        {
            const uint32_t candidateOffset = address - kEeTimerBases[index];
            if (candidateOffset == kEeTimerCountOffset ||
                candidateOffset == kEeTimerModeOffset ||
                candidateOffset == kEeTimerCompareOffset ||
                (index < 2u && candidateOffset == kEeTimerHoldOffset))
            {
                timerIndex = index;
                offset = candidateOffset;
                return true;
            }
        }
        return false;
    }

    constexpr uint64_t ticksUntilMatch(uint32_t count, uint32_t target)
    {
        const uint32_t distance = (target - count) & 0xFFFFu;
        return distance == 0u ? 0x10000ull : static_cast<uint64_t>(distance);
    }

    struct DmaTagView
    {
        uint16_t qwc = 0;
        uint8_t id = 0;
        bool irq = false;
        uint32_t addr = 0;
        uint32_t upper = 0;
    };

    inline DmaTagView decodeDmaTag(uint64_t tag)
    {
        DmaTagView out{};
        out.qwc = static_cast<uint16_t>(tag & 0xFFFFu);
        out.id = static_cast<uint8_t>((tag >> 28u) & 0x7u);
        out.irq = ((tag >> 31u) & 0x1ull) != 0ull;
        out.addr = static_cast<uint32_t>((tag >> 32u) & 0x7FFFFFFFu);
        out.upper = static_cast<uint32_t>((tag >> 16u) & 0xFFFFu);
        return out;
    }

    inline uint32_t gifTagNloop(uint64_t tagLo)
    {
        return static_cast<uint32_t>(tagLo & 0x7FFFu);
    }

    inline uint8_t gifTagFlg(uint64_t tagLo)
    {
        return static_cast<uint8_t>((tagLo >> 58u) & 0x3u);
    }

    inline uint32_t gifTagNreg(uint64_t tagLo)
    {
        uint32_t nreg = static_cast<uint32_t>((tagLo >> 60u) & 0xFu);
        return nreg == 0u ? 16u : nreg;
    }

}

// Helpers for GS VRAM addressing (PSMCT32 path).
static inline uint32_t gs_vram_offset(uint32_t basePage, uint32_t x, uint32_t y, uint32_t fbw)
{
    // basePage is in 2048-byte units; fbw is in blocks of 64 pixels.
    uint32_t strideBytes = fbw * 64 * 4;
    return basePage * 2048 + y * strideBytes + x * 4;
}

PS2Memory::PS2Memory()
    : m_rdram(nullptr), m_scratchpad(nullptr), iop_ram(nullptr), m_seenGifCopy(false), m_gsVRAM(nullptr)
{
    ps2SetScratchpadHostPtr(nullptr);
}

void ps2ProfilerRegisterCurrentThread(const char *name);
extern std::atomic<uint64_t> g_perfSyncNs;
namespace
{
    // One PS2Memory per process; kept out of the class so its layout (compiled into the game code) does not change.
    struct VifJob
    {
        std::vector<PS2Memory::PendingTransfer> gif, vif1;
        uint32_t fbrst = 0u;
        uint32_t gifTadr = 0u; // GIF TADR when the list was queued (a VIF1 interrupt handler's PATH3 kick uses it)
        bool duringIrqStall = false; // queued while a VIF1 list waited on its I bit: the handler's own transfers
        bool privWrite = false;
        uint32_t privAddress = 0u;
        uint64_t privValue = 0u, privMask = 0u;
        std::vector<uint8_t> gsPacket;
    };
    std::atomic<uint32_t> g_deferredPrivWrites{0};
    constexpr uint32_t kGsPrivPacketBytes = 20u; // GIF packets are whole qwords
    // The VU thread hands the frontend batches, not single packets: a mutex and a wake per packet, over 100,000 a second,
    // was a tenth of its time. Each record: size, padding to 16 bytes, the packet.
    constexpr uint32_t kGsBatchHeader = 16u;
    constexpr size_t kGsBatchBytes = 256u * 1024u, kGsBatchQueue = 128u;
    // grows without zeroing what is about to be overwritten
    template <typename T>
    struct NoInitAllocator : std::allocator<T>
    {
        template <typename U> struct rebind { using other = NoInitAllocator<U>; };
        template <typename U, typename... Args>
        void construct(U *where, Args &&...args) { ::new (static_cast<void *>(where)) U(std::forward<Args>(args)...); }
        template <typename U>
        void construct(U *where) { ::new (static_cast<void *>(where)) U; }
    };
    using GsBatch = std::vector<uint8_t, NoInitAllocator<uint8_t>>;
    GsBatch g_gsBatch; // VU thread only
    struct VifWorker
    {
        std::thread thread;
        std::mutex mutex;
        std::condition_variable work, idle;
        std::deque<VifJob> queue;
        std::atomic<uint64_t> submitted{0}, done{0};
        std::atomic<bool> enabled{false};
        bool stop = false;
        std::function<uint32_t()> fbrstSource;
    } g_vifWorker;
    // Second stage: the VU thread hands finished GIF packets to the GS frontend's own thread.
    struct GsWorker
    {
        std::thread thread;
        std::mutex mutex;
        std::condition_variable work, idle;
        std::deque<GsBatch> queue;
        std::vector<GsBatch> spare;
        std::atomic<uint64_t> submitted{0}, done{0};
        bool stop = false;
        std::function<void(const uint8_t *, uint32_t)> process;
    } g_gsWorker;
    thread_local bool t_onVifWorker = false;
    thread_local bool t_vifProducer = false;
    thread_local uint32_t t_vifFbrst = 0u;
    thread_local uint32_t t_vifGifTadr = 0u;
    // The EE runs ahead of the VIF worker: by the time a VIF1 I-bit handler kicks the GIF chain, the guest may already
    // have pointed GIF TADR at the next frame's list. The kick made during a stall uses the TADR of the stalled list.
    std::atomic<uint32_t> g_irqGifTadr{0u};
    constexpr uint64_t kMaxQueuedVifJobs = 1024u;
    // The queued PATH3 fragments joined, for walking GIF tags (games open tens of gates per frame over one list), and how
    // far gates have released it. The fragments stay in m_path3MaskedFifo: an unmask without gates sends them as they came.
    std::vector<uint8_t> g_path3Stream;
    size_t g_path3Joined = 0u, g_path3Consumed = 0u;
    void resetPath3Stream()
    {
        g_path3Stream.clear();
        g_path3Joined = 0u;
        g_path3Consumed = 0u;
    }
}

static void flushGsBatch();

bool PS2Memory::onVifWorker() { return t_onVifWorker; }

// PS2X_VIF1_IRQ_STATS=1: every 5 s, VIF1 I-bit stalls: count, total time, time until the EE took the interrupt
// (consumeIntcCauses), time spent running GIF jobs inside the stall.
namespace
{
    const bool g_irqStats = std::getenv("PS2X_VIF1_IRQ_STATS") != nullptr;
    std::atomic<int64_t> g_irqStallStartNs{0};
    std::atomic<uint64_t> g_irqStalls{0}, g_irqTotalNs{0}, g_irqTakeNs{0}, g_irqGifNs{0};
    int64_t irqNowNs() { return std::chrono::steady_clock::now().time_since_epoch().count(); }
}

void PS2Memory::stallVif1ForInterrupt()
{
    if (g_irqStats && t_onVifWorker)
        g_irqStallStartNs.store(irqNowNs(), std::memory_order_release);
    g_irqGifTadr.store(t_onVifWorker ? t_vifGifTadr : 0u, std::memory_order_release);
    m_vif1IrqStall.store(true, std::memory_order_release);
    queueIntcCause(5u);
    {
        std::lock_guard<std::mutex> lock(g_vifWorker.mutex);
        g_vifWorker.idle.notify_all(); // an EE waiting in gifVifSync goes on and takes the interrupt
    }
    if (!t_onVifWorker)
    {
        m_vif1IrqStall.store(false, std::memory_order_release);
        static std::atomic<unsigned> offWorker{0};
        const unsigned n = ++offWorker;
        if ((n & (n - 1)) == 0 && std::getenv("PS2X_VIF1_IRQ_LOG"))
            std::fprintf(stderr, "[vif1-irq] off-worker=%u\n", n);
        return;
    }
    // The stalled list's PATH1/2 output so far goes out first: the arbiter sorts what it holds by path priority, which
    // would move the handler's PATH3 upload behind the draws that follow the I bit (MotorStorm AE reuses texture slots
    // between the segments of its upload chain).
    if (m_gifArbiter)
        m_gifArbiter->drain();
    // PS2X_VIF1_IRQ_WAIT_US: longest stall (default 20000 us); the EE delivers the interrupt at its next dispatch.
    static const long waitUs = std::getenv("PS2X_VIF1_IRQ_WAIT_US") ? std::atol(std::getenv("PS2X_VIF1_IRQ_WAIT_US")) : 20000;
    const auto stallStart = std::chrono::steady_clock::now();
    const auto deadline = stallStart + std::chrono::microseconds(waitUs);
    // The EE runs ahead of this worker, so the handler's GIF kick can land behind the next frame's VIF1 list in the
    // queue: take the GIF-only jobs queued during the stall wherever they are. One more pass after STC catches a kick
    // that came just before the release.
    bool finalPass = false;
    for (;;)
    {
        const bool stalled = m_vif1IrqStall.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline;
        if (!stalled)
        {
            if (finalPass)
                break;
            finalPass = true;
        }
        VifJob job;
        bool have = false;
        {
            std::lock_guard<std::mutex> lock(g_vifWorker.mutex);
            for (auto it = g_vifWorker.queue.begin(); it != g_vifWorker.queue.end(); ++it)
            {
                if ((it->duringIrqStall || it == g_vifWorker.queue.begin()) && it->vif1.empty() && !it->privWrite &&
                    it->gsPacket.empty() && !it->gif.empty())
                {
                    job = std::move(*it);
                    g_vifWorker.queue.erase(it);
                    have = true;
                    break;
                }
            }
        }
        if (have)
            finalPass = false;
        if (have)
        {
            const int64_t gifStart = g_irqStats ? irqNowNs() : 0;
            runGifTransfers(job.gif);
            if (m_gifArbiter)
                m_gifArbiter->drain();
            g_vifWorker.done.fetch_add(1, std::memory_order_acq_rel);
            g_vifWorker.idle.notify_all();
            if (g_irqStats)
                g_irqGifNs += static_cast<uint64_t>(irqNowNs() - gifStart);
            continue;
        }
        if (stalled)
        {
            // The EE handler usually releases the list within microseconds; sleep_for(50us) slept ~1 ms on Windows
            // (timer resolution), ~100 stalls a second in MotorStorm AE races = 15% of this thread. Yield first.
            if (std::chrono::steady_clock::now() - stallStart < std::chrono::milliseconds(2))
                std::this_thread::yield();
            else
                std::this_thread::sleep_for(std::chrono::microseconds(50));
        }
    }
    static const bool irqLog = std::getenv("PS2X_VIF1_IRQ_LOG") != nullptr;
    if (irqLog)
    {
        static std::atomic<unsigned> stalls{0}, timeouts{0};
        const bool timedOut = m_vif1IrqStall.load(std::memory_order_acquire);
        const unsigned n = ++stalls;
        const unsigned t = timedOut ? ++timeouts : timeouts.load();
        if ((n & (n - 1)) == 0 || (timedOut && (t & (t - 1)) == 0))
            std::fprintf(stderr, "[vif1-irq] stalls=%u timeouts=%u\n", n, t);
    }
    if (g_irqStats)
    {
        const int64_t now = irqNowNs();
        ++g_irqStalls;
        g_irqTotalNs += static_cast<uint64_t>(now - g_irqStallStartNs.exchange(0));
        static std::atomic<int64_t> lastPrint{0};
        int64_t last = lastPrint.load();
        if (now - last > 5'000'000'000ll && lastPrint.compare_exchange_strong(last, now))
        {
            const uint64_t n = g_irqStalls.exchange(0);
            std::fprintf(stderr, "[vif1-irq-stats] stalls=%llu total=%.0fms take=%.0fms gif=%.0fms avg=%.0fus\n",
                         (unsigned long long)n, g_irqTotalNs.load() / 1e6, g_irqTakeNs.load() / 1e6, g_irqGifNs.load() / 1e6,
                         n ? g_irqTotalNs.load() / 1e3 / n : 0.0);
            g_irqTotalNs = 0;
            g_irqTakeNs = 0;
            g_irqGifNs = 0;
        }
    }
    m_vif1IrqStall.store(false, std::memory_order_release);
}
uint32_t PS2Memory::vifJobFbrst() { return t_vifFbrst; }

void PS2Memory::startVifWorker(std::function<uint32_t()> fbrstSource, std::function<void(const uint8_t *, uint32_t)> gsPacket)
{
    if (g_vifWorker.enabled.load(std::memory_order_acquire))
        return;
    g_vifWorker.fbrstSource = std::move(fbrstSource);
    g_vifWorker.stop = false;
    g_vifWorker.thread = std::thread([this]
    {
        t_onVifWorker = true;
        ps2ProfilerRegisterCurrentThread("vif");
        for (;;)
        {
            VifJob job;
            {
                std::unique_lock<std::mutex> lock(g_vifWorker.mutex);
                g_vifWorker.work.wait(lock, [] { return g_vifWorker.stop || !g_vifWorker.queue.empty(); });
                if (g_vifWorker.queue.empty())
                    return;
                job = std::move(g_vifWorker.queue.front());
                g_vifWorker.queue.pop_front();
            }
            PS2TL(TL_VIF_BEGIN, static_cast<uint32_t>(job.vif1.size()), job.privWrite ? job.privAddress : static_cast<uint32_t>(job.gif.size()));
            t_vifFbrst = job.fbrst;
            t_vifGifTadr = job.gifTadr;
            if (job.privWrite)
            {
                // behind the GS frontend too: it must have finished the frame being flipped to
                uint8_t record[kGsPrivPacketBytes];
                std::memcpy(record, &job.privAddress, 4);
                std::memcpy(record + 4, &job.privValue, 8);
                std::memcpy(record + 12, &job.privMask, 8);
                if (!queueGsPacket(record, sizeof(record)))
                    applyGsPrivWrite(job.privAddress, job.privValue, job.privMask);
            }
            if (!job.gsPacket.empty())
                queueGsPacket(job.gsPacket.data(), static_cast<uint32_t>(job.gsPacket.size()));
            static const bool asyncPaths = std::getenv("PS2X_ASYNC_PATHS") != nullptr;
            if (asyncPaths && !job.gif.empty() && m_path3Masked)
            {
                m_path3MaskedFifo.clear();
                resetPath3Stream();
            }
            // PS2X_DROP_VIF_AT=<s>: stage ceiling test, from then on the VIF work is thrown away.
            static const double dropAt = std::getenv("PS2X_DROP_VIF_AT") ? std::atof(std::getenv("PS2X_DROP_VIF_AT")) : 0.0;
            static const auto workerStart = std::chrono::steady_clock::now();
            if (dropAt <= 0.0 || std::chrono::duration<double>(std::chrono::steady_clock::now() - workerStart).count() < dropAt)
            {
                runGifTransfers(job.gif);
                runVif1Transfers(job.vif1);
            }
            if (m_gifArbiter)
                m_gifArbiter->drain();
            flushGsBatch();
            PS2TL(TL_VIF_END);
            g_vifWorker.done.fetch_add(1u, std::memory_order_release);
            std::lock_guard<std::mutex> lock(g_vifWorker.mutex);
            g_vifWorker.idle.notify_all();
        }
    });
    if (const char *gsThread = std::getenv("PS2X_GS_THREAD"); !gsThread || gsThread[0] != '0')
    {
        g_gsWorker.process = std::move(gsPacket);
        g_gsWorker.stop = false;
        g_gsWorker.thread = std::thread([this]
        {
            ps2ProfilerRegisterCurrentThread("gsfe");
            for (;;)
            {
                GsBatch packet;
                {
                    std::unique_lock<std::mutex> lock(g_gsWorker.mutex);
                    if (g_gsWorker.queue.empty())
                        PS2TL(TL_GSFE_IDLE);
                    g_gsWorker.work.wait(lock, [] { return g_gsWorker.stop || !g_gsWorker.queue.empty(); });
                    PS2TL(TL_GSFE_BUSY, static_cast<uint32_t>(g_gsWorker.queue.size()));
                    if (g_gsWorker.queue.empty())
                        return;
                    packet = std::move(g_gsWorker.queue.front());
                    g_gsWorker.queue.pop_front();
                }
                for (size_t offset = 0u; offset < packet.size();)
                {
                    uint32_t size = 0u;
                    std::memcpy(&size, packet.data() + offset, 4);
                    const uint8_t *record = packet.data() + offset + kGsBatchHeader;
                    if (size == kGsPrivPacketBytes)
                    {
                        uint32_t address = 0u;
                        uint64_t value = 0u, mask = 0u;
                        std::memcpy(&address, record, 4);
                        std::memcpy(&value, record + 4, 8);
                        std::memcpy(&mask, record + 12, 8);
                        applyGsPrivWrite(address, value, mask);
                    }
                    else
                        g_gsWorker.process(record, size);
                    offset += kGsBatchHeader + ((size + 15u) & ~15u);
                }
                g_gsWorker.done.fetch_add(1u, std::memory_order_release);
                std::lock_guard<std::mutex> lock(g_gsWorker.mutex);
                if (g_gsWorker.spare.size() < 256u)
                    g_gsWorker.spare.push_back(std::move(packet));
                g_gsWorker.idle.notify_all();
            }
        });
    }
    g_vifWorker.enabled.store(true, std::memory_order_release);
}

void PS2Memory::applyGsPrivWrite(uint32_t address, uint64_t value, uint64_t mask)
{
    if (uint64_t *reg = gsRegPtr(gs_regs, address))
        *reg = (*reg & ~mask) | (value & mask);
    PS2TL(TL_DISP_APPLY, address, static_cast<uint32_t>(value));
    g_deferredPrivWrites.fetch_sub(1u, std::memory_order_acq_rel);
}

static const uint32_t kMaxDeferredDisplayWrites = std::getenv("PS2X_DISPLAY_BACKLOG") ? static_cast<uint32_t>(std::atoi(std::getenv("PS2X_DISPLAY_BACKLOG"))) : 14u;
bool PS2Memory::deferGsPrivWrite(uint32_t address, uint64_t value, uint64_t mask)
{
    if (!t_vifProducer || !g_vifWorker.enabled.load(std::memory_order_acquire))
        return false;
    const uint32_t regOff = (address - PS2_GS_PRIV_REG_BASE) & ~0x7u;
    const bool displayReg = regOff == 0x0000u || regOff == 0x0020u || (regOff >= 0x0070u && regOff <= 0x00A0u) || regOff == 0x00E0u;
    if (!displayReg)
        return false;
    if (g_vifWorker.done.load(std::memory_order_acquire) == g_vifWorker.submitted.load(std::memory_order_relaxed) &&
        g_gsWorker.done.load(std::memory_order_acquire) == g_gsWorker.submitted.load(std::memory_order_acquire))
        return false; // nothing to wait for
    // PMODE/SMODE2/DISPFB/DISPLAY arrive together at every vblank; this many queued means several frames are behind.
    if (g_deferredPrivWrites.load(std::memory_order_acquire) >= kMaxDeferredDisplayWrites)
    {
        // Wait for the backlog to shrink, not for the pipeline to empty: draining it would throw the overlap away.
        PS2TL(TL_BACKLOG_BEGIN);
        const auto waitStart = std::chrono::steady_clock::now();
        std::unique_lock<std::mutex> lock(g_vifWorker.mutex);
        while (g_deferredPrivWrites.load(std::memory_order_acquire) + 5u >= kMaxDeferredDisplayWrites)
            ps2_wait::waitFor(g_vifWorker.idle, lock, std::chrono::microseconds(200));
        lock.unlock();
        g_perfSyncNs += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - waitStart).count();
        PS2TL(TL_BACKLOG_END);
    }
    PS2TL(TL_DISP_DEFER, address, static_cast<uint32_t>(value));
    VifJob job;
    job.privWrite = true;
    job.privAddress = address;
    job.privValue = value;
    job.privMask = mask;
    g_deferredPrivWrites.fetch_add(1u, std::memory_order_acq_rel);
    g_vifWorker.submitted.fetch_add(1u, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(g_vifWorker.mutex);
        g_vifWorker.queue.push_back(std::move(job));
    }
    g_vifWorker.work.notify_one();
    return true;
}

bool PS2Memory::queueGsRegisterWrites(const uint64_t *pairs, size_t count)
{
    if (!t_vifProducer || !g_vifWorker.enabled.load(std::memory_order_acquire) || !g_gsWorker.thread.joinable() || count == 0u)
        return false;
    VifJob job;
    job.gsPacket.resize((count + 1u) * 16u);
    const uint64_t tag[2] = {static_cast<uint64_t>(count) | (1ull << 15u) | (1ull << 60u), 0x0Eull}; // EOP, PACKED, NREG 1: A+D
    std::memcpy(job.gsPacket.data(), tag, sizeof(tag));
    std::memcpy(job.gsPacket.data() + 16u, pairs, count * 16u);
    g_vifWorker.submitted.fetch_add(1u, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(g_vifWorker.mutex);
        g_vifWorker.queue.push_back(std::move(job));
    }
    g_vifWorker.work.notify_one();
    return true;
}

bool PS2Memory::queueGsPacket(const uint8_t *data, uint32_t sizeBytes)
{
    if (!t_onVifWorker || !g_gsWorker.process)
        return false;
    const size_t start = g_gsBatch.size();
    g_gsBatch.resize(start + kGsBatchHeader + ((sizeBytes + 15u) & ~15u));
    std::memcpy(g_gsBatch.data() + start, &sizeBytes, 4);
    std::memcpy(g_gsBatch.data() + start + kGsBatchHeader, data, sizeBytes);
    if (g_gsBatch.size() >= kGsBatchBytes)
        flushGsBatch();
    return true;
}

static void flushGsBatch()
{
    if (g_gsBatch.empty())
        return;
    GsBatch next;
    {
        std::unique_lock<std::mutex> lock(g_gsWorker.mutex);
        if (g_gsWorker.queue.size() >= kGsBatchQueue)
            PS2TL(TL_FEQ_BEGIN);
        g_gsWorker.idle.wait(lock, [] { return g_gsWorker.queue.size() < kGsBatchQueue; });
        PS2TL(TL_FEQ_END);
        g_gsWorker.submitted.fetch_add(1u, std::memory_order_relaxed);
        g_gsWorker.queue.push_back(std::move(g_gsBatch));
        if (!g_gsWorker.spare.empty())
        {
            next = std::move(g_gsWorker.spare.back());
            g_gsWorker.spare.pop_back();
        }
    }
    g_gsWorker.work.notify_one();
    next.clear();
    g_gsBatch = std::move(next);
    if (g_gsBatch.capacity() < kGsBatchBytes)
        g_gsBatch.reserve(kGsBatchBytes + 8192u);
}

void PS2Memory::stopVifWorker()
{
    if (!g_vifWorker.enabled.exchange(false, std::memory_order_acq_rel))
        return;
    {
        std::lock_guard<std::mutex> lock(g_vifWorker.mutex);
        g_vifWorker.stop = true;
    }
    g_vifWorker.work.notify_all();
    g_vifWorker.thread.join();
    if (g_gsWorker.thread.joinable())
    {
        {
            std::lock_guard<std::mutex> lock(g_gsWorker.mutex);
            g_gsWorker.stop = true;
        }
        g_gsWorker.work.notify_all();
        g_gsWorker.thread.join();
    }
}

void PS2Memory::gifVifSync() const
{
    if (!t_vifProducer)
        return;
    const uint64_t target = g_vifWorker.submitted.load(std::memory_order_relaxed);
    const auto gsIdle = [] { return g_gsWorker.done.load(std::memory_order_acquire) == g_gsWorker.submitted.load(std::memory_order_acquire); };
    // A VIF1 list stalled on an I bit waits for the EE's interrupt handler: the EE must not wait for it.
    const auto vifStalled = [this] { return m_vif1IrqStall.load(std::memory_order_acquire); };
    if (vifStalled() || (g_vifWorker.done.load(std::memory_order_acquire) == target && gsIdle()))
        return;
    PS2TL(TL_SYNC_BEGIN, static_cast<uint32_t>(reinterpret_cast<uintptr_t>(PS2X_RETURN_ADDRESS())));
    const auto waitStart = std::chrono::steady_clock::now();
    for (int spin = 0; spin < 4000; ++spin)
    {
        if (g_vifWorker.done.load(std::memory_order_acquire) == target || vifStalled())
            break;
        _mm_pause();
    }
    if (g_vifWorker.done.load(std::memory_order_acquire) != target && !vifStalled())
    {
        std::unique_lock<std::mutex> lock(g_vifWorker.mutex);
        g_vifWorker.idle.wait(lock, [&] { return g_vifWorker.done.load(std::memory_order_acquire) == target || vifStalled(); });
    }
    if (vifStalled())
    {
        PS2TL(TL_SYNC_END);
        return;
    }
    // The VU thread has finished, so the GS queue only shrinks from here.
    if (!gsIdle())
    {
        std::unique_lock<std::mutex> lock(g_gsWorker.mutex);
        g_gsWorker.idle.wait(lock, gsIdle);
    }
    PS2TL(TL_SYNC_END);
    g_perfSyncNs += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - waitStart).count();
}

PS2Memory::~PS2Memory()
{
    stopVifWorker();
    if (m_rdram)
    {
        delete[] m_rdram;
        m_rdram = nullptr;
    }

    if (m_scratchpad)
    {
        ps2SetScratchpadHostPtr(nullptr);
        delete[] m_scratchpad;
        m_scratchpad = nullptr;
    }

    if (m_gsVRAM)
    {
        delete[] m_gsVRAM;
        m_gsVRAM = nullptr;
    }

    if (m_vu1Code)
    {
        delete[] m_vu1Code;
        m_vu1Code = nullptr;
    }
    if (m_vu1Data)
    {
        delete[] m_vu1Data;
        m_vu1Data = nullptr;
    }
    if (m_vu0Code)
    {
        delete[] m_vu0Code;
        m_vu0Code = nullptr;
    }
    if (m_vu0Data)
    {
        delete[] m_vu0Data;
        m_vu0Data = nullptr;
    }

    if (iop_ram)
    {
        delete[] iop_ram;
        iop_ram = nullptr;
    }
}

bool PS2Memory::initialize(size_t ramSize)
{
    auto cleanup = [this]()
    {
        delete[] m_rdram;
        delete[] m_scratchpad;
        delete[] iop_ram;
        delete[] m_gsVRAM;
        delete[] m_vu0Code;
        delete[] m_vu0Data;
        delete[] m_vu1Code;
        delete[] m_vu1Data;
        m_rdram = nullptr;
        m_scratchpad = nullptr;
        ps2SetScratchpadHostPtr(nullptr);
        iop_ram = nullptr;
        m_gsVRAM = nullptr;
        m_vu0Code = nullptr;
        m_vu0Data = nullptr;
        m_vu1Code = nullptr;
        m_vu1Data = nullptr;
    };

    cleanup();
    m_seenGifCopy = false;
    m_dmaStartCount.store(0, std::memory_order_relaxed);
    m_gifCopyCount.store(0, std::memory_order_relaxed);
    m_gsWriteCount.store(0, std::memory_order_relaxed);
    m_vifWriteCount.store(0, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(m_completedDmacMutex);
        m_completedDmacCauses.clear();
    }
    m_codeRegions.clear();
    m_path3Masked = false;
    m_path3MaskedFifo.clear();
    resetPath3Stream();
    m_vif1PendingPath2ImageQwc = 0u;
    m_vif1PendingPath2DirectHl = false;
    m_vif1Carry.clear();
    resetEeTimers();

    try
    {
        // Allocate main RAM
        m_rdram = new uint8_t[ramSize];
        std::memset(m_rdram, 0, ramSize);

        // Allocate scratchpad
        m_scratchpad = new uint8_t[PS2_SCRATCHPAD_SIZE];
        std::memset(m_scratchpad, 0, PS2_SCRATCHPAD_SIZE);
        ps2SetScratchpadHostPtr(m_scratchpad);

        // Initialize EE TLB entries (R5900 has 48 entries).
        m_tlbEntries.assign(48, TLBEntry{0, 0, 0, false});

        // Allocate IOP RAM
        iop_ram = new uint8_t[2 * 1024 * 1024]; // 2MB

        // Initialize IOP RAM with zeros
        std::memset(iop_ram, 0, 2 * 1024 * 1024);

        // Initialize I/O registers
        m_ioRegisters.clear();

        // Initialize GS registers
        memset(&gs_regs, 0, sizeof(gs_regs));
        // memset zero-fills std::atomic<uint64_t>::csr's bytes, which is not itself
        // a guaranteed-valid atomic store; make the zero-initialization explicit.
        gs_regs.csr.store(0);
        gs_regs.dispfb1 = (0ULL << 0) | (10ULL << 9) | (0ULL << 15) | (0ULL << 32) | (0ULL << 43);
        gs_regs.display1 = (0ULL << 0) | (0ULL << 12) | (0ULL << 23) | (0ULL << 27) | (639ULL << 32) | (447ULL << 44);
        gs_regs.dispfb2 = gs_regs.dispfb1;
        gs_regs.display2 = gs_regs.display1;

        // Allocate GS VRAM (4MB)
        m_gsVRAM = new uint8_t[PS2_GS_VRAM_SIZE];
        std::memset(m_gsVRAM, 0, PS2_GS_VRAM_SIZE);

        m_vu0Code = new uint8_t[PS2_VU0_CODE_SIZE];
        m_vu0Data = new uint8_t[PS2_VU0_DATA_SIZE];
        std::memset(m_vu0Code, 0, PS2_VU0_CODE_SIZE);
        std::memset(m_vu0Data, 0, PS2_VU0_DATA_SIZE);

        m_vu1Code = new uint8_t[PS2_VU1_CODE_SIZE];
        m_vu1Data = new uint8_t[PS2_VU1_DATA_SIZE];
        std::memset(m_vu1Code, 0, PS2_VU1_CODE_SIZE);
        std::memset(m_vu1Data, 0, PS2_VU1_DATA_SIZE);
        markVU0CodeModified();
        markVU1CodeModified();

        // Initialize VIF registers
        memset(&vif0_regs, 0, sizeof(vif0_regs));
        memset(&vif1_regs, 0, sizeof(vif1_regs));

        // Initialize DMA registers
        memset(dma_regs, 0, sizeof(dma_regs));

        return true;
    }
    catch (const std::exception &e)
    {
        std::cerr << "Error initializing PS2 memory: " << e.what() << std::endl;
        cleanup();
        return false;
    }
}

void PS2Memory::resetEeTimers() noexcept
{
    m_eeTimers = {};
}

uint32_t PS2Memory::advanceEeTimers(uint64_t eeCycles) noexcept
{
    if (eeCycles == 0u)
    {
        return 0u;
    }

    constexpr uint32_t kGifStat = 0x10003020u;
    constexpr uint32_t kGifFqcMask = 0x1F000000u;
    auto gifStatIt = m_ioRegisters.find(kGifStat);
    if (gifStatIt != m_ioRegisters.end())
        gifStatIt->second &= ~kGifFqcMask;

    uint32_t interruptMask = 0u;
    for (size_t index = 0; index < m_eeTimers.size(); ++index)
    {
        EeTimer &timer = m_eeTimers[index];
        if ((timer.mode & kEeTimerModeCue) == 0u)
        {
            continue;
        }

        const uint64_t clockHz = kEeTimerClockHz[timer.mode & kEeTimerModeClksMask];
        const uint64_t wholeSeconds = eeCycles / kEeClockHz;
        const uint64_t remainingCycles = eeCycles % kEeClockHz;
        const uint64_t scaled = remainingCycles * clockHz + timer.clockRemainder;
        const uint64_t ticks = wholeSeconds * clockHz + scaled / kEeClockHz;
        timer.clockRemainder = scaled % kEeClockHz;
        if (ticks == 0u)
        {
            continue;
        }

        const uint32_t oldCount = timer.count & 0xFFFFu;
        const uint32_t compare = timer.compare & 0xFFFFu;
        const uint64_t compareDistance = ticksUntilMatch(oldCount, compare);
        const uint64_t overflowDistance = 0x10000ull - oldCount;
        const bool zeroReturn = (timer.mode & kEeTimerModeZret) != 0u;
        const bool compareReached = ticks >= compareDistance;
        bool overflowReached = false;

        if (zeroReturn)
        {
            overflowReached = ticks >= overflowDistance && overflowDistance <= compareDistance;
            if (compareReached)
            {
                const uint64_t remaining = ticks - compareDistance;
                timer.count = compare == 0u
                                  ? static_cast<uint32_t>(remaining & 0xFFFFu)
                                  : static_cast<uint32_t>(remaining % compare);
            }
            else
            {
                timer.count = static_cast<uint32_t>((oldCount + ticks) & 0xFFFFu);
            }
        }
        else
        {
            overflowReached = ticks >= overflowDistance;
            timer.count = static_cast<uint32_t>((oldCount + ticks) & 0xFFFFu);
        }

        if (compareReached && (timer.mode & kEeTimerModeCmpe) != 0u && (timer.mode & kEeTimerModeEquf) == 0u)
        {
            timer.mode |= kEeTimerModeEquf;
            interruptMask |= 1u << index;
        }
        if (overflowReached && (timer.mode & kEeTimerModeOvfe) != 0u && (timer.mode & kEeTimerModeOvff) == 0u)
        {
            timer.mode |= kEeTimerModeOvff;
            interruptMask |= 1u << index;
        }
    }
    return interruptMask;
}

uint64_t PS2Memory::cyclesUntilNextEeTimerInterrupt() const noexcept
{
    uint64_t nearest = std::numeric_limits<uint64_t>::max();
    for (const EeTimer &timer : m_eeTimers)
    {
        if ((timer.mode & kEeTimerModeCue) == 0u)
        {
            continue;
        }

        const uint32_t count = timer.count & 0xFFFFu;
        const uint32_t compare = timer.compare & 0xFFFFu;
        const uint64_t compareDistance = ticksUntilMatch(count, compare);
        const uint64_t overflowDistance = 0x10000ull - count;
        uint64_t eventTicks = std::numeric_limits<uint64_t>::max();

        if ((timer.mode & kEeTimerModeCmpe) != 0u &&
            (timer.mode & kEeTimerModeEquf) == 0u)
        {
            eventTicks = compareDistance;
        }
        const bool overflowCanOccur = (timer.mode & kEeTimerModeZret) == 0u ||
                                      overflowDistance <= compareDistance;
        if (overflowCanOccur &&
            (timer.mode & kEeTimerModeOvfe) != 0u &&
            (timer.mode & kEeTimerModeOvff) == 0u)
        {
            eventTicks = std::min(eventTicks, overflowDistance);
        }
        if (eventTicks == std::numeric_limits<uint64_t>::max())
        {
            continue;
        }

        const uint64_t clockHz = kEeTimerClockHz[timer.mode & kEeTimerModeClksMask];
        const uint64_t numerator = eventTicks * kEeClockHz - timer.clockRemainder;
        const uint64_t cycles = (numerator + clockHz - 1u) / clockHz;
        nearest = std::min(nearest, std::max<uint64_t>(1u, cycles));
    }
    return nearest;
}

bool PS2Memory::isScratchpad(uint32_t address) const
{
    return ps2IsScratchpadAddress(address);
}

uint8_t *PS2Memory::mapVuMemory(uint32_t physAddr, uint32_t size, uint32_t &offset, uint32_t &limit)
{
    return const_cast<uint8_t *>(static_cast<const PS2Memory *>(this)->mapVuMemory(physAddr, size, offset, limit));
}

const uint8_t *PS2Memory::mapVuMemory(uint32_t physAddr, uint32_t size, uint32_t &offset, uint32_t &limit) const
{
    if (physAddr >= PS2_VU1_CODE_BASE && physAddr < PS2_VU1_DATA_BASE + PS2_VU1_DATA_SIZE)
        gifVifSync();
    auto mapRange = [&](uint32_t base, uint32_t rangeSize, const uint8_t *ptr) -> const uint8_t *
    {
        if (!ptr || physAddr < base)
        {
            return nullptr;
        }
        const uint32_t local = physAddr - base;
        if (local >= rangeSize || size > (rangeSize - local))
        {
            return nullptr;
        }
        offset = local;
        limit = rangeSize;
        return ptr;
    };

    if (const uint8_t *ptr = mapRange(PS2_VU0_CODE_BASE, PS2_VU0_CODE_SIZE, m_vu0Code))
    {
        return ptr;
    }
    if (const uint8_t *ptr = mapRange(PS2_VU0_DATA_BASE, PS2_VU0_DATA_SIZE, m_vu0Data))
    {
        return ptr;
    }
    if (const uint8_t *ptr = mapRange(PS2_VU1_CODE_BASE, PS2_VU1_CODE_SIZE, m_vu1Code))
    {
        return ptr;
    }
    return mapRange(PS2_VU1_DATA_BASE, PS2_VU1_DATA_SIZE, m_vu1Data);
}

uint32_t PS2Memory::translateAddress(uint32_t virtualAddress)
{
    if (isScratchpad(virtualAddress))
    {
        return ps2ScratchpadOffset(virtualAddress);
    }

    // EE uncached aliases of main RAM (per PS2 memory map):
    //   0x20000000-0x3FFFFFFF -> 32MB mirror of RDRAM
    // This includes the accelerated window rooted at 0x30100000.
    if (Ps2IsUncachedRamMirrorAddress(virtualAddress))
    {
        return virtualAddress & PS2_RAM_MASK;
    }

    // KSEG0/KSEG1 direct-mapped window.
    if (Ps2IsKseg01Address(virtualAddress))
    {
        return Ps2DirectMappedPhysicalAddress(virtualAddress);
    }

    // In this runtime, low segments are treated as physical-style addresses already.
    if (virtualAddress < 0x80000000)
    {
        return virtualAddress;
    }

    // KSEG2/KSEG3 are TLB mapped.
    if (Ps2IsKseg23Address(virtualAddress))
    {
        for (const auto &entry : m_tlbEntries)
        {
            if (entry.valid)
            {
                // PageMask uses bits [24:13]. Build an address-level mask (plus 4KB base page bits).
                const uint32_t mask = entry.mask & 0x01FFE000u;
                const uint32_t compareMask = ~(mask | 0xFFFu);
                if ((virtualAddress & compareMask) == (entry.vpn & compareMask))
                {
                    // TLB hit
                    const uint32_t pageOffsetMask = mask | 0xFFFu;
                    const uint32_t physBase = entry.pfn << 12;
                    return physBase | (virtualAddress & pageOffsetMask);
                }
            }
        }
        throw std::runtime_error("TLB miss for address: 0x" + std::to_string(virtualAddress));
    }

    return virtualAddress;
}

bool PS2Memory::tlbRead(uint32_t index, uint32_t &vpn, uint32_t &pfn, uint32_t &mask, bool &valid) const
{
    if (index >= m_tlbEntries.size())
    {
        return false;
    }

    const TLBEntry &entry = m_tlbEntries[index];
    vpn = entry.vpn;
    pfn = entry.pfn;
    mask = entry.mask;
    valid = entry.valid;
    return true;
}

bool PS2Memory::tlbWrite(uint32_t index, uint32_t vpn, uint32_t pfn, uint32_t mask, bool valid)
{
    if (index >= m_tlbEntries.size())
    {
        return false;
    }

    TLBEntry &entry = m_tlbEntries[index];
    entry.vpn = vpn & 0xFFFFF000u;
    entry.pfn = pfn & 0x000FFFFFu;
    entry.mask = mask & 0x01FFE000u;
    entry.valid = valid;
    return true;
}

int32_t PS2Memory::tlbProbe(uint32_t vpn) const
{
    const uint32_t normalizedVpn = vpn & 0xFFFFF000u;
    for (uint32_t i = 0; i < static_cast<uint32_t>(m_tlbEntries.size()); ++i)
    {
        const TLBEntry &entry = m_tlbEntries[i];
        if (!entry.valid)
        {
            continue;
        }

        const uint32_t mask = entry.mask & 0x01FFE000u;
        const uint32_t compareMask = ~(mask | 0xFFFu);
        if ((normalizedVpn & compareMask) == (entry.vpn & compareMask))
        {
            return static_cast<int32_t>(i);
        }
    }

    return -1;
}

uint8_t PS2Memory::read8(uint32_t address)
{
    const bool scratch = isScratchpad(address);
    uint32_t physAddr = translateAddress(address);

    if (scratch)
    {
        return m_scratchpad[physAddr];
    }
    if (physAddr < PS2_RAM_SIZE)
    {
        return m_rdram[physAddr];
    }
    uint32_t vuOffset = 0;
    uint32_t vuLimit = 0;
    if (const uint8_t *vuMem = mapVuMemory(physAddr, sizeof(uint8_t), vuOffset, vuLimit))
    {
        (void)vuLimit;
        return vuMem[vuOffset];
    }
    else if (isIoRegister(physAddr))
    {
        uint32_t regAddr = physAddr & ~0x3;
        uint32_t value = readIORegister(regAddr);
        uint32_t shift = (physAddr & 3) * 8;
        return static_cast<uint8_t>((value >> shift) & 0xFF);
    }

    return 0;
}

uint16_t PS2Memory::read16(uint32_t address)
{
    if (address & 1)
    {
        throw std::runtime_error("Unaligned 16-bit read at address: 0x" + std::to_string(address));
    }

    const bool scratch = isScratchpad(address);
    uint32_t physAddr = translateAddress(address);

    if (scratch)
    {
        return loadScalar<uint16_t>(m_scratchpad, physAddr, PS2_SCRATCHPAD_SIZE, "read16 scratchpad", address);
    }
    if (physAddr < PS2_RAM_SIZE)
    {
        return loadScalar<uint16_t>(m_rdram, physAddr, PS2_RAM_SIZE, "read16 rdram", address);
    }
    uint32_t vuOffset = 0;
    uint32_t vuLimit = 0;
    if (const uint8_t *vuMem = mapVuMemory(physAddr, sizeof(uint16_t), vuOffset, vuLimit))
    {
        return loadScalar<uint16_t>(vuMem, vuOffset, vuLimit, "read16 vu", address);
    }
    else if (isIoRegister(physAddr))
    {
        uint32_t regAddr = physAddr & ~0x3;
        uint32_t value = readIORegister(regAddr);
        uint32_t shift = (physAddr & 2) * 8;
        return static_cast<uint16_t>((value >> shift) & 0xFFFF);
    }

    return 0;
}

uint32_t PS2Memory::read32(uint32_t address)
{
    if (address & 3)
    {
        throw std::runtime_error("Unaligned 32-bit read at address: 0x" + std::to_string(address));
    }

    if (isGsPrivReg(address))
    {
        static const bool asyncPaths = std::getenv("PS2X_ASYNC_PATHS") != nullptr;
        if (asyncPaths && ((address - PS2_GS_PRIV_REG_BASE) & ~0x7u) == kGsCsrRegOffset)
            return static_cast<uint32_t>(((gs_regs.csr.load() | 2u) >> ((address & 7u) * 8u)) & 0xFFFFFFFFull);
        static const bool csrNoSync32 = std::getenv("PS2X_CSR_NOSYNC") != nullptr;
        if (csrNoSync32 && ((address - PS2_GS_PRIV_REG_BASE) & ~0x7u) == kGsCsrRegOffset)
            return static_cast<uint32_t>((gs_regs.csr.load() >> ((address & 7u) * 8u)) & 0xFFFFFFFFull);
        gifVifSync();
        uint32_t off = address & 7;
        const uint32_t regOff = (address - PS2_GS_PRIV_REG_BASE) & ~0x7u;
        if (regOff == kGsCsrRegOffset)
        {
            uint64_t val = gs_regs.csr.load();
            return (uint32_t)(val >> (off * 8));
        }
        uint64_t *reg = gsRegPtr(gs_regs, address);
        if (!reg)
            return 0;
        uint64_t val = *reg;
        return (uint32_t)(val >> (off * 8));
    }

    const bool scratch = isScratchpad(address);
    uint32_t physAddr = translateAddress(address);

    if (scratch)
    {
        return loadScalar<uint32_t>(m_scratchpad, physAddr, PS2_SCRATCHPAD_SIZE, "read32 scratchpad", address);
    }
    if (physAddr < PS2_RAM_SIZE)
    {
        return loadScalar<uint32_t>(m_rdram, physAddr, PS2_RAM_SIZE, "read32 rdram", address);
    }
    uint32_t vuOffset = 0;
    uint32_t vuLimit = 0;
    if (const uint8_t *vuMem = mapVuMemory(physAddr, sizeof(uint32_t), vuOffset, vuLimit))
    {
        return loadScalar<uint32_t>(vuMem, vuOffset, vuLimit, "read32 vu", address);
    }
    else if (isIoRegister(physAddr))
    {
        return readIORegister(physAddr);
    }

    return 0;
}

uint64_t PS2Memory::read64(uint32_t address)
{
    if (address & 7)
    {
        throw std::runtime_error("Unaligned 64-bit read at address: 0x" + std::to_string(address));
    }

    if (isGsPrivReg(address))
    {
        // PS2X_GSREAD_STATS=1: count 64-bit privileged-register reads per register (each one waits for VIF and GS)
        static const bool readStats = std::getenv("PS2X_GSREAD_STATS") != nullptr;
        if (readStats)
        {
            static std::atomic<uint32_t> counts[32];
            static auto last = std::chrono::steady_clock::now();
            const uint32_t off = address - PS2_GS_PRIV_REG_BASE;
            ++counts[((off >> 8) & 0x10u) | ((off >> 4) & 15u)];
            if (std::chrono::steady_clock::now() - last > std::chrono::seconds(5))
            {
                last = std::chrono::steady_clock::now();
                std::fprintf(stderr, "[gsread64]");
                for (uint32_t i = 0; i < 32u; ++i)
                    if (const uint32_t c = counts[i].exchange(0))
                        std::fprintf(stderr, " %04x:%u", ((i & 0x10u) << 8) | ((i & 15u) << 4), c);
                std::fputc('\n', stderr);
            }
        }
        const uint32_t regOff = (address - PS2_GS_PRIV_REG_BASE) & ~0x7u;
        // PS2X_CSR_NOSYNC=1: CSR reads return the current value without draining the VIF/GS queues first. A loop polling
        // FINISH/SIGNAL still sees them once the workers get there; GTA LCS reads CSR ~100x/s and waited for every one.
        static const bool csrNoSync = std::getenv("PS2X_CSR_NOSYNC") != nullptr;
        if (regOff == kGsCsrRegOffset && csrNoSync)
            return gs_regs.csr.load();
        gifVifSync();
        if (regOff == kGsCsrRegOffset)
        {
            return gs_regs.csr.load();
        }
        uint64_t *reg = gsRegPtr(gs_regs, address);
        return reg ? *reg : 0;
    }

    const bool scratch = isScratchpad(address);
    uint32_t physAddr = translateAddress(address);

    if (scratch)
    {
        return loadScalar<uint64_t>(m_scratchpad, physAddr, PS2_SCRATCHPAD_SIZE, "read64 scratchpad", address);
    }
    if (physAddr < PS2_RAM_SIZE)
    {
        return loadScalar<uint64_t>(m_rdram, physAddr, PS2_RAM_SIZE, "read64 rdram", address);
    }
    uint32_t vuOffset = 0;
    uint32_t vuLimit = 0;
    if (const uint8_t *vuMem = mapVuMemory(physAddr, sizeof(uint64_t), vuOffset, vuLimit))
    {
        return loadScalar<uint64_t>(vuMem, vuOffset, vuLimit, "read64 vu", address);
    }

    // 64-bit IO read: compose from the two adjacent 32-bit IO register slots
    // to avoid any side-effects from read32 handlers.
    if (isIoRegister(address))
    {
        uint32_t lo = m_ioRegisters.count(address) ? m_ioRegisters[address] : 0u;
        uint32_t hi = m_ioRegisters.count(address + 4) ? m_ioRegisters[address + 4] : 0u;
        return static_cast<uint64_t>(lo) | (static_cast<uint64_t>(hi) << 32);
    }
    return (uint64_t)read32(address) | ((uint64_t)read32(address + 4) << 32);
}

__m128i PS2Memory::read128(uint32_t address)
{
    // EE LQ ignores the low 4 bits of the effective address (alignment is forced, not faulted); Sony's libc memcpy
    // relies on it (upstream PR #400).
    address &= ~0xFu;

    const bool scratch = isScratchpad(address);
    uint32_t physAddr = translateAddress(address);

    if (!scratch && physAddr == 0x10007000u) // IPU_out_FIFO
    {
        alignas(16) uint8_t fifoData[16] = {};
        m_ipu.takeOutput(fifoData, sizeof(fifoData));
        return _mm_loadu_si128(reinterpret_cast<const __m128i *>(fifoData));
    }
    if (scratch)
    {
        inRange(physAddr, sizeof(__m128i), PS2_SCRATCHPAD_SIZE, "read128 scratchpad", address);
        return _mm_loadu_si128(reinterpret_cast<__m128i *>(&m_scratchpad[physAddr]));
    }
    if (physAddr < PS2_RAM_SIZE)
    {
        inRange(physAddr, sizeof(__m128i), PS2_RAM_SIZE, "read128 rdram", address);
        return _mm_loadu_si128(reinterpret_cast<__m128i *>(&m_rdram[physAddr]));
    }
    uint32_t vuOffset = 0;
    uint32_t vuLimit = 0;
    if (const uint8_t *vuMem = mapVuMemory(physAddr, sizeof(__m128i), vuOffset, vuLimit))
    {
        inRange(vuOffset, sizeof(__m128i), vuLimit, "read128 vu", address);
        return _mm_loadu_si128(reinterpret_cast<const __m128i *>(vuMem + vuOffset));
    }

    // 128-bit reads are primarily for quad-word loads in the EE, which are only valid for RAM areas
    // Return zeroes for unsupported areas
    return _mm_setzero_si128();
}

void PS2Memory::write8(uint32_t address, uint8_t value)
{
    const bool scratch = isScratchpad(address);
    uint32_t physAddr = translateAddress(address);

    if (scratch)
    {
        m_scratchpad[physAddr] = value;
    }
    else if (physAddr < PS2_RAM_SIZE)
    {
        m_rdram[physAddr] = value;
    }
    else
    {
        uint32_t vuOffset = 0;
        uint32_t vuLimit = 0;
        if (uint8_t *vuMem = mapVuMemory(physAddr, sizeof(uint8_t), vuOffset, vuLimit))
        {
            (void)vuLimit;
            vuMem[vuOffset] = value;
            if (vuMem == m_vu0Code)
                markVU0CodeModified();
            else if (vuMem == m_vu1Code)
                markVU1CodeModified();
            return;
        }
    }
    if (isIoRegister(physAddr))
    {
        // IO registers - handle byte writes by modifying the appropriate byte in the word
        uint32_t regAddr = physAddr & ~0x3;
        uint32_t shift = (physAddr & 3) * 8;
        uint32_t mask = ~(0xFF << shift);
        uint32_t newValue = (m_ioRegisters[regAddr] & mask) | ((uint32_t)value << shift);
        writeIORegister(regAddr, newValue);
    }
}

void PS2Memory::write16(uint32_t address, uint16_t value)
{
    if (address & 1)
    {
        throw std::runtime_error("Unaligned 16-bit write at address: 0x" + std::to_string(address));
    }

    const bool scratch = isScratchpad(address);
    uint32_t physAddr = translateAddress(address);

    if (scratch)
    {
        storeScalar<uint16_t>(m_scratchpad, physAddr, PS2_SCRATCHPAD_SIZE, value, "write16 scratchpad", address);
    }
    else if (physAddr < PS2_RAM_SIZE)
    {
        storeScalar<uint16_t>(m_rdram, physAddr, PS2_RAM_SIZE, value, "write16 rdram", address);
    }
    else
    {
        uint32_t vuOffset = 0;
        uint32_t vuLimit = 0;
        if (uint8_t *vuMem = mapVuMemory(physAddr, sizeof(uint16_t), vuOffset, vuLimit))
        {
            storeScalar<uint16_t>(vuMem, vuOffset, vuLimit, value, "write16 vu", address);
            if (vuMem == m_vu0Code)
                markVU0CodeModified();
            else if (vuMem == m_vu1Code)
                markVU1CodeModified();
            return;
        }
    }
    if (isIoRegister(physAddr))
    {
        uint32_t regAddr = physAddr & ~0x3;
        uint32_t shift = (physAddr & 2) * 8;
        uint32_t mask = ~(0xFFFF << shift);
        uint32_t newValue = (m_ioRegisters[regAddr] & mask) | ((uint32_t)value << shift);
        writeIORegister(regAddr, newValue);
    }
}

void PS2Memory::write32(uint32_t address, uint32_t value)
{
    if (address & 3)
    {
        throw std::runtime_error("Unaligned 32-bit write at address: 0x" + std::to_string(address));
    }

    if (isGsPrivReg(address))
    {
        uint32_t off = address & 7;
        if (deferGsPrivWrite(address, static_cast<uint64_t>(value) << (off * 8), 0xFFFFFFFFULL << (off * 8)))
            return;
        gifVifSync();
        const uint32_t regOff = (address - PS2_GS_PRIV_REG_BASE) & ~0x7u;
        if (regOff == kGsCsrRegOffset)
        {
            // CSR: bits 0..1 of the low dword are write-one-to-clear status bits.
            // Done as a single atomic RMW -- see writeCsrHalf's comment.
            writeCsrHalf(gs_regs.csr, off, value);
        }
        else if (uint64_t *reg = gsRegPtr(gs_regs, address))
        {
            uint64_t mask = 0xFFFFFFFFULL << (off * 8);
            uint64_t newVal = (*reg & ~mask) | ((uint64_t)value << (off * 8));
            *reg = newVal;
        }
        return;
    }

    const bool scratch = isScratchpad(address);
    uint32_t physAddr = translateAddress(address);

    if (scratch)
    {
        storeScalar<uint32_t>(m_scratchpad, physAddr, PS2_SCRATCHPAD_SIZE, value, "write32 scratchpad", address);
    }
    else if (physAddr < PS2_RAM_SIZE)
    {
        // Check if this might be code modification
        markModified(address, 4);

        storeScalar<uint32_t>(m_rdram, physAddr, PS2_RAM_SIZE, value, "write32 rdram", address);
    }
    else
    {
        uint32_t vuOffset = 0;
        uint32_t vuLimit = 0;
        if (uint8_t *vuMem = mapVuMemory(physAddr, sizeof(uint32_t), vuOffset, vuLimit))
        {
            storeScalar<uint32_t>(vuMem, vuOffset, vuLimit, value, "write32 vu", address);
            if (vuMem == m_vu0Code)
                markVU0CodeModified();
            else if (vuMem == m_vu1Code)
                markVU1CodeModified();
            return;
        }
    }
    if (isIoRegister(physAddr))
    {
        writeIORegister(physAddr, value);
    }
}

void PS2Memory::write64(uint32_t address, uint64_t value)
{
    if (address & 7)
    {
        throw std::runtime_error("Unaligned 64-bit write at address: 0x" + std::to_string(address));
    }

    if (isGsPrivReg(address))
    {
        if (deferGsPrivWrite(address, value, ~0ULL))
            return;
        gifVifSync();
        const uint32_t regOff = (address - PS2_GS_PRIV_REG_BASE) & ~0x7u;
        if (regOff == kGsCsrRegOffset)
        {
            // CSR: bits 0..1 are write-one-to-clear status bits. Done as a single
            // atomic RMW -- see writeCsrFull's comment.
            writeCsrFull(gs_regs.csr, value);
        }
        else if (uint64_t *reg = gsRegPtr(gs_regs, address))
        {
            *reg = value;
        }
        return;
    }

    const bool scratch = isScratchpad(address);
    uint32_t physAddr = translateAddress(address);

    if (scratch)
    {
        storeScalar<uint64_t>(m_scratchpad, physAddr, PS2_SCRATCHPAD_SIZE, value, "write64 scratchpad", address);
    }
    else if (physAddr < PS2_RAM_SIZE)
    {
        markModified(address, 8);
        storeScalar<uint64_t>(m_rdram, physAddr, PS2_RAM_SIZE, value, "write64 rdram", address);
    }
    else
    {
        uint32_t vuOffset = 0;
        uint32_t vuLimit = 0;
        if (uint8_t *vuMem = mapVuMemory(physAddr, sizeof(uint64_t), vuOffset, vuLimit))
        {
            storeScalar<uint64_t>(vuMem, vuOffset, vuLimit, value, "write64 vu", address);
            if (vuMem == m_vu0Code)
                markVU0CodeModified();
            else if (vuMem == m_vu1Code)
                markVU1CodeModified();
            return;
        }
    }
    if (isIoRegister(physAddr))
    {
        write32(address, (uint32_t)value);
        write32(address + 4, (uint32_t)(value >> 32));
    }
}

void PS2Memory::write128(uint32_t address, __m128i value)
{
    // EE SQ ignores the low 4 bits of the effective address, like LQ (upstream PR #400).
    address &= ~0xFu;

    const bool scratch = isScratchpad(address);
    uint32_t physAddr = translateAddress(address);

    if (!scratch && physAddr == 0x10007010u) // IPU_in_FIFO
    {
        alignas(16) uint8_t fifoData[16];
        _mm_storeu_si128(reinterpret_cast<__m128i *>(fifoData), value);
        m_ipu.writeInFifo(fifoData, sizeof(fifoData));
        pumpIpu();
        return;
    }
    if (!scratch && physAddr == 0x10004000u) // VIF0_FIFO
    {
        alignas(16) uint8_t fifoData[16];
        _mm_storeu_si128(reinterpret_cast<__m128i *>(fifoData), value);
        processVIF0Data(fifoData, sizeof(fifoData));
        return;
    }
    if (!scratch && physAddr == 0x10005000u) // VIF1_FIFO
    {
        alignas(16) uint8_t fifoData[16];
        _mm_storeu_si128(reinterpret_cast<__m128i *>(fifoData), value);
        gifVifSync();
        processVIF1Data(fifoData, sizeof(fifoData));
        return;
    }

    if (scratch)
    {
        inRange(physAddr, sizeof(__m128i), PS2_SCRATCHPAD_SIZE, "write128 scratchpad", address);
        _mm_storeu_si128(reinterpret_cast<__m128i *>(&m_scratchpad[physAddr]), value);
    }
    else if (physAddr < PS2_RAM_SIZE)
    {
        markModified(address, 16);
        inRange(physAddr, sizeof(__m128i), PS2_RAM_SIZE, "write128 rdram", address);
        _mm_storeu_si128(reinterpret_cast<__m128i *>(&m_rdram[physAddr]), value);
    }
    else
    {
        uint32_t vuOffset = 0;
        uint32_t vuLimit = 0;
        if (uint8_t *vuMem = mapVuMemory(physAddr, sizeof(__m128i), vuOffset, vuLimit))
        {
            inRange(vuOffset, sizeof(__m128i), vuLimit, "write128 vu", address);
            _mm_storeu_si128(reinterpret_cast<__m128i *>(vuMem + vuOffset), value);
            if (vuMem == m_vu0Code)
                markVU0CodeModified();
            else if (vuMem == m_vu1Code)
                markVU1CodeModified();
            return;
        }
    }
    if (isIoRegister(physAddr))
    {
        // Non-RAM 128-bit stores are modeled as two 64-bit stores.
        uint64_t lo = _mm_extract_epi64(value, 0);
        uint64_t hi = _mm_extract_epi64(value, 1);

        write64(address, lo);
        write64(address + 8, hi);
    }
}

bool PS2Memory::writeIORegister(uint32_t address, uint32_t value)
{
    if ((address & 0xFFFFF000u) == 0x10003000u)
        gifVifSync(); // GIF and VIF registers
    // INTC_STAT: writing 1 to a bit clears it (acknowledge), as on the EE. Storing the written value made
    // MotorStorm AE's VSync wait (ack VBLANK_START = 4, then poll INTC_STAT bit 2 or the SetVSyncFlag flag) return at
    // once; the kernel's later VSync write then landed in the dead stack frame and smashed a return address.
    // PS2X_INTC_STAT_PLAIN=1 restores the old plain-register behaviour.
    if (address == 0x1000F000u && !intcStatPlain())
    {
        m_intcStat.fetch_and(~value, std::memory_order_acq_rel);
        return true;
    }
    size_t timerIndex = 0u;
    uint32_t timerOffset = 0u;
    if (decodeEeTimerRegister(address, timerIndex, timerOffset))
    {
        EeTimer &timer = m_eeTimers[timerIndex];
        switch (timerOffset)
        {
        case kEeTimerCountOffset:
            timer.count = value & 0xFFFFu;
            timer.clockRemainder = 0u;
            break;
        case kEeTimerModeOffset:
        {
            const uint32_t previousMode = timer.mode;
            const uint32_t status = (previousMode & kEeTimerModeStatusMask) & ~(value & kEeTimerModeStatusMask);
            timer.mode = (value & kEeTimerModeConfigMask) | status;
            if (((previousMode ^ timer.mode) & (kEeTimerModeClksMask | kEeTimerModeCue)) != 0u)
            {
                timer.clockRemainder = 0u;
            }
            break;
        }
        case kEeTimerCompareOffset:
            timer.compare = value & 0xFFFFu;
            break;
        case kEeTimerHoldOffset:
            timer.hold = value & 0xFFFFu;
            break;
        default:
            return false;
        }
        return true;
    }

    if (isGsPrivReg(address))
    {
        gifVifSync();
        // NB: unreachable from write8/16/32/64 today since those all funnel IO
        // register writes through addresses in PS2_IO_BASE's range, which is
        // disjoint from PS2_GS_PRIV_REG_BASE; kept correct for direct callers.
        m_ioRegisters[address] = value;
        const uint32_t off = address & 7u;
        const uint32_t regOff = (address - PS2_GS_PRIV_REG_BASE) & ~0x7u;
        if (regOff == kGsCsrRegOffset)
        {
            writeCsrHalf(gs_regs.csr, off, value);
        }
        else if (uint64_t *reg = gsRegPtr(gs_regs, address))
        {
            const uint64_t mask = 0xFFFFFFFFull << (off * 8u);
            *reg = (*reg & ~mask) | (static_cast<uint64_t>(value) << (off * 8u));
        }
        m_gsWriteCount.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    // PS2X_IPU_TRACE=1: log IPU register writes and IPU DMA kicks (channels 3 FROM_IPU, 4 TO_IPU).
    {
        static const bool ipuTrace = std::getenv("PS2X_IPU_TRACE") != nullptr;
        static unsigned ipuLogs = 0;
        if (ipuTrace && ipuLogs < 20000u &&
            ((address >= 0x10002000u && address <= 0x10002030u) || address == 0x1000B000u || address == 0x1000B400u ||
             address == 0x1000B010u || address == 0x1000B020u || address == 0x1000B410u || address == 0x1000B420u ||
             address == 0x1000B430u))
        {
            ++ipuLogs;
            std::fprintf(stderr, "[ipu:w] reg=%08x value=%08x\n", address, value);
        }
    }
    if (address == 0x1000A000u && (value & 0x100u) != 0u)
    {
        m_gifStatBusyReads.store(2, std::memory_order_relaxed);
        static const bool snapTadr = std::getenv("PS2X_VIF1_IRQ_SNAP_TADR") != nullptr; // experiment, worse in MSAE
        const uint32_t stalledTadr = g_irqGifTadr.load(std::memory_order_acquire);
        if (snapTadr && ((value >> 2u) & 3u) == 1u && stalledTadr != 0u && m_vif1IrqStall.load(std::memory_order_acquire))
            m_ioRegisters[0x1000A030u] = stalledTadr;
    }
    // PS2X_GIFCH_TRACE=1: GIF channel (2) and DMAC control register writes.
    {
        static const bool gifchTrace = std::getenv("PS2X_GIFCH_TRACE") != nullptr;
        static unsigned gifchLogs = 0;
        if (gifchTrace && gifchLogs < 200000u &&
            ((address >= 0x1000A000u && address <= 0x1000A080u) || address == 0x1000E000u || address == 0x1000E010u ||
             address == 0x1000F590u || address == 0x10003000u || address == 0x10003010u))
        {
            ++gifchLogs;
            std::fprintf(stderr, "[gifch:w] reg=%08x value=%08x\n", address, value);
        }
    }
    static const bool ipuStub = std::getenv("PS2X_IPU_STUB") != nullptr; // old register-only behaviour
    if (!ipuStub && address >= 0x10002000u && address <= 0x10002030u)
    {
        if (address == 0x10002000u)
            m_ipu.writeCmd(value);
        else if (address == 0x10002010u)
            m_ipu.writeCtrl(value);
        pumpIpu();
        return true;
    }
    if (address >= 0x10002000 && address <= 0x10002030)
    {
        if (address == 0x10002010)
        {
            m_ioRegisters[address] = value & ~(1u << 31);
            if (value & (1u << 30))
            {
                m_ioRegisters[0x10002000] = 0;
                m_ioRegisters[0x10002020] = 0;
                m_ioRegisters[0x10002030] = 0;
            }
        }
        else
        {
            m_ioRegisters[address] = value;
        }
        return true;
    }

    if (address == 0x1000E010u)
    {
        const uint32_t current = m_ioRegisters.count(address) ? m_ioRegisters[address] : 0u;
        uint32_t status = current & 0x3FFu;
        uint32_t mask = (current >> 16) & 0x3FFu;

        // D_STAT low bits are W1C status, high bits [16..25] toggle masks on write-one.
        status &= ~(value & 0x3FFu);
        mask ^= ((value >> 16) & 0x3FFu);

        uint32_t next = (current & ~((0x3FFu) | (0x3FFu << 16) | (1u << 31)));
        next |= status | (mask << 16);
        if ((status & mask) != 0u)
            next |= (1u << 31);
        m_ioRegisters[address] = next;
        return true;
    }

    m_ioRegisters[address] = value;

    if (address >= 0x10003C00u && address < 0x10003E00u)
    {
        m_vifWriteCount.fetch_add(1, std::memory_order_relaxed);

        switch (address)
        {
        case 0x10003C10u:     // VIF1_FBRST
            if (value & 0x1u) // RST
            {
                const bool wasPath3Masked = m_path3Masked;
                std::memset(&vif1_regs, 0, sizeof(vif1_regs));
                m_vif1PendingPath2ImageQwc = 0u;
                m_vif1PendingPath2DirectHl = false;
                m_vif1Carry.clear();
                m_path3Masked = false;
                if (wasPath3Masked)
                    flushMaskedPath3Packets();
            }
            if (value & 0x8u) // STC
            {
                m_vif1IrqStall.store(false, std::memory_order_release);
                vif1_regs.stat &= ~((1u << 8) | (1u << 9) | (1u << 10) | (1u << 11) | (1u << 12) | (1u << 13));
            }
            break;
        case 0x10003C30u:
            vif1_regs.mark = value & 0xFFFFu;
            vif1_regs.stat &= ~(1u << 6); // clear MRK flag on CPU write
            break;
        case 0x10003C40u:
            vif1_regs.cycle = value & 0xFFFFu;
            break;
        case 0x10003C50u:
            vif1_regs.mode = value & 0x3u;
            break;
        case 0x10003C60u:
            vif1_regs.num = value & 0xFFu;
            break;
        case 0x10003C70u:
            vif1_regs.mask = value;
            break;
        case 0x10003C80u:
            vif1_regs.code = value;
            break;
        case 0x10003C90u:
            vif1_regs.itops = value & 0x3FFu;
            break;
        case 0x10003CA0u:
            vif1_regs.base = value & 0x3FFu;
            break;
        case 0x10003CB0u:
            vif1_regs.ofst = value & 0x3FFu;
            break;
        case 0x10003CC0u:
            vif1_regs.tops = value & 0x3FFu;
            break;
        case 0x10003CD0u:
            vif1_regs.itop = value & 0x3FFu;
            break;
        case 0x10003CE0u:
            vif1_regs.top = value & 0x3FFu;
            break;
        default:
            break;
        }

        return true;
    }

    if (address >= 0x10003800u && address < 0x10003A00u)
    {
        m_vifWriteCount.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    if (address >= 0x10008000 && address < 0x1000F000)
    {
        if ((address & 0xFF) == 0x00 && (value & 0x100))
        {
            const auto dctrlIt = m_ioRegisters.find(0x1000E000u);
            const bool dmacEnabled = (dctrlIt == m_ioRegisters.end()) || ((dctrlIt->second & 0x1u) != 0u);
            // PS2X_DMA_STATS=1: once per second, how many times each DMA channel was started (and its last CHCR,
            // MADR/TADR/QWC) and the D_CTRL value; shows which paths a scene feeds (e.g. VIF1 idle, MFIFO in use).
            static const bool dmaStats = std::getenv("PS2X_DMA_STATS") != nullptr;
            if (dmaStats)
            {
                static std::atomic<uint32_t> starts[0x70]{};
                static uint32_t lastChcr[0x70]{}, lastMadr[0x70]{}, lastTadr[0x70]{}, lastQwc[0x70]{};
                static auto last = std::chrono::steady_clock::now();
                static std::mutex statsMutex;
                const uint32_t ch = ((address - 0x10008000u) >> 8) & 0x7Fu; // 0x00 VIF0, 0x10 VIF1, 0x20 GIF .. 0x54 SPR_TO
                starts[ch].fetch_add(1u, std::memory_order_relaxed);
                std::lock_guard<std::mutex> lock(statsMutex);
                const uint32_t base = address & 0xFFFFFF00u;
                lastChcr[ch] = value;
                lastMadr[ch] = m_ioRegisters[base + 0x10u];
                lastQwc[ch] = m_ioRegisters[base + 0x20u];
                lastTadr[ch] = m_ioRegisters[base + 0x30u];
                // Creating analysis/dma-log.trigger logs the next 200 channel starts one by one.
                static uint32_t logLeft = 0, checkTick = 0;
                if (logLeft == 0u && (checkTick++ & 63u) == 0u && std::filesystem::exists("analysis/dma-log.trigger"))
                {
                    std::filesystem::remove("analysis/dma-log.trigger");
                    logLeft = 200u;
                }
                if (logLeft)
                {
                    --logLeft;
                    std::fprintf(stderr, "[dma-log] %x chcr=%x madr=%x qwc=%x tadr=%x asr0=%x\n", base, value, lastMadr[ch],
                                 lastQwc[ch], lastTadr[ch], m_ioRegisters[base + 0x40u]);
                    if ((value & 0xCu) == 0x4u && m_rdram) // source chain: walk the tags without transferring
                    {
                        uint32_t t = lastTadr[ch], stack[2]{}, sp = 0;
                        for (int n = 0; n < 48; ++n)
                        {
                            const uint32_t phys = t & 0x1FFFFFF0u;
                            if ((t & 0x80000000u) || phys + 16u > PS2_RAM_SIZE)
                            {
                                std::fprintf(stderr, "    tag@%x (spr/out of range)\n", t);
                                break;
                            }
                            uint32_t w[4];
                            std::memcpy(w, m_rdram + phys, 16);
                            const uint32_t id = (w[0] >> 28) & 7u, q = w[0] & 0xFFFFu, addr = w[1];
                            std::fprintf(stderr, "    tag@%x id=%u qwc=%x addr=%x irq=%u vif=%08x %08x\n", t, id, q, addr, w[0] >> 31, w[2], w[3]);
                            if (id == 0u) { t = t + 16u + q * 16u; std::fprintf(stderr, "    (refe end)\n"); break; }
                            else if (id == 1u) t = t + 16u + q * 16u;
                            else if (id == 2u) t = addr;
                            else if (id == 3u || id == 4u) t = t + 16u;
                            else if (id == 5u) { if (sp < 2) stack[sp++] = t + 16u + q * 16u; t = addr; }
                            else if (id == 6u) { if (sp == 0) { std::fprintf(stderr, "    (ret end)\n"); break; } t = stack[--sp]; }
                            else { std::fprintf(stderr, "    (end)\n"); break; }
                        }
                    }
                }
                const auto now = std::chrono::steady_clock::now();
                if (now - last >= std::chrono::seconds(1))
                {
                    last = now;
                    std::fprintf(stderr, "[dma-stats] dctrl=%x", dctrlIt == m_ioRegisters.end() ? 0u : dctrlIt->second);
                    for (uint32_t i = 0; i < 0x70u; ++i)
                    {
                        const uint32_t n = starts[i].exchange(0u);
                        if (n)
                            std::fprintf(stderr, " %x:%u(chcr=%x madr=%x qwc=%x tadr=%x)", 0x8000u + i * 0x100u, n, lastChcr[i],
                                         lastMadr[i], lastQwc[i], lastTadr[i]);
                    }
                    std::fprintf(stderr, "\n");
                }
            }
            if (!dmacEnabled)
            {
                return true;
            }

            const uint32_t channelBase = address & 0xFFFFFF00;
            static const bool ipuStubDma = std::getenv("PS2X_IPU_STUB") != nullptr;
            if (!ipuStubDma && channelBase == 0x1000B400u) // TO_IPU: the whole block enters the IPU input FIFO
            {
                const uint32_t src = ps2DmaAddress(m_ioRegisters[0x1000B410u]);
                const uint32_t qwc = m_ioRegisters[0x1000B420u] & 0xFFFFu;
                m_ioRegisters[address] = value;
                if (m_rdram && src < PS2_RAM_SIZE && qwc)
                {
                    const uint32_t bytes = std::min<uint32_t>(qwc * 16u, PS2_RAM_SIZE - src);
                    m_ioRegisters[0x1000B410u] += qwc * 16u;
                    m_ioRegisters[0x1000B420u] = 0u;
                    m_ipu.writeInFifo(m_rdram + src, bytes);
                }
                completeDmacChannel(0x1000B400u, 4u);
                pumpIpu();
                return true;
            }
            if (!ipuStubDma && channelBase == 0x1000B000u) // FROM_IPU: filled as the IPU produces output
            {
                m_ioRegisters[address] = value;
                pumpIpu();
                return true;
            }
            if (Ps2GifOrderTraceEnabled() && (channelBase == 0x1000A000u || channelBase == 0x10009000u))
            {
                auto peek = [&](uint32_t reg) { const auto it = m_ioRegisters.find(reg); return it == m_ioRegisters.end() ? 0u : it->second; };
                char detail[160];
                std::snprintf(detail, sizeof(detail), "{\"channel\":%u,\"chcr\":%u,\"madr\":%u,\"qwc\":%u,\"tadr\":%u}",
                    channelBase, value, peek(channelBase + 0x10), peek(channelBase + 0x20), peek(channelBase + 0x30));
                Ps2TraceGifOrder("dma-start", channelBase == 0x1000A000u ? 3u : 2u, nullptr, 0u, this,
                    m_pendingGifTransfers.size(), m_pendingVif1Transfers.size(), detail);
            }
            // A real GIF channel cannot start a new list while the old one is held
            // by MSKPATH3, so a kick means the guest abandoned the unsent remainder.
            // Ben 10 builds one list for every material but opens gates only for the
            // ones it draws; keeping the rest would feed last frame's materials to
            // this frame's gates.
            static const bool asyncPaths = std::getenv("PS2X_ASYNC_PATHS") != nullptr;
            if (channelBase == 0x1000A000u && !asyncPaths)
                gifVifSync();
            if (channelBase == 0x1000A000u && m_path3Masked && !asyncPaths)
            {
                m_path3MaskedFifo.clear();
                resetPath3Stream();
            }

            const uint32_t madr = ps2DmaAddress(m_ioRegisters[channelBase + 0x10]);
            const uint32_t qwc = m_ioRegisters[channelBase + 0x20];
            m_dmaStartCount.fetch_add(1, std::memory_order_relaxed);
            PS2TL(TL_KICK, channelBase, qwc);

            if (tryProcessScratchpadDma(channelBase, value))
            {
                return true;
            }

            if ((channelBase == 0x1000A000u || channelBase == 0x10009000u || channelBase == 0x10008000u) && (m_gsVRAM || channelBase == 0x10008000u))
            {
                auto enqueueTransfer = [&](uint32_t srcAddr, uint32_t qwCount)
                {
                    if (qwCount == 0)
                        return;
                    const bool scratch = isScratchpad(srcAddr);
                    PendingTransfer pt;
                    pt.fromScratchpad = scratch;
                    pt.srcAddr = srcAddr;
                    pt.qwc = qwCount;
                    if (channelBase == 0x1000A000u)
                        m_pendingGifTransfers.push_back(pt);
                    else if (channelBase == 0x10009000u)
                        m_pendingVif1Transfers.push_back(pt);
                    else if (channelBase == 0x10008000u)
                        m_pendingVif0Transfers.push_back(pt);
                };

                uint32_t chcr = value;
                uint32_t mode = (chcr >> 2) & 0x3;
                auto &channelQueue = (channelBase == 0x1000A000u)   ? m_pendingGifTransfers
                                     : (channelBase == 0x10009000u) ? m_pendingVif1Transfers
                                                                    : m_pendingVif0Transfers;
                const size_t queuedBefore = channelQueue.size();

                if (mode == 0 && qwc > 0)
                {
                    enqueueTransfer(madr, qwc);
                    // The transfer completes at once here: MADR ends after the data (QWC is
                    // cleared on completion), as the DMAC leaves it.
                    m_ioRegisters[channelBase + 0x10] = (madr + qwc * 16u) & 0x7FFFFFF0u;
                }
                else if (mode == 1)
                {
                    uint32_t tagAddr = ps2DmaAddress(m_ioRegisters[channelBase + 0x30]);
                    uint32_t asr0 = ps2DmaAddress(m_ioRegisters[channelBase + 0x40]);
                    uint32_t asr1 = ps2DmaAddress(m_ioRegisters[channelBase + 0x50]);
                    uint32_t asp = (chcr >> 4) & 0x3u;
                    const bool tieEnabled = (chcr & (1u << 7)) != 0u;
                    // Hardware has no tag limit; this only stops a corrupted chain that loops
                    // forever. Ben 10 sends over 4,096 tags in one frame.
                    const int kMaxChainTags = 1 << 20;
                    std::vector<uint8_t> chainBuf;
                    static std::atomic<size_t> lastChainBytes{0u};
                    chainBuf.reserve(lastChainBytes.load(std::memory_order_relaxed) + 4096u);

                    auto appendData = [&](uint32_t srcAddr, uint32_t qwCount)
                    {
                        const uint64_t bytes64 = static_cast<uint64_t>(qwCount) * 16ull;
                        uint32_t bytes = (bytes64 > 0xFFFFFFFFull) ? 0xFFFFFFFFu : static_cast<uint32_t>(bytes64);
                        const bool scratch = isScratchpad(srcAddr);
                        uint32_t src = 0;
                        src = translateAddress(srcAddr);
                        const uint8_t *base2;
                        uint32_t maxSz2;
                        if (scratch)
                        {
                            base2 = m_scratchpad;
                            maxSz2 = PS2_SCRATCHPAD_SIZE;
                        }
                        else
                        {
                            base2 = m_rdram;
                            maxSz2 = PS2_RAM_SIZE;
                        }

                        while (bytes > 0)
                        {
                            if (src >= maxSz2)
                                src = 0;
                            uint32_t chunk = bytes;
                            if (src + chunk > maxSz2)
                                chunk = maxSz2 - src;
                            if (chunk == 0)
                                break;
                            // Source bytes before DMA chain concatenation loses guest addresses.
                            static int sourceLeft = std::getenv("PS2X_TEXUPLOAD_SOURCE_TRACE") ? std::atoi(std::getenv("PS2X_TEXUPLOAD_SOURCE_TRACE")) : 0;
                            if (sourceLeft > 0)
                            {
                                --sourceLeft;
                                uint32_t nonzero = 0;
                                uint64_t hash = 14695981039346656037ull;
                                for (uint32_t i = 0; i < chunk; ++i)
                                {
                                    nonzero += base2[src + i] != 0;
                                    hash = (hash ^ base2[src + i]) * 1099511628211ull;
                                }
                                std::fprintf(stderr, "[dma:source] channel=%x next_tag=%08x guest=%08x phys=%08x spr=%u offset=%zu bytes=%u nz=%u hash=%016llx\n",
                                    channelBase, tagAddr, srcAddr, src, static_cast<unsigned>(scratch), chainBuf.size(), chunk, nonzero,
                                    static_cast<unsigned long long>(hash));
                            }
                            chainBuf.insert(chainBuf.end(), base2 + src, base2 + src + chunk);
                            bytes -= chunk;
                            src += chunk;
                        }
                    };

                    auto appendVifTagData = [&](uint32_t localTagAddr)
                    {
                        uint32_t tagPhys = 0u;
                        const bool tagScratch = isScratchpad(localTagAddr);
                        tagPhys = translateAddress(localTagAddr);

                        const uint8_t *localBase = tagScratch ? m_scratchpad : m_rdram;
                        const uint32_t localMax = tagScratch ? PS2_SCRATCHPAD_SIZE : PS2_RAM_SIZE;
                        if (tagPhys + 16u > localMax)
                            return;

                        // CHCR.TTE sends the DMAtag's upper 64 bits to the channel before
                        // the tag payload. VIF chains use those bytes for two VIFcodes.
                        chainBuf.insert(chainBuf.end(), localBase + tagPhys + 8u, localBase + tagPhys + 16u);
                    };

                    const bool isVifChannel =
                        channelBase == 0x10009000u || channelBase == 0x10008000u;
                    const bool transferTagData = isVifChannel && (chcr & 0x40u) != 0u;

                    int tagsProcessed = 0;
                    uint32_t lastTagUpper = (chcr >> 16) & 0xFFFFu;
                    // MADR as the DMAC leaves it: the end of the last tag's payload (PCSX2 hwDmacSrcChain).
                    uint32_t endMadr = 0u;
                    bool endMadrValid = false;

                    while (tagsProcessed < kMaxChainTags)
                    {
                        const uint32_t currentTagAddr = tagAddr;
                        const bool tagInSPR = isScratchpad(tagAddr);
                        uint32_t physTag = 0;
                        try
                        {
                            physTag = translateAddress(tagAddr);
                        }
                        catch (...)
                        {
                            break;
                        }
                        const uint8_t *tagBase;
                        uint32_t tagMax;
                        if (tagInSPR)
                        {
                            tagBase = m_scratchpad;
                            tagMax = PS2_SCRATCHPAD_SIZE;
                        }
                        else
                        {
                            tagBase = m_rdram;
                            tagMax = PS2_RAM_SIZE;
                        }
                        if (physTag + 16 > tagMax)
                            break;

                        const uint8_t *tp = tagBase + physTag;
                        uint64_t tag = loadScalar<uint64_t>(tp, 0, 16, "dma chain tag", tagAddr);
                        uint16_t tagQwc = static_cast<uint16_t>(tag & 0xFFFF);
                        uint32_t id = static_cast<uint32_t>((tag >> 28) & 0x7);
                        const bool irq = ((tag >> 31) & 0x1ull) != 0ull;
                        uint32_t addr = ps2DmaAddress(static_cast<uint32_t>(tag >> 32));
                        if (addr >= 0x80000000u)
                            addr &= 0x7FFFFFFFu;
                        lastTagUpper = static_cast<uint32_t>((tag >> 16) & 0xFFFFu);
                        ++tagsProcessed;

                        uint32_t dataAddr = 0;
                        bool hasPayload = (tagQwc > 0);
                        bool endChain = false;

                        switch (id)
                        {
                        case 0:
                            dataAddr = addr;
                            tagAddr = tagAddr + 16;
                            endChain = true;
                            break;
                        case 1:
                            dataAddr = tagAddr + 16;
                            tagAddr = dataAddr + static_cast<uint32_t>(tagQwc) * 16u;
                            break;
                        case 2:
                            dataAddr = tagAddr + 16;
                            tagAddr = addr;
                            break;
                        case 3:
                        case 4:
                            dataAddr = addr;
                            tagAddr = tagAddr + 16;
                            break;
                        case 5:
                            dataAddr = tagAddr + 16;
                            {
                                const uint32_t retAddr = dataAddr + static_cast<uint32_t>(tagQwc) * 16u;
                                if (asp == 0u)
                                {
                                    asr0 = retAddr;
                                    asp = 1u;
                                }
                                else if (asp == 1u)
                                {
                                    asr1 = retAddr;
                                    asp = 2u;
                                }
                            }
                            tagAddr = addr;
                            break;
                        case 6:
                            dataAddr = tagAddr + 16;
                            if (asp == 2u)
                            {
                                tagAddr = asr1;
                                asp = 1u;
                            }
                            else if (asp == 1u)
                            {
                                tagAddr = asr0;
                                asp = 0u;
                            }
                            else
                            {
                                endChain = true;
                            }
                            break;
                        case 7:
                            dataAddr = tagAddr + 16;
                            endChain = true;
                            break;
                        default:
                            hasPayload = false;
                            endChain = true;
                            break;
                        }

                        if (transferTagData)
                            appendVifTagData(currentTagAddr);
                        if (id <= 7u)
                        {
                            endMadr = (dataAddr + static_cast<uint32_t>(tagQwc) * 16u) & 0x7FFFFFF0u;
                            endMadrValid = true;
                        }

                        if (hasPayload)
                            appendData(dataAddr, tagQwc);
                        if (irq && tieEnabled)
                            endChain = true;
                        if (endChain)
                            break;
                    }

                    if (tagsProcessed >= kMaxChainTags)
                    {
                        static std::atomic<bool> warned{false};
                        if (!warned.exchange(true, std::memory_order_relaxed))
                            std::cerr << "[dma] chain stopped at the tag limit on channel 0x" << std::hex << channelBase
                                      << " tadr=0x" << tagAddr << std::dec << std::endl;
                    }
                    m_ioRegisters[channelBase + 0x30] = tagAddr;
                    if (endMadrValid)
                        m_ioRegisters[channelBase + 0x10] = endMadr;
                    m_ioRegisters[channelBase + 0x40] = asr0;
                    m_ioRegisters[channelBase + 0x50] = asr1;
                    chcr = (chcr & ~(0x3u << 4)) | ((asp & 0x3u) << 4);
                    chcr = (chcr & 0x0000FFFFu) | (lastTagUpper << 16);
                    m_ioRegisters[channelBase + 0x00] = chcr;

                    if (!chainBuf.empty())
                    {
                        lastChainBytes.store(chainBuf.size(), std::memory_order_relaxed);
                        PendingTransfer pt;
                        pt.fromScratchpad = false;
                        pt.srcAddr = 0;
                        pt.qwc = 0;
                        pt.chainData = std::move(chainBuf);
                        if (channelBase == 0x1000A000)
                        {
                            m_pendingGifTransfers.push_back(std::move(pt));
                        }
                        else if (channelBase == 0x10009000u)
                        {
                            m_pendingVif1Transfers.push_back(std::move(pt));
                        }
                        else if (channelBase == 0x10008000u)
                        {
                            m_pendingVif0Transfers.push_back(std::move(pt));
                        }
                    }
                    // else if (channelBase == 0x10009000u)
                    // {

                    // }
                }
                else if (qwc > 0)
                {
                    enqueueTransfer(madr, qwc);
                }

                // A kick with nothing to send (QWC 0, or a chain of empty tags) still ends the transfer:
                // STR clears and the channel interrupt fires. GT4 sleeps until that interrupt.
                if (channelQueue.size() == queuedBefore)
                {
                    const uint32_t cause = (channelBase == 0x1000A000u) ? 2u : (channelBase == 0x10009000u) ? 1u : 0u;
                    completeDmacChannel(channelBase, cause);
                    m_ioRegisters[channelBase + 0x20] = 0;
                }
                const bool autoProcessTransfers =
                    (channelBase == 0x1000A000u) ? (m_gifPacketCallback || m_gifArbiter != nullptr) : true;
                if (autoProcessTransfers)
                {
                    processPendingTransfers();
                }
            }
        }
        return true;
    }

    if (address >= 0x10000000 && address < 0x10010000)
    {
        if (address >= 0x10000200 && address < 0x10000300)
        {
            return true;
        }
        if (address >= 0x10000000 && address < 0x10000100)
        {
            return true;
        }
    }

    return false;
}

bool PS2Memory::tryProcessScratchpadDma(uint32_t channelBase, uint32_t chcr)
{
    static constexpr uint32_t kSprFromChannel = 0x1000D000u;
    static constexpr uint32_t kSprToChannel = 0x1000D400u;
    if (channelBase != kSprFromChannel && channelBase != kSprToChannel)
        return false;

    const uint32_t mode = (chcr >> 2u) & 0x3u;
    if (mode == 1u && channelBase == kSprFromChannel)
    {
        // Destination chain (fromSPR): each DMAtag sits in the scratchpad and gives the RAM destination (ADDR) and
        // the qword count of the data that follows it. CNT/CNTS continue with the next tag after the data, END stops,
        // and so does a tag with IRQ when CHCR.TIE is set. MotorStorm AE's vertex morph loop (0x2866e0) returns its
        // packed positions and normals this way; without it every car, driver and rival mesh drew from zeroed buffers.
        uint32_t sadr = m_ioRegisters[channelBase + 0x80u] & 0x3FF0u;
        uint32_t madr = m_ioRegisters[channelBase + 0x10u] & 0x7FFFFFF0u;
        uint32_t lastTag = 0u;
        for (unsigned guard = 0; guard < 1024u; ++guard)
        {
            uint32_t tag[2];
            std::memcpy(tag, m_scratchpad + sadr, sizeof(tag));
            lastTag = tag[0];
            sadr = (sadr + 16u) & (PS2_SCRATCHPAD_SIZE - 1u);
            const uint32_t tagQwc = tag[0] & 0xFFFFu;
            const uint32_t id = (tag[0] >> 28u) & 0x7u;
            madr = tag[1] & 0x7FFFFFF0u;
            uint32_t mainOffset = 0u;
            try
            {
                mainOffset = translateAddress(madr);
            }
            catch (const std::exception &)
            {
                break;
            }
            for (uint32_t q = 0; q < tagQwc && mainOffset + 16u <= PS2_RAM_SIZE; ++q)
            {
                std::memcpy(m_rdram + mainOffset, m_scratchpad + sadr, 16u);
                markModified(mainOffset, 16u);
                mainOffset += 16u;
                sadr = (sadr + 16u) & (PS2_SCRATCHPAD_SIZE - 1u);
            }
            madr += tagQwc * 16u;
            if (id == 7u || (id != 0u && id != 1u) || ((chcr & 0x80u) && (tag[0] & 0x80000000u)))
                break;
        }
        m_ioRegisters[channelBase] = (m_ioRegisters[channelBase] & 0x0000FFFFu) | (lastTag & 0xFFFF0000u);
        m_ioRegisters[channelBase + 0x10u] = madr;
        m_ioRegisters[channelBase + 0x20u] = 0u;
        m_ioRegisters[channelBase + 0x80u] = sadr;
        completeDmacChannel(channelBase, 8u);
        return true;
    }
    if (mode != 0u)
    {
        static bool warned[2] = {};
        bool &w = warned[channelBase == kSprFromChannel ? 0 : 1];
        if (!w)
        {
            w = true;
            std::fprintf(stderr, "[dma] scratchpad channel 0x%x mode %u is not supported (chcr=0x%x)\n", channelBase, mode, chcr);
        }
        return false;
    }

    const uint32_t qwc = m_ioRegisters[channelBase + 0x20u] & 0xFFFFu;
    const uint32_t byteCount = qwc * 16u;
    const uint32_t originalMadr = m_ioRegisters[channelBase + 0x10u] & 0x7FFFFFF0u;
    const uint32_t originalSadr = m_ioRegisters[channelBase + 0x80u] & 0x3FF0u;

    uint32_t mainOffset = 0u;
    try
    {
        mainOffset = translateAddress(originalMadr);
    }
    catch (const std::exception &)
    {
        return false;
    }

    if (mainOffset > PS2_RAM_SIZE || byteCount > PS2_RAM_SIZE - mainOffset)
        return false;

    const bool fromScratchpad = channelBase == kSprFromChannel;
    uint32_t scratchOffset = originalSadr;
    uint32_t bytesLeft = byteCount;
    uint32_t copied = 0u;
    while (bytesLeft != 0u)
    {
        const uint32_t scratchChunk = PS2_SCRATCHPAD_SIZE - scratchOffset;
        const uint32_t chunk = std::min(bytesLeft, scratchChunk);
        if (fromScratchpad)
        {
            std::memcpy(m_rdram + mainOffset + copied, m_scratchpad + scratchOffset, chunk);
            markModified(mainOffset + copied, chunk);
        }
        else
        {
            std::memcpy(m_scratchpad + scratchOffset, m_rdram + mainOffset + copied, chunk);
        }

        copied += chunk;
        bytesLeft -= chunk;
        scratchOffset = (scratchOffset + chunk) & (PS2_SCRATCHPAD_SIZE - 1u);
    }

    m_ioRegisters[channelBase + 0x10u] = (originalMadr + byteCount) & 0x7FFFFFF0u;
    m_ioRegisters[channelBase + 0x20u] = 0u;
    m_ioRegisters[channelBase + 0x80u] = (originalSadr + byteCount) & 0x3FF0u;
    completeDmacChannel(channelBase, fromScratchpad ? 8u : 9u);
    return true;
}

void PS2Memory::pumpIpu()
{
    const uint32_t chcr = m_ioRegisters.count(0x1000B000u) ? m_ioRegisters[0x1000B000u] : 0u;
    if ((chcr & 0x100u) == 0u || !m_rdram)
        return;
    uint32_t qwc = m_ioRegisters[0x1000B020u] & 0xFFFFu;
    uint32_t dst = ps2DmaAddress(m_ioRegisters[0x1000B010u]);
    const uint32_t ready = static_cast<uint32_t>(std::min<size_t>(qwc, m_ipu.outputBytes() / 16u));
    if (ready && dst < PS2_RAM_SIZE)
    {
        const uint32_t bytes = std::min<uint32_t>(ready * 16u, PS2_RAM_SIZE - dst);
        m_ipu.takeOutput(m_rdram + dst, bytes);
        m_ioRegisters[0x1000B010u] += ready * 16u;
        qwc -= ready;
        m_ioRegisters[0x1000B020u] = qwc;
    }
    if (qwc == 0u)
        completeDmacChannel(0x1000B000u, 3u);
}

void PS2Memory::completeDmacChannel(uint32_t channelBase, uint32_t cause)
{
    static constexpr uint32_t kDStat = 0x1000E010u;
    m_ioRegisters[channelBase] &= ~0x100u;

    uint32_t dstat = m_ioRegisters.count(kDStat) ? m_ioRegisters[kDStat] : 0u;
    dstat |= 1u << cause;
    const uint32_t status = dstat & 0x3FFu;
    const uint32_t mask = (dstat >> 16u) & 0x3FFu;
    if ((status & mask) != 0u)
        dstat |= 1u << 31u;
    else
        dstat &= ~(1u << 31u);
    m_ioRegisters[kDStat] = dstat;
    queueCompletedDmacCause(cause);
}

void PS2Memory::materializeTransfer(PendingTransfer &p)
{
    if (!p.chainData.empty() || p.qwc == 0u)
        return;
    uint32_t srcPhys = 0;
    try
    {
        srcPhys = translateAddress(p.srcAddr);
    }
    catch (const std::exception &)
    {
        p.qwc = 0u;
        return;
    }
    const uint32_t limit = p.fromScratchpad ? PS2_SCRATCHPAD_SIZE : PS2_RAM_SIZE;
    const uint8_t *base = p.fromScratchpad ? m_scratchpad : m_rdram;
    size_t left = static_cast<size_t>(p.qwc) * 16u;
    p.chainData.reserve(left);
    while (left != 0u)
    {
        if (srcPhys >= limit)
            srcPhys = 0;
        const size_t chunk = std::min<size_t>(left, limit - srcPhys);
        p.chainData.insert(p.chainData.end(), base + srcPhys, base + srcPhys + chunk);
        left -= chunk;
        srcPhys += static_cast<uint32_t>(chunk);
    }
}

void PS2Memory::runGifTransfers(std::vector<PendingTransfer> &transfers)
{
    for (size_t idx = 0; idx < transfers.size(); ++idx)
    {
        auto &p = transfers[idx];
        if (!p.chainData.empty())
        {
            m_seenGifCopy = true;
            m_gifCopyCount.fetch_add(1, std::memory_order_relaxed);
            submitGifPacket(GifPathId::Path3, p.chainData.data(), static_cast<uint32_t>(p.chainData.size()), false);
        }
        else if (p.qwc > 0)
        {
            const uint64_t bytes64 = static_cast<uint64_t>(p.qwc) * 16ull;
            uint32_t sizeBytes = (bytes64 > 0xFFFFFFFFull) ? 0xFFFFFFFFu : static_cast<uint32_t>(bytes64);
            uint32_t srcPhys = 0;
            try
            {
                srcPhys = translateAddress(p.srcAddr);
            }
            catch (const std::exception &)
            {
                continue;
            }
            if (p.fromScratchpad)
            {
                uint32_t bytesLeft = sizeBytes;
                while (bytesLeft >= 16)
                {
                    if (srcPhys >= PS2_SCRATCHPAD_SIZE)
                        srcPhys = 0;
                    uint32_t chunk = bytesLeft;
                    if (srcPhys + chunk > PS2_SCRATCHPAD_SIZE)
                        chunk = PS2_SCRATCHPAD_SIZE - srcPhys;
                    if (chunk == 0)
                        break;
                    m_seenGifCopy = true;
                    m_gifCopyCount.fetch_add(1, std::memory_order_relaxed);
                    submitGifPacket(GifPathId::Path3, m_scratchpad + srcPhys, chunk, false);
                    bytesLeft -= chunk;
                    srcPhys += chunk;
                }
            }
            else
            {
                uint32_t bytesLeft = sizeBytes;
                while (bytesLeft >= 16)
                {
                    if (srcPhys >= PS2_RAM_SIZE)
                        srcPhys = 0;
                    uint32_t chunk = bytesLeft;
                    if (srcPhys + chunk > PS2_RAM_SIZE)
                        chunk = PS2_RAM_SIZE - srcPhys;
                    if (chunk == 0)
                        break;
                    m_seenGifCopy = true;
                    m_gifCopyCount.fetch_add(1, std::memory_order_relaxed);
                    submitGifPacket(GifPathId::Path3, m_rdram + srcPhys, chunk, false);
                    bytesLeft -= chunk;
                    srcPhys += chunk;
                }
            }
        }
    }
    transfers.clear();
}

void PS2Memory::runVif1Transfers(std::vector<PendingTransfer> &transfers)
{
    for (auto &p : transfers)
    {
        if (!p.chainData.empty())
        {
            processVIF1Data(p.chainData.data(), static_cast<uint32_t>(p.chainData.size()));
        }
        else if (p.qwc > 0)
        {
            uint32_t srcPhys = 0;
            const uint64_t bytes64 = static_cast<uint64_t>(p.qwc) * 16ull;
            uint32_t sizeBytes = (bytes64 > 0xFFFFFFFFull) ? 0xFFFFFFFFu : static_cast<uint32_t>(bytes64);
            try
            {
                srcPhys = translateAddress(p.srcAddr);
            }
            catch (const std::exception &)
            {
                continue;
            }
            if (p.fromScratchpad)
            {
                uint32_t bytesLeft = sizeBytes;
                while (bytesLeft > 0)
                {
                    if (srcPhys >= PS2_SCRATCHPAD_SIZE)
                        srcPhys = 0;
                    uint32_t chunk = bytesLeft;
                    if (srcPhys + chunk > PS2_SCRATCHPAD_SIZE)
                        chunk = PS2_SCRATCHPAD_SIZE - srcPhys;
                    if (chunk == 0)
                        break;
                    processVIF1Data(m_scratchpad + srcPhys, chunk);
                    bytesLeft -= chunk;
                    srcPhys += chunk;
                }
            }
            else
            {
                uint32_t bytesLeft = sizeBytes;
                while (bytesLeft > 0)
                {
                    if (srcPhys >= PS2_RAM_SIZE)
                        srcPhys = 0;
                    uint32_t chunk = bytesLeft;
                    if (srcPhys + chunk > PS2_RAM_SIZE)
                        chunk = PS2_RAM_SIZE - srcPhys;
                    if (chunk == 0)
                        break;
                    processVIF1Data(srcPhys, chunk);
                    bytesLeft -= chunk;
                    srcPhys += chunk;
                }
            }
        }
    }
    transfers.clear();
}

void PS2Memory::processPendingTransfers()
{
    if (!m_pendingGifTransfers.empty() || !m_pendingVif0Transfers.empty() || !m_pendingVif1Transfers.empty())
        Ps2TraceGifOrder("pending-begin", 0u, nullptr, 0u, this,
            m_pendingGifTransfers.size(), m_pendingVif1Transfers.size(), "{}");
    const bool hadGif = !m_pendingGifTransfers.empty();
    uint32_t observedGifQwc = 0u;
    for (const auto &transfer : m_pendingGifTransfers)
    {
        const uint64_t transferQwc = !transfer.chainData.empty()
                                         ? (transfer.chainData.size() / 16u)
                                         : transfer.qwc;
        observedGifQwc = static_cast<uint32_t>(std::min<uint64_t>(16u, static_cast<uint64_t>(observedGifQwc) + transferQwc));
    }
    if (observedGifQwc != 0u)
    {
        constexpr uint32_t kGifStat = 0x10003020u;
        constexpr uint32_t kGifFqcMask = 0x1F000000u;
        uint32_t &gifStat = m_ioRegisters[kGifStat];
        gifStat = (gifStat & ~kGifFqcMask) | (observedGifQwc << 24u);
    }

    const bool toWorker = g_vifWorker.enabled.load(std::memory_order_acquire) && !t_onVifWorker;
    if (!toWorker)
        runGifTransfers(m_pendingGifTransfers);

    const bool hadVif0 = !m_pendingVif0Transfers.empty();
    for (auto &p : m_pendingVif0Transfers)
    {
        if (!p.chainData.empty())
        {
            processVIF0Data(p.chainData.data(), static_cast<uint32_t>(p.chainData.size()));
        }
        else if (p.qwc > 0)
        {
            uint32_t srcPhys = 0;
            const uint64_t bytes64 = static_cast<uint64_t>(p.qwc) * 16ull;
            uint32_t sizeBytes = (bytes64 > 0xFFFFFFFFull) ? 0xFFFFFFFFu : static_cast<uint32_t>(bytes64);
            try
            {
                srcPhys = translateAddress(p.srcAddr);
            }
            catch (const std::exception &)
            {
                continue;
            }
            if (p.fromScratchpad)
            {
                uint32_t bytesLeft = sizeBytes;
                while (bytesLeft > 0)
                {
                    if (srcPhys >= PS2_SCRATCHPAD_SIZE)
                        srcPhys = 0;
                    uint32_t chunk = bytesLeft;
                    if (srcPhys + chunk > PS2_SCRATCHPAD_SIZE)
                        chunk = PS2_SCRATCHPAD_SIZE - srcPhys;
                    if (chunk == 0)
                        break;
                    processVIF0Data(m_scratchpad + srcPhys, chunk);
                    bytesLeft -= chunk;
                    srcPhys += chunk;
                }
            }
            else
            {
                uint32_t bytesLeft = sizeBytes;
                while (bytesLeft > 0)
                {
                    if (srcPhys >= PS2_RAM_SIZE)
                        srcPhys = 0;
                    uint32_t chunk = bytesLeft;
                    if (srcPhys + chunk > PS2_RAM_SIZE)
                        chunk = PS2_RAM_SIZE - srcPhys;
                    if (chunk == 0)
                        break;
                    processVIF0Data(srcPhys, chunk);
                    bytesLeft -= chunk;
                    srcPhys += chunk;
                }
            }
        }
    }
    m_pendingVif0Transfers.clear();

    const bool hadVif1 = !m_pendingVif1Transfers.empty();
    if (toWorker)
    {
        if (hadGif || hadVif1)
        {
            // The guest may reuse the source memory as soon as its DMA completes, which it does right below.
            VifJob job;
            job.gif = std::move(m_pendingGifTransfers);
            job.vif1 = std::move(m_pendingVif1Transfers);
            for (PendingTransfer &transfer : job.gif)
                materializeTransfer(transfer);
            for (PendingTransfer &transfer : job.vif1)
                materializeTransfer(transfer);
            job.fbrst = g_vifWorker.fbrstSource ? g_vifWorker.fbrstSource() : 0u;
            {
                const auto tadrIt = m_ioRegisters.find(0x1000A030u);
                job.gifTadr = tadrIt == m_ioRegisters.end() ? 0u : tadrIt->second;
            }
            job.duringIrqStall = m_vif1IrqStall.load(std::memory_order_acquire);
            m_pendingGifTransfers.clear();
            m_pendingVif1Transfers.clear();
            t_vifProducer = true;
            if (g_vifWorker.submitted.load(std::memory_order_relaxed) - g_vifWorker.done.load(std::memory_order_acquire) >= kMaxQueuedVifJobs)
                gifVifSync();
            g_vifWorker.submitted.fetch_add(1u, std::memory_order_relaxed);
            {
                std::lock_guard<std::mutex> lock(g_vifWorker.mutex);
                g_vifWorker.queue.push_back(std::move(job));
            }
            g_vifWorker.work.notify_one();
        }
    }
    else
    {
        runVif1Transfers(m_pendingVif1Transfers);
        if (m_gifArbiter)
            m_gifArbiter->drain();
    }

    static constexpr uint32_t GIF_CHANNEL = 0x1000A000;
    static constexpr uint32_t VIF0_CHANNEL = 0x10008000;
    static constexpr uint32_t VIF1_CHANNEL = 0x10009000;
    static constexpr uint32_t D_STAT = 0x1000E010u;

    auto raiseDStatChannel = [&](uint32_t channelBit)
    {
        uint32_t dstat = m_ioRegisters.count(D_STAT) ? m_ioRegisters[D_STAT] : 0u;
        dstat |= (1u << channelBit);

        const uint32_t status = dstat & 0x3FFu;
        const uint32_t mask = (dstat >> 16) & 0x3FFu;
        if ((status & mask) != 0u)
            dstat |= (1u << 31);
        else
            dstat &= ~(1u << 31);

        m_ioRegisters[D_STAT] = dstat;
    };

    if (hadGif)
    {
        raiseDStatChannel(2u); // GIF channel
        queueCompletedDmacCause(2u);
        m_ioRegisters[GIF_CHANNEL + 0x00] &= ~0x100u;
        m_ioRegisters[GIF_CHANNEL + 0x20] = 0;
    }
    if (hadVif0)
    {
        raiseDStatChannel(0u); // VIF0 channel
        queueCompletedDmacCause(0u);
        m_ioRegisters[VIF0_CHANNEL + 0x00] &= ~0x100u;
        m_ioRegisters[VIF0_CHANNEL + 0x20] = 0;
    }
    if (hadVif1)
    {
        raiseDStatChannel(1u); // VIF1 channel
        queueCompletedDmacCause(1u);
        m_ioRegisters[VIF1_CHANNEL + 0x00] &= ~0x100u;
        m_ioRegisters[VIF1_CHANNEL + 0x20] = 0;
    }
}

void PS2Memory::queueCompletedDmacCause(uint32_t cause)
{
    std::lock_guard<std::mutex> lock(m_completedDmacMutex);
    m_completedDmacCauses.push_back(cause);
    m_causesPending.store(true, std::memory_order_release);
}

void PS2Memory::queueIntcCause(uint32_t cause)
{
    raiseIntcStat(1u << cause);
    {
        std::lock_guard<std::mutex> lock(m_completedDmacMutex);
        m_pendingIntcCauses.push_back(cause);
        m_causesPending.store(true, std::memory_order_release);
    }
    if (m_intcWake)
        m_intcWake();
}

std::vector<uint32_t> PS2Memory::consumeIntcCauses()
{
    std::lock_guard<std::mutex> lock(m_completedDmacMutex);
    std::vector<uint32_t> causes;
    causes.swap(m_pendingIntcCauses);
    if (g_irqStats && !causes.empty())
        if (const int64_t start = g_irqStallStartNs.load(std::memory_order_acquire))
            for (const uint32_t cause : causes)
                if (cause == 5u)
                {
                    g_irqTakeNs += static_cast<uint64_t>(irqNowNs() - start);
                    break;
                }
    m_causesPending.store(!m_completedDmacCauses.empty(), std::memory_order_release);
    return causes;
}

std::vector<uint32_t> PS2Memory::consumeCompletedDmacCauses()
{
    std::lock_guard<std::mutex> lock(m_completedDmacMutex);
    std::vector<uint32_t> causes;
    causes.swap(m_completedDmacCauses);
    m_causesPending.store(!m_pendingIntcCauses.empty(), std::memory_order_release);
    return causes;
}

void PS2Memory::flushMaskedPath3Packets(bool drainImmediately, bool firstPacketOnly)
{
    if (m_path3Masked || m_path3MaskedFifo.empty())
        return;

    auto emit = [&](const uint8_t *packetData, uint32_t packetSize)
    {
        if (m_gifArbiter)
            m_gifArbiter->submit(GifPathId::Path3, packetData, packetSize, false);
        else if (m_gifPacketCallback)
            m_gifPacketCallback(packetData, packetSize);
    };

    if (firstPacketOnly)
    {
        // Opening the VIF PATH3 gate starts a packet, not the entire DMA list.
        // A subsequent MSKPATH3 closes it at EOP. Keep later materials resident
        // in the queue until the next gate opening or an unmasked FLUSHA.
        // Join submission fragments before walking tags: EOP belongs to the tag
        // but takes effect only after all its payload (including IMAGE) arrives.
        // The list stays in one buffer and the read position moves: games open
        // tens of gates per frame over a list of many megabytes in total.
        for (; g_path3Joined < m_path3MaskedFifo.size(); ++g_path3Joined)
            g_path3Stream.insert(g_path3Stream.end(), m_path3MaskedFifo[g_path3Joined].begin(), m_path3MaskedFifo[g_path3Joined].end());
        const std::vector<uint8_t> &stream = g_path3Stream;
        const size_t start = g_path3Consumed;
        size_t offset = start;
        while (stream.size() - offset >= 16u)
        {
            uint64_t tag = 0;
            std::memcpy(&tag, stream.data() + offset, sizeof(tag));
            const uint64_t loops = tag & 0x7fffu;
            const unsigned format = static_cast<unsigned>((tag >> 58u) & 3u);
            const uint64_t nreg = (tag >> 60u) ? (tag >> 60u) : 16u;
            const uint64_t payload = format >= 2u ? loops * 16u :
                format == 0u ? loops * nreg * 16u : ((loops * nreg + 1u) / 2u) * 16u;
            if (payload > stream.size() - offset - 16u)
                return; // retain the incomplete packet until more data arrives
            offset += 16u + static_cast<size_t>(payload);
            if (tag & (1ull << 15u))
            {
                emit(stream.data() + start, static_cast<uint32_t>(offset - start));
                if (offset >= stream.size())
                {
                    m_path3MaskedFifo.clear();
                    resetPath3Stream();
                }
                else
                {
                    g_path3Consumed = offset;
                }
                if (m_gifArbiter && drainImmediately)
                    m_gifArbiter->drain();
                return;
            }
        }
        return;
    }

    if (g_path3Consumed != 0u)
    {
        // gates released the start of the list: what is left goes as one block
        for (; g_path3Joined < m_path3MaskedFifo.size(); ++g_path3Joined)
            g_path3Stream.insert(g_path3Stream.end(), m_path3MaskedFifo[g_path3Joined].begin(), m_path3MaskedFifo[g_path3Joined].end());
        if (g_path3Stream.size() - g_path3Consumed >= 16u)
            emit(g_path3Stream.data() + g_path3Consumed, static_cast<uint32_t>(g_path3Stream.size() - g_path3Consumed));
        m_path3MaskedFifo.clear();
    }
    resetPath3Stream();
    for (const auto &packet : m_path3MaskedFifo)
    {
        if (packet.size() >= 16u)
            emit(packet.data(), static_cast<uint32_t>(packet.size()));
    }
    m_path3MaskedFifo.clear();

    if (m_gifArbiter && drainImmediately)
        m_gifArbiter->drain();
}

void PS2Memory::submitGifPacket(GifPathId pathId, const uint8_t *data, uint32_t sizeBytes, bool drainImmediately, bool path2DirectHl)
{
    gifVifSync();
    if (!data || sizeBytes < 16)
        return;

    if (Ps2GifOrderTraceEnabled())
    {
        char detail[160];
        std::snprintf(detail, sizeof(detail), "{\"drain\":%s,\"direct_hl\":%s,\"masked\":%s}",
            drainImmediately ? "true" : "false", path2DirectHl ? "true" : "false", m_path3Masked ? "true" : "false");
        Ps2TraceGifOrder("submit", static_cast<unsigned>(pathId), data, sizeBytes, this,
            m_pendingGifTransfers.size(), m_pendingVif1Transfers.size(), detail);
    }
    if (pathId == GifPathId::Path3)
    {
        if (m_path3Masked)
        {
            m_path3MaskedFifo.emplace_back(data, data + sizeBytes);
            return;
        }
        flushMaskedPath3Packets(false);
    }

    if (m_gifArbiter && drainImmediately && !Ps2GifOrderTraceEnabled() && m_gifArbiter->sendDirect(data, sizeBytes))
        return; // nothing to arbitrate against: no copy
    if (m_gifArbiter)
        m_gifArbiter->submit(pathId, data, sizeBytes, path2DirectHl);
    else if (m_gifPacketCallback)
        m_gifPacketCallback(data, sizeBytes);

    if (m_gifArbiter && drainImmediately)
        m_gifArbiter->drain();
}

void PS2Memory::processGIFPacket(uint32_t srcPhysAddr, uint32_t qwCount)
{
    if (!m_rdram || qwCount == 0)
        return;
    const uint64_t bytes64 = static_cast<uint64_t>(qwCount) * 16ull;
    uint32_t sizeBytes = (bytes64 > 0xFFFFFFFFull) ? 0xFFFFFFFFu : static_cast<uint32_t>(bytes64);
    uint32_t bytesLeft = sizeBytes;
    while (bytesLeft >= 16)
    {
        if (srcPhysAddr >= PS2_RAM_SIZE)
            srcPhysAddr = 0;
        uint32_t chunk = bytesLeft;
        if (srcPhysAddr + chunk > PS2_RAM_SIZE)
            chunk = PS2_RAM_SIZE - srcPhysAddr;
        if (chunk == 0)
            break;

        m_seenGifCopy = true;
        m_gifCopyCount.fetch_add(1, std::memory_order_relaxed);
        submitGifPacket(GifPathId::Path3, m_rdram + srcPhysAddr, chunk);

        bytesLeft -= chunk;
        srcPhysAddr += chunk;
    }
}

void PS2Memory::processGIFPacket(const uint8_t *data, uint32_t sizeBytes)
{
    if (m_gifArbiter)
        submitGifPacket(GifPathId::Path3, data, sizeBytes);
    else if (m_gifPacketCallback && data && sizeBytes >= 16)
        m_gifPacketCallback(data, sizeBytes);
}

bool PS2Memory::tryProcessNativeGifImageUploadChain(GS &gs, uint32_t tadr, uint32_t chcr)
{
    static constexpr uint32_t GIF_CHANNEL = 0x1000A000u;
    static constexpr uint32_t D_STAT = 0x1000E010u;
    static constexpr uint32_t D_CTRL = 0x1000E000u;

    if (!m_rdram || !m_gsVRAM || m_path3Masked)
        return false;
    if (m_gifArbiter && !m_gifArbiter->empty())
        return false;
    if ((chcr & 0x100u) == 0u || ((chcr >> 2u) & 0x3u) != 1u)
        return false;
    if ((chcr & (1u << 7u)) != 0u || ((chcr >> 4u) & 0x3u) != 0u)
        return false;

    const auto dctrlIt = m_ioRegisters.find(D_CTRL);
    if (dctrlIt != m_ioRegisters.end() && ((dctrlIt->second & 0x1u) == 0u))
        return false;

    auto resolveContiguous = [&](uint32_t guestAddr, uint32_t bytes, const uint8_t *&out) -> bool
    {
        try
        {
            const bool scratch = isScratchpad(guestAddr);
            const uint32_t phys = translateAddress(guestAddr);
            const uint8_t *base = scratch ? m_scratchpad : m_rdram;
            const uint32_t limit = scratch ? PS2_SCRATCHPAD_SIZE : PS2_RAM_SIZE;
            if (!base || phys > limit || bytes > limit - phys)
                return false;
            out = base + phys;
            return true;
        }
        catch (const std::exception &)
        {
            return false;
        }
    };

    auto loadDmaTagAt = [&](uint32_t guestAddr, DmaTagView &out) -> bool
    {
        const uint8_t *ptr = nullptr;
        if (!resolveContiguous(guestAddr, 16u, ptr))
            return false;
        out = decodeDmaTag(loadScalar<uint64_t>(ptr, 0u, 16u, "native gif dma tag", guestAddr));
        return true;
    };

    auto decodeSetupPayload = [&](const uint8_t *payload, uint64_t (&regs)[4]) -> bool
    {
        const uint64_t tagLo = loadScalar<uint64_t>(payload, 0u, 80u, "native gif setup tag", 0u);
        const uint64_t tagHi = loadScalar<uint64_t>(payload, 8u, 80u, "native gif setup regs", 0u);
        if (gifTagNloop(tagLo) != 4u ||
            gifTagFlg(tagLo) != GIF_FMT_PACKED ||
            gifTagNreg(tagLo) != 1u ||
            (tagHi & 0xFull) != 0x0Eull)
        {
            return false;
        }

        static constexpr uint8_t kExpectedRegs[4] = {
            GS_REG_BITBLTBUF,
            GS_REG_TRXPOS,
            GS_REG_TRXREG,
            GS_REG_TRXDIR,
        };

        uint32_t offset = 16u;
        for (uint32_t i = 0; i < 4u; ++i)
        {
            regs[i] = loadScalar<uint64_t>(payload, offset, 80u, "native gif setup value", 0u);
            const uint64_t reg = loadScalar<uint64_t>(payload, offset + 8u, 80u, "native gif setup register", 0u);
            if ((reg & 0xFFu) != kExpectedRegs[i])
                return false;
            offset += 16u;
        }

        const uint32_t trxdirMode = static_cast<uint32_t>(regs[3] & 0x3ull);
        const uint32_t rrw = static_cast<uint32_t>(regs[2] & 0xFFFull);
        const uint32_t rrh = static_cast<uint32_t>((regs[2] >> 32u) & 0xFFFull);
        return trxdirMode == 0u && rrw != 0u && rrh != 0u;
    };

    DmaTagView setupTag{};
    if (!loadDmaTagAt(tadr, setupTag) ||
        setupTag.id != 1u ||
        setupTag.qwc != 5u ||
        setupTag.irq)
    {
        return false;
    }

    const uint8_t *setupPayload = nullptr;
    const uint32_t setupPayloadAddr = tadr + 16u;
    if (!resolveContiguous(setupPayloadAddr, 5u * 16u, setupPayload))
        return false;

    uint64_t setupRegs[4] = {};
    if (!decodeSetupPayload(setupPayload, setupRegs))
        return false;

    uint32_t imageTagDmaAddr = setupPayloadAddr + 5u * 16u;
    DmaTagView imageTagDma{};
    if (!loadDmaTagAt(imageTagDmaAddr, imageTagDma) ||
        imageTagDma.id != 1u ||
        imageTagDma.qwc != 1u ||
        imageTagDma.irq)
    {
        return false;
    }

    const uint8_t *imageGifTag = nullptr;
    if (!resolveContiguous(imageTagDmaAddr + 16u, 16u, imageGifTag))
        return false;

    const uint64_t imageTagLo = loadScalar<uint64_t>(imageGifTag, 0u, 16u, "native gif image tag", imageTagDmaAddr + 16u);
    if (gifTagFlg(imageTagLo) != GIF_FMT_IMAGE)
        return false;

    const uint32_t imageQwc = gifTagNloop(imageTagLo);
    if (imageQwc == 0u)
        return false;

    const uint64_t imageBytes64 = static_cast<uint64_t>(imageQwc) * 16ull;
    if (imageBytes64 > 0xFFFFFFFFull)
        return false;
    const uint32_t imageBytes = static_cast<uint32_t>(imageBytes64);

    const uint32_t payloadTagAddr = imageTagDmaAddr + 32u;
    DmaTagView payloadTag{};
    if (!loadDmaTagAt(payloadTagAddr, payloadTag) ||
        payloadTag.qwc != imageQwc ||
        payloadTag.irq)
    {
        return false;
    }

    uint32_t imageDataAddr = 0u;
    uint32_t finalTadr = payloadTagAddr;
    uint32_t lastTagUpper = payloadTag.upper;
    if (payloadTag.id == 3u || payloadTag.id == 4u)
    {
        imageDataAddr = payloadTag.addr;
        const uint32_t terminalTagAddr = payloadTagAddr + 16u;
        DmaTagView terminalTag{};
        if (!loadDmaTagAt(terminalTagAddr, terminalTag) ||
            terminalTag.qwc != 0u ||
            terminalTag.irq ||
            (terminalTag.id != 0u && terminalTag.id != 7u))
        {
            return false;
        }
        finalTadr = (terminalTag.id == 0u) ? (terminalTagAddr + 16u) : terminalTagAddr;
        lastTagUpper = terminalTag.upper;
    }
    else if (payloadTag.id == 7u)
    {
        imageDataAddr = payloadTagAddr + 16u;
        finalTadr = payloadTagAddr;
    }
    else
    {
        return false;
    }

    const uint8_t *imageData = nullptr;
    if (!resolveContiguous(imageDataAddr, imageBytes, imageData))
        return false;

    m_dmaStartCount.fetch_add(1, std::memory_order_relaxed);
    m_seenGifCopy = true;
    m_gifCopyCount.fetch_add(1, std::memory_order_relaxed);
    if (Ps2GifOrderTraceEnabled())
    {
        char traceDetail[320];
        std::snprintf(traceDetail, sizeof(traceDetail), "{\"tadr\":%u,\"source_guest\":%u,\"chcr\":%u,\"bitbltbuf\":\"%016llx\",\"trxpos\":\"%016llx\",\"trxreg\":\"%016llx\",\"trxdir\":\"%016llx\"}",
            tadr, imageDataAddr, chcr, static_cast<unsigned long long>(setupRegs[0]), static_cast<unsigned long long>(setupRegs[1]),
            static_cast<unsigned long long>(setupRegs[2]), static_cast<unsigned long long>(setupRegs[3]));
        Ps2TraceGifOrder("native-image", 3u, imageData, imageBytes, this,
            m_pendingGifTransfers.size(), m_pendingVif1Transfers.size(), traceDetail);
    }
    gs.uploadImageNative(setupRegs[0], setupRegs[1], setupRegs[2], setupRegs[3], imageData, imageBytes);

    m_ioRegisters[GIF_CHANNEL + 0x30u] = finalTadr;
    m_ioRegisters[GIF_CHANNEL + 0x40u] = 0u;
    m_ioRegisters[GIF_CHANNEL + 0x50u] = 0u;
    m_ioRegisters[GIF_CHANNEL + 0x00u] = ((chcr & 0x0000FFFFu) | (lastTagUpper << 16u)) & ~0x100u;
    m_ioRegisters[GIF_CHANNEL + 0x20u] = 0u;

    uint32_t dstat = m_ioRegisters.count(D_STAT) ? m_ioRegisters[D_STAT] : 0u;
    dstat |= (1u << 2u);
    const uint32_t status = dstat & 0x3FFu;
    const uint32_t mask = (dstat >> 16u) & 0x3FFu;
    if ((status & mask) != 0u)
        dstat |= (1u << 31u);
    else
        dstat &= ~(1u << 31u);
    m_ioRegisters[D_STAT] = dstat;
    queueCompletedDmacCause(2u);
    return true;
}

bool PS2Memory::tryProcessNativeGifPackedChain(GS &gs, uint32_t tadr, uint32_t chcr)
{
    static constexpr uint32_t GIF_CHANNEL = 0x1000A000u;
    static constexpr uint32_t D_STAT = 0x1000E010u;
    static constexpr uint32_t D_CTRL = 0x1000E000u;

    if (!m_rdram || !m_gsVRAM || m_path3Masked)
        return false;
    if (m_gifArbiter && !m_gifArbiter->empty())
        return false;
    if ((chcr & 0x100u) == 0u || ((chcr >> 2u) & 0x3u) != 1u)
        return false;
    if ((chcr & (1u << 7u)) != 0u || ((chcr >> 4u) & 0x3u) != 0u)
        return false;

    const auto dctrlIt = m_ioRegisters.find(D_CTRL);
    if (dctrlIt != m_ioRegisters.end() && ((dctrlIt->second & 0x1u) == 0u))
        return false;

    auto resolveContiguous = [&](uint32_t guestAddr, uint32_t bytes, const uint8_t *&out) -> bool
    {
        try
        {
            const bool scratch = isScratchpad(guestAddr);
            const uint32_t phys = translateAddress(guestAddr);
            const uint8_t *base = scratch ? m_scratchpad : m_rdram;
            const uint32_t limit = scratch ? PS2_SCRATCHPAD_SIZE : PS2_RAM_SIZE;
            if (!base || phys > limit || bytes > limit - phys)
                return false;
            out = base + phys;
            return true;
        }
        catch (const std::exception &)
        {
            return false;
        }
    };

    const uint8_t *tagPtr = nullptr;
    if (!resolveContiguous(tadr, 16u, tagPtr))
        return false;

    const DmaTagView tag = decodeDmaTag(loadScalar<uint64_t>(tagPtr, 0u, 16u, "native packed gif dma tag", tadr));
    if (tag.id != 7u || tag.qwc == 0u || tag.irq)
        return false;

    const uint64_t payloadBytes64 = static_cast<uint64_t>(tag.qwc) * 16ull;
    if (payloadBytes64 > 0xFFFFFFFFull)
        return false;
    const uint32_t payloadBytes = static_cast<uint32_t>(payloadBytes64);

    const uint8_t *payload = nullptr;
    if (!resolveContiguous(tadr + 16u, payloadBytes, payload))
        return false;
    Ps2TraceGifOrder("native-packed-attempt", 3u, payload, payloadBytes, this,
        m_pendingGifTransfers.size(), m_pendingVif1Transfers.size(), "{}");
    if (!gs.processNativePackedGIFPacket(payload, payloadBytes))
        return false;
    Ps2TraceGifOrder("native-packed-accepted", 3u, payload, payloadBytes, this,
        m_pendingGifTransfers.size(), m_pendingVif1Transfers.size(), "{}");

    m_dmaStartCount.fetch_add(1, std::memory_order_relaxed);
    m_seenGifCopy = true;
    m_gifCopyCount.fetch_add(1, std::memory_order_relaxed);

    m_ioRegisters[GIF_CHANNEL + 0x30u] = tadr;
    m_ioRegisters[GIF_CHANNEL + 0x40u] = 0u;
    m_ioRegisters[GIF_CHANNEL + 0x50u] = 0u;
    m_ioRegisters[GIF_CHANNEL + 0x00u] = ((chcr & 0x0000FFFFu) | (tag.upper << 16u)) & ~0x100u;
    m_ioRegisters[GIF_CHANNEL + 0x20u] = 0u;

    uint32_t dstat = m_ioRegisters.count(D_STAT) ? m_ioRegisters[D_STAT] : 0u;
    dstat |= (1u << 2u);
    const uint32_t status = dstat & 0x3FFu;
    const uint32_t mask = (dstat >> 16u) & 0x3FFu;
    if ((status & mask) != 0u)
        dstat |= (1u << 31u);
    else
        dstat &= ~(1u << 31u);
    m_ioRegisters[D_STAT] = dstat;
    queueCompletedDmacCause(2u);
    return true;
}

int PS2Memory::pollDmaRegisters()
{
    return 0;
}

bool PS2Memory::intcStatPlain() noexcept
{
    static const bool plain = std::getenv("PS2X_INTC_STAT_PLAIN") != nullptr;
    return plain;
}

uint32_t PS2Memory::readIORegister(uint32_t address)
{
    if (address == 0x1000F000u && !intcStatPlain())
        return m_intcStat.load(std::memory_order_acquire);
    static const bool asyncPaths = std::getenv("PS2X_ASYNC_PATHS") != nullptr;
    if ((address & 0xFFFFF000u) == 0x10003000u && !asyncPaths)
        gifVifSync(); // GIF and VIF registers
    size_t timerIndex = 0u;
    uint32_t timerOffset = 0u;
    if (decodeEeTimerRegister(address, timerIndex, timerOffset))
    {
        const EeTimer &timer = m_eeTimers[timerIndex];
        switch (timerOffset)
        {
        case kEeTimerCountOffset:
            return timer.count & 0xFFFFu;
        case kEeTimerModeOffset:
            return timer.mode & (kEeTimerModeConfigMask | kEeTimerModeStatusMask);
        case kEeTimerCompareOffset:
            return timer.compare & 0xFFFFu;
        case kEeTimerHoldOffset:
            return timer.hold & 0xFFFFu;
        default:
            return 0u;
        }
    }

    if (isGsPrivReg(address))
    {
        if (asyncPaths && ((address - PS2_GS_PRIV_REG_BASE) & ~0x7u) == kGsCsrRegOffset)
            return static_cast<uint32_t>(((gs_regs.csr.load() | 2u) >> ((address & 7u) * 8u)) & 0xFFFFFFFFull);
        gifVifSync();
        // NB: unreachable from read8/16/32/64 today, same reasoning as the write
        // path above; kept correct for direct callers.
        const uint32_t off = address & 7u;
        const uint32_t regOff = (address - PS2_GS_PRIV_REG_BASE) & ~0x7u;
        if (regOff == kGsCsrRegOffset)
        {
            return static_cast<uint32_t>((gs_regs.csr.load() >> (off * 8u)) & 0xFFFFFFFFull);
        }
        if (uint64_t *reg = gsRegPtr(gs_regs, address))
        {
            return static_cast<uint32_t>((*reg >> (off * 8u)) & 0xFFFFFFFFull);
        }
        return 0u;
    }

    static const bool ipuStubRead = std::getenv("PS2X_IPU_STUB") != nullptr;
    if (!ipuStubRead && address >= 0x10002000u && address <= 0x10002034u)
    {
        switch (address)
        {
        case 0x10002000u: return m_ipu.readCmd();
        case 0x10002004u: return m_ipu.readCmdHi();
        case 0x10002010u: return m_ipu.readCtrl();
        case 0x10002020u: return m_ipu.readTop();
        case 0x10002024u: return m_ipu.readTopHi();
        case 0x10002030u: return m_ipu.readBp();
        default: return 0u;
        }
    }
    if (address >= 0x10002000 && address <= 0x10002030)
    {
        uint32_t val = 0;
        switch (address)
        {
        case 0x10002000:
            val = m_ioRegisters[address];
            break;
        case 0x10002010:
            val = m_ioRegisters[address] & ~(1u << 31);
            break;
        case 0x10002020:
        case 0x10002030:
            val = m_ioRegisters[address];
            break;
        default:
            val = 0;
            break;
        }
        return val;
    }

    if (address == 0x10003020u) // GIF_STAT
    {
        uint32_t stat = m_ioRegisters.count(address) ? m_ioRegisters[address] : 0u;
        const uint32_t mode = m_ioRegisters.count(0x10003010u) ? m_ioRegisters[0x10003010u] : 0u;
        const uint32_t ctrl = m_ioRegisters.count(0x10003000u) ? m_ioRegisters[0x10003000u] : 0u;

        // M3R and IMT mirror GIF_MODE, PSE mirrors GIF_CTRL, and M3P is the
        // effective PATH3 mask controlled by the VIF1 MSKPATH3 command.
        stat = (stat & ~0xFu) |
               (mode & 0x1u) |
               (m_path3Masked ? 0x2u : 0u) |
               (mode & 0x4u) |
               (ctrl & 0x8u);
        // A GIF DMA finishes inside the kick here, but code that kicks PATH3 and then waits to see it run (MotorStorm
        // AE's VIF1 interrupt handler polls OPH and APATH before releasing the VIF) must see it active once.
        if (m_gifStatBusyReads.load(std::memory_order_relaxed) > 0)
        {
            m_gifStatBusyReads.fetch_sub(1, std::memory_order_relaxed);
            stat |= (1u << 9u) | (3u << 10u); // OPH, APATH = PATH3
        }
        return stat;
    }

    if (address >= 0x10000000 && address < 0x10010000)
    {
        if (address >= 0x10008000 && address < 0x1000F000)
        {
            if ((address & 0xFF) == 0x00)
            {
                uint32_t channelStatus = m_ioRegisters[address] & ~0x100u;
                m_ioRegisters[address] = channelStatus;
                return channelStatus;
            }
        }

        if (address >= 0x10000200 && address < 0x10000300)
        {
            return 0;
        }

        if (address >= 0x1000F200 && address <= 0x1000F260)
        {
            if (address == 0x1000F230)
            {
                return 0x60000;
            }
            if (address == 0x1000F240)
            {
                return 0xF0000002;
            }
            return 0;
        }
    }

    auto it = m_ioRegisters.find(address);
    if (it != m_ioRegisters.end())
    {
        return it->second;
    }

    return 0;
}

void PS2Memory::registerCodeRegion(uint32_t start, uint32_t end)
{
    if (end <= start)
    {
        std::cerr << "Ignoring invalid code region: start=0x" << std::hex << start
                  << " end=0x" << end << std::dec << std::endl;
        return;
    }

    if ((end - start) > PS2_RAM_SIZE)
    {
        std::cerr << "Ignoring oversized code region: start=0x" << std::hex << start
                  << " end=0x" << end << std::dec << std::endl;
        return;
    }

    for (const auto &existing : m_codeRegions)
    {
        if (existing.start == start && existing.end == end)
        {
            return;
        }
    }

    CodeRegion region;
    region.start = start;
    region.end = end;

    size_t sizeInWords = (end - start + 3u) / 4u;
    region.modified.resize(sizeInWords, false);

    m_codeRegions.push_back(region);
    RUNTIME_LOG("Registered code region: " << std::hex << start << " - " << end << std::dec);
}

bool PS2Memory::isAddressInRegion(uint32_t address, const CodeRegion &region)
{
    return (address >= region.start && address < region.end);
}

bool PS2Memory::isCodeAddress(uint32_t address) const
{
    for (const auto &region : m_codeRegions)
    {
        if (address >= region.start && address < region.end)
        {
            return true;
        }
    }
    return false;
}

void PS2Memory::markModified(uint32_t address, uint32_t size)
{
    if (size == 0)
    {
        return;
    }

    const uint64_t writeEnd = static_cast<uint64_t>(address) + static_cast<uint64_t>(size);
    for (auto &region : m_codeRegions)
    {
        const uint64_t regionStart = region.start;
        const uint64_t regionEnd = region.end;
        if (writeEnd <= regionStart || static_cast<uint64_t>(address) >= regionEnd)
        {
            continue;
        }

        uint32_t overlapStart = static_cast<uint32_t>(std::max<uint64_t>(address, regionStart));
        uint32_t overlapEnd = static_cast<uint32_t>(std::min<uint64_t>(writeEnd, regionEnd));

        for (uint32_t addr = overlapStart; addr < overlapEnd; addr += 4)
        {
            size_t bitIndex = (addr - region.start) / 4;
            if (bitIndex < region.modified.size())
            {
                region.modified[bitIndex] = true;
                RUNTIME_LOG("Marked code at " << std::hex << addr << std::dec << " as modified");
            }
        }
    }
}

bool PS2Memory::isCodeModified(uint32_t address, uint32_t size)
{
    if (size == 0)
    {
        return false;
    }

    const uint64_t writeEnd = static_cast<uint64_t>(address) + static_cast<uint64_t>(size);
    for (const auto &region : m_codeRegions)
    {
        const uint64_t regionStart = region.start;
        const uint64_t regionEnd = region.end;
        if (writeEnd <= regionStart || static_cast<uint64_t>(address) >= regionEnd)
        {
            continue;
        }

        uint32_t overlapStart = static_cast<uint32_t>(std::max<uint64_t>(address, regionStart));
        uint32_t overlapEnd = static_cast<uint32_t>(std::min<uint64_t>(writeEnd, regionEnd));

        for (uint32_t addr = overlapStart; addr < overlapEnd; addr += 4)
        {
            size_t bitIndex = (addr - region.start) / 4;
            if (bitIndex < region.modified.size() && region.modified[bitIndex])
            {
                return true; // Found modified code
            }
        }
    }

    return false; // No modifications found
}

void PS2Memory::clearModifiedFlag(uint32_t address, uint32_t size)
{
    if (size == 0)
    {
        return;
    }

    const uint64_t writeEnd = static_cast<uint64_t>(address) + static_cast<uint64_t>(size);
    for (auto &region : m_codeRegions)
    {
        const uint64_t regionStart = region.start;
        const uint64_t regionEnd = region.end;
        if (writeEnd <= regionStart || static_cast<uint64_t>(address) >= regionEnd)
        {
            continue;
        }

        uint32_t overlapStart = static_cast<uint32_t>(std::max<uint64_t>(address, regionStart));
        uint32_t overlapEnd = static_cast<uint32_t>(std::min<uint64_t>(writeEnd, regionEnd));

        for (uint32_t addr = overlapStart; addr < overlapEnd; addr += 4)
        {
            size_t bitIndex = (addr - region.start) / 4;
            if (bitIndex < region.modified.size())
            {
                region.modified[bitIndex] = false;
            }
        }
    }
}
