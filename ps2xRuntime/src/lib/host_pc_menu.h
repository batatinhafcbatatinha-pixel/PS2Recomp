#pragma once
// "PC Settings" menu of the PS2 ports (F1, or Select+Start on a gamepad): display mode, monitor, aspect,
// picture filter, vsync, volume, an fps counter, vibration, the mouse and the pad layout, saved to pc-settings.ini
// next to the game. It is drawn
// over the game with raylib in the look of each game's own menus (colours, layout, letter spacing); the
// fonts come from the system, nothing from the game is bundled.
#include "host_pad.h"
#include "host_video_settings.h"
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#if defined(_WIN32)
extern "C" __declspec(dllimport) unsigned short __stdcall GetUserDefaultUILanguage(void);
#endif

namespace host_menu {
    struct Theme {
        const char *gameId;        // ELF name, e.g. "SLUS_218.15"
        const char *title;         // window title
        std::vector<const char *> fonts;
        Color dim, panel, rule, titleText, text, muted, selBar, selText, outline;
        bool band;                 // full-width band (else a centred panel)
        float roundness;           // panel corners
        bool upper;
        float spacing, textSize, titleSize;
    };

    // Looks taken from each game's own menus.
    inline const std::vector<Theme> &themes() {
        static const std::vector<Theme> list = {
            // Call of Duty: World at War - Final Fronts: dark grey band with thin grey rules, thin spaced capitals,
            // the selected entry on a dark bronze bar.
            {"SLUS_217.46", "Call of Duty: World at War - Final Fronts",
             {"C:/Windows/Fonts/bahnschrift.ttf", "C:/Windows/Fonts/arialn.ttf", "C:/Windows/Fonts/arial.ttf"},
             {0, 0, 0, 120}, {14, 14, 14, 235}, {92, 92, 88, 255}, {200, 200, 192, 255}, {138, 138, 132, 255},
             {96, 96, 92, 255}, {47, 39, 25, 255}, {222, 220, 210, 255}, {0, 0, 0, 0}, true, 0.0f, true, 3.0f, 26, 30},
            // Ben 10 Alien Force: black panel, Omnitrix green frame and title, white text, green selection.
            {"SLUS_218.15", "Ben 10: Alien Force",
             {"C:/Windows/Fonts/arialbd.ttf", "C:/Windows/Fonts/segoeuib.ttf"},
             {0, 0, 0, 140}, {6, 10, 6, 235}, {92, 208, 40, 255}, {120, 230, 60, 255}, {236, 240, 236, 255},
             {120, 130, 120, 255}, {70, 170, 30, 255}, {6, 12, 4, 255}, {0, 0, 0, 0}, false, 0.06f, false, 1.0f, 28, 38},
            // Boku no Natsuyasumi 2: summer sky, white rounded letters with a blue outline.
            {"SCPS_150.26", "Boku no Natsuyasumi 2",
             {"C:/Windows/Fonts/seguisb.ttf", "C:/Windows/Fonts/segoeui.ttf"},
             {0, 0, 0, 60}, {120, 176, 224, 200}, {250, 252, 255, 255}, {255, 255, 255, 255}, {255, 255, 255, 255},
             {214, 230, 245, 255}, {255, 255, 255, 110}, {255, 255, 255, 255}, {38, 84, 150, 255}, false, 0.12f, false, 1.5f, 28, 38},
        };
        return list;
    }
    inline const Theme &fallbackTheme() {
        static const Theme theme{"", "", {"C:/Windows/Fonts/segoeui.ttf", "C:/Windows/Fonts/arial.ttf"},
            {0, 0, 0, 120}, {20, 22, 28, 235}, {90, 96, 110, 255}, {240, 240, 240, 255}, {225, 225, 225, 255},
            {130, 130, 130, 255}, {60, 90, 150, 255}, {255, 255, 255, 255}, {0, 0, 0, 0}, false, 0.04f, false, 1.0f, 26, 34};
        return theme;
    }

    inline const Theme *theme = nullptr;
    inline std::string gameTitle;
    inline bool open = false;
    inline bool suppressPad = false;  // keeps the pad neutral until the keys that closed the menu are released
    inline bool portuguese = false;
    inline int selected = 0;
    inline int filter = 1;            // 0 sharp (nearest), 1 smooth (bilinear)
    inline int vsync = 0;
    inline int volume = 100;
    inline int showFps = 0;
    inline int vibration = 1;
    inline bool hasModernLayout = false;  // games with a remapped pad layout
    // The mouse is the right stick, so a game with its own sensitivity and invert options applies them to the mouse:
    // the menu must not offer them twice.
    inline bool gameHasLookOptions = false;
    inline int modernLayout = 0;          // triggers do L1/R1, bumpers do L2/R2
    inline int mouseLook = 0;             // mouse = right stick
    inline int mouseSensitivity = 100;    // percent
    inline int mouseInvert = 0;
    inline bool debugCursor = false;      // Shift+F1 debugger open: it needs the pointer
    inline Font font{};
    inline bool fontLoaded = false;

    inline const char *T(const char *pt, const char *en) { return portuguese ? pt : en; }

    inline void save() {
        std::ofstream file("pc-settings.ini");
        file << "aspect=" << (host_video::stretch ? 1 : 0) << "\nfilter=" << filter << "\nvsync=" << vsync
             << "\nvolume=" << volume << "\nshow_fps=" << showFps << "\nmouse_look=" << mouseLook
             << "\nmouse_sensitivity=" << mouseSensitivity << "\nmouse_invert=" << mouseInvert
             << "\nmodern_layout=" << modernLayout << "\nvibration=" << vibration << "\n";
    }
    inline void load() {
        std::ifstream file("pc-settings.ini");
        std::string line;
        std::map<std::string, int> values;
        while (std::getline(file, line)) {
            const size_t eq = line.find('=');
            if (eq != std::string::npos) values[line.substr(0, eq)] = std::atoi(line.c_str() + eq + 1);
        }
        auto get = [&](const char *key, int fallback, int lo, int hi) {
            auto it = values.find(key);
            return it == values.end() ? fallback : std::clamp(it->second, lo, hi);
        };
        host_video::stretch = get("aspect", 0, 0, 1) != 0;
        filter = get("filter", 1, 0, 1);
        vsync = get("vsync", 0, 0, 1);
        volume = get("volume", 100, 0, 100);
        showFps = get("show_fps", 0, 0, 1);
        // The shooter starts with PC controls; the other games have no free camera for the mouse to turn.
        hasModernLayout = gameHasLookOptions = std::strcmp(theme->gameId, "SLUS_217.46") == 0;
        vibration = get("vibration", 1, 0, 1);
        mouseLook = get("mouse_look", hasModernLayout ? 1 : 0, 0, 1);
        mouseSensitivity = gameHasLookOptions ? 100 : get("mouse_sensitivity", 100, 10, 300);
        mouseInvert = gameHasLookOptions ? 0 : get("mouse_invert", 0, 0, 1);
        modernLayout = hasModernLayout ? get("modern_layout", 1, 0, 1) : 0;
    }
    inline void applyRuntime() {
        if (vsync) SetWindowState(FLAG_VSYNC_HINT); else ClearWindowState(FLAG_VSYNC_HINT);
        SetMasterVolume(volume / 100.0f);
    }

    // Called once the window exists; `windowTitle` carries the guest ELF name.
    inline void initialize(const char *windowTitle) {
        theme = &fallbackTheme();
        for (const Theme &candidate : themes())
            if (windowTitle && std::strstr(windowTitle, candidate.gameId)) theme = &candidate;
        gameTitle = theme->title[0] ? theme->title : (windowTitle ? windowTitle : "");
        host_video::windowTitle = gameTitle;
        if (!gameTitle.empty()) SetWindowTitle(gameTitle.c_str());
#if defined(_WIN32)
        portuguese = (GetUserDefaultUILanguage() & 0x3ff) == 0x16;
#else
        const char *lang = std::getenv("LANG");
        portuguese = lang && std::strncmp(lang, "pt", 2) == 0;
#endif
        load();
        applyRuntime();
        host_pad::initialize();
        std::vector<int> codepoints;
        for (int c = 32; c < 127; ++c) codepoints.push_back(c);
        for (int c : {0xC0, 0xC1, 0xC2, 0xC3, 0xC7, 0xC9, 0xCA, 0xCD, 0xD3, 0xD4, 0xD5, 0xDA,
                      0xE0, 0xE1, 0xE2, 0xE3, 0xE7, 0xE9, 0xEA, 0xED, 0xF3, 0xF4, 0xF5, 0xFA, 0x2039, 0x203A})
            codepoints.push_back(c);
        std::vector<const char *> paths = theme->fonts;
        for (const char *extra : {"/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", "/usr/share/fonts/TTF/DejaVuSans.ttf",
                                  "/System/Library/Fonts/Supplemental/Arial.ttf", "/Library/Fonts/Arial.ttf"})
            paths.push_back(extra);
        for (const char *path : paths) {
            if (!FileExists(path)) continue;
            font = LoadFontEx(path, 64, codepoints.data(), static_cast<int>(codepoints.size()));
            if (font.texture.id != 0) {
                SetTextureFilter(font.texture, TEXTURE_FILTER_BILINEAR);
                fontLoaded = true;
                break;
            }
        }
        if (!fontLoaded) font = GetFontDefault();
        std::cerr << "[pc-menu] game=\"" << gameTitle << "\" F1: PC settings" << std::endl;
    }

    inline std::string upper(std::string text) {
        for (size_t i = 0; i < text.size(); ++i) {
            const unsigned char c = static_cast<unsigned char>(text[i]);
            if (c == 0xC3 && i + 1 < text.size()) {
                unsigned char &next = reinterpret_cast<unsigned char &>(text[i + 1]);
                if (next >= 0xA0 && next <= 0xBE && next != 0xB7) next -= 0x20;
                ++i;
            } else if (c < 0x80) text[i] = static_cast<char>(std::toupper(c));
        }
        return text;
    }

    struct Item { std::string label, value; int id; float slider = -1.0f; };  // slider: 0 to 1, drawn as a bar
    enum { kDisplay, kMonitor, kAspect, kFilter, kVsync, kVolume, kFps, kVibration, kLayout, kMouseLook, kMouseSpeed, kMouseInvert, kResume };
    constexpr int kMinSensitivity = 10, kMaxSensitivity = 300;
    constexpr float kSliderOffset = 330.0f, kSliderWidth = 220.0f;  // from the right edge of the rows

    inline std::vector<Item> items() {
        std::vector<Item> list;
        const char *modes[] = {T("Janela", "Window"), T("Tela cheia sem bordas", "Borderless fullscreen"), T("Tela cheia", "Fullscreen")};
        const int mode = host_video::mode == host_video::Mode::Windowed ? 0 : host_video::mode == host_video::Mode::Borderless ? 1 : 2;
        list.push_back({T("Modo de exibição", "Display mode"), modes[mode], kDisplay});
        if (GetMonitorCount() > 1) {
            const int monitor = GetCurrentMonitor();
            list.push_back({"Monitor", std::to_string(monitor + 1) + " (" + std::to_string(GetMonitorWidth(monitor)) + "x" +
                                           std::to_string(GetMonitorHeight(monitor)) + ")", kMonitor});
        }
        list.push_back({T("Proporção", "Aspect ratio"), host_video::stretch ? T("Esticada", "Stretched") : T("4:3 (original)", "4:3 (original)"), kAspect});
        list.push_back({T("Filtro da imagem", "Picture filter"), filter ? T("Suave", "Smooth") : T("Nítido (pixels)", "Sharp (pixels)"), kFilter});
        list.push_back({T("Sincronização vertical", "Vertical sync"), vsync ? T("Ligada", "On") : T("Desligada", "Off"), kVsync});
        list.push_back({T("Volume", "Volume"), std::to_string(volume) + "%", kVolume});
        list.push_back({T("Mostrar FPS", "Show FPS"), showFps ? T("Sim", "Yes") : T("Não", "No"), kFps});
        list.push_back({T("Vibração do controle", "Controller vibration"), vibration ? T("Ligada", "On") : T("Desligada", "Off"), kVibration});
        if (hasModernLayout)
            list.push_back({T("Layout do controle", "Controller layout"),
                            modernLayout ? T("Moderno (LT mira, RT atira)", "Modern (LT aim, RT fire)") : T("Original do PS2", "Original PS2"), kLayout});
        list.push_back({T("Mouse como câmera", "Mouse look"), mouseLook ? T("Ligado", "On") : T("Desligado", "Off"), kMouseLook});
        if (!gameHasLookOptions) {
            list.push_back({T("Sensibilidade do mouse", "Mouse sensitivity"), std::to_string(mouseSensitivity) + "%", kMouseSpeed,
                            static_cast<float>(mouseSensitivity - kMinSensitivity) / (kMaxSensitivity - kMinSensitivity)});
            list.push_back({T("Inverter mouse", "Invert mouse"), mouseInvert ? T("Sim", "Yes") : T("Não", "No"), kMouseInvert});
        }
        list.push_back({T("Voltar ao jogo", "Back to game"), "", kResume});
        return list;
    }

    inline bool mouseArmed = false;
    inline void close() {
        open = false;
        mouseArmed = false;
        suppressPad = true;
        save();
    }

    inline void change(int id, int direction) {
        switch (id) {
        case kDisplay: {
            const int mode = host_video::mode == host_video::Mode::Windowed ? 0 : host_video::mode == host_video::Mode::Borderless ? 1 : 2;
            const int next = (mode + direction + 3) % 3;
            host_video::apply(next == 0 ? host_video::Mode::Windowed : next == 1 ? host_video::Mode::Borderless : host_video::Mode::Fullscreen);
            break;
        }
        case kMonitor: {
            const int count = GetMonitorCount();
            const int target = (GetCurrentMonitor() + direction + count) % count;
            const host_video::Mode mode = host_video::mode;
            if (mode != host_video::Mode::Windowed) host_video::apply(host_video::Mode::Windowed, false);
            const Vector2 origin = GetMonitorPosition(target);
            SetWindowPosition(static_cast<int>(origin.x) + 40, static_cast<int>(origin.y) + 40);
            host_video::apply(mode, false);
            break;
        }
        case kAspect: host_video::stretch = !host_video::stretch; break;
        case kFilter: filter = !filter; break;
        case kVsync: vsync = !vsync; applyRuntime(); break;
        case kVolume: volume = std::clamp(volume + direction * 10, 0, 100); applyRuntime(); break;
        case kFps: showFps = !showFps; break;
        case kVibration: vibration = !vibration; break;
        case kLayout: modernLayout = !modernLayout; break;
        case kMouseLook: mouseLook = !mouseLook; break;
        case kMouseSpeed: mouseSensitivity = std::clamp(mouseSensitivity + direction * 10, kMinSensitivity, kMaxSensitivity); break;
        case kMouseInvert: mouseInvert = !mouseInvert; break;
        case kResume: close(); return;
        }
        save();
    }

    inline bool padPressed(int button) {
        for (int pad = 0; pad < host_pad::kMaxPads; ++pad)
            if (host_pad::pressed(pad, button)) return true;
        return false;
    }
    inline bool padDown(int button) {
        for (int pad = 0; pad < host_pad::kMaxPads; ++pad)
            if (host_pad::down(pad, button)) return true;
        return false;
    }

    // Layout on a 1280x720 canvas, scaled to the window.
    struct Layout { float scale, x0, y0, rowH, top, left, right; };
    inline Layout layout(size_t rows) {
        Layout l{};
        l.scale = std::min(GetScreenWidth() / 1280.0f, GetScreenHeight() / 720.0f);
        l.x0 = (GetScreenWidth() - 1280.0f * l.scale) * 0.5f;
        l.y0 = (GetScreenHeight() - 720.0f * l.scale) * 0.5f;
        l.rowH = std::min(48.0f, 530.0f / rows);  // the mouse rows must still fit the canvas
        const float height = 110.0f + rows * l.rowH + 60.0f;
        l.top = (720.0f - height) * 0.5f;
        l.left = theme->band ? 230.0f : 300.0f;
        l.right = theme->band ? 1050.0f : 980.0f;
        return l;
    }

    // Keyboard, gamepad and mouse; runs every host frame before the pad is read for the game.
    // PS2X_PC_MENU_TEST=<seconds>: opens the menu after that time, selects row 2, and saves pc-menu-test.png
    // in the working directory one second later (visual check without a keyboard).
    inline void testStep() {
        static const char *env = std::getenv("PS2X_PC_MENU_TEST");
        static int stage = 0;
        if (!env || stage > 1) return;
        const double at = std::atof(env);
        if (stage == 0 && GetTime() >= at) { open = true; selected = 2; stage = 1; }
        else if (stage == 1 && GetTime() >= at + 1.0) { TakeScreenshot("pc-menu-test.png"); stage = 2; }
    }

    inline void update() {
        host_pad::rumbleEnabled = vibration && !open;
        host_pad::update();
        testStep();
        if (suppressPad) {
            const bool held = IsKeyDown(KEY_ESCAPE) || IsKeyDown(KEY_ENTER) || IsKeyDown(KEY_X) || IsKeyDown(KEY_C) ||
                              IsKeyDown(KEY_SPACE) || IsKeyDown(KEY_BACKSPACE) || IsKeyDown(KEY_F1) || padDown(GAMEPAD_BUTTON_RIGHT_FACE_RIGHT) ||
                              padDown(GAMEPAD_BUTTON_RIGHT_FACE_DOWN) || padDown(GAMEPAD_BUTTON_MIDDLE_LEFT) ||
                              padDown(GAMEPAD_BUTTON_MIDDLE_RIGHT) || IsMouseButtonDown(MOUSE_BUTTON_LEFT);
            if (!held) suppressPad = false;
        }
        const bool shift = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
        if (IsKeyPressed(KEY_F1) && shift) debugCursor = !debugCursor;
        const bool padCombo = (padPressed(GAMEPAD_BUTTON_MIDDLE_RIGHT) && padDown(GAMEPAD_BUTTON_MIDDLE_LEFT)) ||
                              (padPressed(GAMEPAD_BUTTON_MIDDLE_LEFT) && padDown(GAMEPAD_BUTTON_MIDDLE_RIGHT));
        if (!open) {
            if ((IsKeyPressed(KEY_F1) && !shift) || padCombo) { open = true; selected = 0; }
            return;
        }
        if ((IsKeyPressed(KEY_F1) && !shift) || IsKeyPressed(KEY_ESCAPE) || IsKeyPressed(KEY_BACKSPACE) || padCombo ||
            padPressed(GAMEPAD_BUTTON_RIGHT_FACE_RIGHT)) {
            close();
            return;
        }
        const std::vector<Item> list = items();
        const int count = static_cast<int>(list.size());
        static double stickRepeat = 0.0;
        float stickY = 0.0f, stickX = 0.0f;
        for (int pad = 0; pad < host_pad::kMaxPads; ++pad) {
            stickY += host_pad::axis(pad, GAMEPAD_AXIS_LEFT_Y);
            stickX += host_pad::axis(pad, GAMEPAD_AXIS_LEFT_X);
        }
        int stickStep = 0, stickSide = 0;
        if ((std::abs(stickY) > 0.6f || std::abs(stickX) > 0.6f) && GetTime() >= stickRepeat) {
            if (std::abs(stickY) >= std::abs(stickX)) stickStep = stickY > 0 ? 1 : -1;
            else stickSide = stickX > 0 ? 1 : -1;
            stickRepeat = GetTime() + 0.22;
        } else if (std::abs(stickY) <= 0.6f && std::abs(stickX) <= 0.6f) stickRepeat = 0.0;
        if (IsKeyPressed(KEY_UP) || IsKeyPressed(KEY_W) || padPressed(GAMEPAD_BUTTON_LEFT_FACE_UP) || stickStep < 0)
            selected = (selected + count - 1) % count;
        if (IsKeyPressed(KEY_DOWN) || IsKeyPressed(KEY_S) || padPressed(GAMEPAD_BUTTON_LEFT_FACE_DOWN) || stickStep > 0)
            selected = (selected + 1) % count;
        selected = std::clamp(selected, 0, count - 1);
        const int id = list[selected].id;
        if (IsKeyPressed(KEY_LEFT) || IsKeyPressed(KEY_A) || padPressed(GAMEPAD_BUTTON_LEFT_FACE_LEFT) || stickSide < 0) {
            if (id != kResume) change(id, -1);
        } else if (IsKeyPressed(KEY_RIGHT) || IsKeyPressed(KEY_D) || padPressed(GAMEPAD_BUTTON_LEFT_FACE_RIGHT) || stickSide > 0) {
            if (id != kResume) change(id, 1);
        } else if (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_SPACE) || IsKeyPressed(KEY_X) ||
                   padPressed(GAMEPAD_BUTTON_RIGHT_FACE_DOWN)) {
            change(id, 1);
        }
        // Mouse: hover selects, left click advances the value, right click goes back.
        const Layout l = layout(list.size());
        const Vector2 mouse = GetMousePosition();
        const float mx = (mouse.x - l.x0) / l.scale, my = (mouse.y - l.y0) / l.scale;
        // Hover only counts once the mouse moves after the menu opened (else it steals the keyboard selection).
        static Vector2 lastMouse{-1.0f, -1.0f};
        static bool dragging = false;
        if (!mouseArmed) { lastMouse = mouse; dragging = false; }
        mouseArmed = true;
        const bool moved = mouse.x != lastMouse.x || mouse.y != lastMouse.y;
        lastMouse = mouse;
        const float rowsTop = l.top + 110.0f;
        const float sliderLeft = l.right - kSliderOffset;
        const auto dragSlider = [&] {
            const float position = std::clamp((mx - sliderLeft) / kSliderWidth, 0.0f, 1.0f);
            const int steps = (kMaxSensitivity - kMinSensitivity) / 5;
            mouseSensitivity = kMinSensitivity + static_cast<int>(std::lround(position * steps)) * 5;
        };
        if (dragging) {
            if (IsMouseButtonDown(MOUSE_BUTTON_LEFT)) dragSlider();
            else { dragging = false; save(); }
        } else if (mx >= l.left - 30.0f && mx <= l.right + 30.0f && my >= rowsTop && my < rowsTop + count * l.rowH) {
            const int row = static_cast<int>((my - rowsTop) / l.rowH);
            if (moved) selected = row;
            const bool onSlider = list[row].slider >= 0.0f && mx >= sliderLeft - 12.0f && mx <= sliderLeft + kSliderWidth + 12.0f;
            if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                if (onSlider) { dragging = true; dragSlider(); }
                else change(list[row].id, 1);
            } else if (IsMouseButtonPressed(MOUSE_BUTTON_RIGHT) && list[row].id != kResume) change(list[row].id, -1);
        }
    }

    inline void text(const std::string &value, float x, float y, float size, Color color, const Layout &l, int align = 0) {
        const std::string s = theme->upper ? upper(value) : value;
        const float px = size * l.scale, spacing = theme->spacing * l.scale;
        const Vector2 measure = MeasureTextEx(font, s.c_str(), px, spacing);
        float sx = l.x0 + x * l.scale;
        if (align == 1) sx -= measure.x;
        else if (align == 2) sx -= measure.x * 0.5f;
        const float sy = l.y0 + y * l.scale - measure.y * 0.5f;
        if (theme->outline.a) {
            const float o = std::max(1.0f, 2.0f * l.scale);
            for (int dx = -1; dx <= 1; ++dx)
                for (int dy = -1; dy <= 1; ++dy)
                    if (dx || dy) DrawTextEx(font, s.c_str(), {sx + dx * o, sy + dy * o}, px, spacing, theme->outline);
        }
        DrawTextEx(font, s.c_str(), {sx, sy}, px, spacing, color);
    }
    inline Rectangle rect(const Layout &l, float x, float y, float w, float h) {
        return {l.x0 + x * l.scale, l.y0 + y * l.scale, w * l.scale, h * l.scale};
    }

    inline void drawFps(double fps) {
        if (!showFps) return;
        char buffer[32];
        std::snprintf(buffer, sizeof(buffer), "%.0f FPS", fps);
        const float size = std::max(16.0f, GetScreenHeight() / 40.0f);
        DrawRectangle(8, 8, static_cast<int>(MeasureTextEx(font, buffer, size, 1.0f).x) + 16, static_cast<int>(size) + 8, Fade(BLACK, 0.6f));
        DrawTextEx(font, buffer, {16.0f, 12.0f}, size, 1.0f, theme ? theme->text : WHITE);
    }

    inline void draw() {
        if (!open || !theme) return;
        const std::vector<Item> list = items();
        const Layout l = layout(list.size());
        const float height = 110.0f + list.size() * l.rowH + 60.0f;
        DrawRectangle(0, 0, GetScreenWidth(), GetScreenHeight(), theme->dim);
        if (theme->band) {
            // Full-width band with thin rules, like the game's main menu strip.
            const Rectangle band{0.0f, l.y0 + l.top * l.scale, static_cast<float>(GetScreenWidth()), height * l.scale};
            DrawRectangleRec(band, theme->panel);
            DrawRectangleRec({0.0f, band.y, band.width, std::max(1.0f, 2.0f * l.scale)}, theme->rule);
            DrawRectangleRec({0.0f, band.y + band.height - std::max(1.0f, 2.0f * l.scale), band.width, std::max(1.0f, 2.0f * l.scale)}, theme->rule);
            DrawRectangleRec(rect(l, l.left - 30.0f, l.top + 84.0f, l.right - l.left + 60.0f, 1.0f), theme->rule);
        } else {
            const Rectangle panel = rect(l, l.left - 50.0f, l.top, l.right - l.left + 100.0f, height);
            DrawRectangleRounded(panel, theme->roundness, 8, theme->panel);
            DrawRectangleRoundedLinesEx(panel, theme->roundness, 8, std::max(1.0f, 3.0f * l.scale), theme->rule);
        }
        text(T("Configurações do PC", "PC Settings"), theme->band ? l.left : 640.0f, l.top + 50.0f, theme->titleSize,
             theme->titleText, l, theme->band ? 0 : 2);
        const float rowsTop = l.top + 110.0f;
        for (size_t i = 0; i < list.size(); ++i) {
            const bool sel = static_cast<int>(i) == selected;
            const float y = rowsTop + i * l.rowH;
            if (sel) {
                const Rectangle bar = rect(l, l.left - 30.0f, y + 4.0f, l.right - l.left + 60.0f, l.rowH - 8.0f);
                if (theme->roundness > 0.0f) DrawRectangleRounded(bar, 0.4f, 8, theme->selBar);
                else DrawRectangleRec(bar, theme->selBar);
            }
            const Color color = sel ? theme->selText : (list[i].id == kResume ? theme->muted : theme->text);
            const float cy = y + l.rowH * 0.5f;
            if (list[i].id == kResume) {
                text(list[i].label, theme->band ? l.left : 640.0f, cy, theme->textSize, color, l, theme->band ? 0 : 2);
                continue;
            }
            text(list[i].label, l.left, cy, theme->textSize, color, l);
            if (list[i].slider >= 0.0f) {
                const float left = l.right - kSliderOffset;
                DrawRectangleRounded(rect(l, left, cy - 3.0f, kSliderWidth, 6.0f), 1.0f, 6, Fade(color, 0.35f));
                DrawRectangleRounded(rect(l, left, cy - 3.0f, kSliderWidth * list[i].slider, 6.0f), 1.0f, 6, color);
                DrawCircleV({l.x0 + (left + kSliderWidth * list[i].slider) * l.scale, l.y0 + cy * l.scale}, 9.0f * l.scale, color);
                text(list[i].value, l.right, cy, theme->textSize, color, l, 1);
                continue;
            }
            text(sel ? "\xE2\x80\xB9  " + list[i].value + "  \xE2\x80\xBA" : list[i].value, l.right, cy, theme->textSize, color, l, 1);
        }
        text(T("Setas: escolher e mudar   Enter: mudar   F1 / Esc: voltar ao jogo",
               "Arrows: choose and change   Enter: change   F1 / Esc: back to game"),
             640.0f, l.top + height - 30.0f, 18.0f, theme->muted, l, 2);
    }
}
