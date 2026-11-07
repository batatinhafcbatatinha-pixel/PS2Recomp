#include "runtime/ps2_pad.h"
#include "ps2_host_backend.h"
#include "host_pc_menu.h"
#include "host_pad.h"
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <algorithm>
#include <cmath>
#include <mutex>
#include <array>
#include <vector>

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

    constexpr uint16_t kNoButton = 0u;

    struct KeyBind { int key; uint16_t button; };

    // Keyboard and mouse of one game. W A S D is always the left stick and I J K L the right stick.
    struct InputProfile {
        const char *gameId;  // ELF name; empty = any other game
        std::vector<KeyBind> keys;
        uint16_t mouseLeft, mouseRight, mouseMiddle, wheel;
        bool wasdIsDpad;     // games that use the D-pad to move
    };

    // Call of Duty is a PC shooter layout: R1 fire, L1 aim, R2 frag, L2 second grenade, X jump, Circle stance,
    // Square reload/use, Triangle switch weapon, L3 sprint, R3 melee (the slots of the game's own binding table,
    // 0x484120 in SLUS_217.46). Japanese releases: Circle confirms, Cross cancels.
    const std::vector<InputProfile> &inputProfiles()
    {
        static const std::vector<InputProfile> list = {
            {"SLUS_217.46",
             {{KEY_UP, PAD_UP}, {KEY_DOWN, PAD_DOWN}, {KEY_LEFT, PAD_LEFT}, {KEY_RIGHT, PAD_RIGHT},
              {KEY_SPACE, PAD_CROSS}, {KEY_C, PAD_CIRCLE}, {KEY_LEFT_CONTROL, PAD_CIRCLE}, {KEY_BACKSPACE, PAD_CIRCLE},
              {KEY_R, PAD_SQUARE}, {KEY_E, PAD_SQUARE}, {KEY_ONE, PAD_TRIANGLE}, {KEY_TWO, PAD_TRIANGLE},
              {KEY_G, PAD_R2}, {KEY_Q, PAD_L2}, {KEY_LEFT_SHIFT, PAD_L3}, {KEY_F, PAD_R3}, {KEY_V, PAD_R3},
              {KEY_ENTER, PAD_START}, {KEY_ESCAPE, PAD_START}, {KEY_TAB, PAD_SELECT}},
             PAD_R1, PAD_L1, PAD_R3, PAD_TRIANGLE, false},
            {"(japanese)",  // chosen by serial prefix in inputProfile()
             {{KEY_UP, PAD_UP}, {KEY_DOWN, PAD_DOWN}, {KEY_LEFT, PAD_LEFT}, {KEY_RIGHT, PAD_RIGHT},
              {KEY_SPACE, PAD_CIRCLE}, {KEY_C, PAD_CIRCLE}, {KEY_X, PAD_CROSS}, {KEY_ESCAPE, PAD_CROSS},
              {KEY_BACKSPACE, PAD_CROSS}, {KEY_Z, PAD_SQUARE}, {KEY_V, PAD_TRIANGLE}, {KEY_Q, PAD_L1}, {KEY_E, PAD_R1},
              {KEY_LEFT_SHIFT, PAD_L2}, {KEY_RIGHT_SHIFT, PAD_R2}, {KEY_LEFT_CONTROL, PAD_L3}, {KEY_R, PAD_R3},
              {KEY_ENTER, PAD_START}, {KEY_TAB, PAD_SELECT}},
             PAD_CIRCLE, PAD_CROSS, PAD_SQUARE, kNoButton, true},
            {"",
             {{KEY_UP, PAD_UP}, {KEY_DOWN, PAD_DOWN}, {KEY_LEFT, PAD_LEFT}, {KEY_RIGHT, PAD_RIGHT},
              {KEY_X, PAD_CROSS}, {KEY_SPACE, PAD_CROSS}, {KEY_C, PAD_CIRCLE}, {KEY_ESCAPE, PAD_CIRCLE},
              {KEY_BACKSPACE, PAD_CIRCLE}, {KEY_Z, PAD_SQUARE}, {KEY_KP_0, PAD_SQUARE}, {KEY_V, PAD_TRIANGLE},
              {KEY_KP_1, PAD_TRIANGLE}, {KEY_Q, PAD_L1}, {KEY_E, PAD_R1}, {KEY_LEFT_SHIFT, PAD_L2},
              {KEY_RIGHT_SHIFT, PAD_R2}, {KEY_LEFT_CONTROL, PAD_L3}, {KEY_R, PAD_R3},
              {KEY_ENTER, PAD_START}, {KEY_TAB, PAD_SELECT}},
             PAD_SQUARE, PAD_TRIANGLE, PAD_CIRCLE, kNoButton, true},
        };
        return list;
    }

    const InputProfile &inputProfile()
    {
        static const InputProfile *profile = nullptr;
        if (!profile)
        {
            profile = &inputProfiles().back();
            const char *const id = host_menu::theme ? host_menu::theme->gameId : "";
            // Japanese releases (SCPS, SLPS, SLPM, SCAJ, SLAJ, SCPM, PAPX serials): Circle confirms, Cross cancels.
            static const char *const japanese[] = {"SCPS", "SLPS", "SLPM", "SCAJ", "SLAJ", "SCPM", "PAPX"};
            for (const char *prefix : japanese)
                if (host_menu::guestElf.find(prefix) != std::string::npos)
                    profile = &inputProfiles()[1];
            for (const InputProfile &candidate : inputProfiles())
                if (candidate.gameId[0] && std::strcmp(candidate.gameId, id) == 0)
                    profile = &candidate;
        }
        return *profile;
    }

    // The pointer is held while the mouse turns the camera, and given back for the F1 menu, the debugger and
    // other windows. A frame or two after each change only carry the recentring jump.
    bool mouseCaptured = false;
    int mouseSkipFrames = 0;

    void updateMouseCapture()
    {
        const bool want = host_menu::mouseLook && IsWindowFocused() && !host_menu::open && !host_menu::debugCursor;
        if (want != mouseCaptured)
        {
            mouseCaptured = want;
            mouseSkipFrames = 2;
            if (want)
                DisableCursor();
            else
                EnableCursor();
        }
    }

    // Mouse speed as right-stick deflection. The stick only sets a turn rate, so the speed is averaged over a few
    // host frames (the game reads the pad at its own, lower rate) and starts past the dead zone of the stick.
    void applyMouseLook(uint8_t *data)
    {
        static float speedX = 0.0f, speedY = 0.0f;
        constexpr float kFullSpeed = 1500.0f;  // counts per second for a full stick at 100%
        constexpr float kAverageSeconds = 0.035f;
        constexpr float kStickDeadZone = 0.20f;
        constexpr float kStill = 0.01f;

        Vector2 delta{0.0f, 0.0f};
        if (mouseCaptured)
            delta = GetMouseDelta();
        if (mouseSkipFrames > 0)
        {
            --mouseSkipFrames;
            delta = {0.0f, 0.0f};
        }
        const float seconds = std::clamp(GetFrameTime(), 0.001f, 0.1f);
        const float blend = 1.0f - std::exp(-seconds / kAverageSeconds);
        speedX += (delta.x / seconds - speedX) * blend;
        speedY += (delta.y / seconds - speedY) * blend;

        const float scale = host_menu::mouseSensitivity / 100.0f / kFullSpeed;
        const float x = speedX * scale;
        const float y = (host_menu::mouseInvert ? -speedY : speedY) * scale;
        const float length = std::hypot(x, y);
        if (length < kStill || data[4] != kPadStickCenter || data[5] != kPadStickCenter)
            return;
        const float stick = (kStickDeadZone + (1.0f - kStickDeadZone) * std::min(length, 1.0f)) / length;
        data[4] = static_cast<uint8_t>(std::clamp(128 + static_cast<int>(std::lround(x * stick * 127.0f)), 1, 255));
        data[5] = static_cast<uint8_t>(std::clamp(128 + static_cast<int>(std::lround(y * stick * 127.0f)), 1, 255));
    }
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
    const bool useGamepad = host_pad::available(kGamepad);
    static int connected[2] = {-1, -1};
    if (port >= 0 && port < 2 && connected[port] != static_cast<int>(useGamepad)) {
        connected[port] = useGamepad;
        std::cerr << "[pad:host] port=" << port << " connected=" << useGamepad;
        if (useGamepad) std::cerr << " name=" << host_pad::name(kGamepad);
        std::cerr << std::endl;
    }
    auto clearBit = [&btns](uint16_t mask)
    { btns &= ~mask; };

    // The PC settings menu owns the keyboard and pads while it is open (and until its keys are released).
    if (host_menu::open || host_menu::suppressPad)
        return true;
    if (useGamepad)
    {
        const auto IsGamepadButtonDown = host_pad::down;
        // Modern layout: the triggers do what L1/R1 do on the PS2 pad, and the bumpers what L2/R2 do.
        const bool modern = host_menu::modernLayout;
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
            clearBit(modern ? PAD_L2 : PAD_L1);
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_RIGHT_TRIGGER_1))
            clearBit(modern ? PAD_R2 : PAD_R1);
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_LEFT_TRIGGER_2))
            clearBit(modern ? PAD_L1 : PAD_L2);
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_RIGHT_TRIGGER_2))
            clearBit(modern ? PAD_R1 : PAD_R2);
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_MIDDLE_RIGHT))
            clearBit(PAD_START);
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_MIDDLE_LEFT))
            clearBit(PAD_SELECT);
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_LEFT_THUMB))
            clearBit(PAD_L3);
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_RIGHT_THUMB))
            clearBit(PAD_R3);

        float lx = host_pad::axis(kGamepad, GAMEPAD_AXIS_LEFT_X);
        float ly = host_pad::axis(kGamepad, GAMEPAD_AXIS_LEFT_Y);
        float rx = host_pad::axis(kGamepad, GAMEPAD_AXIS_RIGHT_X);
        float ry = host_pad::axis(kGamepad, GAMEPAD_AXIS_RIGHT_Y);
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
        const InputProfile &profile = inputProfile();
        const bool altHeld = IsKeyDown(KEY_LEFT_ALT) || IsKeyDown(KEY_RIGHT_ALT);  // Alt+Enter is fullscreen
        for (const KeyBind &bind : profile.keys)
            if (IsKeyDown(bind.key) && !(bind.key == KEY_ENTER && altHeld))
                clearBit(bind.button);
        if (profile.wasdIsDpad)
        {
            if (IsKeyDown(KEY_W)) clearBit(PAD_UP);
            if (IsKeyDown(KEY_S)) clearBit(PAD_DOWN);
            if (IsKeyDown(KEY_A)) clearBit(PAD_LEFT);
            if (IsKeyDown(KEY_D)) clearBit(PAD_RIGHT);
        }
        if (IsWindowFocused())
        {
            if (IsMouseButtonDown(MOUSE_BUTTON_LEFT) && profile.mouseLeft) clearBit(profile.mouseLeft);
            if (IsMouseButtonDown(MOUSE_BUTTON_RIGHT) && profile.mouseRight) clearBit(profile.mouseRight);
            if (IsMouseButtonDown(MOUSE_BUTTON_MIDDLE) && profile.mouseMiddle) clearBit(profile.mouseMiddle);
            // A wheel step is a short press, long enough for the game's own frame rate.
            static double wheelUntil = 0.0;
            if (GetMouseWheelMove() != 0.0f) wheelUntil = GetTime() + 0.12;
            if (GetTime() < wheelUntil && profile.wheel) clearBit(profile.wheel);
        }
        if (IsKeyDown(KEY_A) || IsKeyDown(KEY_D)) data[6] = IsKeyDown(KEY_A) == IsKeyDown(KEY_D) ? 128 : IsKeyDown(KEY_A) ? 1 : 255;
        if (IsKeyDown(KEY_W) || IsKeyDown(KEY_S)) data[7] = IsKeyDown(KEY_W) == IsKeyDown(KEY_S) ? 128 : IsKeyDown(KEY_W) ? 1 : 255;
        if (IsKeyDown(KEY_J) || IsKeyDown(KEY_L)) data[4] = IsKeyDown(KEY_J) == IsKeyDown(KEY_L) ? 128 : IsKeyDown(KEY_J) ? 1 : 255;
        if (IsKeyDown(KEY_I) || IsKeyDown(KEY_K)) data[5] = IsKeyDown(KEY_I) == IsKeyDown(KEY_K) ? 128 : IsKeyDown(KEY_I) ? 1 : 255;
        applyMouseLook(data);
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
    // DualShock 2 pressure bytes: games in pressure mode read these instead of the digital bits.
    static constexpr uint16_t kPressureOrder[12] = {PAD_RIGHT, PAD_LEFT, PAD_UP, PAD_DOWN, PAD_TRIANGLE, PAD_CIRCLE,
                                                    PAD_CROSS, PAD_SQUARE, PAD_L1, PAD_R1, PAD_L2, PAD_R2};
    for (int i = 0; i < 12; ++i)
        data[8 + i] = (btns & kPressureOrder[i]) ? 0u : 0xFFu;
    return true;
}

namespace {
    std::mutex hostPadMutex;
    std::array<std::array<uint8_t, 32>, 2> hostPads{};
    bool hostPadsReady = false;
}
// Called on the game's thread by scePadSetActDirect and friends.
void ps2SetHostPadRumble(int port, uint16_t low, uint16_t high) {
    host_pad::setRumble(port, low, high);
}
void ps2UpdateHostPadInput() {
    updateMouseCapture();
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
