// Entry of a port built by tools/ps2port: installs the port's tables into the translated function table, then runs
// the shared PS2Recomp frontend. The tables are generated (build/port-gen): functions outside the Ghidra map
// (gap-functions.inc), SDK functions served by the runtime's native handlers (sdk-handlers.inc) and the default
// environment of this game (port-env.inc, from port.toml [env]). A profile may add handlers (PORT_PROFILE_HANDLERS).
#define main PortSharedMain
#include PS2X_SHARED_MAIN_CPP
#undef main
#include "ps2_syscalls.h"
#include "ps2_stubs.h"
#include <cstdlib>

struct PortTableEntry { uint32_t address; PS2Runtime::RecompiledFunction function; };
#include "gap-functions.inc"
#include "sdk-handlers.inc"
#include "port-env.inc"

extern const uint32_t g_ps2RecompiledFunctionTableBase;
extern const uint32_t g_ps2RecompiledFunctionTableSlotCount;
extern PS2Runtime::RecompiledFunction g_ps2RecompiledFunctionTable[];

static PS2Runtime::RecompiledFunction* PortSlot(uint32_t address) {
    if (address < g_ps2RecompiledFunctionTableBase || (address & 3u)) return nullptr;
    const uint32_t slot = (address - g_ps2RecompiledFunctionTableBase) / 4u;
    return slot < g_ps2RecompiledFunctionTableSlotCount ? &g_ps2RecompiledFunctionTable[slot] : nullptr;
}

static void PortInstall(const PortTableEntry* entries) {
    for (; entries->function; ++entries)
        if (auto* slot = PortSlot(entries->address)) *slot = entries->function;
}

#ifdef PORT_PROFILE_HANDLERS
// The profile defines: static void PortProfileInstall() using PortSlot / PortInstall.
#include PORT_PROFILE_HANDLERS
#endif

int main(int argc, char** argv) {
    for (auto* e = kPortEnv; (*e)[0]; ++e) {
        if (std::getenv((*e)[0])) continue;  // an explicit environment value wins
#ifdef _WIN32
        _putenv_s((*e)[0], (*e)[1]);
#else
        setenv((*e)[0], (*e)[1], 0);
#endif
    }
    // A gap start lies outside every Ghidra function: the table slot there held a neighbour's label binding.
    PortInstall(kPortGapFunctions);
    // SDK library code that talks to the IOP is served by the runtime (there is no IOP processor answering it).
    if (!std::getenv("PS2X_GUEST_SDK")) PortInstall(kPortSdkHandlers);
#ifdef PORT_PROFILE_HANDLERS
    PortProfileInstall();
#endif
    return PortSharedMain(argc, argv);
}
