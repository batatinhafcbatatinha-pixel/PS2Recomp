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
    // sceSifSearchModuleByName is in SIF.cpp (it answers from the modules the runtime loaded).

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
