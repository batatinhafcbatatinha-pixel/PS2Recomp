// Run-time recompilation of guest code that has no translation (see runtime/ps2_hot_api.h).
// Enabled by PS2X_HOTRECOMP=<tools/ps2port/hotrecomp.py> (tools/ps2port play sets it, with the compiler environment);
// work files go to PS2X_OBSERVE_DIR/hot. A failure leaves the old behaviour (missing-function report).
//
// Each library is named after the address and a hash of the guest code it was translated from
// (hot_<addr>_<hash>.dll), so later runs load it directly, and code that overlays put at the same address gets its
// own library. A translated function first checks that the guest code is still the one it was made from; when an
// overlay replaced it, it asks for the translation of the new code (retranslate).
#include "runtime/ps2_hot_api.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <map>
#include <mutex>
#include <string>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <dlfcn.h>
#endif

PS2Runtime::RecompiledFunction ps2xHotRecompile(uint8_t *rdram, uint32_t address);

namespace
{
    constexpr uint32_t kSignatureBytes = 64u;

    uint8_t *g_rdram = nullptr;

    PS2Runtime::RecompiledFunction retranslate(PS2Runtime *runtime, uint32_t address)
    {
        PS2Runtime::RecompiledFunction fn = ps2xHotRecompile(g_rdram, address);
        if (fn)
            runtime->registerFunction(address, fn);
        return fn;
    }

    const PS2HotApi &hotApi()
    {
        static const PS2HotApi api = {
            kPS2HotApiVersion,
            &PS2Runtime::Load8, &PS2Runtime::Load16, &PS2Runtime::Load32, &PS2Runtime::Load64, &PS2Runtime::Load128,
            &PS2Runtime::Store8, &PS2Runtime::Store16, &PS2Runtime::Store32, &PS2Runtime::Store64, &PS2Runtime::Store128,
            &PS2Runtime::dispatchGuestBranch,
            &PS2Runtime::eeCheckpointDue,
            &PS2Runtime::handleBreak,
            &PS2Runtime::handleTrap,
            &PS2Runtime::SignalException,
            static_cast<void (PS2Runtime::*)(uint8_t *, R5900Context *)>(&PS2Runtime::handleSyscall),
            static_cast<void (PS2Runtime::*)(uint8_t *, R5900Context *, uint32_t)>(&PS2Runtime::handleSyscall),
            &PS2Runtime::lookupFunction,
            &retranslate,
        };
        return api;
    }

    void *openLibrary(const std::filesystem::path &path)
    {
#ifdef _WIN32
        return reinterpret_cast<void *>(LoadLibraryW(path.wstring().c_str()));
#else
        return dlopen(path.string().c_str(), RTLD_NOW | RTLD_LOCAL);
#endif
    }

    void *symbol(void *library, const char *name)
    {
#ifdef _WIN32
        return reinterpret_cast<void *>(GetProcAddress(reinterpret_cast<HMODULE>(library), name));
#else
        return dlsym(library, name);
#endif
    }

    uint32_t fnv32(const uint8_t *data, size_t size)
    {
        uint32_t h = 2166136261u;
        for (size_t i = 0; i < size; ++i)
            h = (h ^ data[i]) * 16777619u;
        return h;
    }
}

// Returns the translated function for address, compiling it now if needed; nullptr when that is not possible.
PS2Runtime::RecompiledFunction ps2xHotRecompile(uint8_t *rdram, uint32_t address)
{
    static const char *script = std::getenv("PS2X_HOTRECOMP");
    static const char *observe = std::getenv("PS2X_OBSERVE_DIR");
    if (!script || !*script || !observe || !*observe || !rdram)
        return nullptr;
    g_rdram = rdram;
    const uint32_t offset = address & (PS2_RAM_SIZE - 1u);
    if (offset + kSignatureBytes > PS2_RAM_SIZE)
        return nullptr;
    const uint32_t hash = fnv32(rdram + offset, kSignatureBytes);

    static std::mutex mutex;
    static std::map<std::pair<uint32_t, uint32_t>, PS2Runtime::RecompiledFunction> done;
    std::lock_guard<std::mutex> lock(mutex);
    const auto key = std::make_pair(address, hash);
    if (auto it = done.find(key); it != done.end())
        return it->second;
    done[key] = nullptr; // one attempt per code version and run

    const std::filesystem::path dir = std::filesystem::path(observe) / "hot";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    char tag[32];
    std::snprintf(tag, sizeof(tag), "%08x_%08x", address, hash);
#ifdef _WIN32
    const std::filesystem::path library = dir / ("hot_" + std::string(tag) + ".dll");
#else
    const std::filesystem::path library = dir / ("hot_" + std::string(tag) + ".so");
#endif
    if (!std::filesystem::exists(library, ec))
    {
        const std::filesystem::path ram = dir / ("ram_" + std::string(tag) + ".bin");
        if (FILE *f = std::fopen(ram.string().c_str(), "wb"))
        {
            std::fwrite(rdram, 1, PS2_RAM_SIZE, f);
            std::fclose(f);
        }
        else
        {
            return nullptr;
        }
        const char *python = std::getenv("PS2X_HOTRECOMP_PYTHON");
        const std::string inner = std::string("\"") + (python && *python ? python : "python") + "\" \"" + script + "\" \"" +
                                  dir.string() + "\" " + tag;
        std::cerr << "[hot] translating guest function 0x" << std::hex << address << std::dec
                  << " now (it is added to the next build)" << std::endl;
#ifdef _WIN32
        const int status = std::system(("\"" + inner + "\"").c_str());
#else
        const int status = std::system(inner.c_str());
#endif
        std::filesystem::remove(ram, ec);
        if (status != 0 || !std::filesystem::exists(library, ec))
        {
            std::cerr << "[hot] 0x" << std::hex << address << std::dec << ": translation failed (see "
                      << (dir / ("hot_" + std::string(tag) + ".log")).string() << ")" << std::endl;
            return nullptr;
        }
    }
    void *handle = openLibrary(library);
    auto bind = handle ? reinterpret_cast<PS2HotBindFn>(symbol(handle, "ps2x_hot_bind")) : nullptr;
    auto entry = handle ? reinterpret_cast<PS2HotEntryFn>(symbol(handle, "ps2x_hot_entry")) : nullptr;
    if (!bind || !entry)
    {
        std::cerr << "[hot] cannot load " << library.string() << std::endl;
        return nullptr;
    }
    bind(&hotApi());
    PS2Runtime::RecompiledFunction fn = entry();
    done[key] = fn;
    std::cerr << "[hot] 0x" << std::hex << address << std::dec << ": native code loaded (" << library.filename().string() << ")" << std::endl;
    return fn;
}
