#pragma once

// Ahead-of-time (static) recompilation of IOP modules (IRX).
// tools/irx_aot.py translates an IRX file into C++ (one function per module); the game executable
// compiles the generated file, whose static initializer registers it here. When the IOP loads an IRX
// whose image hash matches, the emulator runs the module natively instead of interpreting it.
// The generated code keeps the interpreter's semantics (R3000 load delay, branch delay slots, cycle
// count = executed instructions) and hands any instruction it does not translate (syscall, break,
// cop0 access, overflow/address exceptions) back to the interpreter.

#include <cstdint>

namespace ps2x::iop
{
    struct AotState
    {
        uint32_t gpr[32];
        uint32_t hi;
        uint32_t lo;
        uint32_t pc;
        uint32_t pendingLoadReg;
        uint32_t pendingLoadValue;
        uint32_t branchTarget;
        bool pendingLoad;
        bool branchPending;
    };

    struct AotMemory
    {
        void *self;
        const uint8_t *ram; // 2 MB IOP RAM (reads only; writes go through the callbacks)
        uint8_t (*read8)(void *self, uint32_t address);
        uint16_t (*read16)(void *self, uint32_t address);
        uint32_t (*read32)(void *self, uint32_t address);
        void (*write8)(void *self, uint32_t address, uint8_t value);
        void (*write16)(void *self, uint32_t address, uint16_t value);
        void (*write32)(void *self, uint32_t address, uint32_t value);
    };

    // Runs from state.pc (inside the module) until budget instructions have run and a branch is taken,
    // or until code it cannot run natively. Returns the number of guest instructions executed.
    using AotModuleFn = uint32_t (*)(AotState &state, const AotMemory &memory, uint32_t base, uint32_t budget);

    struct AotModuleInfo
    {
        const char *name;
        uint64_t imageHash; // FNV-1a 64 of the whole IRX file
        uint32_t imageSize;
        uint32_t codeSize;  // translated range [base, base + codeSize)
        AotModuleFn fn;
    };

    void registerAotModule(const AotModuleInfo &info);
    const AotModuleInfo *findAotModule(uint64_t imageHash, uint32_t imageSize);
    uint64_t aotImageHash(const void *data, uint32_t size);

    struct AotModuleRegistrar
    {
        explicit AotModuleRegistrar(const AotModuleInfo &info) { registerAotModule(info); }
    };
}
