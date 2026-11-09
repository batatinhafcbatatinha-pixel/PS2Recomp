// See ps2_timed_wait.h: libc++ on Windows truncates short timed waits to zero; wait the rounded-up time instead.
#include "ps2_timed_wait.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace ps2_wait
{
    void waitWhileEqual(const volatile void *address, const void *compare, std::size_t size)
    {
        WaitOnAddress(const_cast<volatile void *>(address), const_cast<void *>(compare), size, INFINITE);
    }

    void wakeOne(void *address)
    {
        WakeByAddressSingle(address);
    }
}
#endif

#if defined(_WIN32) && defined(_LIBCPP_VERSION)
namespace ps2_wait
{
    void waitMilliseconds(std::condition_variable &cv, std::unique_lock<std::mutex> &lock, long long ms)
    {
        // libc++'s Windows condition_variable and mutex are a CONDITION_VARIABLE and an SRWLOCK.
        SleepConditionVariableSRW(reinterpret_cast<PCONDITION_VARIABLE>(cv.native_handle()),
                                  reinterpret_cast<PSRWLOCK>(lock.mutex()->native_handle()),
                                  static_cast<DWORD>(ms > 0x7FFFFFFF ? 0x7FFFFFFF : ms), 0);
    }
}
#endif
