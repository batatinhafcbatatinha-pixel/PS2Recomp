#define main Boku2SharedMain
#include PS2X_SHARED_MAIN_CPP // the PS2Recomp frontend (path set by project/CMakeLists.txt)
#undef main
#include "recovered/Boku2BootMain_0x20a920.cpp"
struct Boku2TableEntry { uint32_t address; PS2Runtime::RecompiledFunction function; };
#include "sdk-handlers.inc"
#include "gap-functions.inc"
extern const uint32_t g_ps2RecompiledFunctionTableBase;
extern const uint32_t g_ps2RecompiledFunctionTableSlotCount;
extern PS2Runtime::RecompiledFunction g_ps2RecompiledFunctionTable[];
std::string ps2xDispatchTrail();
static PS2Runtime::RecompiledFunction g_probeOriginal = nullptr;
static void Boku2Probe(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime) {
    static int n = 0;
    static const char* raFilter = std::getenv("BOKU2_PROBE_RA");
    if (raFilter && getRegU32(ctx, 31) != std::strtoul(raFilter, nullptr, 16)) { g_probeOriginal(rdram, ctx, runtime); return; }
    ++n;
    const bool show = n <= 6 || (n & (n - 1)) == 0;
    if (show) std::cerr << "[probe] pc=0x" << std::hex << ctx->pc << " ra=0x" << getRegU32(ctx, 31) << " a0=0x" << getRegU32(ctx, 4) << " a1=0x" << getRegU32(ctx, 5) << " a2=0x" << getRegU32(ctx, 6) << " sp=0x" << getRegU32(ctx, 29) << std::dec << " trail=" << ps2xDispatchTrail() << std::endl;
    if (const char* peek = std::getenv("BOKU2_PEEK"); show && peek) { const uint32_t a = std::strtoul(peek, nullptr, 16) & 0x1FFFFFCu; std::cerr << "[probe] peek 0x" << std::hex << a; for (int i = 0; i < 8; ++i) std::cerr << ' ' << *reinterpret_cast<const uint32_t*>(rdram + a + i * 4); std::cerr << std::dec << std::endl; }
    g_probeOriginal(rdram, ctx, runtime);
}
int main(int argc, char** argv) {
    if ((0x20a920u-g_ps2RecompiledFunctionTableBase)/4 >= g_ps2RecompiledFunctionTableSlotCount || g_ps2RecompiledFunctionTable[(0x20a920u-g_ps2RecompiledFunctionTableBase)/4]) return 2;
    g_ps2RecompiledFunctionTable[(0x20a920u-g_ps2RecompiledFunctionTableBase)/4] = Boku2BootMain_0x20a920;
    if ((0x20a93cu-g_ps2RecompiledFunctionTableBase)/4 >= g_ps2RecompiledFunctionTableSlotCount || g_ps2RecompiledFunctionTable[(0x20a93cu-g_ps2RecompiledFunctionTableBase)/4]) return 2;
    g_ps2RecompiledFunctionTable[(0x20a93cu-g_ps2RecompiledFunctionTableBase)/4] = Boku2BootMain_0x20a920;
    if ((0x20a948u-g_ps2RecompiledFunctionTableBase)/4 >= g_ps2RecompiledFunctionTableSlotCount || g_ps2RecompiledFunctionTable[(0x20a948u-g_ps2RecompiledFunctionTableBase)/4]) return 2;
    g_ps2RecompiledFunctionTable[(0x20a948u-g_ps2RecompiledFunctionTableBase)/4] = Boku2BootMain_0x20a920;
    if ((0x20a950u-g_ps2RecompiledFunctionTableBase)/4 >= g_ps2RecompiledFunctionTableSlotCount || g_ps2RecompiledFunctionTable[(0x20a950u-g_ps2RecompiledFunctionTableBase)/4]) return 2;
    g_ps2RecompiledFunctionTable[(0x20a950u-g_ps2RecompiledFunctionTableBase)/4] = Boku2BootMain_0x20a920;
    if ((0x20a958u-g_ps2RecompiledFunctionTableBase)/4 >= g_ps2RecompiledFunctionTableSlotCount || g_ps2RecompiledFunctionTable[(0x20a958u-g_ps2RecompiledFunctionTableBase)/4]) return 2;
    g_ps2RecompiledFunctionTable[(0x20a958u-g_ps2RecompiledFunctionTableBase)/4] = Boku2BootMain_0x20a920;
    if ((0x20a960u-g_ps2RecompiledFunctionTableBase)/4 >= g_ps2RecompiledFunctionTableSlotCount || g_ps2RecompiledFunctionTable[(0x20a960u-g_ps2RecompiledFunctionTableBase)/4]) return 2;
    g_ps2RecompiledFunctionTable[(0x20a960u-g_ps2RecompiledFunctionTableBase)/4] = Boku2BootMain_0x20a920;
    if ((0x20a970u-g_ps2RecompiledFunctionTableBase)/4 >= g_ps2RecompiledFunctionTableSlotCount || g_ps2RecompiledFunctionTable[(0x20a970u-g_ps2RecompiledFunctionTableBase)/4]) return 2;
    g_ps2RecompiledFunctionTable[(0x20a970u-g_ps2RecompiledFunctionTableBase)/4] = Boku2BootMain_0x20a920;
    if ((0x20a97cu-g_ps2RecompiledFunctionTableBase)/4 >= g_ps2RecompiledFunctionTableSlotCount || g_ps2RecompiledFunctionTable[(0x20a97cu-g_ps2RecompiledFunctionTableBase)/4]) return 2;
    g_ps2RecompiledFunctionTable[(0x20a97cu-g_ps2RecompiledFunctionTableBase)/4] = Boku2BootMain_0x20a920;
    if ((0x20a984u-g_ps2RecompiledFunctionTableBase)/4 >= g_ps2RecompiledFunctionTableSlotCount || g_ps2RecompiledFunctionTable[(0x20a984u-g_ps2RecompiledFunctionTableBase)/4]) return 2;
    g_ps2RecompiledFunctionTable[(0x20a984u-g_ps2RecompiledFunctionTableBase)/4] = Boku2BootMain_0x20a920;
    for (const auto& entry : kBoku2GapFunctions) {
        const uint32_t slot = (entry.address - g_ps2RecompiledFunctionTableBase) / 4;
        if (entry.address >= g_ps2RecompiledFunctionTableBase && slot < g_ps2RecompiledFunctionTableSlotCount &&
            !g_ps2RecompiledFunctionTable[slot])
            g_ps2RecompiledFunctionTable[slot] = entry.function;
    }
    // SDK library matches (scripts/gen_sdk_handlers.py) run as the runtime's native IOP-side services
    // (SIF RPC, CD, pad, memory card, ...) instead of translated guest code waiting on an absent IOP.
    if (!std::getenv("BOKU2_GUEST_SDK")) {
        for (const auto& entry : kBoku2SdkHandlers) {
            const uint32_t slot = (entry.address - g_ps2RecompiledFunctionTableBase) / 4;
            if (entry.address < g_ps2RecompiledFunctionTableBase || slot >= g_ps2RecompiledFunctionTableSlotCount) return 2;
            g_ps2RecompiledFunctionTable[slot] = entry.function;
        }
    }
    if (const char* probe = std::getenv("BOKU2_PROBE")) {
        const uint32_t slot = (std::strtoul(probe, nullptr, 16) - g_ps2RecompiledFunctionTableBase) / 4;
        g_probeOriginal = g_ps2RecompiledFunctionTable[slot];
        g_ps2RecompiledFunctionTable[slot] = Boku2Probe;
    }
    return Boku2SharedMain(argc, argv);
}
