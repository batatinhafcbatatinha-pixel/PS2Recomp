// Operating-system services for the PC settings menu (see host_os.h).
#include "host_os.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#if defined(_MSC_VER)
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "opengl32.lib")
#endif
#else
#include <cstring>
#include <fstream>
#include <iterator>
#include <unistd.h>
#if defined(__APPLE__)
#include <crt_externs.h>
#include <mach-o/dyld.h>
#endif
#endif

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <iterator>
#include <cstdlib>
#include <iostream>

// raylib's desktop build carries GLFW; only the few calls used here are declared.
#if !defined(PLATFORM_VITA) && !defined(__vita__) && !defined(__ANDROID__)
#define HOST_OS_GLFW 1
extern "C" {
struct GLFWmonitor;
struct GLFWwindow;
struct GLFWvidmode { int width, height, redBits, greenBits, blueBits, refreshRate; };
GLFWmonitor **glfwGetMonitors(int *count);
const GLFWvidmode *glfwGetVideoModes(GLFWmonitor *monitor, int *count);
GLFWmonitor *glfwGetWindowMonitor(GLFWwindow *window);
void glfwSetWindowMonitor(GLFWwindow *window, GLFWmonitor *monitor, int x, int y, int width, int height, int refreshRate);
}
#endif

extern "C" const unsigned char *
#if defined(_WIN32)
    __stdcall
#endif
    glGetString(unsigned int name);

namespace host_os {
#if defined(_WIN32)
    namespace {
        const wchar_t *kGpuKey = L"Software\\Microsoft\\DirectX\\UserGpuPreferences";
        std::wstring exePath() {
            wchar_t path[MAX_PATH * 2];
            const DWORD length = GetModuleFileNameW(nullptr, path, static_cast<DWORD>(std::size(path)));
            return std::wstring(path, length);
        }
    }

    bool canRestart() { return true; }

    bool restartGame() {
        std::wstring commandLine = GetCommandLineW();
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process{};
        const std::wstring exe = exePath();
        if (!CreateProcessW(exe.c_str(), commandLine.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process)) {
            std::cerr << "[pc-menu] restart failed (" << GetLastError() << ")" << std::endl;
            return false;
        }
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        std::cerr << "[pc-menu] restarting" << std::endl;
        ExitProcess(0);
    }

    void quitGame() {
        std::cerr << "[pc-menu] quit from the settings menu" << std::endl;
        ExitProcess(0);
    }

    bool hasGpuPreference() { return true; }

    int gpuPreference() {
        wchar_t value[128] = {};
        DWORD size = sizeof(value);
        if (RegGetValueW(HKEY_CURRENT_USER, kGpuKey, exePath().c_str(), RRF_RT_REG_SZ, nullptr, value, &size) != ERROR_SUCCESS)
            return 0;
        const wchar_t *found = wcsstr(value, L"GpuPreference=");
        return found ? std::clamp(_wtoi(found + 14), 0, 2) : 0;
    }

    void setGpuPreference(int value) {
        const std::wstring exe = exePath();
        if (value <= 0) {
            RegDeleteKeyValueW(HKEY_CURRENT_USER, kGpuKey, exe.c_str());
            return;
        }
        const std::wstring data = L"GpuPreference=" + std::to_wstring(std::min(value, 2)) + L";";
        RegSetKeyValueW(HKEY_CURRENT_USER, kGpuKey, exe.c_str(), REG_SZ, data.c_str(),
                        static_cast<DWORD>((data.size() + 1) * sizeof(wchar_t)));
    }
#else
    namespace {
        std::vector<std::string> arguments() {
            std::vector<std::string> list;
#if defined(__APPLE__)
            char **argv = *_NSGetArgv();
            for (int i = 0; i < *_NSGetArgc(); ++i) list.emplace_back(argv[i]);
#else
            std::ifstream file("/proc/self/cmdline", std::ios::binary);
            const std::string all((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
            for (size_t start = 0; start < all.size();) {
                const size_t end = all.find('\0', start);
                list.push_back(all.substr(start, end == std::string::npos ? std::string::npos : end - start));
                if (end == std::string::npos) break;
                start = end + 1;
            }
#endif
            return list;
        }
        std::string exePath() {
#if defined(__APPLE__)
            char path[4096];
            uint32_t size = sizeof(path);
            return _NSGetExecutablePath(path, &size) == 0 ? std::string(path) : std::string();
#else
            return "/proc/self/exe";
#endif
        }
    }

    bool canRestart() { return !arguments().empty() && !exePath().empty(); }

    bool restartGame() {
        std::vector<std::string> list = arguments();
        const std::string exe = exePath();
        if (list.empty() || exe.empty()) return false;
        std::vector<char *> argv;
        for (std::string &argument : list) argv.push_back(argument.data());
        argv.push_back(nullptr);
        std::cerr << "[pc-menu] restarting" << std::endl;
        execv(exe.c_str(), argv.data());
        std::cerr << "[pc-menu] restart failed" << std::endl;
        return false;
    }

    void quitGame() {
        std::cerr << "[pc-menu] quit from the settings menu" << std::endl;
        std::_Exit(0);
    }

    bool hasGpuPreference() { return false; }
    int gpuPreference() { return 0; }
    void setGpuPreference(int) {}
#endif

    std::string gpuName() {
        const unsigned char *name = glGetString(0x1F01 /* GL_RENDERER */);
        return name ? reinterpret_cast<const char *>(name) : "";
    }

#if defined(HOST_OS_GLFW)
    std::vector<int> refreshRates(int monitor, int width, int height) {
        std::vector<int> rates;
        int monitorCount = 0;
        GLFWmonitor **monitors = glfwGetMonitors(&monitorCount);
        if (!monitors || monitor < 0 || monitor >= monitorCount) return rates;
        int modeCount = 0;
        const GLFWvidmode *modes = glfwGetVideoModes(monitors[monitor], &modeCount);
        for (int i = 0; modes && i < modeCount; ++i)
            if (modes[i].width == width && modes[i].height == height && modes[i].refreshRate > 0 &&
                std::find(rates.begin(), rates.end(), modes[i].refreshRate) == rates.end())
                rates.push_back(modes[i].refreshRate);
        std::sort(rates.begin(), rates.end());
        return rates;
    }

    void setFullscreenRefresh(void *window, int width, int height, int hz) {
        GLFWwindow *handle = static_cast<GLFWwindow *>(window);
        GLFWmonitor *monitor = handle ? glfwGetWindowMonitor(handle) : nullptr;
        if (!monitor) return;
        glfwSetWindowMonitor(handle, monitor, 0, 0, width, height, hz > 0 ? hz : -1 /* GLFW_DONT_CARE */);
    }
#else
    std::vector<int> refreshRates(int, int, int) { return {}; }
    void setFullscreenRefresh(void *, int, int, int) {}
#endif
}
