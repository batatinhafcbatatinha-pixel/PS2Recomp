// The patches and cheats chosen in the launcher (PCSX2 .pnach), prepared by tools/ps2port hotpatch into a list
// (PS2X_PATCHES, observed/patches.txt):
//   once <address> <size> <value>    written into guest memory once, when the executable is loaded
//   frame <address> <size> <value>   written again at every VBlank (cheats that hold a value: money, health...)
//   hot <address>                    a function whose code a patch changes: its retranslated library (observed/hot,
//                                    named after the patched code) replaces the build's translation
// Writes go in before the libraries load: a library checks that the guest code is the one it was made from.
#include "ps2_patches.h"
#include "ps2_runtime.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

PS2Runtime::RecompiledFunction ps2xHotRecompile(uint8_t *rdram, uint32_t address); // ps2_hot.cpp

namespace
{
    struct Write
    {
        uint32_t address;
        uint32_t size;
        uint32_t value;
    };

    std::mutex g_mutex;
    std::vector<Write> g_frame;

    void apply(uint8_t *rdram, const Write &w)
    {
        const uint32_t offset = w.address & (PS2_RAM_SIZE - 1u);
        if (offset + w.size > PS2_RAM_SIZE)
            return;
        std::memcpy(rdram + offset, &w.value, w.size); // little endian, like the EE
    }
}

namespace ps2_patches
{
    void load(PS2Runtime &runtime, uint8_t *rdram)
    {
        const char *path = std::getenv("PS2X_PATCHES");
        if (!path || !*path || !rdram)
            return;
        std::ifstream file(path);
        if (!file)
            return;
        std::vector<uint32_t> hot;
        size_t once = 0;
        std::lock_guard<std::mutex> lock(g_mutex);
        g_frame.clear();
        std::string line;
        while (std::getline(file, line))
        {
            std::istringstream in(line);
            std::string kind;
            if (!(in >> kind) || kind.empty() || kind[0] == '#')
                continue;
            if (kind == "hot")
            {
                uint32_t address = 0;
                if (in >> std::hex >> address)
                    hot.push_back(address);
                continue;
            }
            Write w{};
            if (!(in >> std::hex >> w.address >> std::dec >> w.size >> std::hex >> w.value) || (w.size != 1 && w.size != 2 && w.size != 4))
                continue;
            apply(rdram, w);
            if (kind == "frame")
                g_frame.push_back(w);
            else
                ++once;
        }
        size_t loaded = 0;
        for (uint32_t address : hot)
        {
            if (PS2Runtime::RecompiledFunction fn = ps2xHotRecompile(rdram, address))
            {
                runtime.registerFunction(address, fn);
                ++loaded;
            }
            else
            {
                std::cerr << "[patches] no library for 0x" << std::hex << address << std::dec
                          << " (run tools/ps2port hotpatch)" << std::endl;
            }
        }
        std::cerr << "[patches] " << once << " writes once, " << g_frame.size() << " every frame, " << loaded << "/"
                  << hot.size() << " patched functions (" << path << ")" << std::endl;
    }

    void everyVblank(uint8_t *rdram)
    {
        if (!rdram)
            return;
        std::lock_guard<std::mutex> lock(g_mutex);
        for (const Write &w : g_frame)
            apply(rdram, w);
    }
}
