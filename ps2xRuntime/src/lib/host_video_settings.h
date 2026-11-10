#pragma once
#include "ps2_host_backend.h"
#include "host_os.h"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <fstream>
#include <string>
#include <iostream>
extern std::atomic<uint64_t> g_perfVu1Ns, g_perfGsNs, g_perfIopNs, g_perfIdleNs, g_perfSyncNs;
namespace host_video {
    enum class Mode { Windowed, Fullscreen, Borderless };
    inline Mode mode = Mode::Windowed;
    inline bool stretch = false;
    inline double fps = 0.0;          // game frames per second over the last second
    inline std::string windowTitle;   // game name for the title bar
    inline const char *name(Mode value) {
        return value == Mode::Fullscreen ? "fullscreen" : value == Mode::Borderless ? "borderless" : "windowed";
    }
    // Windowed size: 1920x1080, reduced to a 16:9 window that fits 90% of smaller monitors.
    inline int resolutionWidth = 0, resolutionHeight = 0;  // 0 = automatic; set from the F1 menu
    inline int refreshRate = 0;                            // exclusive fullscreen; 0 = the highest the monitor offers
    inline void windowSize(int &width, int &height) {
        int monitor = GetCurrentMonitor();
        if (resolutionWidth > 0) {
            // larger than the monitor: the window keeps the shape, the picture still renders at the chosen size
            const float fit = std::min({1.0f, GetMonitorWidth(monitor) / static_cast<float>(resolutionWidth),
                                        GetMonitorHeight(monitor) / static_cast<float>(resolutionHeight)});
            width = static_cast<int>(resolutionWidth * fit);
            height = static_cast<int>(resolutionHeight * fit);
            return;
        }
        width = std::min({1920, GetMonitorWidth(monitor) * 9 / 10, GetMonitorHeight(monitor) * 9 / 10 * 16 / 9});
        if (width <= 0) width = 1920;
        height = width * 9 / 16;
    }
    inline void apply(Mode next, bool save = true) {
        if (IsWindowFullscreen()) ToggleFullscreen();
        if (IsWindowState(FLAG_BORDERLESS_WINDOWED_MODE)) ToggleBorderlessWindowed();
        if (resolutionWidth > 0) { int width, height; windowSize(width, height); SetWindowSize(width, height); }
        else SetWindowSize(1920, 1080);
        if (next == Mode::Fullscreen) {
            ToggleFullscreen();
            if (refreshRate > 0) host_os::setFullscreenRefresh(GetWindowHandle(), GetScreenWidth(), GetScreenHeight(), refreshRate);
        }
        else if (next == Mode::Borderless) ToggleBorderlessWindowed();
        else {
            int monitor = GetCurrentMonitor(), width, height;
            windowSize(width, height);
            Vector2 origin = GetMonitorPosition(monitor);
            SetWindowPosition(static_cast<int>(origin.x) + std::max(0, (GetMonitorWidth(monitor)-width)/2),
                              static_cast<int>(origin.y) + std::max(0, (GetMonitorHeight(monitor)-height)/2));
            SetWindowSize(width, height);
        }
        mode = next;
        if (save) { std::ofstream file("video-settings.cfg"); file << name(mode) << '\n'; }
        std::cerr << "[video] mode=" << name(mode) << " client=" << GetScreenWidth() << "x" << GetScreenHeight() << std::endl;
    }
    inline void initialize() {
        SetExitKey(KEY_NULL);
        std::string setting;
        std::ifstream file("video-settings.cfg"); file >> setting;
        if (const char *overrideMode = std::getenv("PS2X_WINDOW_MODE")) setting = overrideMode;
        // Test runs: start on the first monitor with this refresh rate.
        if (const char *monitorHz = std::getenv("PS2X_MONITOR_HZ")) {
            for (int monitor = 0; monitor < GetMonitorCount(); ++monitor) {
                if (GetMonitorRefreshRate(monitor) != std::atoi(monitorHz)) continue;
                const Vector2 origin = GetMonitorPosition(monitor);
                SetWindowPosition(static_cast<int>(origin.x) + 40, static_cast<int>(origin.y) + 40);
                break;
            }
        }
        apply(setting == "fullscreen" ? Mode::Fullscreen : setting == "borderless" ? Mode::Borderless : Mode::Windowed, false);
        std::cerr << "[video] F9: window | F10: borderless | F11/Alt+Enter: fullscreen | F8: help" << std::endl;
    }
    inline void selfTest() {
        if (!std::getenv("PS2X_VIDEO_SELF_TEST")) return;
        Mode initial = mode;
        int width, height;
        windowSize(width, height);
        for (Mode requested : {Mode::Windowed, Mode::Borderless, Mode::Fullscreen, Mode::Windowed}) {
            apply(requested, false);
            bool passed = requested == Mode::Fullscreen ? IsWindowFullscreen() :
                requested == Mode::Borderless ? IsWindowState(FLAG_BORDERLESS_WINDOWED_MODE) :
                !IsWindowFullscreen() && !IsWindowState(FLAG_BORDERLESS_WINDOWED_MODE) && GetScreenWidth() == width && GetScreenHeight() == height;
            std::cerr << "[video:test] mode=" << name(requested) << " result=" << (passed ? "PASS" : "FAIL") << std::endl;
            if (!passed) throw std::runtime_error("Video mode self-test failed");
        }
        apply(initial, false);
    }
    // Game frames per second, counted as changes of the displayed framebuffer.
    inline void countFrame(uint32_t displayBuffer, bool displayWritten = false) {
        static uint32_t lastBuffer = 0, frames = 0, presents = 0;
        static double windowStart = GetTime();
        static double lastFlip = GetTime(), worstGap = 0.0; static uint32_t hitches = 0; // longest wait between flips in this window
        if (displayBuffer != lastBuffer || displayWritten) {
            lastBuffer = displayBuffer; ++frames;
            const double t = GetTime(), gap = t - lastFlip;
            lastFlip = t;
            worstGap = std::max(worstGap, gap);
            if (gap > 0.025) ++hitches;
        }
        ++presents;
        const double now = GetTime();
        if (now - windowStart >= 1.0) {
            fps = frames / (now - windowStart);
            char title[160];
            std::snprintf(title, sizeof(title), "%s | %.1f fps", windowTitle.c_str(), fps);
            SetWindowTitle(title);
            static uint32_t logs = 0;
            const double span = (now - windowStart) * 1e7; // percent of one core
            const double vu1 = g_perfVu1Ns.exchange(0) / span, gsp = g_perfGsNs.exchange(0) / span, iop = g_perfIopNs.exchange(0) / span,
                         idle = g_perfIdleNs.exchange(0) / span, sync = g_perfSyncNs.exchange(0) / span;
            static const uint32_t logEvery = std::getenv("PS2X_PERF_EVERY") ? static_cast<uint32_t>(std::atoi(std::getenv("PS2X_PERF_EVERY"))) : 5u;
            if (logs++ % logEvery == 0u)
                std::cerr << "[perf] game_fps=" << fps << " present_fps=" << presents / (now - windowStart)
                          << " vu1=" << vu1 << "% gs=" << gsp << "% iop=" << iop << "% ee_idle=" << idle << "% ee_sync=" << sync << "% worst_ms=" << static_cast<int>(worstGap * 1000.0) << " hitches=" << hitches << std::endl;
            frames = presents = hitches = 0;
            worstGap = 0.0;
            windowStart = now;
        }
    }
    inline void update() {
        if (IsKeyPressed(KEY_F7)) {
            stretch = !stretch;
            std::cerr << "[video] aspect=" << (stretch ? "stretch" : "4:3") << std::endl;
        }
        if (IsKeyPressed(KEY_F9)) apply(Mode::Windowed);
        else if (IsKeyPressed(KEY_F10)) apply(mode == Mode::Borderless ? Mode::Windowed : Mode::Borderless);
        else if (IsKeyPressed(KEY_F11) || ((IsKeyDown(KEY_LEFT_ALT) || IsKeyDown(KEY_RIGHT_ALT)) && IsKeyPressed(KEY_ENTER)))
            apply(mode == Mode::Fullscreen ? Mode::Windowed : Mode::Fullscreen);
    }
    inline void drawHelp() {
        static bool show = false;
        if (IsKeyPressed(KEY_F8)) show = !show;
        if (!show) return;
        DrawRectangle(16, 16, 850, 94, Fade(BLACK, 0.85f));
        DrawText("F1: PC Settings   F7: 4:3/Stretch   F9: Window   F10: Borderless   F11: Fullscreen", 28, 28, 18, WHITE);
        DrawText("Gamepad: A/B/X/Y = Cross/Circle/Square/Triangle; Start = Menu", 28, 55, 18, WHITE);
        DrawText("Keyboard: Enter Start, X Select, C Back, arrows Move. F8: hide help", 28, 82, 18, WHITE);
    }
}
