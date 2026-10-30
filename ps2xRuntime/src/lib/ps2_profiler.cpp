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
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {
#ifdef _WIN32
    struct SampledThread
    {
        HANDLE handle;
        const char *name;
        std::unordered_map<uint64_t, uint32_t> hits;
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
            for (size_t i = 0; i < sorted.size() && i < 4000u; ++i)
                std::fprintf(file, "%llx %u\n", static_cast<unsigned long long>(sorted[i].first), sorted[i].second);
        }
        std::fclose(file);
        (void)base;
    }

    void samplerLoop()
    {
        const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        timeBeginPeriod(1);
        auto lastReport = std::chrono::steady_clock::now();
        for (;;)
        {
            Sleep(1);
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                for (SampledThread &thread : g_threads)
                {
                    if (SuspendThread(thread.handle) == static_cast<DWORD>(-1))
                        continue;
                    CONTEXT context{};
                    context.ContextFlags = CONTEXT_CONTROL;
                    if (GetThreadContext(thread.handle, &context))
                    {
                        const uintptr_t ip = static_cast<uintptr_t>(context.Rip);
                        // Offsets outside the image (system DLLs) are grouped as 0.
                        uint64_t offset = ip >= base && ip - base < 0x40000000u ? ip - base : 0u;
                        if (offset == 0u)
                        {
                            // Outside the image: tag by module (high bit + module base) so the
                            // report can name the DLL.
                            HMODULE module = nullptr;
                            if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                                   reinterpret_cast<LPCWSTR>(ip), &module))
                                offset = (1ull << 63) | reinterpret_cast<uintptr_t>(module);
                        }
                        ++thread.hits[offset];
                        ++thread.total;
                    }
                    ResumeThread(thread.handle);
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

void ps2ProfilerRegisterCurrentThread(const char *name)
{
#ifdef _WIN32
    const char *enabled = std::getenv("PS2X_PROFILE");
    if (!enabled || enabled[0] != '1')
        return;
    HANDLE handle = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE,
                               GetCurrentThreadId());
    if (!handle)
        return;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_threads.push_back(SampledThread{handle, name, {}, 0});
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
            g_watchFault = address;
            VirtualProtect(reinterpret_cast<void *>(g_watchPage), 0x1000, PAGE_READWRITE, &old);
            info->ContextRecord->EFlags |= 0x100; // trap after the faulting store
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        if (code == EXCEPTION_SINGLE_STEP && g_watchFault != 0)
        {
            const uintptr_t start = reinterpret_cast<uintptr_t>(g_watchHost + g_watchGuest);
            if (g_watchFault + 16u > start && g_watchFault < start + g_watchLength)
            {
                const uint32_t offset = static_cast<uint32_t>((g_watchFault & ~uintptr_t(3)) - reinterpret_cast<uintptr_t>(g_watchHost));
                uint32_t bits = 0;
                std::memcpy(&bits, g_watchHost + offset, 4);
                float value = 0.0f;
                std::memcpy(&value, &bits, 4);
                const bool abnormal = !(value == value) || value > 1.0e7f || value < -1.0e7f;
                const uint32_t n = g_watchLogs.fetch_add(1);
                if (n < 64u || (abnormal && n < 4096u))
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
