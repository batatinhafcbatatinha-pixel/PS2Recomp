// GOW-Port: adaptado de Taylor N. Albarnaz / LightVelox (GPL-3.0).
// Origen: PS2Recomp sotc-port, ac9efa070638ad3b3accd284de6f898d5ab271d1.
#include "runtime/gs/gs_threaded_backend.h"
#include "runtime/gs/gs_hw_backend.h"
#include "runtime/gs/gs_shared_present.h"
#include "gs_inventory.h"
#include "ThreadPriority.h"
#include "ThreadNaming.h"
#include "../ps2_timeline.h"

#include <atomic>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#if defined(USE_SSE2NEON)
#include "sse2neon.h"
#else
#include <xmmintrin.h>
#endif
#if defined(_MSC_VER)
#include <intrin.h>
#endif
#include <chrono>

void ps2ProfilerRegisterCurrentThread(const char *name);

namespace
{
    // PS2X_GS_WAITSTATS=1: time the producer (game thread) spends blocked on the GS thread, by reason, printed every
    // 5 s as [gs:wait] (ms blocked in the window / number of waits).
    enum WaitReason { kWaitCall, kWaitFlush, kWaitFinish, kWaitSync, kWaitPresent, kWaitClear, kWaitRead, kWaitQueueFull, kWaitCount };
    const char *const kWaitNames[kWaitCount] = {"call", "flush", "finish", "sync", "present", "clear", "read", "queue-full"};
    thread_local int t_waitReason = kWaitCall;

    bool waitStatsOn()
    {
        static const bool on = [] { const char *v = std::getenv("PS2X_GS_WAITSTATS"); return v && *v && *v != '0'; }();
        return on;
    }

    void noteWait(int reason, std::chrono::steady_clock::duration spent)
    {
        static std::atomic<int64_t> ns[kWaitCount]{};
        static std::atomic<uint32_t> count[kWaitCount]{};
        static std::atomic<int64_t> windowStart{0};
        ns[reason] += std::chrono::duration_cast<std::chrono::nanoseconds>(spent).count();
        ++count[reason];
        const int64_t now = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        int64_t start = windowStart.load();
        if (start == 0)
            windowStart.compare_exchange_strong(start, now);
        else if (now - start >= 5000000000ll && windowStart.compare_exchange_strong(start, now))
        {
            std::fprintf(stderr, "[gs:wait] window=%.1fs", (now - start) / 1e9);
            for (int i = 0; i < kWaitCount; ++i)
                std::fprintf(stderr, " %s=%.0fms/%u", kWaitNames[i], ns[i].exchange(0) / 1e6, count[i].exchange(0));
            std::fprintf(stderr, "\n");
        }
    }

    struct WaitTag
    {
        int saved;
        explicit WaitTag(int reason) : saved(t_waitReason) { t_waitReason = reason; }
        ~WaitTag() { t_waitReason = saved; }
    };

    constexpr size_t kPublishBytes = 64u * 1024u;
    constexpr size_t kMaxPendingChunks = 256u;
    std::atomic<bool> s_enabledByDefault{false};

    bool discardDraws()
    {
        static const bool discard = []
        {
            const char *value = std::getenv("PS2X_GS_DISCARD_DRAWS");
            return value && *value && *value != '0';
        }();
        return discard;
    }

    struct LoadClutArgs
    {
        GSTex0Reg tex0;
        GSTexClutReg texclut;
    };

    struct WriteVramArgs
    {
        uint32_t psm;
        uint32_t base;
        uint32_t bw;
        uint32_t x;
        uint32_t y;
        uint32_t value;
    };

    template <typename T>
    T readAt(const uint8_t *data)
    {
        T value;
        std::memcpy(&value, data, sizeof(T));
        return value;
    }
}

GSThreadedBackend::GSThreadedBackend(std::unique_ptr<GSRasterBackend> inner)
    : m_inner(std::move(inner))
{
    const char *compact = std::getenv("PS2X_GS_COMPACT_QUEUE");
    m_compactQueue = !compact || *compact != '0';
    if (const char *limit = std::getenv("PS2X_GS_QUEUE_CHUNKS"))
        m_queueChunkLimit = static_cast<size_t>(std::clamp(std::atoi(limit), 1, static_cast<int>(kMaxPendingChunks)));
    m_open.reserve(kPublishBytes + 4096u);
    m_worker = std::thread([this]
                           { WorkerLoop(); });
}

GSThreadedBackend::~GSThreadedBackend()
{
    // GOW-Port: completar incluso el último bloque antes de cerrar el contexto GL.
    Drain();
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        m_stop = true;
        m_pending.clear();
    }
    m_workCv.notify_all();
    if (m_worker.joinable())
        m_worker.join();
}

std::unique_ptr<GSRasterBackend> GSThreadedBackend::MakeDefault()
{
    // GOW-Port: the native render-target renderer is the only GS renderer (no VRAM-emulating rasteriser).
    return std::make_unique<GSThreadedBackend>(std::make_unique<GSHwBackend>());
}

void GSThreadedBackend::SetEnabledByDefault(bool enabled)
{
    s_enabledByDefault.store(enabled);
}


void GSThreadedBackend::Enqueue(Op op, const void *payload, uint32_t size)
{
    std::lock_guard<std::mutex> lock(m_producerMutex);
    const uint32_t csr = _mm_getcsr();
    if (csr != m_lastMxcsr)
    {
        m_lastMxcsr = csr;
        const uint8_t header = static_cast<uint8_t>(Op::Mxcsr);
        const uint32_t csrSize = sizeof(csr);
        m_open.push_back(header);
        m_open.insert(m_open.end(), reinterpret_cast<const uint8_t *>(&csrSize), reinterpret_cast<const uint8_t *>(&csrSize) + 4);
        m_open.insert(m_open.end(), reinterpret_cast<const uint8_t *>(&csr), reinterpret_cast<const uint8_t *>(&csr) + 4);
    }
    const auto append = [&](Op kind, const void *bytes, uint32_t length)
    {
        uint8_t header[5];
        header[0] = static_cast<uint8_t>(kind);
        std::memcpy(header + 1u, &length, sizeof(length));
        m_open.insert(m_open.end(), header, header + sizeof(header));
        if (length)
            m_open.insert(m_open.end(), static_cast<const uint8_t *>(bytes), static_cast<const uint8_t *>(bytes) + length);
    };
    if (op == Op::Submit && m_compactQueue)
    {
        const auto &draw = *static_cast<const GSPrimitiveBatch *>(payload);
        if (!m_drawStateValid || !(m_lastDrawState == draw.state)) // field-wise: GSDrawState has padding
        {
            std::memcpy(&m_lastDrawState, &draw.state, sizeof(draw.state));
            m_drawStateValid = true;
            append(Op::DrawState, &draw.state, sizeof(draw.state));
        }
        append(Op::Vertices, &draw, static_cast<uint32_t>(offsetof(GSPrimitiveBatch, state)));
    }
    else
        append(op, payload, size);
    if (m_open.size() >= kPublishBytes)
        PublishLocked();
}

void GSThreadedBackend::PublishLocked()
{
    if (m_open.empty())
        return;
    std::vector<uint8_t> next;
    {
        std::unique_lock<std::mutex> lock(m_queueMutex);
        if (waitStatsOn() && !m_stop && m_pending.size() >= m_queueChunkLimit)
        {
            const auto t0 = std::chrono::steady_clock::now();
            m_doneCv.wait(lock, [&]
                          { return m_stop || m_pending.size() < m_queueChunkLimit; });
            noteWait(kWaitQueueFull, std::chrono::steady_clock::now() - t0);
        }
        if (m_pending.size() >= m_queueChunkLimit)
            PS2TL(TL_QUEUE_FULL_BEGIN);
        m_doneCv.wait(lock, [&]
                      { return m_stop || m_pending.size() < m_queueChunkLimit; });
        PS2TL(TL_QUEUE_FULL_END);
        m_pending.push_back(std::move(m_open));
        ++m_published;
        if (!m_spare.empty())
        {
            next = std::move(m_spare.back());
            m_spare.pop_back();
        }
    }
    m_workCv.notify_one();
    next.clear();
    m_open = std::move(next);
    if (m_open.capacity() < kPublishBytes)
        m_open.reserve(kPublishBytes + 4096u);
}

void GSThreadedBackend::Drain() const
{
    auto *self = const_cast<GSThreadedBackend *>(this);
    uint64_t target = 0u;
    {
        std::lock_guard<std::mutex> lock(m_producerMutex);
        self->PublishLocked();
        std::lock_guard<std::mutex> queueLock(m_queueMutex);
        target = m_published;
    }
    const auto t0 = std::chrono::steady_clock::now();
    std::unique_lock<std::mutex> lock(m_queueMutex);
    m_doneCv.wait(lock, [&]
                  { return m_completed >= target || m_stop; });
    if (waitStatsOn())
        noteWait(t_waitReason, std::chrono::steady_clock::now() - t0);
}

void GSThreadedBackend::WorkerLoop()
{
    ThreadNaming::SetCurrentThreadName("GSThread");
    ps2ProfilerRegisterCurrentThread("gs");
    ThreadPriority::RaiseCurrentThread(ThreadPriority::Level::AboveNormal);
    GSPrimitiveBatch draw;
    for (;;)
    {
        std::vector<uint8_t> chunk;
        {
            std::unique_lock<std::mutex> lock(m_queueMutex);
            if (m_pending.empty())
                PS2TL(TL_GS_IDLE);
            m_workCv.wait(lock, [&]
                          { return m_stop || !m_pending.empty(); });
            PS2TL(TL_GS_BUSY, static_cast<uint32_t>(m_pending.size()));
            if (m_stop)
            {
                m_inner.reset();
                return;
            }
            chunk = std::move(m_pending.front());
            m_pending.pop_front();
        }
        {
            std::lock_guard<std::mutex> exec(m_execMutex);
            Execute(chunk, draw);
        }
        {
            std::lock_guard<std::mutex> lock(m_queueMutex);
            ++m_completed;
            if (m_spare.size() < 64u)
                m_spare.push_back(std::move(chunk));
        }
        m_doneCv.notify_all();
    }
}

static uint32_t s_phasePresents = 0u;

void GSThreadedBackend::Execute(const std::vector<uint8_t> &chunk, GSPrimitiveBatch &draw)
{
    const uint8_t *data = chunk.data();
    const size_t size = chunk.size();
    size_t offset = 0u;
    // PS2X_GS_PHASES=1: count, bytes and time per kind of queued operation, every 120 presents.
    static const bool phases = std::getenv("PS2X_GS_PHASES") != nullptr;
    static uint64_t phaseTicks[16], phaseCount[16], phaseBytes[16];
    uint64_t phaseStart = 0u;
    int phaseOp = -1;
    while (offset + 5u <= size)
    {
        const Op op = static_cast<Op>(data[offset]);
        const uint32_t length = readAt<uint32_t>(data + offset + 1u);
        const uint8_t *payload = data + offset + 5u;
        offset += 5u + length;
        if (phases)
        {
            const uint64_t now = __rdtsc();
            if (phaseOp >= 0)
                phaseTicks[phaseOp] += now - phaseStart;
            phaseStart = now;
            phaseOp = static_cast<int>(op) & 15;
            ++phaseCount[phaseOp];
            phaseBytes[phaseOp] += length;
            if (op == Op::Present && ++s_phasePresents == 120u)
            {
                s_phasePresents = 0u;
                std::fprintf(stderr, "[gs:phases]");
                for (int i = 0; i < 16; ++i)
                    if (phaseCount[i])
                        std::fprintf(stderr, " op%d n=%.0f kb=%.0f mcyc=%.2f;", i, phaseCount[i] / 120.0, phaseBytes[i] / 120.0 / 1024.0, phaseTicks[i] / 120.0 / 1e6);
                std::fprintf(stderr, " upload-kb:");
                extern uint64_t g_uploadBytesByPsm[64];
                for (int i = 0; i < 64; ++i)
                    if (g_uploadBytesByPsm[i])
                    {
                        std::fprintf(stderr, " psm%02x=%.0f", i, g_uploadBytesByPsm[i] / 120.0 / 1024.0);
                        g_uploadBytesByPsm[i] = 0u;
                    }
                std::fputc(10, stderr);
                std::memset(phaseCount, 0, sizeof(phaseCount));
                std::memset(phaseBytes, 0, sizeof(phaseBytes));
                std::memset(phaseTicks, 0, sizeof(phaseTicks));
            }
        }
        switch (op)
        {
        case Op::Mxcsr:
            _mm_setcsr(readAt<uint32_t>(payload));
            break;
        case Op::Submit:
            if (!discardDraws())
                m_inner->Submit(readAt<GSPrimitiveBatch>(payload));
            break;
        case Op::LoadClut:
        {
            const LoadClutArgs args = readAt<LoadClutArgs>(payload);
            m_inner->LoadClut(args.tex0, args.texclut);
            break;
        }
        case Op::BeginTransfer:
            m_inner->BeginTransfer(readAt<GSTransferCommand>(payload));
            break;
        case Op::Upload:
            m_inner->UploadImage(payload, length);
            break;
        case Op::TextureFlush:
            m_inner->TextureFlush();
            break;
        case Op::WriteVram:
        {
            const WriteVramArgs a = readAt<WriteVramArgs>(payload);
            m_inner->WriteVram(a.psm, a.base, a.bw, a.x, a.y, a.value);
            break;
        }
        case Op::Sync:
            m_inner->Sync(readAt<GSSyncReason>(payload));
            break;
        case Op::DrawState:
            std::memcpy(&draw.state, payload, sizeof(draw.state));
            break;
        case Op::Vertices:
            std::memcpy(&draw, payload, offsetof(GSPrimitiveBatch, state));
            if (!discardDraws())
                m_inner->Submit(draw);
            break;
        case Op::Present:
            PS2TL(TL_PRESENT_BEGIN);
            m_inner->Present(readAt<GSPresentationRequest>(payload));
            PS2TL(TL_PRESENT_END);
            break;
        case Op::Call:
            (*readAt<const std::function<void()> *>(payload))();
            break;
        }
    }
    if (phases && phaseOp >= 0)
        phaseTicks[phaseOp] += __rdtsc() - phaseStart;
}

void GSThreadedBackend::RunOnWorker(const std::function<void()> &function) const
{
    auto *self = const_cast<GSThreadedBackend *>(this);
    const std::function<void()> *pointer = &function;
    self->Enqueue(Op::Call, &pointer, sizeof(pointer));
    Drain();
}

void GSThreadedBackend::Initialize(uint8_t *vram, uint32_t vramSize)
{
    RunOnWorker([&] {
        m_inner->Initialize(vram, vramSize);
        m_sharedPresentation.store(m_inner->UsesSharedPresentation(), std::memory_order_release);
    });
}

void GSThreadedBackend::Reset()
{
    RunOnWorker([&] { m_inner->Reset(); });
}

void GSThreadedBackend::Submit(const GSPrimitiveBatch &batch)
{
    if (GsInventory::get().enabled())
        GsInventory::get().draw(batch);
    Enqueue(Op::Submit, &batch, sizeof(batch));
}

void GSThreadedBackend::LoadClut(const GSTex0Reg &tex0, const GSTexClutReg &texclut)
{
    if (GsInventory::get().enabled())
        GsInventory::get().clut(tex0);
    const LoadClutArgs args{tex0, texclut};
    Enqueue(Op::LoadClut, &args, sizeof(args));
}

void GSThreadedBackend::BeginTransfer(const GSTransferCommand &command)
{
    if (GsInventory::get().enabled())
        GsInventory::get().transfer(command);
    Enqueue(Op::BeginTransfer, &command, sizeof(command));
}

void GSThreadedBackend::UploadImage(const uint8_t *data, uint32_t sizeBytes)
{
    Enqueue(Op::Upload, data, data ? sizeBytes : 0u);
}

void GSThreadedBackend::Flush()
{
    // GOW-Port: barrera para lectura/presentación y llamadas del frontend existente.
    WaitTag tag(kWaitFlush);
    RunOnWorker([&] { m_inner->Flush(); });
}

void GSThreadedBackend::TextureFlush()
{
    Enqueue(Op::TextureFlush, nullptr, 0u);
}

// GOW-Port: GOW_GS_FINISH_ASINCRONO=1 (opcional, desactivado por defecto) encola FINISH en el hilo del GS
// sin esperar ni glFinish: el EE ve el bit FINISH enseguida. Las lecturas de VRAM desde el EE ya sincronizan
// por su cuenta (RunOnWorker) y el hilo del GS conserva el orden de los comandos.
static bool gowFinishAsync()
{
    static const bool on = [] { const char *v = std::getenv("GOW_GS_FINISH_ASINCRONO"); return v && v[0] == '1'; }();
    return on;
}

void GSThreadedBackend::Sync(GSSyncReason reason)
{
    if (reason == GSSyncReason::Finish && gowFinishAsync())
    {
        Enqueue(Op::Sync, &reason, sizeof(reason));
        return;
    }
    if (reason == GSSyncReason::Presentation && UsesSharedPresentation())
        return;
    // GOW-Port: FINISH solo se publica cuando terminaron los comandos anteriores.
    WaitTag tag(reason == GSSyncReason::Finish ? kWaitFinish : reason == GSSyncReason::Presentation ? kWaitPresent : kWaitSync);
    RunOnWorker([&] { m_inner->Sync(reason); });
}

PresentationFrame GSThreadedBackend::Present(const GSPresentationRequest &request)
{
    if (GsInventory::get().enabled())
        GsInventory::get().present();
    if (UsesSharedPresentation() && !request.forceReadback)
    {
        Enqueue(Op::Present, &request, sizeof(request));
        std::lock_guard<std::mutex> lock(m_producerMutex);
        PublishLocked();
        return {};
    }
    PresentationFrame frame;
    WaitTag tag(kWaitPresent);
    RunOnWorker([&] { frame = m_inner->Present(request); });
    return frame;
}

bool GSThreadedBackend::ClearFramebuffer(const GSContext &context, uint32_t rgba)
{
    if (GsInventory::get().enabled())
        GsInventory::get().clear();
    bool result = false;
    WaitTag tag(kWaitClear);
    RunOnWorker([&] { result = m_inner->ClearFramebuffer(context, rgba); });
    return result;
}

uint32_t GSThreadedBackend::ConsumeLocalToHostBytes(uint8_t *dst, uint32_t maxBytes)
{
    if (GsInventory::get().enabled())
        GsInventory::get().consumeL2H();
    uint32_t result = 0u;
    RunOnWorker([&] { result = m_inner->ConsumeLocalToHostBytes(dst, maxBytes); });
    return result;
}

uint32_t GSThreadedBackend::ReadVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y) const
{
    if (GsInventory::get().enabled())
        GsInventory::get().cpuRead();
    uint32_t result = 0u;
    WaitTag tag(kWaitRead);
    RunOnWorker([&] { result = m_inner->ReadVram(psm, base, bw, x, y); });
    return result;
}

void GSThreadedBackend::WriteVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value)
{
    if (GsInventory::get().enabled())
        GsInventory::get().cpuWrite();
    const WriteVramArgs args{psm, base, bw, x, y, value};
    Enqueue(Op::WriteVram, &args, sizeof(args));
}

void GSThreadedBackend::SnapshotVram(std::vector<uint8_t> &out) const
{
    if (GsInventory::get().enabled())
        GsInventory::get().snapshot();
    RunOnWorker([&] { m_inner->SnapshotVram(out); });
}

GSTransferSnapshot GSThreadedBackend::GetTransferSnapshot() const
{
    GSTransferSnapshot result;
    RunOnWorker([&] { result = m_inner->GetTransferSnapshot(); });
    return result;
}

bool GSThreadedBackend::ExportState(GSBackendState &out)
{
    bool result = false;
    RunOnWorker([&]
                {
        auto *access = dynamic_cast<GSBackendStateAccess *>(m_inner.get());
        result = access && access->ExportState(out); });
    return result;
}

bool GSThreadedBackend::ImportState(const GSBackendState &state)
{
    bool result = false;
    RunOnWorker([&]
                {
        auto *access = dynamic_cast<GSBackendStateAccess *>(m_inner.get());
        result = access && access->ImportState(state); });
    return result;
}
