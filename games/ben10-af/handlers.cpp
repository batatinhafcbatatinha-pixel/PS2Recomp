// Ben 10: Alien Force: what is still specific to this game in the generic pipeline (tools/ps2port, port.toml).
// Included by tools/ps2port/project/main.cpp after the table helpers (PortSlot).
//
// The engine has a 30 and a 60 fps mode, chosen by field +8 of its settings object (0x473A30): the method at 0x116C08
// turns it into the frame period and the ticks per frame at 0x4DAC98, and the main loop waits one VBlank instead of two
// (0x1D9CD8). The disc ships mode 0; mode 1 keeps game speed right at 60 fps. BEN10_30FPS=1 keeps the original mode.
// route: {0x116c08u, Ben10SetFrameRate}
static PS2Runtime::RecompiledFunction g_ben10SetFrameRate = nullptr;

static void Ben10SetFrameRate(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime) {
    const uint32_t settings = getRegU32(ctx, 4);
    *reinterpret_cast<uint32_t*>(rdram + ((settings + 8u) & 0x1FFFFFFu)) = 1u;
    g_ben10SetFrameRate(rdram, ctx, runtime);
}

static void PortProfileInstall() {
    if (std::getenv("BEN10_30FPS")) return;
    if (auto* slot = PortSlot(0x116c08u); slot && *slot) {
        g_ben10SetFrameRate = *slot;
        *slot = Ben10SetFrameRate;
    }
}
