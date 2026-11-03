#pragma once
// PS2X_TIMELINE=<file>: records what every thread does during a short window, so one frame can be followed across the
// EE, VIF, GS frontend, GS and host threads. PS2X_TL_AT=<seconds after boot> (default 120), PS2X_TL_SECS (default 6).
// Off, each mark costs one relaxed load.
#include <atomic>
#include <cstdint>

enum PS2TimelineEvent : uint32_t
{
    TL_SYNC_BEGIN = 1, TL_SYNC_END, TL_VIF_BEGIN, TL_VIF_END, TL_DISP_DEFER, TL_DISP_APPLY, TL_GUEST_IN, TL_GUEST_OUT,
    TL_VBLANK, TL_GAME_FRAME, TL_GSFE_BUSY, TL_GSFE_IDLE, TL_GS_BUSY, TL_GS_IDLE, TL_PRESENT_BEGIN, TL_PRESENT_END,
    TL_SWAP_BEGIN, TL_SWAP_END, TL_KICK, TL_BACKLOG_BEGIN, TL_BACKLOG_END, TL_EE_IDLE_BEGIN, TL_EE_IDLE_END,
    TL_QUEUE_FULL_BEGIN, TL_QUEUE_FULL_END, TL_FEQ_BEGIN, TL_FEQ_END
};

extern std::atomic<bool> g_timelineOn;
void ps2TimelineRecord(uint32_t event, uint32_t a, uint32_t b);
void ps2TimelinePoll(); // host thread: opens the window, writes the file when it closes

inline void PS2TL(uint32_t event, uint32_t a = 0u, uint32_t b = 0u)
{
    if (g_timelineOn.load(std::memory_order_relaxed))
        ps2TimelineRecord(event, a, b);
}
