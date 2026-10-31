#pragma once

#include "runtime/gs/gs_types.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace GSMip
{
    struct Selection
    {
        bool enabled = false;
        unsigned level0 = 0, level1 = 0;
        float blend = 0.0f;
        bool linear = false;
    };

    inline Selection Select(uint64_t tex1, float q, bool fst = false)
    {
        Selection result;
        const unsigned mmin = static_cast<unsigned>((tex1 >> 6u) & 7u);
        const unsigned maximum = std::min<unsigned>((tex1 >> 2u) & 7u, 6u);
        if (!maximum || mmin < 2u || mmin > 5u) return result;
        result.enabled = true;
        const int rawK = static_cast<int>((tex1 >> 32u) & 0xfffu);
        const float k = static_cast<float>(rawK >= 2048 ? rawK - 4096 : rawK) / 16.0f;
        float lod = k;
        if (!(tex1 & 1u) && !fst)
        {
            const unsigned l = static_cast<unsigned>((tex1 >> 19u) & 3u);
            const float magnitude = std::max(std::fabs(q), std::numeric_limits<float>::min());
            lod -= std::log2(magnitude) * static_cast<float>(1u << l);
        }
        if (std::isnan(lod)) lod = 0.0f;
        result.linear = lod <= 0.0f ? ((tex1 >> 5u) & 1u) != 0u : mmin >= 4u;
        lod = std::clamp(lod, 0.0f, static_cast<float>(maximum));
        if ((mmin & 1u) == 0u)
            result.level0 = result.level1 = static_cast<unsigned>(std::floor(lod + 0.5f));
        else
        {
            result.level0 = static_cast<unsigned>(std::floor(lod));
            result.level1 = std::min(result.level0 + 1u, maximum);
            result.blend = lod - static_cast<float>(result.level0);
        }
        return result;
    }

    inline GSTex0Reg LevelTexture(const GSTex0Reg &base, uint64_t miptbp1, uint64_t miptbp2, unsigned level)
    {
        GSTex0Reg texture = base;
        level = std::min(level, 6u);
        if (!level) return texture;
        const uint64_t registers = level <= 3u ? miptbp1 : miptbp2;
        const unsigned shift = ((level - 1u) % 3u) * 20u;
        texture.tbp0 = static_cast<uint32_t>((registers >> shift) & 0x3fffu);
        texture.tbw = static_cast<uint8_t>((registers >> (shift + 14u)) & 0x3fu);
        texture.tw = static_cast<uint8_t>(base.tw > level ? base.tw - level : 0u);
        texture.th = static_cast<uint8_t>(base.th > level ? base.th - level : 0u);
        return texture;
    }

    inline uint64_t ScaleClamp(uint64_t clamp, unsigned level)
    {
        uint64_t scaled = clamp & 15u;
        for (unsigned shift : {4u, 14u, 24u, 34u})
            scaled |= (((clamp >> shift) & 0x3ffu) >> std::min(level, 6u)) << shift;
        return scaled;
    }

    inline uint32_t Blend(uint32_t a, uint32_t b, float weight)
    {
        uint32_t color = 0;
        for (unsigned shift = 0; shift < 32u; shift += 8u)
        {
            const float x = static_cast<float>((a >> shift) & 255u);
            const float y = static_cast<float>((b >> shift) & 255u);
            color |= static_cast<uint32_t>(x + (y - x) * weight + 0.5f) << shift;
        }
        return color;
    }
}
