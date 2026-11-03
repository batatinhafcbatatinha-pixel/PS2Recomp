#pragma once
// "PC Settings" menu of the PS2 ports (F1, or Select+Start on a gamepad): display mode, monitor, refresh rate,
// resolution, aspect, upscaling (FSR 1.0), anti-aliasing, vsync, frame limit, graphics card, volume, an fps counter,
// vibration, the mouse and the pad layout, menu language, restart and quit, saved to pc-settings.ini next to the
// game. Every row has a line of help under the list. It is drawn
// over the game with raylib in the look of each game's own menus (colours, layout, letter spacing); the
// fonts come from the system, nothing from the game is bundled.
#include "host_os.h"
#include "host_pad.h"
#include "host_video_settings.h"
#include <cctype>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <utility>
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
    inline bool systemPortuguese = false;
    inline int uiLanguage = -1;       // menu language: -1 follows Windows, 0 Portuguese, 1 English
    inline int selected = 0;
    inline int scrollTop = 0;         // first row shown when the list is longer than the panel
    inline int filter = 1;            // 0 sharp (nearest), 1 smooth (bilinear), 2 AMD FSR 1.0
    inline int vsync = 0;
    inline int fpsLimit = 60;         // presentation only, the guest vblank stays at 60 Hz: 0 none, -1 the monitor's rate
    inline int gpuPreference = 0, bootGpuPreference = 0;  // Windows graphics preference, applied at the next start
    inline std::string gpuName;
    inline int volume = 100;
    inline int showFps = 0;
    inline int fxaa = 0;              // 0 off, 1-3 FXAA low to high, 4-7 supersampling 2x, 4x, 8x, 16x
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
    inline void applyLanguage() { portuguese = uiLanguage < 0 ? systemPortuguese : uiLanguage == 0; }


    // FXAA over the upscaled picture. The PS2 frame stays at its own resolution, so this softens
    // its stair-steps and shimmer where the plain bilinear filter leaves them visible.
    inline const char *kFxaaShader = R"(#version 330
in vec2 fragTexCoord;
in vec4 fragColor;
out vec4 finalColor;
uniform sampler2D texture0;
uniform vec2 texel;
uniform float spanMax;
uniform float reduceMul;
float luma(vec3 c) { return dot(c, vec3(0.299, 0.587, 0.114)); }
void main() {
    vec3 center = texture(texture0, fragTexCoord).rgb;
    float lumaM = luma(center);
    float lumaNW = luma(texture(texture0, fragTexCoord + vec2(-1.0, -1.0) * texel).rgb);
    float lumaNE = luma(texture(texture0, fragTexCoord + vec2( 1.0, -1.0) * texel).rgb);
    float lumaSW = luma(texture(texture0, fragTexCoord + vec2(-1.0,  1.0) * texel).rgb);
    float lumaSE = luma(texture(texture0, fragTexCoord + vec2( 1.0,  1.0) * texel).rgb);
    float lumaMin = min(lumaM, min(min(lumaNW, lumaNE), min(lumaSW, lumaSE)));
    float lumaMax = max(lumaM, max(max(lumaNW, lumaNE), max(lumaSW, lumaSE)));
    vec2 dir = vec2(-((lumaNW + lumaNE) - (lumaSW + lumaSE)), (lumaNW + lumaSW) - (lumaNE + lumaSE));
    float reduce = max((lumaNW + lumaNE + lumaSW + lumaSE) * 0.25 * reduceMul, 1.0 / 128.0);
    float scale = 1.0 / (min(abs(dir.x), abs(dir.y)) + reduce);
    dir = clamp(dir * scale, vec2(-spanMax), vec2(spanMax)) * texel;
    vec3 a = 0.5 * (texture(texture0, fragTexCoord + dir * (1.0 / 3.0 - 0.5)).rgb +
                    texture(texture0, fragTexCoord + dir * (2.0 / 3.0 - 0.5)).rgb);
    vec3 b = a * 0.5 + 0.25 * (texture(texture0, fragTexCoord - dir * 0.5).rgb +
                               texture(texture0, fragTexCoord + dir * 0.5).rgb);
    float lumaB = luma(b);
    finalColor = vec4(lumaB < lumaMin || lumaB > lumaMax ? a : b, 1.0) * fragColor;
}
)";
    inline Shader fxaaProgram{};
    inline bool beginFxaa(float textureWidth, float textureHeight) {
        if (fxaa <= 0 || fxaa > 3) return false;
        if (fxaaProgram.id == 0) fxaaProgram = LoadShaderFromMemory(nullptr, kFxaaShader);
        if (!IsShaderValid(fxaaProgram)) return false;
        const float texel[2] = {1.0f / textureWidth, 1.0f / textureHeight};
        const float spans[3] = {4.0f, 8.0f, 16.0f}, reductions[3] = {1.0f / 4.0f, 1.0f / 8.0f, 1.0f / 16.0f};
        SetShaderValue(fxaaProgram, GetShaderLocation(fxaaProgram, "texel"), texel, SHADER_UNIFORM_VEC2);
        SetShaderValue(fxaaProgram, GetShaderLocation(fxaaProgram, "spanMax"), &spans[fxaa - 1], SHADER_UNIFORM_FLOAT);
        SetShaderValue(fxaaProgram, GetShaderLocation(fxaaProgram, "reduceMul"), &reductions[fxaa - 1], SHADER_UNIFORM_FLOAT);
        BeginShaderMode(fxaaProgram);
        return true;
    }

    // A frame rendered above the window's size: a plain bilinear fetch would skip most of its pixels, so average them.
    inline const char *kBoxShader = R"(#version 330
in vec2 fragTexCoord;
in vec4 fragColor;
out vec4 finalColor;
uniform sampler2D texture0;
uniform vec2 texel;
uniform int taps;
void main() {
    vec3 sum = vec3(0.0);
    for (int y = 0; y < taps; ++y)
        for (int x = 0; x < taps; ++x)
            sum += texture(texture0, fragTexCoord + (vec2(x, y) - 0.5 * float(taps - 1)) * texel).rgb;
    finalColor = vec4(sum / float(taps * taps), 1.0) * fragColor;
}
)";
    inline Shader boxProgram{};
    inline bool beginDownscale(float textureWidth, float textureHeight, float ratio) {
        if (filter == 0 || (fxaa >= 1 && fxaa <= 3) || ratio <= 1.2f) return false;
        if (boxProgram.id == 0) boxProgram = LoadShaderFromMemory(nullptr, kBoxShader);
        if (!IsShaderValid(boxProgram)) return false;
        const float texel[2] = {1.0f / textureWidth, 1.0f / textureHeight};
        const int taps = std::clamp(static_cast<int>(std::ceil(ratio)), 2, 8);
        SetShaderValue(boxProgram, GetShaderLocation(boxProgram, "texel"), texel, SHADER_UNIFORM_VEC2);
        SetShaderValue(boxProgram, GetShaderLocation(boxProgram, "taps"), &taps, SHADER_UNIFORM_INT);
        BeginShaderMode(boxProgram);
        return true;
    }

    // Picture sizes offered by the menu. A size above the monitor still renders at that size and is shown scaled down.
    inline std::vector<std::pair<int, int>> resolutions() {
        return {{0, 0}, {1280, 720}, {1366, 768}, {1600, 900}, {1920, 1080}, {2560, 1080}, {2560, 1440}, {3440, 1440}, {3840, 1600}, {3840, 2160}};
    }
    inline std::string resolutionName(int width, int height) {
        std::string text = std::to_string(width) + "x" + std::to_string(height);
        if (height == 2160) text += " (4K)";
        else if (width == 2560 && height == 1440) text += " (2K)";
        else if (width * 9 > height * 16 + 8) text += " (ultrawide)";
        return text;
    }
    // Render targets scale the PS2's 448 lines by a whole factor, to at least the chosen height; supersampling multiplies
    // the pixel count on top of that. The GS renderer reads it once, at startup.
    inline int renderScale() {
        const int height = host_video::resolutionHeight > 0 ? host_video::resolutionHeight : GetMonitorHeight(GetCurrentMonitor());
        const float samples = fxaa > 3 ? static_cast<float>(1 << (fxaa - 3)) : 1.0f;
        return std::clamp(static_cast<int>(std::ceil(height / 448.0f * std::sqrt(samples) - 0.001f)), 1, 12);
    }
    inline int startScale = 0;
    inline bool scaleFromMenu = false;  // PS2X_GS_HW_SCALE was set here (not by the launcher), so a restart may change it
    inline void setScaleVariable(int scale) {
        const std::string value = std::to_string(scale);
#if defined(_WIN32)
        _putenv_s("PS2X_GS_HW_SCALE", value.c_str());
#else
        setenv("PS2X_GS_HW_SCALE", value.c_str(), 1);
#endif
    }

    inline void save() {
        std::ofstream file("pc-settings.ini");
        file << "aspect=" << (host_video::stretch ? 1 : 0) << "\nfilter=" << filter << "\nvsync=" << vsync
             << "\nvolume=" << volume << "\nshow_fps=" << showFps << "\nmouse_look=" << mouseLook
             << "\nmouse_sensitivity=" << mouseSensitivity << "\nmouse_invert=" << mouseInvert
             << "\nmodern_layout=" << modernLayout << "\nvibration=" << vibration << "\nfxaa=" << fxaa
             << "\nres_w=" << host_video::resolutionWidth << "\nres_h=" << host_video::resolutionHeight
             << "\nrefresh=" << host_video::refreshRate << "\nfps_limit=" << fpsLimit << "\nmenu_language=" << uiLanguage << "\n";
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
        filter = get("filter", 1, 0, 2);
        vsync = get("vsync", 0, 0, 1);
        volume = get("volume", 100, 0, 100);
        showFps = get("show_fps", 0, 0, 1);
        fxaa = get("fxaa", 0, 0, 7);
        host_video::resolutionWidth = get("res_w", 0, 0, 7680);
        host_video::resolutionHeight = get("res_h", 0, 0, 4320);
        host_video::refreshRate = get("refresh", 0, 0, 1000);
        // The launcher's PS2X_FPS_LIMIT is the default until the menu chooses another limit.
        const char *launcherLimit = std::getenv("PS2X_FPS_LIMIT");
        fpsLimit = get("fps_limit", launcherLimit ? std::max(0, std::atoi(launcherLimit)) : 60, -1, 1000);
        uiLanguage = get("menu_language", -1, -1, 1);
        applyLanguage();
        // The shooter starts with PC controls; the other games have no free camera for the mouse to turn.
        hasModernLayout = gameHasLookOptions = std::strcmp(theme->gameId, "SLUS_217.46") == 0;
        vibration = get("vibration", 1, 0, 1);
        mouseLook = get("mouse_look", hasModernLayout ? 1 : 0, 0, 1);
        mouseSensitivity = gameHasLookOptions ? 100 : get("mouse_sensitivity", 100, 10, 300);
        mouseInvert = gameHasLookOptions ? 0 : get("mouse_invert", 0, 0, 1);
        modernLayout = hasModernLayout ? get("modern_layout", 1, 0, 1) : 0;
        startScale = renderScale();
        if (!std::getenv("PS2X_GS_HW_SCALE")) {
            scaleFromMenu = true;
            setScaleVariable(startScale);
        }
    }
    inline int monitorRate() {
        const int rate = GetMonitorRefreshRate(GetCurrentMonitor());
        return rate > 0 ? rate : 60;
    }
    inline void applyFrameLimit() { SetTargetFPS(fpsLimit < 0 ? monitorRate() : fpsLimit); }
    inline void applyRuntime() {
        if (vsync) SetWindowState(FLAG_VSYNC_HINT); else ClearWindowState(FLAG_VSYNC_HINT);
        SetMasterVolume(volume / 100.0f);
        applyFrameLimit();
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
        systemPortuguese = (GetUserDefaultUILanguage() & 0x3ff) == 0x16;
#else
        const char *lang = std::getenv("LANG");
        systemPortuguese = lang && std::strncmp(lang, "pt", 2) == 0;
#endif
        load();
        bootGpuPreference = gpuPreference = host_os::gpuPreference();
        gpuName = host_os::gpuName();
        if (host_video::resolutionWidth > 0) host_video::apply(host_video::mode, false);
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

    // AMD FidelityFX Super Resolution 1.0 (MIT; Copyright (c) 2021 Advanced Micro Devices), the 32-bit path of
    // ffx_fsr1.h, as in the Xbox 360 ports' menu. EASU enlarges the picture to the window, RCAS sharpens it.
    // Both read whole texels; positions come from fragTexCoord, so a flipped render texture reads the same way.
    inline const char *kEasuShader = R"(#version 330
in vec2 fragTexCoord;
in vec4 fragColor;
out vec4 finalColor;
uniform sampler2D texture0;
uniform vec2 srcSize;
float Lu(vec3 c) { return c.b * 0.5 + (c.r * 0.5 + c.g); }
vec3 Ld(ivec2 p) { return texelFetch(texture0, clamp(p, ivec2(0), ivec2(srcSize) - 1), 0).rgb; }
void Es(inout vec2 dir, inout float len, float w, float a, float b, float c, float d, float e) {
    float lx = max(abs(d - c), abs(c - b)); lx = lx > 0.0 ? 1.0 / lx : 0.0; float dx = d - b; dir.x += dx * w;
    lx = clamp(abs(dx) * lx, 0.0, 1.0); len += lx * lx * w;
    float ly = max(abs(e - c), abs(c - a)); ly = ly > 0.0 ? 1.0 / ly : 0.0; float dy = e - a; dir.y += dy * w;
    ly = clamp(abs(dy) * ly, 0.0, 1.0); len += ly * ly * w;
}
void Et(inout vec3 ac, inout float aw, vec2 o, vec2 dir, vec2 len2, float lob, float clp, vec3 c) {
    vec2 v = vec2(o.x * dir.x + o.y * dir.y, o.x * (-dir.y) + o.y * dir.x) * len2;
    float d2 = min(dot(v, v), clp);
    float wb = 0.4 * d2 - 1.0, wa = lob * d2 - 1.0;
    wb *= wb; wa *= wa; wb = 1.5625 * wb - 0.5625;
    float w = wb * wa; ac += c * w; aw += w;
}
void main() {
    vec2 pp = fragTexCoord * vec2(textureSize(texture0, 0)) - 0.5;
    vec2 fp = floor(pp); pp -= fp; ivec2 q = ivec2(fp);
    vec3 b = Ld(q + ivec2(0, -1)), c = Ld(q + ivec2(1, -1)), e = Ld(q + ivec2(-1, 0)), f = Ld(q),
         g = Ld(q + ivec2(1, 0)), h = Ld(q + ivec2(2, 0)), i = Ld(q + ivec2(-1, 1)), j = Ld(q + ivec2(0, 1)),
         k = Ld(q + ivec2(1, 1)), l = Ld(q + ivec2(2, 1)), n = Ld(q + ivec2(0, 2)), o = Ld(q + ivec2(1, 2));
    float bL = Lu(b), cL = Lu(c), eL = Lu(e), fL = Lu(f), gL = Lu(g), hL = Lu(h), iL = Lu(i), jL = Lu(j),
          kL = Lu(k), lL = Lu(l), nL = Lu(n), oL = Lu(o);
    vec2 dir = vec2(0.0); float len = 0.0;
    Es(dir, len, (1.0 - pp.x) * (1.0 - pp.y), bL, eL, fL, gL, jL);
    Es(dir, len, pp.x * (1.0 - pp.y), cL, fL, gL, hL, kL);
    Es(dir, len, (1.0 - pp.x) * pp.y, fL, iL, jL, kL, nL);
    Es(dir, len, pp.x * pp.y, gL, jL, kL, lL, oL);
    float dr = dot(dir, dir); bool z = dr < 1.0 / 32768.0;
    dr = z ? 1.0 : inversesqrt(dr); dir.x = z ? 1.0 : dir.x; dir *= dr;
    len = len * 0.5; len *= len;
    float st = dot(dir, dir) / max(abs(dir.x), abs(dir.y));
    vec2 len2 = vec2(1.0 + (st - 1.0) * len, 1.0 - 0.5 * len);
    float lob = 0.5 + ((1.0 / 4.0 - 0.04) - 0.5) * len; float clp = 1.0 / lob;
    vec3 ac = vec3(0.0); float aw = 0.0;
    Et(ac, aw, vec2(0.0, -1.0) - pp, dir, len2, lob, clp, b); Et(ac, aw, vec2(1.0, -1.0) - pp, dir, len2, lob, clp, c);
    Et(ac, aw, vec2(-1.0, 1.0) - pp, dir, len2, lob, clp, i); Et(ac, aw, vec2(0.0, 1.0) - pp, dir, len2, lob, clp, j);
    Et(ac, aw, vec2(0.0, 0.0) - pp, dir, len2, lob, clp, f); Et(ac, aw, vec2(-1.0, 0.0) - pp, dir, len2, lob, clp, e);
    Et(ac, aw, vec2(1.0, 1.0) - pp, dir, len2, lob, clp, k); Et(ac, aw, vec2(2.0, 1.0) - pp, dir, len2, lob, clp, l);
    Et(ac, aw, vec2(2.0, 0.0) - pp, dir, len2, lob, clp, h); Et(ac, aw, vec2(1.0, 0.0) - pp, dir, len2, lob, clp, g);
    Et(ac, aw, vec2(1.0, 2.0) - pp, dir, len2, lob, clp, o); Et(ac, aw, vec2(0.0, 2.0) - pp, dir, len2, lob, clp, n);
    vec3 mn = min(min(f, g), min(j, k)), mx = max(max(f, g), max(j, k));
    finalColor = vec4(min(mx, max(mn, ac / aw)), 1.0) * fragColor;
}
)";
    inline const char *kRcasShader = R"(#version 330
in vec2 fragTexCoord;
in vec4 fragColor;
out vec4 finalColor;
uniform sampler2D texture0;
uniform float sharpness;
float Lu(vec3 c) { return c.b * 0.5 + (c.r * 0.5 + c.g); }
vec3 Ld(ivec2 p) { return texelFetch(texture0, clamp(p, ivec2(0), textureSize(texture0, 0) - 1), 0).rgb; }
void main() {
    ivec2 p = ivec2(floor(fragTexCoord * vec2(textureSize(texture0, 0))));
    vec3 b = Ld(p + ivec2(0, -1)), d = Ld(p + ivec2(-1, 0)), e = Ld(p), f = Ld(p + ivec2(1, 0)), h = Ld(p + ivec2(0, 1));
    float bL = Lu(b), dL = Lu(d), eL = Lu(e), fL = Lu(f), hL = Lu(h);
    float nz = 0.25 * (bL + dL + fL + hL) - eL;
    float rg = max(max(max(bL, dL), max(eL, fL)), hL) - min(min(min(bL, dL), min(eL, fL)), hL);
    nz = clamp(abs(nz) / max(rg, 1.0 / 65536.0), 0.0, 1.0); nz = -0.5 * nz + 1.0;
    vec3 mn4 = min(min(b, d), min(f, h)), mx4 = max(max(b, d), max(f, h));
    vec3 hmin = mn4 / max(4.0 * mx4, vec3(1.0 / 65536.0)), hmax = (1.0 - mx4) / min(4.0 * mn4 - 4.0, vec3(-1.0 / 65536.0));
    vec3 lr = max(-hmin, hmax);
    float lobe = max(-(0.25 - 1.0 / 16.0), min(max(lr.r, max(lr.g, lr.b)), 0.0)) * sharpness * nz;
    finalColor = vec4((lobe * (b + d + f + h) + e) / (4.0 * lobe + 1.0), 1.0) * fragColor;
}
)";
    inline Shader easuProgram{}, rcasProgram{};
    inline RenderTexture2D fsrSource{}, fsrUpscaled{};
    inline void fitTarget(RenderTexture2D &target, int width, int height) {
        if (target.id != 0 && target.texture.width == width && target.texture.height == height) return;
        if (target.id != 0) UnloadRenderTexture(target);
        target = LoadRenderTexture(width, height);
    }
    // FSR applies when the picture is smaller than its place on the screen; a larger one goes to the other filters.
    inline bool drawFsr(const Texture2D &frame, const Rectangle &src, const Rectangle &dst) {
        const int sw = static_cast<int>(src.width), sh = static_cast<int>(src.height);
        const int dw = static_cast<int>(std::lround(dst.width)), dh = static_cast<int>(std::lround(dst.height));
        if (filter != 2 || sw < 16 || sh < 16 || (dw <= sw && dh <= sh)) return false;
        if (easuProgram.id == 0) easuProgram = LoadShaderFromMemory(nullptr, kEasuShader);
        if (rcasProgram.id == 0) rcasProgram = LoadShaderFromMemory(nullptr, kRcasShader);
        if (!IsShaderValid(easuProgram) || !IsShaderValid(rcasProgram)) return false;
        Texture2D input = frame;
        Rectangle inputRect = src;
        if (fxaa >= 1 && fxaa <= 3) {
            // FXAA first, at the picture's own size, as the 360 menu does.
            fitTarget(fsrSource, sw, sh);
            BeginTextureMode(fsrSource);
            const bool shaded = beginFxaa(static_cast<float>(frame.width), static_cast<float>(frame.height));
            DrawTexturePro(frame, src, {0.0f, 0.0f, static_cast<float>(sw), static_cast<float>(sh)}, {0.0f, 0.0f}, 0.0f, WHITE);
            if (shaded) EndShaderMode();
            EndTextureMode();
            input = fsrSource.texture;
            inputRect = {0.0f, 0.0f, static_cast<float>(sw), -static_cast<float>(sh)};
        }
        fitTarget(fsrUpscaled, dw, dh);
        if (fsrUpscaled.id == 0) return false;
        BeginTextureMode(fsrUpscaled);
        BeginShaderMode(easuProgram);
        const float size[2] = {static_cast<float>(sw), static_cast<float>(sh)};
        SetShaderValue(easuProgram, GetShaderLocation(easuProgram, "srcSize"), size, SHADER_UNIFORM_VEC2);
        DrawTexturePro(input, inputRect, {0.0f, 0.0f, static_cast<float>(dw), static_cast<float>(dh)}, {0.0f, 0.0f}, 0.0f, WHITE);
        EndShaderMode();
        EndTextureMode();
        BeginShaderMode(rcasProgram);
        const float sharpness = 0.8705506f;  // 0.2 stops, AMD's default, as a linear amount: exp2(-0.2)
        SetShaderValue(rcasProgram, GetShaderLocation(rcasProgram, "sharpness"), &sharpness, SHADER_UNIFORM_FLOAT);
        DrawTexturePro(fsrUpscaled.texture, {0.0f, 0.0f, static_cast<float>(dw), -static_cast<float>(dh)},
                       {std::round(dst.x), std::round(dst.y), static_cast<float>(dw), static_cast<float>(dh)}, {0.0f, 0.0f}, 0.0f, WHITE);
        EndShaderMode();
        return true;
    }
    // The game picture with the chosen upscaling and anti-aliasing.
    inline void drawPicture(const Texture2D &frame, const Rectangle &src, const Rectangle &dst) {
        if (drawFsr(frame, src, dst)) return;
        const bool shaded = beginFxaa(static_cast<float>(frame.width), static_cast<float>(frame.height)) ||
                            beginDownscale(static_cast<float>(frame.width), static_cast<float>(frame.height), src.height / dst.height);
        DrawTexturePro(frame, src, dst, {0.0f, 0.0f}, 0.0f, WHITE);
        if (shaded) EndShaderMode();
    }

    struct Item {
        std::string label, value;
        int id;
        float slider = -1.0f;  // 0 to 1, drawn as a bar
        std::string help;
        bool greyed = false;   // shown but without effect in the current mode
    };
    enum { kDisplay, kMonitor, kRefresh, kVsync, kFpsLimit, kResolution, kFilter, kAspect, kFxaa, kGpu, kFps, kVibration,
           kLayout, kMouseLook, kMouseSpeed, kMouseInvert, kMenuLanguage, kVolume, kResume, kRestart, kQuit };
    constexpr int kMinSensitivity = 10, kMaxSensitivity = 300;
    constexpr float kSliderOffset = 330.0f, kSliderWidth = 220.0f;  // from the right edge of the rows
    constexpr int kVisibleRows = 10;
    constexpr float kRowHeight = 44.0f, kHelpHeight = 84.0f, kHelpSize = 18.0f;

    // Exclusive fullscreen sizes the monitor to the window: the rates offered at that size (else at the desktop's).
    inline std::vector<int> refreshRates() {
        int width = 1920, height = 1080;
        if (host_video::mode == host_video::Mode::Fullscreen) width = GetScreenWidth(), height = GetScreenHeight();
        else if (host_video::resolutionWidth > 0) host_video::windowSize(width, height);
        const int monitor = GetCurrentMonitor();
        std::vector<int> rates = host_os::refreshRates(monitor, width, height);
        if (rates.empty()) rates = host_os::refreshRates(monitor, GetMonitorWidth(monitor), GetMonitorHeight(monitor));
        return rates;
    }
    inline bool restartPending() { return renderScale() != startScale || gpuPreference != bootGpuPreference; }
    inline const std::vector<int> &frameLimits() {
        static const std::vector<int> list = [] {
            std::vector<int> values{30, 60, -1, 120, 144, 240, 0};
            if (const char *launcher = std::getenv("PS2X_FPS_LIMIT")) {
                const int value = std::max(0, std::atoi(launcher));
                if (std::find(values.begin(), values.end(), value) == values.end()) values.insert(values.end() - 1, value);
            }
            return values;
        }();
        return list;
    }
    inline int cycle(const std::vector<int> &list, int current, int direction) {
        const auto found = std::find(list.begin(), list.end(), current);
        if (found == list.end()) return list.front();
        const int count = static_cast<int>(list.size());
        return list[(static_cast<int>(found - list.begin()) + count + direction) % count];
    }

    inline std::vector<Item> items() {
        std::vector<Item> list;
        const std::string restart = renderScale() != startScale ? T(" - ao reiniciar", " - after restart") : "";
        const char *modes[] = {T("Janela", "Window"), T("Tela cheia sem bordas", "Borderless fullscreen"), T("Tela cheia", "Fullscreen")};
        const int mode = host_video::mode == host_video::Mode::Windowed ? 0 : host_video::mode == host_video::Mode::Borderless ? 1 : 2;
        list.push_back({T("Modo de exibição", "Display mode"), modes[mode], kDisplay, -1.0f,
                        T("Janela, tela cheia sem bordas (troca rápida com Alt+Tab) ou tela cheia exclusiva.",
                          "Windowed, borderless fullscreen (quick Alt+Tab) or exclusive fullscreen.")});
        if (GetMonitorCount() > 1) {
            const int monitor = GetCurrentMonitor();
            list.push_back({"Monitor", std::to_string(monitor + 1) + " (" + std::to_string(GetMonitorWidth(monitor)) + "x" +
                                           std::to_string(GetMonitorHeight(monitor)) + ")", kMonitor, -1.0f,
                            T("Em qual monitor o jogo aparece.", "Monitor used to display the game.")});
        }
        {
            const bool exclusive = host_video::mode == host_video::Mode::Fullscreen;
            std::string value;
            if (host_video::refreshRate > 0) value = std::to_string(host_video::refreshRate) + " Hz";
            else {
                const std::vector<int> rates = refreshRates();
                value = std::string(T("Máxima", "Highest")) +
                        (rates.empty() ? std::string() : " (" + std::to_string(rates.back()) + " Hz)");
            }
            list.push_back({T("Taxa de atualização", "Refresh rate"), value, kRefresh, -1.0f,
                            exclusive ? T("Frequência do monitor na tela cheia exclusiva.", "Monitor refresh rate in exclusive fullscreen.")
                                      : T("Frequência do monitor na tela cheia exclusiva. Só vale no modo Tela cheia.",
                                          "Monitor refresh rate in exclusive fullscreen. Applies only in Fullscreen mode."),
                            !exclusive});
        }
        list.push_back({T("Sincronização vertical", "Vertical sync"), vsync ? T("Ligada", "On") : T("Desligada", "Off"), kVsync, -1.0f,
                        T("Espera o monitor para mostrar cada quadro: sem cortes na imagem, um pouco mais de atraso.",
                          "Synchronizes frames with the monitor to prevent tearing, with slightly more latency.")});
        {
            std::string value;
            if (fpsLimit == 60) value = T("60 fps (original)", "60 fps (original)");
            else if (fpsLimit < 0) value = std::string(T("A do monitor", "Monitor rate")) + " (" + std::to_string(monitorRate()) + " fps)";
            else if (fpsLimit == 0) value = T("Sem limite", "Unlimited");
            else value = std::to_string(fpsLimit) + " fps";
            list.push_back({T("Limite de quadros", "Frame limit"), value, kFpsLimit, -1.0f,
                            T("O jogo anda a 60 quadros como no console; um limite maior mostra cada quadro mais cedo em telas "
                              "de 120/144 Hz. 30 economiza energia, mas o movimento fica menos suave.",
                              "The game runs at 60 frames as on the console; a higher limit shows each frame sooner on "
                              "120/144 Hz screens. 30 saves power, but motion is less smooth.")});
        }
        list.push_back({T("Resolução", "Resolution"), host_video::resolutionWidth > 0
                            ? resolutionName(host_video::resolutionWidth, host_video::resolutionHeight) + restart
                            : std::string(T("Automática", "Automatic")) + restart, kResolution, -1.0f,
                        T("Resolução em que o jogo é desenhado: maior = imagem mais nítida, mais trabalho para a placa. "
                          "Automática segue o monitor.",
                          "Higher rendering resolutions give a sharper image and increase GPU load. Automatic follows the monitor.")});
        const char *filters[] = {T("Nítido (pixels)", "Sharp (pixels)"), T("Suave (bilinear)", "Smooth (bilinear)"), "AMD FSR 1.0"};
        list.push_back({T("Ampliação da imagem", "Upscaling"), filters[filter], kFilter, -1.0f,
                        T("Como a imagem é ampliada quando é menor que a tela. AMD FSR 1.0 reconstrói as bordas e aumenta "
                          "a nitidez; Nítido mostra os pixels do PS2.",
                          "How the picture is enlarged when it is smaller than the screen. AMD FSR 1.0 rebuilds edges and "
                          "sharpens; Sharp shows the PS2's pixels.")});
        list.push_back({T("Proporção", "Aspect ratio"), host_video::stretch ? T("Esticada", "Stretched") : T("4:3 (original)", "4:3 (original)"), kAspect, -1.0f,
                        T("O jogo é 4:3. Em telas largas, com barras ou esticado.", "The game is 4:3. On wide screens: with bars or stretched.")});
        const char *aaLevels[] = {T("Desligada", "Off"), "FXAA 1", "FXAA 2", "FXAA 3", "SSAA 2x", "SSAA 4x", "SSAA 8x", "SSAA 16x"};
        list.push_back({T("Suavização", "Anti-aliasing"), std::string(aaLevels[fxaa]) + (fxaa > 3 ? restart : ""), kFxaa, -1.0f,
                        T("FXAA suaviza as bordas serrilhadas com custo quase nulo. SSAA desenha o jogo maior e reduz: "
                          "mais nítido, muito mais trabalho para a placa.",
                          "FXAA smooths jagged edges at almost no cost. SSAA draws the game larger and scales it down: "
                          "sharper, much more work for the graphics card.")});
        if (host_os::hasGpuPreference()) {
            const char *preferences[] = {T("Padrão do Windows", "Windows default"), T("Economia de energia", "Power saving"),
                                         T("Alto desempenho", "High performance")};
            const std::string help = std::string(T("Placa de vídeo usada para desenhar o jogo (preferência gráfica do Windows). Em uso: ",
                                 "Graphics card used to render the game (Windows graphics preference). In use: ")) +
                   (gpuName.empty() ? "?" : gpuName) + ".";
            list.push_back({T("Placa de vídeo", "Graphics card"),
                            std::string(preferences[gpuPreference]) + (gpuPreference != bootGpuPreference ? T(" - ao reiniciar", " - after restart") : ""),
                            kGpu, -1.0f, help});
        }
        list.push_back({T("Mostrar FPS", "Show FPS"), showFps ? T("Sim", "Yes") : T("Não", "No"), kFps, -1.0f,
                        T("Mostra os quadros por segundo no canto da tela.", "Shows frames per second in the corner of the screen.")});
        list.push_back({T("Vibração do controle", "Controller vibration"), vibration ? T("Ligada", "On") : T("Desligada", "Off"), kVibration, -1.0f,
                        T("Liga ou desliga a vibração do controle.", "Turns the controller's vibration on or off.")});
        if (hasModernLayout)
            list.push_back({T("Layout do controle", "Controller layout"),
                            modernLayout ? T("Moderno (LT mira, RT atira)", "Modern (LT aim, RT fire)") : T("Original do PS2", "Original PS2"), kLayout, -1.0f,
                            T("Moderno: os gatilhos miram e atiram, como nos jogos de hoje. Original: os botões do PS2.",
                              "Modern: the triggers aim and fire, as in today's games. Original: the PS2 buttons.")});
        list.push_back({T("Mouse como câmera", "Mouse look"), mouseLook ? T("Ligado", "On") : T("Desligado", "Off"), kMouseLook, -1.0f,
                        T("O mouse move a câmera, como o analógico direito.", "The mouse moves the camera, like the right stick.")});
        if (!gameHasLookOptions) {
            list.push_back({T("Sensibilidade do mouse", "Mouse sensitivity"), std::to_string(mouseSensitivity) + "%", kMouseSpeed,
                            static_cast<float>(mouseSensitivity - kMinSensitivity) / (kMaxSensitivity - kMinSensitivity),
                            T("Velocidade da câmera com o mouse.", "Camera speed with the mouse.")});
            list.push_back({T("Inverter mouse", "Invert mouse"), mouseInvert ? T("Sim", "Yes") : T("Não", "No"), kMouseInvert, -1.0f,
                            T("Inverte o movimento vertical do mouse.", "Inverts the mouse's vertical movement.")});
        }
        {
            const char *languages[] = {"Português", "English"};
            const std::string value = uiLanguage < 0 ? std::string(T("Automático", "Automatic")) + " (" + languages[systemPortuguese ? 0 : 1] + ")"
                                                     : languages[uiLanguage];
            list.push_back({T("Idioma do menu", "Menu language"), value, kMenuLanguage, -1.0f,
                            T("Idioma deste menu. Automático segue o idioma do Windows. A mudança é imediata.",
                              "Language of this settings menu. Automatic follows Windows. Changes apply immediately.")});
        }
        list.push_back({T("Volume", "Volume"), std::to_string(volume) + "%", kVolume, -1.0f, T("Volume geral do jogo.", "Overall game volume.")});
        list.push_back({T("Voltar ao jogo", "Back to game"), "", kResume, -1.0f,
                        T("Salva e volta para onde você estava.", "Saves settings and returns to the game.")});
        if (restartPending() && host_os::canRestart())
            list.push_back({T("Reiniciar o jogo para aplicar", "Restart game to apply"), "", kRestart, -1.0f,
                            T("Fecha e abre o jogo de novo com as opções marcadas \"ao reiniciar\". O progresso não salvo se perde.",
                              "Closes and reopens the game with the settings marked \"after restart\". Unsaved progress is lost.")});
        list.push_back({T("Sair do jogo", "Quit game"), "", kQuit, -1.0f,
                        T("Salva as configurações e fecha o jogo. O progresso não salvo se perde.",
                          "Saves settings and quits the game. Unsaved progress is lost.")});
        return list;
    }
    inline bool isAction(int id) { return id == kResume || id == kRestart || id == kQuit; }

    inline bool mouseArmed = false;
    inline void close() {
        open = false;
        mouseArmed = false;
        suppressPad = true;
        save();
    }

    inline void restartGame() {
        save();
        if (scaleFromMenu) setScaleVariable(renderScale());
        host_os::restartGame();  // returns only when the new process could not start
    }
    inline void quitGame() {
        save();
        host_os::quitGame();
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
            applyFrameLimit();
            break;
        }
        case kRefresh: {
            std::vector<int> list{0};
            for (int rate : refreshRates()) list.push_back(rate);
            host_video::refreshRate = cycle(list, host_video::refreshRate, direction);
            if (host_video::mode == host_video::Mode::Fullscreen)
                host_os::setFullscreenRefresh(GetWindowHandle(), GetScreenWidth(), GetScreenHeight(), host_video::refreshRate);
            break;
        }
        case kResolution: {
            const auto list = resolutions();
            size_t index = 0;
            for (size_t i = 0; i < list.size(); ++i)
                if (list[i].first == host_video::resolutionWidth && list[i].second == host_video::resolutionHeight) index = i;
            const auto &next = list[(index + list.size() + direction) % list.size()];
            host_video::resolutionWidth = next.first;
            host_video::resolutionHeight = next.second;
            host_video::apply(host_video::mode, false);
            break;
        }
        case kFxaa: fxaa = (fxaa + direction + 8) % 8; break;
        case kAspect: host_video::stretch = !host_video::stretch; break;
        case kFilter: filter = (filter + direction + 3) % 3; break;
        case kVsync: vsync = !vsync; applyRuntime(); break;
        case kFpsLimit: fpsLimit = cycle(frameLimits(), fpsLimit, direction); applyFrameLimit(); break;
        case kGpu:
            gpuPreference = (gpuPreference + direction + 3) % 3;
            host_os::setGpuPreference(gpuPreference);
            break;
        case kVolume: volume = std::clamp(volume + direction * 10, 0, 100); applyRuntime(); break;
        case kFps: showFps = !showFps; break;
        case kVibration: vibration = !vibration; break;
        case kLayout: modernLayout = !modernLayout; break;
        case kMouseLook: mouseLook = !mouseLook; break;
        case kMouseSpeed: mouseSensitivity = std::clamp(mouseSensitivity + direction * 10, kMinSensitivity, kMaxSensitivity); break;
        case kMouseInvert: mouseInvert = !mouseInvert; break;
        case kMenuLanguage: uiLanguage = (uiLanguage + 1 + direction + 3) % 3 - 1; applyLanguage(); break;
        case kResume: close(); return;
        case kRestart: restartGame(); return;
        case kQuit: quitGame();
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

    // Layout on a 1280x720 canvas, scaled to the window. A list longer than kVisibleRows scrolls.
    struct Layout { float scale, x0, y0, rowH, top, left, right, height; int visible; };
    inline Layout layout(size_t rows) {
        Layout l{};
        l.scale = std::min(GetScreenWidth() / 1280.0f, GetScreenHeight() / 720.0f);
        l.x0 = (GetScreenWidth() - 1280.0f * l.scale) * 0.5f;
        l.y0 = (GetScreenHeight() - 720.0f * l.scale) * 0.5f;
        l.visible = std::min(static_cast<int>(rows), kVisibleRows);
        l.rowH = kRowHeight;
        l.height = 110.0f + l.visible * l.rowH + kHelpHeight + 40.0f;
        l.top = (720.0f - l.height) * 0.5f;
        l.left = theme->band ? 230.0f : 300.0f;
        l.right = theme->band ? 1050.0f : 980.0f;
        return l;
    }
    inline void keepSelectionVisible(int count) {
        const int visible = std::min(count, kVisibleRows);
        if (selected < scrollTop) scrollTop = selected;
        if (selected >= scrollTop + visible) scrollTop = selected - visible + 1;
        scrollTop = std::clamp(scrollTop, 0, std::max(0, count - visible));
    }

    inline std::string shown(const std::string &value) { return theme->upper ? upper(value) : value; }
    // Width of a text on the canvas (1280x720 units).
    inline float textWidth(const std::string &value, float size, const Layout &l) {
        return MeasureTextEx(font, shown(value).c_str(), size * l.scale, theme->spacing * l.scale).x / l.scale;
    }

    // Keyboard, gamepad and mouse; runs every host frame before the pad is read for the game.
    // PS2X_PC_MENU_TEST=<seconds>: opens the menu after that time, selects row 2, and saves pc-menu-test.png
    // in the working directory one second later (visual check without a keyboard).
    inline void testStep() {
        static const char *env = std::getenv("PS2X_PC_MENU_TEST");
        static int stage = 0;
        if (!env || stage > 1) return;
        const double at = std::atof(env);
        if (stage == 0 && GetTime() >= at) { open = true; selected = 2; scrollTop = 0; stage = 1; }
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
            if ((IsKeyPressed(KEY_F1) && !shift) || padCombo) { open = true; selected = 0; scrollTop = 0; }
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
        const float wheel = GetMouseWheelMove();
        if (IsKeyPressed(KEY_UP) || IsKeyPressed(KEY_W) || padPressed(GAMEPAD_BUTTON_LEFT_FACE_UP) || stickStep < 0 || wheel > 0.0f)
            selected = (selected + count - 1) % count;
        if (IsKeyPressed(KEY_DOWN) || IsKeyPressed(KEY_S) || padPressed(GAMEPAD_BUTTON_LEFT_FACE_DOWN) || stickStep > 0 || wheel < 0.0f)
            selected = (selected + 1) % count;
        if (IsKeyPressed(KEY_HOME) || IsKeyPressed(KEY_PAGE_UP)) selected = 0;
        if (IsKeyPressed(KEY_END) || IsKeyPressed(KEY_PAGE_DOWN)) selected = count - 1;
        selected = std::clamp(selected, 0, count - 1);
        keepSelectionVisible(count);
        const int id = list[selected].id;
        if (IsKeyPressed(KEY_LEFT) || IsKeyPressed(KEY_A) || padPressed(GAMEPAD_BUTTON_LEFT_FACE_LEFT) || stickSide < 0) {
            if (!isAction(id)) change(id, -1);
            return;
        } else if (IsKeyPressed(KEY_RIGHT) || IsKeyPressed(KEY_D) || padPressed(GAMEPAD_BUTTON_LEFT_FACE_RIGHT) || stickSide > 0) {
            if (!isAction(id)) change(id, 1);
            return;
        } else if (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_SPACE) || IsKeyPressed(KEY_X) ||
                   padPressed(GAMEPAD_BUTTON_RIGHT_FACE_DOWN)) {
            change(id, 1);
            return;
        }
        // Mouse: hover selects, left click advances the value (or goes back on the "‹"), right click goes back.
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
        } else if (mx >= l.left - 30.0f && mx <= l.right + 30.0f && my >= rowsTop && my < rowsTop + l.visible * l.rowH) {
            const int row = std::min(count - 1, scrollTop + static_cast<int>((my - rowsTop) / l.rowH));
            if (moved) selected = row;
            const Item &item = list[row];
            const bool onSlider = item.slider >= 0.0f && mx >= sliderLeft - 12.0f && mx <= sliderLeft + kSliderWidth + 12.0f;
            const float valueLeft = l.right - textWidth("\xE2\x80\xB9  " + item.value + "  \xE2\x80\xBA", theme->textSize, l);
            const bool onBackArrow = !isAction(item.id) && item.slider < 0.0f && mx >= valueLeft - 12.0f && mx <= valueLeft + 24.0f;
            if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                if (onSlider) { dragging = true; dragSlider(); }
                else change(item.id, onBackArrow ? -1 : 1);
            } else if (IsMouseButtonPressed(MOUSE_BUTTON_RIGHT) && !isAction(item.id)) change(item.id, -1);
        }
    }

    inline void text(const std::string &value, float x, float y, float size, Color color, const Layout &l, int align = 0) {
        const std::string s = shown(value);
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
    // Splits a help text into lines that fit `width` canvas units.
    inline std::vector<std::string> wrap(const std::string &value, float size, float width, const Layout &l) {
        std::vector<std::string> lines;
        std::string line, word;
        std::istringstream words(value);
        while (words >> word) {
            const std::string candidate = line.empty() ? word : line + " " + word;
            if (!line.empty() && textWidth(candidate, size, l) > width) {
                lines.push_back(line);
                line = word;
            } else line = candidate;
        }
        if (!line.empty()) lines.push_back(line);
        return lines;
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
        const int count = static_cast<int>(list.size());
        selected = std::clamp(selected, 0, count - 1);
        keepSelectionVisible(count);
        DrawRectangle(0, 0, GetScreenWidth(), GetScreenHeight(), theme->dim);
        if (theme->band) {
            // Full-width band with thin rules, like the game's main menu strip.
            const Rectangle band{0.0f, l.y0 + l.top * l.scale, static_cast<float>(GetScreenWidth()), l.height * l.scale};
            DrawRectangleRec(band, theme->panel);
            DrawRectangleRec({0.0f, band.y, band.width, std::max(1.0f, 2.0f * l.scale)}, theme->rule);
            DrawRectangleRec({0.0f, band.y + band.height - std::max(1.0f, 2.0f * l.scale), band.width, std::max(1.0f, 2.0f * l.scale)}, theme->rule);
            DrawRectangleRec(rect(l, l.left - 30.0f, l.top + 84.0f, l.right - l.left + 60.0f, 1.0f), theme->rule);
        } else {
            const Rectangle panel = rect(l, l.left - 50.0f, l.top, l.right - l.left + 100.0f, l.height);
            DrawRectangleRounded(panel, theme->roundness, 8, theme->panel);
            DrawRectangleRoundedLinesEx(panel, theme->roundness, 8, std::max(1.0f, 3.0f * l.scale), theme->rule);
        }
        text(T("Configurações do PC", "PC Settings"), theme->band ? l.left : 640.0f, l.top + 50.0f, theme->titleSize,
             theme->titleText, l, theme->band ? 0 : 2);
        const float rowsTop = l.top + 110.0f;
        for (int row = 0; row < l.visible; ++row) {
            const int i = scrollTop + row;
            if (i >= count) break;
            const Item &item = list[i];
            const bool sel = i == selected;
            const float y = rowsTop + row * l.rowH;
            if (sel) {
                const Rectangle bar = rect(l, l.left - 30.0f, y + 4.0f, l.right - l.left + 60.0f, l.rowH - 8.0f);
                if (theme->roundness > 0.0f) DrawRectangleRounded(bar, 0.4f, 8, theme->selBar);
                else DrawRectangleRec(bar, theme->selBar);
            }
            Color color = sel ? theme->selText : (isAction(item.id) ? theme->muted : theme->text);
            if (item.greyed && !sel) color = theme->muted;
            const float cy = y + l.rowH * 0.5f;
            if (isAction(item.id)) {
                text(item.label, theme->band ? l.left : 640.0f, cy, theme->textSize, color, l, theme->band ? 0 : 2);
                continue;
            }
            text(item.label, l.left, cy, theme->textSize, color, l);
            if (item.slider >= 0.0f) {
                const float left = l.right - kSliderOffset;
                DrawRectangleRounded(rect(l, left, cy - 3.0f, kSliderWidth, 6.0f), 1.0f, 6, Fade(color, 0.35f));
                DrawRectangleRounded(rect(l, left, cy - 3.0f, kSliderWidth * item.slider, 6.0f), 1.0f, 6, color);
                DrawCircleV({l.x0 + (left + kSliderWidth * item.slider) * l.scale, l.y0 + cy * l.scale}, 9.0f * l.scale, color);
                text(item.value, l.right, cy, theme->textSize, color, l, 1);
                continue;
            }
            // A long value gets a smaller font instead of running into the label.
            const std::string value = sel ? "\xE2\x80\xB9  " + item.value + "  \xE2\x80\xBA" : item.value;
            const float room = l.right - l.left - textWidth(item.label, theme->textSize, l) - 30.0f;
            const float width = textWidth(value, theme->textSize, l);
            const float size = width > room ? std::max(theme->textSize * 0.6f, theme->textSize * room / width) : theme->textSize;
            text(value, l.right, cy, size, color, l, 1);
        }
        // Arrows on the right edge when rows are hidden above or below.
        const float ax = l.right + 22.0f, half = 8.0f;
        if (scrollTop > 0) {
            const float ay = rowsTop - 4.0f;
            DrawTriangle({l.x0 + ax * l.scale, l.y0 + (ay - half) * l.scale}, {l.x0 + (ax - half) * l.scale, l.y0 + ay * l.scale},
                         {l.x0 + (ax + half) * l.scale, l.y0 + ay * l.scale}, theme->muted);
        }
        if (scrollTop + l.visible < count) {
            const float ay = rowsTop + l.visible * l.rowH + 4.0f;
            DrawTriangle({l.x0 + (ax - half) * l.scale, l.y0 + ay * l.scale}, {l.x0 + ax * l.scale, l.y0 + (ay + half) * l.scale},
                         {l.x0 + (ax + half) * l.scale, l.y0 + ay * l.scale}, theme->muted);
        }
        // Help for the selected row.
        const float helpTop = rowsTop + l.visible * l.rowH + 18.0f;
        DrawRectangleRec(rect(l, l.left - 30.0f, helpTop - 6.0f, l.right - l.left + 60.0f, 1.0f), Fade(theme->rule, 0.5f));
        const std::vector<std::string> lines = wrap(list[selected].help, kHelpSize, l.right - l.left + 40.0f, l);
        for (size_t i = 0; i < lines.size() && i < 3; ++i)
            text(lines[i], 640.0f, helpTop + 14.0f + i * 22.0f, kHelpSize, theme->text, l, 2);
        text(T("Setas: escolher e mudar   Enter: mudar   F1 / Esc: voltar ao jogo",
               "Arrows: choose and change   Enter: change   F1 / Esc: back to game"),
             640.0f, l.top + l.height - 22.0f, 17.0f, theme->muted, l, 2);
    }
}
