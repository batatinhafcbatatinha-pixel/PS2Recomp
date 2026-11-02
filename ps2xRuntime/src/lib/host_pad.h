#pragma once
// Host pads of the PS2 ports, for the game and for the PC settings menu.
//
// On Windows, SDL3.dll next to the executable (loaded at run time, no header needed) reads every pad SDL knows:
// XInput pads, DualShock 3/4, DualSense, Switch pads, USB adapters for PS2 and original Xbox pads. Without the DLL,
// and on Linux and macOS, raylib/GLFW reads the pads. Either way gamecontrollerdb.txt next to the executable gives
// generic adapters their button names.
//
// Buttons and axes keep raylib's numbers, so callers do not depend on the backend. All of this runs on the window
// thread, once per host frame; only setRumble is called from the game's thread. Vibration needs SDL: raylib has none
// on desktop.
#include "ps2_host_backend.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <type_traits>

#if defined(_WIN32)
extern "C" {
__declspec(dllimport) void *__stdcall LoadLibraryExA(const char *, void *, unsigned long);
__declspec(dllimport) void *__stdcall GetProcAddress(void *, const char *);
}
#endif

namespace host_pad {
    constexpr int kMaxPads = 4;
    constexpr float kTriggerPressed = 0.25f;

#if defined(_WIN32)
    // SDL's own numbers, so that no SDL header is needed to build.
    enum { kSdlInitGamepad = 0x2000 };
    enum { kSdlSouth, kSdlEast, kSdlWest, kSdlNorth, kSdlBack, kSdlGuide, kSdlStart, kSdlLeftStick, kSdlRightStick,
           kSdlLeftShoulder, kSdlRightShoulder, kSdlDpadUp, kSdlDpadDown, kSdlDpadLeft, kSdlDpadRight };

    struct SdlPad;
    struct SdlApi {
        unsigned char (*Init)(uint32_t);
        unsigned char (*SetHint)(const char *, const char *);
        int (*AddMappings)(const char *);
        uint32_t *(*GetGamepads)(int *);
        SdlPad *(*Open)(uint32_t);
        void (*Close)(SdlPad *);
        unsigned char (*Connected)(SdlPad *);
        uint32_t (*GetId)(SdlPad *);
        const char *(*GetName)(SdlPad *);
        void (*Update)(void);
        void (*FlushEvents)(uint32_t, uint32_t);
        unsigned char (*Rumble)(SdlPad *, uint16_t, uint16_t, uint32_t);
        unsigned char (*Button)(SdlPad *, int);
        int16_t (*Axis)(SdlPad *, int);
        void (*Free)(void *);
    };
    inline SdlApi sdl{};
    inline bool sdlRunning = false;
    inline std::array<SdlPad *, kMaxPads> sdlPads{};
    inline double sdlScanAt = 0.0;

    inline bool startSdl(const std::string &directory) {
        void *const library = LoadLibraryExA((directory + "SDL3.dll").c_str(), nullptr, 0x8u /* altered search path */);
        if (!library) return false;
        bool complete = true;
        const auto bind = [&](auto &field, const char *name) {
            void *const address = GetProcAddress(library, name);
            if (!address) complete = false;
            field = reinterpret_cast<std::remove_reference_t<decltype(field)>>(address);
        };
        bind(sdl.Init, "SDL_Init");
        bind(sdl.SetHint, "SDL_SetHint");
        bind(sdl.AddMappings, "SDL_AddGamepadMappingsFromFile");
        bind(sdl.GetGamepads, "SDL_GetGamepads");
        bind(sdl.Open, "SDL_OpenGamepad");
        bind(sdl.Close, "SDL_CloseGamepad");
        bind(sdl.Connected, "SDL_GamepadConnected");
        bind(sdl.GetId, "SDL_GetGamepadID");
        bind(sdl.GetName, "SDL_GetGamepadName");
        bind(sdl.Update, "SDL_UpdateGamepads");
        bind(sdl.FlushEvents, "SDL_FlushEvents");
        bind(sdl.Rumble, "SDL_RumbleGamepad");
        bind(sdl.Button, "SDL_GetGamepadButton");
        bind(sdl.Axis, "SDL_GetGamepadAxis");
        bind(sdl.Free, "SDL_free");
        if (!complete) return false;
        // The window belongs to raylib: pads must work without an SDL window, on SDL's own thread.
        sdl.SetHint("SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS", "1");
        sdl.SetHint("SDL_JOYSTICK_THREAD", "1");
        if (!sdl.Init(kSdlInitGamepad)) return false;
        sdl.AddMappings((directory + "gamecontrollerdb.txt").c_str());
        return true;
    }

    // Open new pads and drop the ones that left, twice a second. A pad keeps its slot while connected.
    inline void scanSdl() {
        for (SdlPad *&pad : sdlPads)
            if (pad && !sdl.Connected(pad)) {
                sdl.Close(pad);
                pad = nullptr;
            }
        int count = 0;
        uint32_t *const ids = sdl.GetGamepads(&count);
        for (int i = 0; ids && i < count; ++i) {
            bool known = false;
            for (SdlPad *pad : sdlPads) known = known || (pad && sdl.GetId(pad) == ids[i]);
            if (known) continue;
            for (SdlPad *&pad : sdlPads)
                if (!pad) {
                    pad = sdl.Open(ids[i]);
                    break;
                }
        }
        if (ids) sdl.Free(ids);
    }
#endif

    struct State {
        bool connected = false;
        uint32_t buttons = 0, previous = 0;  // bit n = raylib GamepadButton n
        std::array<float, 6> axes{};         // raylib GamepadAxis order; triggers -1 to 1
    };
    inline std::array<State, kMaxPads> pads{};

    inline bool rumbleEnabled = true;                            // PC menu option, off while the menu is open
    inline std::array<std::atomic<uint32_t>, kMaxPads> rumbleWanted{};  // low-frequency motor << 16 | high-frequency

    inline void setRumble(int pad, uint16_t low, uint16_t high) {
        if (pad >= 0 && pad < kMaxPads) rumbleWanted[pad].store(static_cast<uint32_t>(low) << 16 | high);
    }

#if defined(_WIN32)
    inline void readSdl(int index, State &state) {
        static constexpr struct { int sdl; int raylib; } kButtons[] = {
            {kSdlDpadUp, GAMEPAD_BUTTON_LEFT_FACE_UP}, {kSdlDpadRight, GAMEPAD_BUTTON_LEFT_FACE_RIGHT},
            {kSdlDpadDown, GAMEPAD_BUTTON_LEFT_FACE_DOWN}, {kSdlDpadLeft, GAMEPAD_BUTTON_LEFT_FACE_LEFT},
            {kSdlNorth, GAMEPAD_BUTTON_RIGHT_FACE_UP}, {kSdlEast, GAMEPAD_BUTTON_RIGHT_FACE_RIGHT},
            {kSdlSouth, GAMEPAD_BUTTON_RIGHT_FACE_DOWN}, {kSdlWest, GAMEPAD_BUTTON_RIGHT_FACE_LEFT},
            {kSdlLeftShoulder, GAMEPAD_BUTTON_LEFT_TRIGGER_1}, {kSdlRightShoulder, GAMEPAD_BUTTON_RIGHT_TRIGGER_1},
            {kSdlBack, GAMEPAD_BUTTON_MIDDLE_LEFT}, {kSdlGuide, GAMEPAD_BUTTON_MIDDLE}, {kSdlStart, GAMEPAD_BUTTON_MIDDLE_RIGHT},
            {kSdlLeftStick, GAMEPAD_BUTTON_LEFT_THUMB}, {kSdlRightStick, GAMEPAD_BUTTON_RIGHT_THUMB},
        };
        SdlPad *const pad = sdlPads[index];
        state.connected = pad != nullptr;
        state.buttons = 0;
        state.axes = {};
        if (!pad) return;
        for (const auto &button : kButtons)
            if (sdl.Button(pad, button.sdl)) state.buttons |= 1u << button.raylib;
        for (int axis = 0; axis < 4; ++axis)
            state.axes[axis] = std::clamp(sdl.Axis(pad, axis) / 32767.0f, -1.0f, 1.0f);
        // SDL triggers go from 0 to 32767.
        state.axes[GAMEPAD_AXIS_LEFT_TRIGGER] = sdl.Axis(pad, 4) / 32767.0f * 2.0f - 1.0f;
        state.axes[GAMEPAD_AXIS_RIGHT_TRIGGER] = sdl.Axis(pad, 5) / 32767.0f * 2.0f - 1.0f;
        if ((state.axes[GAMEPAD_AXIS_LEFT_TRIGGER] + 1.0f) * 0.5f > kTriggerPressed)
            state.buttons |= 1u << GAMEPAD_BUTTON_LEFT_TRIGGER_2;
        if ((state.axes[GAMEPAD_AXIS_RIGHT_TRIGGER] + 1.0f) * 0.5f > kTriggerPressed)
            state.buttons |= 1u << GAMEPAD_BUTTON_RIGHT_TRIGGER_2;
    }
#endif

#if defined(_WIN32)
    // A PS2 motor runs until the game stops it, an SDL rumble for a set time: renew it while it should run, so a
    // crash or a lost pad cannot leave a motor spinning.
    inline void applyRumble(double now) {
        static std::array<uint32_t, kMaxPads> sent{};
        static std::array<double, kMaxPads> renewAt{};
        for (int index = 0; index < kMaxPads; ++index) {
            SdlPad *const pad = sdlPads[index];
            const uint32_t wanted = rumbleEnabled ? rumbleWanted[index].load() : 0u;
            if (!pad) {
                sent[index] = 0u;
                continue;
            }
            if (wanted == sent[index] && (wanted == 0u || now < renewAt[index])) continue;
            sdl.Rumble(pad, static_cast<uint16_t>(wanted >> 16), static_cast<uint16_t>(wanted), 1000u);
            sent[index] = wanted;
            renewAt[index] = now + 0.5;
        }
    }
#endif

    inline void readRaylib(int index, State &state) {
        state.connected = IsGamepadAvailable(index);
        state.buttons = 0;
        state.axes = {};
        if (!state.connected) return;
        for (int button = GAMEPAD_BUTTON_LEFT_FACE_UP; button <= GAMEPAD_BUTTON_RIGHT_THUMB; ++button)
            if (IsGamepadButtonDown(index, button)) state.buttons |= 1u << button;
        for (int axis = 0; axis < 6; ++axis) state.axes[axis] = GetGamepadAxisMovement(index, axis);
    }

    // Called once the window exists.
    inline void initialize() {
        const std::string directory = GetApplicationDirectory();
#if defined(_WIN32)
        sdlRunning = startSdl(directory);
        std::cerr << "[pad:host] backend=" << (sdlRunning ? "SDL3" : "raylib") << std::endl;
        if (sdlRunning) return;
#endif
        const std::string database = directory + "gamecontrollerdb.txt";
        if (char *const text = FileExists(database.c_str()) ? LoadFileText(database.c_str()) : nullptr) {
            SetGamepadMappings(text);
            UnloadFileText(text);
        }
    }

    // Once per host frame, before anything reads the pads.
    inline void update() {
#if defined(_WIN32)
        if (sdlRunning) {
            sdl.Update();
            sdl.FlushEvents(0u, 0xFFFFFFFFu);  // nothing here reads SDL's event queue
            const double now = GetTime();
            if (now >= sdlScanAt) {
                sdlScanAt = now + 0.5;
                scanSdl();
            }
            applyRumble(now);
        }
#endif
        for (int index = 0; index < kMaxPads; ++index) {
            State &state = pads[index];
            state.previous = state.buttons;
#if defined(_WIN32)
            if (sdlRunning) {
                readSdl(index, state);
                continue;
            }
#endif
            readRaylib(index, state);
        }
    }

    inline bool available(int pad) { return pad >= 0 && pad < kMaxPads && pads[pad].connected; }
    inline bool down(int pad, int button) { return available(pad) && (pads[pad].buttons >> button & 1u); }
    inline bool pressed(int pad, int button) {
        return down(pad, button) && !(pads[pad].previous >> button & 1u);
    }
    inline float axis(int pad, int which) { return available(pad) ? pads[pad].axes[which] : 0.0f; }
    inline std::string name(int pad) {
        if (!available(pad)) return {};
#if defined(_WIN32)
        if (sdlRunning) {
            const char *const text = sdl.GetName(sdlPads[pad]);
            return text ? text : "";
        }
#endif
        const char *const text = GetGamepadName(pad);
        return text ? text : "";
    }
}
