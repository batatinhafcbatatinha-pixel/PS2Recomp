// The Sopranos: Road to Respect: what is still specific to this game in the generic pipeline (tools/ps2port, port.toml).
// Included by tools/ps2port/project/main.cpp after the table helpers (PortSlot).
//
// Stream mutex (sema id in 0x1CA3A80). The IOP's STREAM.IRX notifies the EE through the EE RPC server 0x332588
// (sid 0x12344321), whose handler runs the stream callback 0x329EC8, which takes the mutex. On the PS2 that server
// runs in its own thread; here it is dispatched inside whatever EE thread is running. When that is the thread that
// already holds the mutex (inside one of the lock holders below), WaitSema blocked it forever (New Game -> black
// screen, [idle] t1 why=2 obj=3 pc=0x329f1c). The callback then borrows the free mutex for its own run.
// SOP_NO_STREAM_GUARD=1 turns this off.
// route: {0x328550u, 0x328978u, 0x328bc8u, 0x32ad68u, 0x32af00u} lock holders, {0x329ec8u, SopStreamCallback}
#include "runtime/ee_scheduler.h"
#include <map>
#include <mutex>

static const uint32_t kSopStreamLockHolders[] = {0x328550u, 0x328978u, 0x328bc8u, 0x32ad68u, 0x32af00u};
static PS2Runtime::RecompiledFunction g_sopHolderOriginal[5] = {};
static PS2Runtime::RecompiledFunction g_sopStreamCbOriginal = nullptr;
static std::map<int, int> g_sopHolderDepth; // EE thread id -> nesting inside lock holders
static std::mutex g_sopStreamMutex;

template <int I>
static void SopStreamHolder(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime) {
    const int tid = runtime->eeScheduler().currentThreadId();
    { std::lock_guard<std::mutex> lock(g_sopStreamMutex); ++g_sopHolderDepth[tid]; }
    g_sopHolderOriginal[I](rdram, ctx, runtime);
    std::lock_guard<std::mutex> lock(g_sopStreamMutex);
    --g_sopHolderDepth[tid];
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
        static int logs = 0;
        if (logs++ < 8) std::cerr << "[sop] stream callback re-entered by its holder (thread " << tid << ")" << std::endl;
        sema->count = 1;
    }
    g_sopStreamCbOriginal(rdram, ctx, runtime);
    if (lend) {
        if (EeSemaphore* again = ee.semaphore(static_cast<int>(semaId)); again && again->count > 0) again->count = 0;
    }
}

static void PortProfileInstall() {
    if (std::getenv("SOP_NO_STREAM_GUARD")) return;
    PS2Runtime::RecompiledFunction wrappers[5] = {SopStreamHolder<0>, SopStreamHolder<1>, SopStreamHolder<2>,
                                                 SopStreamHolder<3>, SopStreamHolder<4>};
    PS2Runtime::RecompiledFunction* slots[5];
    for (int i = 0; i < 5; ++i) {
        slots[i] = PortSlot(kSopStreamLockHolders[i]);
        if (!slots[i] || !*slots[i]) { std::cerr << "[sop] stream lock holder missing\n"; return; }
    }
    auto* cb = PortSlot(0x329ec8u);
    if (!cb || !*cb) { std::cerr << "[sop] stream callback missing\n"; return; }
    for (int i = 0; i < 5; ++i) {
        g_sopHolderOriginal[i] = *slots[i];
        *slots[i] = wrappers[i];
    }
    g_sopStreamCbOriginal = *cb;
    *cb = SopStreamCallback;
}
