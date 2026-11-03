#pragma once
// Operating-system services for the PC settings menu. They live in host_os.cpp so that windows.h never
// meets raylib's names in the same file.
#include <string>
#include <vector>

namespace host_os {
    // Starts the game again with the same command line and working directory. On success the
    // calling process ends inside this function; false when a new process could not be started.
    bool restartGame();
    [[noreturn]] void quitGame();
    bool canRestart();

    // Windows graphics preference for this executable (Settings > System > Display > Graphics):
    // 0 Windows default, 1 power saving, 2 high performance. It applies from the next start.
    bool hasGpuPreference();
    int gpuPreference();
    void setGpuPreference(int value);
    // Name of the graphics card drawing the window (the current OpenGL context's renderer).
    std::string gpuName();

    // Refresh rates monitor `monitor` (raylib's index) offers at width x height, and switching an exclusive
    // fullscreen window (raylib's GetWindowHandle) to one of them (0 = the highest, GLFW's choice).
    std::vector<int> refreshRates(int monitor, int width, int height);
    void setFullscreenRefresh(void *window, int width, int height, int hz);
}
