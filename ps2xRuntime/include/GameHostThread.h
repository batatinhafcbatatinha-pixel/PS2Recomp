#pragma once

// The thread that runs generated game code needs a large stack: unoptimized translated functions use big native
// frames and guest call chains run deep. Windows gets it from the linker and Linux from the pthread default set in
// PS2Runtime::run. macOS has neither (std::thread is fixed at 512 KiB), so the thread is created directly there.

#include <thread>

#if defined(__APPLE__)
#include <cstddef>
#include <cstdlib>
#include <functional>
#include <pthread.h>
#include <stdexcept>
#include <utility>

class GameHostThread
{
public:
    explicit GameHostThread(std::function<void()> body) : m_body(std::move(body))
    {
        size_t stackMb = 64u; // PS2X_THREAD_STACK_MB overrides it, as on Linux.
        if (const char *env = std::getenv("PS2X_THREAD_STACK_MB"))
        {
            const size_t requested = static_cast<size_t>(std::strtoul(env, nullptr, 10));
            stackMb = requested < 8u ? 8u : requested;
        }

        pthread_attr_t attr;
        if (pthread_attr_init(&attr) != 0)
            throw std::runtime_error("pthread_attr_init failed");
        pthread_attr_setstacksize(&attr, stackMb * 1024u * 1024u);
        const int result = pthread_create(&m_thread, &attr, &GameHostThread::entry, this);
        pthread_attr_destroy(&attr);
        if (result != 0)
            throw std::runtime_error("could not start the game thread");
        m_joinable = true;
    }

    GameHostThread(const GameHostThread &) = delete;
    GameHostThread &operator=(const GameHostThread &) = delete;

    bool joinable() const { return m_joinable; }

    void join()
    {
        pthread_join(m_thread, nullptr);
        m_joinable = false;
    }

private:
    static void *entry(void *self)
    {
        static_cast<GameHostThread *>(self)->m_body();
        return nullptr;
    }

    std::function<void()> m_body;
    pthread_t m_thread{};
    bool m_joinable = false;
};
#else
using GameHostThread = std::thread;
#endif
