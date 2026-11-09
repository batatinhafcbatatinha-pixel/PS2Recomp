#pragma once
// Timed condition-variable waits that behave the same with every C++ library on Windows.
//
// MSVC's STL rounds a short timeout up to whole milliseconds, so wait_for(200us) sleeps about a millisecond. libc++
// (llvm-mingw) truncates the remaining time to milliseconds instead: wait_for(200us) returns after ~4 us and
// wait_until wakes up to a millisecond early, so the loops around these waits spin and fight for the mutex (Ben 10's
// VIF backlog wait took a whole core and starved the GS: 4 fps instead of 60). With libc++ on Windows the wait goes
// to SleepConditionVariableSRW with the timeout rounded up, like MSVC (src/lib/Kernel/ps2_timed_wait.cpp); elsewhere
// these are the standard calls.
#include <chrono>
#include <cstddef>
#include <condition_variable>
#include <mutex>

namespace ps2_wait
{
#if defined(_WIN32)
    // std::atomic<T>::wait / notify_one for Windows threads that hand work back and forth: the kernel's WaitOnAddress
    // directly. MSVC's STL does the same; libc++ first polls with yields and sleeps and notifies through a global
    // table, so a waiter barely sleeps and keeps the producer's cache line bouncing (Ben 10's GL thread: 60% busy
    // doing nothing, game at 20 fps instead of 60).
    void waitWhileEqual(const volatile void *address, const void *compare, std::size_t size);
    void wakeOne(void *address);
#endif
#if defined(_WIN32) && defined(_LIBCPP_VERSION)
    // One wait of `ms` milliseconds at most, woken early by notify_*.
    void waitMilliseconds(std::condition_variable &cv, std::unique_lock<std::mutex> &lock, long long ms);

    template <class Rep, class Period>
    inline void waitFor(std::condition_variable &cv, std::unique_lock<std::mutex> &lock,
                        const std::chrono::duration<Rep, Period> &timeout)
    {
        const long long ms = std::chrono::ceil<std::chrono::milliseconds>(timeout).count();
        if (ms > 0)
            waitMilliseconds(cv, lock, ms);
    }

    // Like cv.wait_until(lock, deadline, pred): true when pred holds, false once the deadline has passed.
    template <class Clock, class Duration, class Pred>
    inline bool waitUntil(std::condition_variable &cv, std::unique_lock<std::mutex> &lock,
                          const std::chrono::time_point<Clock, Duration> &deadline, Pred pred)
    {
        while (!pred())
        {
            const auto now = Clock::now();
            if (now >= deadline)
                return pred();
            waitFor(cv, lock, deadline - now);
        }
        return true;
    }
#else
    template <class Rep, class Period>
    inline void waitFor(std::condition_variable &cv, std::unique_lock<std::mutex> &lock,
                        const std::chrono::duration<Rep, Period> &timeout)
    {
        cv.wait_for(lock, timeout);
    }

    template <class Clock, class Duration, class Pred>
    inline bool waitUntil(std::condition_variable &cv, std::unique_lock<std::mutex> &lock,
                          const std::chrono::time_point<Clock, Duration> &deadline, Pred pred)
    {
        return cv.wait_until(lock, deadline, pred);
    }
#endif
}
