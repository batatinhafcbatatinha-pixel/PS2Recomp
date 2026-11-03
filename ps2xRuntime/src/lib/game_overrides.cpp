#include "game_overrides.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include <cstring>
#include <iostream>

#if PS2X_ENABLE_CODWAW_SUPPLEMENTS
namespace {
    thread_local int32_t codWawMpegRelocation = 0;
    thread_local uint32_t codWawMpegLoadedBase = 0;
}
int32_t codWawMpegDelta() { return codWawMpegRelocation; }
uint32_t codWawMpegAddress(uint32_t address) { return address + codWawMpegRelocation; }
void registerCodWawMpegCtor(PS2Runtime &runtime);
#include "supplemental-mpeg/bindings.inc"
#include "supplemental-mpeg-extra/bindings.inc"

bool tryBindCodWawMpeg(PS2Runtime &runtime, uint8_t *rdram, uint32_t target)
{
    if (!rdram || target < 0x102620u || target > 0x1fe0000u) return false;
    const uint32_t base = target - 0x2620u;
    if (Ps2FastRead32(rdram, base) != 0x32524e53u ||
        Ps2FastRead32(rdram, base + 8u) != 0xaeu ||
        Ps2FastRead32(rdram, base + 16u) != 0x47u ||
        Ps2FastRead32(rdram, base + 24u) != target ||
        std::memcmp(rdram + base + 0xf310u, "PsIIlibmpeg 3100", 15u) != 0)
        return false;
    uint32_t shapeHash = 2166136261u;
    for (uint32_t offset = 0x80u; offset < 0xf2c0u; offset += 4u) {
        uint32_t word = Ps2FastRead32(rdram, base + offset);
        const uint32_t opcode = word >> 26u;
        if (opcode == 2u || opcode == 3u) word &= 0xfc000000u;
        else if (opcode == 9u || opcode == 15u || opcode == 0x23u || opcode == 0x2bu || opcode == 0x39u)
            word &= 0xffff0000u;
        shapeHash = (shapeHash ^ word) * 16777619u;
    }
    if (shapeHash != 0x959d86eeu) {
        std::cerr << "[mpeg:bind] rejected code shape=0x" << std::hex << shapeHash << std::dec << std::endl;
        return false;
    }
    if (codWawMpegLoadedBase != 0u)
        for (uint32_t offset = 0x80u; offset < 0xf2c0u; offset += 4u)
            runtime.registerFunction(codWawMpegLoadedBase + offset, nullptr);
    codWawMpegLoadedBase = base;
    codWawMpegRelocation = static_cast<int32_t>(base - 0x9e7480u);
    registerMpegSupplement(runtime);
    registerMpegExtraSupplement(runtime);
    registerCodWawMpegCtor(runtime);
    std::cerr << "[mpeg:bind] base=0x" << std::hex << base << " init=0x" << target
              << std::dec << " delta=" << codWawMpegRelocation << std::endl;
    return runtime.hasFunction(target);
}
#endif
#include "ps2_runtime_calls.h"
#include "ps2_stubs.h"
#include "ps2_syscalls.h"
#include "ps2_log.h"
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <optional>
#include <vector>

namespace
{
    std::mutex &registryMutex()
    {
        static std::mutex mutex;
        return mutex;
    }

    std::vector<ps2_game_overrides::Descriptor> &descriptorRegistry()
    {
        static std::vector<ps2_game_overrides::Descriptor> registry;
        return registry;
    }

    bool equalsIgnoreCaseAscii(std::string_view lhs, std::string_view rhs)
    {
        if (lhs.size() != rhs.size())
        {
            return false;
        }

        for (size_t i = 0; i < lhs.size(); ++i)
        {
            const auto l = static_cast<unsigned char>(lhs[i]);
            const auto r = static_cast<unsigned char>(rhs[i]);
            if (std::tolower(l) != std::tolower(r))
            {
                return false;
            }
        }

        return true;
    }

    std::string basenameFromPath(const std::string &path)
    {
        std::error_code ec;
        const std::filesystem::path fsPath(path);
        const std::filesystem::path leaf = fsPath.filename();
        if (leaf.empty())
        {
            return path;
        }
        return leaf.string();
    }

    std::optional<PS2Runtime::RecompiledFunction> resolveHandlerByName(std::string_view handlerName)
    {
        const std::string_view resolvedSyscall = ps2_runtime_calls::resolveSyscallName(handlerName);
        if (!resolvedSyscall.empty())
        {
#define PS2_RESOLVE_SYSCALL(name)                   \
    if (resolvedSyscall == std::string_view{#name}) \
    {                                               \
        return &ps2_syscalls::name;                 \
    }
            PS2_SYSCALL_LIST(PS2_RESOLVE_SYSCALL)
#undef PS2_RESOLVE_SYSCALL
        }

        const std::string_view resolvedStub = ps2_runtime_calls::resolveStubName(handlerName);
        if (!resolvedStub.empty())
        {
#define PS2_RESOLVE_STUB(name)                   \
    if (resolvedStub == std::string_view{#name}) \
    {                                            \
        return &ps2_stubs::name;                 \
    }
            PS2_STUB_LIST(PS2_RESOLVE_STUB)
#undef PS2_RESOLVE_STUB
        }

        return std::nullopt;
    }
}
namespace ps2_game_overrides
{
    AutoRegister::AutoRegister(const Descriptor &descriptor)
    {
        registerDescriptor(descriptor);
    }

    void registerDescriptor(const Descriptor &descriptor)
    {
        if (!descriptor.apply)
        {
            std::cerr << "[game_overrides] ignoring descriptor with null apply callback." << std::endl;
            return;
        }

        std::lock_guard<std::mutex> lock(registryMutex());
        descriptorRegistry().push_back(descriptor);
    }

    bool bindAddressHandler(PS2Runtime &runtime, uint32_t address, std::string_view handlerName)
    {
        const auto resolved = resolveHandlerByName(handlerName);
        if (!resolved.has_value())
        {
            std::cerr << "[game_overrides] unresolved handler '" << handlerName
                      << "' for address 0x" << std::hex << address << std::dec << std::endl;
            return false;
        }

        return runtime.replaceFunction(address, resolved.value());
    }

    void applyMatching(PS2Runtime &runtime,
                       const std::string &elfPath,
                       uint32_t entry,
                       uint32_t fileCrc32,
                       bool fileCrcValid)
    {

        std::vector<Descriptor> descriptors;
        {
            std::lock_guard<std::mutex> lock(registryMutex());
            descriptors = descriptorRegistry();
        }

        if (descriptors.empty())
        {
            return;
        }

        const std::string elfName = basenameFromPath(elfPath);
        size_t appliedCount = 0;
        for (const Descriptor &descriptor : descriptors)
        {
            if (!descriptor.apply)
            {
                continue;
            }

            if (descriptor.elfName && descriptor.elfName[0] != '\0')
            {
                if (!equalsIgnoreCaseAscii(descriptor.elfName, elfName))
                {
                    continue;
                }
            }

            if (descriptor.entry != 0u && descriptor.entry != entry)
            {
                continue;
            }

            if (descriptor.crc32 != 0u)
            {
                if (!fileCrcValid || fileCrc32 != descriptor.crc32)
                {
                    continue;
                }
            }

            const char *name = (descriptor.name && descriptor.name[0] != '\0')
                                   ? descriptor.name
                                   : "unnamed";
            RUNTIME_LOG("[game_overrides] applying '" << name << "'");
            descriptor.apply(runtime);
            ++appliedCount;
        }

        if (appliedCount > 0)
        {
            RUNTIME_LOG("[game_overrides] applied " << appliedCount << " matching override(s).");
        }
    }
}
