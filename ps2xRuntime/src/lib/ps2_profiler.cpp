// Built-in sampling profiler (PS2X_PROFILE=1). Samples the instruction pointer of
// the registered threads every millisecond and writes module-relative offsets with
// hit counts to analysis/profile.txt; scripts/profile_report.py resolves them with
// the linker map. Kept in its own file so windows.h stays away from raylib.
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <timeapi.h>
#pragma comment(lib, "winmm.lib")
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {
#ifdef _WIN32
    struct SampledThread
    {
        HANDLE handle;
        const char *name;
        std::unordered_map<uint64_t, uint32_t> hits;
        std::unordered_map<uint64_t, uint32_t> callers; // samples outside the image, by the first two image frames above them
        std::unordered_map<uint64_t, uint32_t> dllIps;  // exact addresses of those samples (module-relative in the report)
        uint64_t total = 0;
    };
    std::mutex g_mutex;
    std::vector<SampledThread> g_threads;
    std::atomic<bool> g_started{false};

    void writeReport(uintptr_t base)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        FILE *file = std::fopen("analysis/profile.txt", "w");
        if (!file)
            return;
        for (const SampledThread &thread : g_threads)
        {
            std::vector<std::pair<uint64_t, uint32_t>> sorted(thread.hits.begin(), thread.hits.end());
            std::sort(sorted.begin(), sorted.end(), [](const auto &a, const auto &b) { return a.second > b.second; });
            std::fprintf(file, "thread %s samples %llu\n", thread.name, static_cast<unsigned long long>(thread.total));
            for (const auto &entry : sorted)
            {
                if ((entry.first >> 63) == 0u)
                    continue;
                wchar_t path[MAX_PATH]{};
                GetModuleFileNameW(reinterpret_cast<HMODULE>(entry.first & ~(1ull << 63)), path, MAX_PATH);
                std::fprintf(file, "module %llx %ls\n", static_cast<unsigned long long>(entry.first), path);
            }
            for (size_t i = 0; i < sorted.size(); ++i)
                std::fprintf(file, "%llx %u\n", static_cast<unsigned long long>(sorted[i].first), sorted[i].second);
            for (const auto &entry : thread.dllIps)
            {
                HMODULE module = nullptr;
                if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                       reinterpret_cast<LPCWSTR>(entry.first), &module))
                {
                    wchar_t path[MAX_PATH]{};
                    GetModuleFileNameW(module, path, MAX_PATH);
                    std::fprintf(file, "dllip %llx %u %ls\n", static_cast<unsigned long long>(entry.first - reinterpret_cast<uintptr_t>(module)),
                                 entry.second, path);
                }
            }
            for (const auto &entry : thread.callers)
                std::fprintf(file, "caller %llx %u\n", static_cast<unsigned long long>(entry.first), entry.second);
        }
        std::fclose(file);
        (void)base;
    }

    // Offsets of the first two frames inside the image above a sample taken in a DLL or the kernel stubs, packed in one
    // key (0: none found).
    uint64_t firstImageFrames(CONTEXT context, uintptr_t base)
    {
        uint64_t key = 0u;
        for (int depth = 0; depth < 32; ++depth)
        {
            DWORD64 imageBase = 0;
            RUNTIME_FUNCTION *function = RtlLookupFunctionEntry(context.Rip, &imageBase, nullptr);
            if (function)
            {
                void *handlerData = nullptr;
                DWORD64 frame = 0;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, context.Rip, function, &context, &handlerData, &frame, nullptr);
            }
            else
            {
                context.Rip = *reinterpret_cast<DWORD64 *>(context.Rsp);
                context.Rsp += 8u;
            }
            const uintptr_t ip = static_cast<uintptr_t>(context.Rip);
            if (ip == 0u)
                return key;
            if (ip >= base && ip - base < 0x40000000u)
            {
                if (key != 0u)
                    return key | (static_cast<uint64_t>(ip - base) << 32);
                key = ip - base;
            }
        }
        return key;
    }

    void samplerLoop()
    {
        const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        timeBeginPeriod(1);
        auto lastReport = std::chrono::steady_clock::now();
        // PS2X_PROFILE_AT=<s>: sample only from then on; boot and menus would otherwise count as idle time.
        static const double sampleFrom = std::getenv("PS2X_PROFILE_AT") ? std::atof(std::getenv("PS2X_PROFILE_AT")) : 0.0;
        const auto samplerStart = std::chrono::steady_clock::now();
        for (;;)
        {
            Sleep(1);
            if (std::chrono::duration<double>(std::chrono::steady_clock::now() - samplerStart).count() < sampleFrom)
                continue;
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                for (SampledThread &thread : g_threads)
                {
                    if (SuspendThread(thread.handle) == static_cast<DWORD>(-1))
                        continue;
                    CONTEXT context{};
                    context.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
                    uint64_t offset = 0u, frames = 0u;
                    uintptr_t ip = 0u;
                    const bool sampled = GetThreadContext(thread.handle, &context) != 0;
                    if (sampled)
                    {
                        ip = static_cast<uintptr_t>(context.Rip);
                        // Offsets outside the image (system DLLs) are grouped as 0.
                        offset = ip >= base && ip - base < 0x40000000u ? ip - base : 0u;
                        if (offset == 0u)
                        {
                            // Outside the image: tag by module (high bit + module base) so the
                            // report can name the DLL.
                            HMODULE module = nullptr;
                            if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                                   reinterpret_cast<LPCWSTR>(ip), &module))
                                offset = (1ull << 63) | reinterpret_cast<uintptr_t>(module);
                            frames = firstImageFrames(context, base);
                        }
                    }
                    ResumeThread(thread.handle);
                    // Never allocate while the thread is suspended: it may hold the heap lock.
                    if (sampled)
                    {
                        ++thread.hits[offset];
                        ++thread.total;
                        if (offset >> 63)
                        {
                            ++thread.callers[frames];
                            ++thread.dllIps[ip];
                        }
                    }
                }
            }
            if (std::chrono::steady_clock::now() - lastReport > std::chrono::seconds(10))
            {
                writeReport(base);
                lastReport = std::chrono::steady_clock::now();
            }
        }
    }
#endif
}

#ifdef _WIN32
namespace {
    // One line for an unhandled crash, in image offsets (resolve with the linker map).
    LONG WINAPI crashReport(EXCEPTION_POINTERS *info)
    {
        const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        const CONTEXT &c = *info->ContextRecord;
        std::fprintf(stderr, "[crash] code=%08lx rip=%llx (image+%llx) access=%llx rsp=%llx stack:",
                     info->ExceptionRecord->ExceptionCode, static_cast<unsigned long long>(c.Rip),
                     static_cast<unsigned long long>(c.Rip - base),
                     static_cast<unsigned long long>(info->ExceptionRecord->NumberParameters > 1 ? info->ExceptionRecord->ExceptionInformation[1] : 0u),
                     static_cast<unsigned long long>(c.Rsp));
        const uintptr_t *sp = reinterpret_cast<const uintptr_t *>(c.Rsp);
        for (int i = 0, shown = 0; i < 256 && shown < 12; ++i)
            if (sp[i] >= base && sp[i] - base < 0x40000000u)
            {
                std::fprintf(stderr, " %llx", static_cast<unsigned long long>(sp[i] - base));
                ++shown;
            }
        std::fputc(10, stderr);
        std::fflush(stderr);
        return EXCEPTION_CONTINUE_SEARCH;
    }
    const LPTOP_LEVEL_EXCEPTION_FILTER g_crashFilter = SetUnhandledExceptionFilter(crashReport);
    // PS2X_EH_STATS=1: every 5 s, C++ exceptions thrown and their commonest origins (image offsets of the first frames).
    std::atomic<uint32_t> g_ehCount{0};
    std::unordered_map<uint64_t, uint32_t> g_ehOrigins;
    std::mutex g_ehMutex;
    LONG CALLBACK ehStats(EXCEPTION_POINTERS *info)
    {
        if (info->ExceptionRecord->ExceptionCode != 0xE06D7363u)
            return EXCEPTION_CONTINUE_SEARCH;
        ++g_ehCount;
        const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        void *frames[32]{};
        const USHORT n = RtlCaptureStackBackTrace(1, 32, frames, nullptr);
        uint64_t key = 0u;
        for (USHORT i = 0, found = 0; i < n && found < 2u; ++i)
        {
            const uintptr_t ip = reinterpret_cast<uintptr_t>(frames[i]);
            if (ip >= base && ip - base < 0x40000000u)
                key |= static_cast<uint64_t>(ip - base) << (found++ * 32u);
        }
        std::lock_guard<std::mutex> lock(g_ehMutex);
        ++g_ehOrigins[key];
        static auto last = std::chrono::steady_clock::now();
        const auto now = std::chrono::steady_clock::now();
        if (now - last > std::chrono::seconds(5))
        {
            last = now;
            std::fprintf(stderr, "[eh] count=%u", g_ehCount.exchange(0));
            for (int shown = 0; shown < 6 && !g_ehOrigins.empty(); ++shown)
            {
                auto top = g_ehOrigins.begin();
                for (auto it = g_ehOrigins.begin(); it != g_ehOrigins.end(); ++it)
                    if (it->second > top->second)
                        top = it;
                std::fprintf(stderr, " %llx<-%llx=%u", static_cast<unsigned long long>(top->first & 0xFFFFFFFFu),
                             static_cast<unsigned long long>(top->first >> 32), top->second);
                g_ehOrigins.erase(top);
            }
            g_ehOrigins.clear();
            std::fputc(10, stderr);
        }
        return EXCEPTION_CONTINUE_SEARCH;
    }
    const bool g_ehStatsOn = std::getenv("PS2X_EH_STATS") && AddVectoredExceptionHandler(1, ehStats);

}
#endif

void ps2ProfilerRegisterCurrentThread(const char *name)
{
#ifdef _WIN32
    // PS2X_PIN=game:2,vif:4,...: thread name to logical processor.
    if (const char *pins = std::getenv("PS2X_PIN"))
    {
        const std::string list = std::string(",") + pins, key = std::string(",") + name + ":";
        const size_t at = list.find(key);
        if (at != std::string::npos)
            SetThreadAffinityMask(GetCurrentThread(), 1ull << std::atoi(list.c_str() + at + key.size()));
    }
    const char *enabled = std::getenv("PS2X_PROFILE");
    if (!enabled || enabled[0] != '1')
        return;
    HANDLE handle = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE,
                               GetCurrentThreadId());
    if (!handle)
        return;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_threads.push_back(SampledThread{handle, name, {}, {}, {}, 0});
    }
    if (!g_started.exchange(true))
        std::thread(samplerLoop).detach();
#else
    (void)name;
#endif
}

// Guest memory watchpoint for debugging (PS2X_WATCH=0xADDR:LEN). The host page holding
// the range is write-protected; each write is single-stepped and, when it lands in the
// range, logged with the EE PC reported by the callback and the new 32-bit value.
namespace {
#ifdef _WIN32
    uint8_t *g_watchHost = nullptr;
    uint32_t g_watchGuest = 0, g_watchLength = 0;
    uintptr_t g_watchPage = 0;
    uint32_t (*g_watchPc)() = nullptr;
    void (*g_watchDump)() = nullptr;
    thread_local uintptr_t g_watchFault = 0;
    std::atomic<uint32_t> g_watchLogs{0};

    LONG CALLBACK watchHandler(EXCEPTION_POINTERS *info)
    {
        DWORD old = 0;
        const DWORD code = info->ExceptionRecord->ExceptionCode;
        if (code == EXCEPTION_ACCESS_VIOLATION && info->ExceptionRecord->ExceptionInformation[0] == 1)
        {
            const uintptr_t address = static_cast<uintptr_t>(info->ExceptionRecord->ExceptionInformation[1]);
            if ((address & ~uintptr_t(0xFFF)) != g_watchPage)
                return EXCEPTION_CONTINUE_SEARCH;
            // PS2X_WATCH_RIPS=1: print each distinct host instruction that stores into the watched page once, with its
            // symbol (generated FUN_/gap_ code vs. runtime HLE code that writes guest memory).
            static const bool rips = std::getenv("PS2X_WATCH_RIPS") != nullptr;
            if (rips)
            {
                static std::mutex ripLock;
                static std::unordered_set<uintptr_t> seen;
                const uintptr_t rip = reinterpret_cast<uintptr_t>(info->ExceptionRecord->ExceptionAddress);
                std::lock_guard<std::mutex> guard(ripLock);
                if (seen.insert(rip).second && seen.size() < 20000u)
                {
                    static bool symInit = false;
                    if (!symInit)
                    {
                        SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
                        SymInitialize(GetCurrentProcess(), nullptr, TRUE);
                        symInit = true;
                    }
                    alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 256] = {};
                    auto *symbol = reinterpret_cast<SYMBOL_INFO *>(buffer);
                    symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
                    symbol->MaxNameLen = 255;
                    DWORD64 displacement = 0;
                    const char *name = SymFromAddr(GetCurrentProcess(), rip, &displacement, symbol) ? symbol->Name : "?";
                    std::fprintf(stderr, "[watch:rip] guest=0x%x tid=%lu rip=%p %s+0x%llx pc=0x%x\n",
                                 static_cast<uint32_t>(address - reinterpret_cast<uintptr_t>(g_watchHost)),
                                 static_cast<unsigned long>(GetCurrentThreadId()), info->ExceptionRecord->ExceptionAddress,
                                 name, static_cast<unsigned long long>(displacement), g_watchPc ? g_watchPc() : 0u);
                }
            }
            g_watchFault = address;
            VirtualProtect(reinterpret_cast<void *>(g_watchPage), 0x1000, PAGE_READWRITE, &old);
            info->ContextRecord->EFlags |= 0x100; // trap after the faulting store
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        if (code == EXCEPTION_SINGLE_STEP && g_watchFault != 0)
        {
            const uintptr_t start = reinterpret_cast<uintptr_t>(g_watchHost + g_watchGuest);
            // PS2X_WATCH_FOREIGN=1: report only stores from a host thread other than the first one that wrote the
            // watched range (the EE executor), i.e. DMA/IOP/GS/audio threads writing guest RAM behind the EE's back.
            static const bool foreignOnly = std::getenv("PS2X_WATCH_FOREIGN") != nullptr;
            if (foreignOnly && g_watchFault + 16u > start && g_watchFault < start + g_watchLength)
            {
                static DWORD eeTid = 0;
                const DWORD tid = GetCurrentThreadId();
                if (eeTid == 0)
                    eeTid = tid;
                if (tid != eeTid)
                {
                    const uint32_t n = g_watchLogs.fetch_add(1);
                    if (n < 2000u)
                    {
                        const uint32_t offset = static_cast<uint32_t>((g_watchFault & ~uintptr_t(3)) - reinterpret_cast<uintptr_t>(g_watchHost));
                        uint32_t bits = 0;
                        std::memcpy(&bits, g_watchHost + offset, 4);
                        std::fprintf(stderr, "[watch:foreign] tid=%lu ee_tid=%lu guest=0x%x value=0x%08x pc=0x%x rip=%p\n",
                                     static_cast<unsigned long>(tid), static_cast<unsigned long>(eeTid), offset, bits,
                                     g_watchPc ? g_watchPc() : 0u, info->ExceptionRecord->ExceptionAddress);
                    }
                }
                g_watchFault = 0;
                VirtualProtect(reinterpret_cast<void *>(g_watchPage), 0x1000, PAGE_READONLY, &old);
                return EXCEPTION_CONTINUE_EXECUTION;
            }
            if (g_watchFault + 16u > start && g_watchFault < start + g_watchLength)
            {
                // PS2X_WATCH_CHANGES: report the watched word itself, only when its value changes.
                static const bool changesOnly = std::getenv("PS2X_WATCH_CHANGES") != nullptr;
                static uint32_t last = 0;
                uint32_t offset = static_cast<uint32_t>((g_watchFault & ~uintptr_t(3)) - reinterpret_cast<uintptr_t>(g_watchHost));
                if (changesOnly)
                    offset = g_watchGuest & ~3u;
                uint32_t bits = 0;
                std::memcpy(&bits, g_watchHost + offset, 4);
                if (changesOnly && bits == last)
                {
                    g_watchFault = 0;
                    VirtualProtect(reinterpret_cast<void *>(g_watchPage), 0x1000, PAGE_READONLY, &old);
                    return EXCEPTION_CONTINUE_EXECUTION;
                }
                last = bits;
                float value = 0.0f;
                std::memcpy(&value, &bits, 4);
                const bool abnormal = !(value == value) || value > 1.0e7f || value < -1.0e7f;
                const uint32_t n = g_watchLogs.fetch_add(1);
                if (n < 64u || (abnormal && n < 4096u) || (changesOnly && n < 200000u))
                    std::fprintf(stderr, "[watch] guest=0x%x value=0x%08x (%g) pc=0x%x%s\n", offset, bits, value,
                                 g_watchPc ? g_watchPc() : 0u, abnormal ? " ABNORMAL" : "");
                if (abnormal && g_watchDump && n < 4096u && !(value > 9.0e29f || value < -9.0e29f))
                    g_watchDump();
            }
            g_watchFault = 0;
            VirtualProtect(reinterpret_cast<void *>(g_watchPage), 0x1000, PAGE_READONLY, &old);
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        return EXCEPTION_CONTINUE_SEARCH;
    }
#endif
}

void ps2WatchInstall(uint8_t *rdram, uint32_t (*currentPc)(), void (*dump)())
{
#ifdef _WIN32
    const char *spec = std::getenv("PS2X_WATCH");
    if (!spec || !rdram)
        return;
    unsigned guest = 0, length = 4;
    if (std::sscanf(spec, "%x:%u", &guest, &length) < 1)
        return;
    g_watchHost = rdram;
    g_watchGuest = guest & 0x1FFFFFFu;
    g_watchLength = length;
    g_watchPc = currentPc;
    g_watchDump = dump;
    g_watchPage = reinterpret_cast<uintptr_t>(rdram + g_watchGuest) & ~uintptr_t(0xFFF);
    AddVectoredExceptionHandler(1, watchHandler);
    DWORD old = 0;
    VirtualProtect(reinterpret_cast<void *>(g_watchPage), 0x1000, PAGE_READONLY, &old);
    std::fprintf(stderr, "[watch] guest 0x%x len %u\n", g_watchGuest, g_watchLength);
#else
    (void)rdram;
    (void)currentPc;
    (void)dump;
#endif
}
