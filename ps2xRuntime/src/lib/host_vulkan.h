// Vulkan output for the PS2 runtime (pc-settings.ini api=1, the launcher's Graphics API): the GS frame as pixels,
// presented through port_gfx (third_party/port_gfx, shared with the other recompilers) with FSR 1.0 or NVIDIA DLSS
// (filter=3) on a child window covering raylib's. raylib keeps the window, input and the PC menu: while the menu is
// open the Vulkan picture steps aside and the OpenGL one, menu on top, shows. OpenGL stays when Vulkan cannot start.
#pragma once

#if defined(_WIN32) && defined(PS2X_PORT_GFX)
#include "port_gfx.h"
#include <cstdint>
#include <cstdlib>
#include <iostream>

// windows.h collides with raylib's names; the few calls needed, declared directly.
extern "C" {
__declspec(dllimport) void *__stdcall CreateWindowExW(unsigned long, const wchar_t *, const wchar_t *, unsigned long,
                                                      int, int, int, int, void *, void *, void *, void *);
__declspec(dllimport) int __stdcall DestroyWindow(void *);
__declspec(dllimport) int __stdcall ShowWindow(void *, int);
__declspec(dllimport) int __stdcall SetWindowPos(void *, void *, int, int, int, int, unsigned int);
__declspec(dllimport) int __stdcall GetClientRect(void *, long *);
__declspec(dllimport) void *__stdcall GetModuleHandleW(const wchar_t *);
}

namespace host_vulkan {
    inline int state = -1;          // -1 not started, 0 OpenGL, 1 Vulkan
    inline void *child = nullptr;
    inline long childSize[2] = {0, 0};
    inline bool shown = false;
    inline int signature = -1;      // the settings port_gfx was opened with

    inline bool active() { return state == 1; }

    inline int settingsSignature() {
        return host_menu::filter | (host_menu::vsync << 2) | ((host_video::stretch ? 1 : 0) << 3) |
               (host_menu::dlssQuality << 4) | ((host_menu::gpuIndex + 1) << 8);
    }

    inline void stop() {
        port_gfx_close();
        if (child) DestroyWindow(child);
        child = nullptr;
        shown = false;
        childSize[0] = childSize[1] = 0;
    }

    inline void start() {
        const char *forced = std::getenv("PS2X_GRAPHICS_API");
        const bool wanted = forced && *forced ? (forced[0] == 'v' || forced[0] == 'V') : host_menu::graphicsApi == 1;
        state = 0;
        if (!wanted) return;
        constexpr unsigned long kChild = 0x40000000ul, kVisible = 0x10000000ul, kDisabled = 0x08000000ul;
        void *parent = GetWindowHandle();
        long rect[4] = {0, 0, 0, 0};
        GetClientRect(parent, rect);
        // Disabled: clicks and keys go on to raylib's window.
        child = CreateWindowExW(0, L"STATIC", L"", kChild | kVisible | kDisabled, 0, 0, rect[2], rect[3], parent,
                                nullptr, GetModuleHandleW(nullptr), nullptr);
        PortGfxConfig config;
        port_gfx_defaults(&config);
        config.vsync = host_menu::vsync;
        config.adapter = host_menu::gpuIndex;
        config.scaler = host_menu::filter == 0 ? PORT_GFX_NEAREST : host_menu::filter == 1 ? PORT_GFX_LINEAR
                      : host_menu::filter == 2 ? PORT_GFX_FSR : PORT_GFX_DLSS;
        config.stretch = host_video::stretch ? 1 : 0;
        config.aspect = host_video::stretch ? 0.0f : 4.0f / 3.0f;   // a TV's shape, as the OpenGL path shows it
        config.dlss_quality = host_menu::dlssQuality;
        port_gfx_config_from_env(&config);
        if (child && port_gfx_open(child, &config)) {
            state = 1;
            shown = true;
            signature = settingsSignature();
            ClearWindowState(FLAG_VSYNC_HINT);   // Vulkan paces the frames; OpenGL waiting too would halve them
            return;
        }
        std::cerr << "[video] Vulkan: " << port_gfx_error() << "; staying on OpenGL" << std::endl;
        stop();
    }

    // Shows the frame through Vulkan. False when OpenGL shows it (Vulkan off, or the PC menu open).
    inline bool present(const void *pixels, uint32_t width, uint32_t height, uint32_t pitch) {
        if (state < 0) start();
        if (state != 1) return false;
        if (signature != settingsSignature()) {   // the PC menu changed a video setting
            stop();
            state = -1;
            start();
            if (state != 1) return false;
        }
        if (host_menu::open) {
            if (shown) { ShowWindow(child, 0); shown = false; }
            return false;
        }
        if (!shown) { ShowWindow(child, 5); shown = true; }
        long rect[4] = {0, 0, 0, 0};
        GetClientRect(GetWindowHandle(), rect);
        if (rect[2] != childSize[0] || rect[3] != childSize[1]) {
            SetWindowPos(child, nullptr, 0, 0, rect[2], rect[3], 0x0004u | 0x0010u);   // no z-order, no activate
            childSize[0] = rect[2];
            childSize[1] = rect[3];
        }
        if (pixels && width && height) port_gfx_present_pixels_rgba(pixels, width, height, pitch);
        return true;
    }
}
#else
namespace host_vulkan {
    inline bool active() { return false; }
    inline bool present(const void *, uint32_t, uint32_t, uint32_t) { return false; }
    inline void stop() {}
}
#endif
