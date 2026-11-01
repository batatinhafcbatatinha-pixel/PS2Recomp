#include "runtime/ps2_pad.h"
#include "ps2_host_backend.h"
#include "host_pc_menu.h"
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <algorithm>
#include <cmath>
#include <mutex>
#include <array>

namespace
{
    constexpr uint8_t kPadAnalogMarker = 0x73;
    constexpr uint8_t kPadStickCenter = 0x80;

    constexpr uint16_t PAD_LEFT = 0x0080u;
    constexpr uint16_t PAD_DOWN = 0x0040u;
    constexpr uint16_t PAD_RIGHT = 0x0020u;
    constexpr uint16_t PAD_UP = 0x0010u;
    constexpr uint16_t PAD_START = 0x0008u;
    constexpr uint16_t PAD_R3 = 0x0004u;
    constexpr uint16_t PAD_L3 = 0x0002u;
    constexpr uint16_t PAD_SELECT = 0x0001u;
    constexpr uint16_t PAD_SQUARE = 0x8000u;
    constexpr uint16_t PAD_CROSS = 0x4000u;
    constexpr uint16_t PAD_CIRCLE = 0x2000u;
    constexpr uint16_t PAD_TRIANGLE = 0x1000u;
    constexpr uint16_t PAD_R1 = 0x0800u;
    constexpr uint16_t PAD_L1 = 0x0400u;
    constexpr uint16_t PAD_R2 = 0x0200u;
    constexpr uint16_t PAD_L2 = 0x0100u;
}

static bool readHostPadState(int port, uint8_t *data, size_t size)
{
    if (!data || size < 32)
        return false;

    std::memset(data, 0, 32);
    data[0] = 0x01;
    data[1] = kPadAnalogMarker;
    data[2] = 0xFF;
    data[3] = 0xFF;
    data[4] = data[5] = data[6] = data[7] = kPadStickCenter;

    uint16_t btns = 0xFFFFu;
    const int kGamepad = port;
    const bool useGamepad = IsGamepadAvailable(kGamepad);
    static int connected[2] = {-1, -1};
    if (port >= 0 && port < 2 && connected[port] != static_cast<int>(useGamepad)) {
        connected[port] = useGamepad;
        std::cerr << "[pad:host] port=" << port << " connected=" << useGamepad;
        if (useGamepad) std::cerr << " name=" << GetGamepadName(kGamepad);
        std::cerr << std::endl;
    }
    auto clearBit = [&btns](uint16_t mask)
    { btns &= ~mask; };

    // The PC settings menu owns the keyboard and pads while it is open (and until its keys are released).
    if (host_menu::open || host_menu::suppressPad)
        return true;
    if (useGamepad)
    {
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_LEFT_FACE_UP))
            clearBit(PAD_UP);
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_LEFT_FACE_DOWN))
            clearBit(PAD_DOWN);
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_LEFT_FACE_LEFT))
            clearBit(PAD_LEFT);
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_LEFT_FACE_RIGHT))
            clearBit(PAD_RIGHT);
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_RIGHT_FACE_DOWN))
            clearBit(PAD_CROSS);
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_RIGHT_FACE_RIGHT))
            clearBit(PAD_CIRCLE);
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_RIGHT_FACE_LEFT))
            clearBit(PAD_SQUARE);
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_RIGHT_FACE_UP))
            clearBit(PAD_TRIANGLE);
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_LEFT_TRIGGER_1))
            clearBit(PAD_L1);
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_RIGHT_TRIGGER_1))
            clearBit(PAD_R1);
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_LEFT_TRIGGER_2))
            clearBit(PAD_L2);
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_RIGHT_TRIGGER_2))
            clearBit(PAD_R2);
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_MIDDLE_RIGHT))
            clearBit(PAD_START);
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_MIDDLE_LEFT))
            clearBit(PAD_SELECT);
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_LEFT_THUMB))
            clearBit(PAD_L3);
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_RIGHT_THUMB))
            clearBit(PAD_R3);

        float lx = GetGamepadAxisMovement(kGamepad, GAMEPAD_AXIS_LEFT_X);
        float ly = GetGamepadAxisMovement(kGamepad, GAMEPAD_AXIS_LEFT_Y);
        float rx = GetGamepadAxisMovement(kGamepad, GAMEPAD_AXIS_RIGHT_X);
        float ry = GetGamepadAxisMovement(kGamepad, GAMEPAD_AXIS_RIGHT_Y);
        auto analog = [](float value) {
            constexpr float deadzone = 0.18f;
            value = std::clamp(value, -1.0f, 1.0f);
            if (std::abs(value) <= deadzone) return kPadStickCenter;
            value = std::copysign((std::abs(value) - deadzone) / (1.0f - deadzone), value);
            return static_cast<uint8_t>(std::clamp(128 + static_cast<int>(std::lround(value * 127)), 0, 255));
        };
        data[6] = analog(lx); data[7] = analog(ly);
        data[4] = analog(rx); data[5] = analog(ry);
    }
    if (port == 0)
    {
        if (IsKeyDown(KEY_UP) || IsKeyDown(KEY_W))
            clearBit(PAD_UP);
        if (IsKeyDown(KEY_DOWN) || IsKeyDown(KEY_S))
            clearBit(PAD_DOWN);
        if (IsKeyDown(KEY_LEFT) || IsKeyDown(KEY_A))
            clearBit(PAD_LEFT);
        if (IsKeyDown(KEY_RIGHT) || IsKeyDown(KEY_D))
            clearBit(PAD_RIGHT);
        if (IsKeyDown(KEY_X) || IsKeyDown(KEY_SPACE))
            clearBit(PAD_CROSS);
        if (IsKeyDown(KEY_C) || IsKeyDown(KEY_ESCAPE))
            clearBit(PAD_CIRCLE);
        if (IsKeyDown(KEY_Z) || IsKeyDown(KEY_KP_0))
            clearBit(PAD_SQUARE);
        if (IsKeyDown(KEY_V) || IsKeyDown(KEY_KP_1))
            clearBit(PAD_TRIANGLE);
        if (IsKeyDown(KEY_Q))
            clearBit(PAD_L1);
        if (IsKeyDown(KEY_E))
            clearBit(PAD_R1);
        if (IsKeyDown(KEY_LEFT_SHIFT))
            clearBit(PAD_L2);
        if (IsKeyDown(KEY_RIGHT_SHIFT))
            clearBit(PAD_R2);
        if (IsKeyDown(KEY_ENTER) && !IsKeyDown(KEY_LEFT_ALT) && !IsKeyDown(KEY_RIGHT_ALT))
            clearBit(PAD_START);
        if (IsKeyDown(KEY_TAB))
            clearBit(PAD_SELECT);
        if (IsKeyDown(KEY_LEFT_CONTROL)) clearBit(PAD_L3);
        if (IsKeyDown(KEY_R)) clearBit(PAD_R3);
        if (IsKeyDown(KEY_A) || IsKeyDown(KEY_D)) data[6] = IsKeyDown(KEY_A) == IsKeyDown(KEY_D) ? 128 : IsKeyDown(KEY_A) ? 1 : 255;
        if (IsKeyDown(KEY_W) || IsKeyDown(KEY_S)) data[7] = IsKeyDown(KEY_W) == IsKeyDown(KEY_S) ? 128 : IsKeyDown(KEY_W) ? 1 : 255;
        if (IsKeyDown(KEY_J) || IsKeyDown(KEY_L)) data[4] = IsKeyDown(KEY_J) == IsKeyDown(KEY_L) ? 128 : IsKeyDown(KEY_J) ? 1 : 255;
        if (IsKeyDown(KEY_I) || IsKeyDown(KEY_K)) data[5] = IsKeyDown(KEY_I) == IsKeyDown(KEY_K) ? 128 : IsKeyDown(KEY_I) ? 1 : 255;
    }

    // Optional deterministic controller input for port validation. The file is
    // a hexadecimal pressed-button mask added to the keyboard/gamepad state, so a
    // scripted session stays playable; absent configuration changes nothing. Up to
    // four hexadecimal stick values may follow the mask.
    const char *padFile = std::getenv("PS2X_PAD_FILE");
    if (port == 0 && padFile && *padFile) {
        std::ifstream input(padFile);
        uint32_t pressed = 0;
        if (input >> std::hex >> pressed) {
            btns &= static_cast<uint16_t>(~pressed);
            uint32_t stick = 0; // optional: right X, right Y, left X, left Y
            for (int axis = 4; axis < 8 && input >> std::hex >> stick; ++axis)
                data[axis] = static_cast<uint8_t>(stick);
            static uint32_t lastPressed = 0xffffffffu;
            static uint32_t logs = 0;
            if (pressed != lastPressed && logs++ < 64u)
                std::cerr << "[pad:test] pressed=0x" << std::hex << pressed << std::dec << std::endl;
            lastPressed = pressed;
        }
    }
    data[2] = static_cast<uint8_t>(btns & 0xFF);
    data[3] = static_cast<uint8_t>(btns >> 8);
    return true;
}

namespace {
    std::mutex hostPadMutex;
    std::array<std::array<uint8_t, 32>, 2> hostPads{};
    bool hostPadsReady = false;
}
void ps2UpdateHostPadInput() {
    std::array<std::array<uint8_t, 32>, 2> next{};
    for (int port = 0; port < 2; ++port) readHostPadState(port, next[port].data(), 32);
    std::lock_guard<std::mutex> lock(hostPadMutex);
    hostPads = next;
    hostPadsReady = true;
}
bool PSPadBackend::readState(int port, int slot, uint8_t *data, size_t size) {
    if (!data || size < 32 || port < 0 || port >= 2 || slot != 0) return false;
    std::lock_guard<std::mutex> lock(hostPadMutex);
    if (!hostPadsReady) {
        std::memset(data, 0, 32);
        data[0] = 1; data[1] = 0x73; data[2] = data[3] = 0xff;
        data[4] = data[5] = data[6] = data[7] = 0x80;
    } else std::memcpy(data, hostPads[port].data(), 32);
    return true;
}
