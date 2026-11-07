#include "ps2x/iop/iop_aot.h"

#include <mutex>
#include <vector>

namespace ps2x::iop
{
    namespace
    {
        std::vector<AotModuleInfo> &registry()
        {
            static std::vector<AotModuleInfo> modules;
            return modules;
        }
        std::mutex &registryMutex()
        {
            static std::mutex mutex;
            return mutex;
        }
    }

    void registerAotModule(const AotModuleInfo &info)
    {
        std::lock_guard<std::mutex> lock(registryMutex());
        registry().push_back(info);
    }

    const AotModuleInfo *findAotModule(uint64_t imageHash, uint32_t imageSize)
    {
        std::lock_guard<std::mutex> lock(registryMutex());
        for (const AotModuleInfo &info : registry())
            if (info.imageHash == imageHash && info.imageSize == imageSize)
                return &info;
        return nullptr;
    }

    uint64_t aotImageHash(const void *data, uint32_t size)
    {
        const auto *bytes = static_cast<const uint8_t *>(data);
        uint64_t hash = 0xCBF29CE484222325ull;
        for (uint32_t i = 0; i < size; ++i)
            hash = (hash ^ bytes[i]) * 0x100000001B3ull;
        return hash;
    }
}
