// IOP-side services the SDK's EE client libraries reach through RPC servers that no IOP processor answers here:
// the libsif loadfile client (module search/stop/unload), the rest of the libfs file client (stat, devctl, mkdir,
// ...) and the power-off handler. Every port used to reimplement these in its own main.cpp; they are the same
// for every game, so they live here and tools/ps2port binds them wherever the analyzer or a signature finds them.
#include "Common.h"
#include "ps2_syscalls.h"

#include <map>
#include <mutex>
#include <string>

namespace ps2_stubs
{
    // sceSifSearchModuleByName(name): the client binds the loadfile RPC (0x80000006) and spins until it answers.
    // Modules are loaded natively by the runtime: report the ones this run already asked for by name (the boot code
    // checks again before each load), "not loaded" (-1) otherwise so the game calls sceSifLoadModule.
    void sceSifSearchModuleByName(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        static std::mutex mutex;
        static std::map<std::string, int32_t> seen;
        const std::string name = readPs2CStringBounded(rdram, runtime, getRegU32(ctx, 4), 64);
        int32_t result = -1;
        {
            std::lock_guard<std::mutex> lock(mutex);
            const auto it = seen.find(name);
            if (it != seen.end())
                result = it->second;
            else
                seen.emplace(name, static_cast<int32_t>(seen.size()) + 1);
        }
        RUNTIME_LOG("[sceSifSearchModuleByName] \"" << sanitizeForLog(name) << "\" -> " << result << std::endl);
        setReturnS32(ctx, result);
    }

    // sceSifStopModule(id, args, argp, &result) / sceSifUnloadModule(id): nothing runs on an IOP to stop.
    void sceSifStopModule(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)runtime;
        if (uint8_t *result = getMemPtr(rdram, getRegU32(ctx, 7)))
            std::memset(result, 0, 4);
        setReturnS32(ctx, static_cast<int32_t>(getRegU32(ctx, 4)));
    }

    void sceSifUnloadModule(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        (void)runtime;
        setReturnS32(ctx, static_cast<int32_t>(getRegU32(ctx, 4)));
    }

    // libfs: the SDK's file client sends these to the IOP's FILEIO server, which is not bound on the EE side.
    void sceGetstat(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ps2_syscalls::fioGetstat(rdram, ctx, runtime);
    }

    void sceMkdir(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ps2_syscalls::fioMkdir(rdram, ctx, runtime);
    }

    // sceDevctl(name, cmd, arg, arglen, buf, buflen): the network adapter (dev9) accepts power commands; there is no
    // other device to control (ENODEV).
    void sceDevctl(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const std::string name = readPs2CStringBounded(rdram, runtime, getRegU32(ctx, 4), 64);
        const uint32_t buflen = getRegU32(ctx, 9);
        if (uint8_t *buf = getMemPtr(rdram, getRegU32(ctx, 8)); buf && buflen && buflen <= 0x400)
            std::memset(buf, 0, buflen);
        const int32_t result = name.rfind("dev9", 0) == 0 ? 0 : -19;
        RUNTIME_LOG("[sceDevctl] \"" << sanitizeForLog(name) << "\" cmd=0x" << std::hex << getRegU32(ctx, 5) << std::dec
                                     << " -> " << result << std::endl);
        setReturnS32(ctx, result);
    }

    void sceSync(uint8_t *, R5900Context *ctx, PS2Runtime *) { setReturnS32(ctx, 0); }
    void sceChstat(uint8_t *, R5900Context *ctx, PS2Runtime *) { setReturnS32(ctx, -1); }
    void sceRename(uint8_t *, R5900Context *ctx, PS2Runtime *) { setReturnS32(ctx, -1); }
    void sceFormat(uint8_t *, R5900Context *ctx, PS2Runtime *) { setReturnS32(ctx, -19); }
    void sceMount(uint8_t *, R5900Context *ctx, PS2Runtime *) { setReturnS32(ctx, -19); }
    void sceUmount(uint8_t *, R5900Context *ctx, PS2Runtime *) { setReturnS32(ctx, -19); }
    void sceSymlink(uint8_t *, R5900Context *ctx, PS2Runtime *) { setReturnS32(ctx, -19); }
    void sceReadlink(uint8_t *, R5900Context *ctx, PS2Runtime *) { setReturnS32(ctx, -19); }
    void sceFsSetIopBuf(uint8_t *, R5900Context *ctx, PS2Runtime *) { setReturnS32(ctx, 0); }
    void sceFsSetIopPrio(uint8_t *, R5900Context *ctx, PS2Runtime *) { setReturnS32(ctx, 0); }

    // scePowerOffHandler(func, arg): a PC has no power-button event; "no previous handler".
    void scePowerOffHandler(uint8_t *, R5900Context *ctx, PS2Runtime *) { setReturnS32(ctx, 0); }
}
