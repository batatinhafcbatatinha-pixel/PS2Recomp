// Based on Blackline Interactive implementation
#include "runtime/ps2_memory.h"
#include <algorithm>
#include <cstring>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <emmintrin.h>

// PS2X_VIF_SEQLOG=<seconds>:<count>: after <seconds>, logs <count> VIF1 BASE/OFFSET/MSCAL/MSCNT/STCYCL/UNPACK commands in
// order with TOPS/DBF, to follow a double-buffered upload against the program that reads it.
// Unpack history for diagnostics: the last 256 UNPACKs (VU range, TOPS, first 64 source bytes), written next to a
// VU1 dump by ps2VifWriteUnpackHistory.
namespace
{
    struct UnpackHistoryEntry
    {
        uint32_t vuStart, vuCount, tops, opcode;
        uint8_t bytes[64];
    };
    UnpackHistoryEntry g_unpackHistory[256];
    uint32_t g_unpackHistoryNext = 0;
}
void ps2VifWriteUnpackHistory(FILE *file)
{
    for (uint32_t i = 0; i < 256u; ++i)
        std::fwrite(&g_unpackHistory[(g_unpackHistoryNext + i) & 255u], 1, sizeof(UnpackHistoryEntry), file);
}

static bool vifSeqLogOn()
{
    static const char *spec = std::getenv("PS2X_VIF_SEQLOG");
    if (!spec)
        return false;
    static const double at = std::atof(spec);
    static const int maxLogs = std::strchr(spec, ':') ? std::atoi(std::strchr(spec, ':') + 1) : 400;
    static const auto t0 = std::chrono::steady_clock::now();
    static int logs = 0;
    if (logs >= maxLogs || std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < at)
        return false;
    ++logs;
    return true;
}

enum VIFCmd : uint8_t
{
    VIF_NOP = 0x00,
    VIF_STCYCL = 0x01,
    VIF_OFFSET = 0x02,
    VIF_BASE = 0x03,
    VIF_ITOP = 0x04,
    VIF_STMOD = 0x05,
    VIF_MSKPATH3 = 0x06,
    VIF_MARK = 0x07,
    VIF_FLUSHE = 0x10,
    VIF_FLUSH = 0x11,
    VIF_FLUSHA = 0x13,
    VIF_MSCAL = 0x14,
    VIF_MSCALF = 0x15,
    VIF_MSCNT = 0x17,
    VIF_STMASK = 0x20,
    VIF_STROW = 0x30,
    VIF_STCOL = 0x31,
    VIF_MPG = 0x4A,
    VIF_DIRECT = 0x50,
    VIF_DIRECTHL = 0x51,
};

namespace
{
    constexpr uint8_t kGifFmtImage = 2u;

    // UNPACK, one loop per format and one for the write stage. Same results as the general loop below it, which stays for
    // CL < WL (fill writes), the undefined formats and the command trace. A frame unpacks hundreds of thousands of vectors.
    template <uint32_t Comps, typename T, bool Signed>
    void decodeUnpack(const uint8_t *src, uint32_t count, __m128i *out)
    {
        for (uint32_t i = 0u; i < count; ++i, src += Comps * sizeof(T))
        {
            uint32_t v[4] = {};
            for (uint32_t c = 0u; c < Comps; ++c)
            {
                T raw;
                std::memcpy(&raw, src + c * sizeof(T), sizeof(T));
                if constexpr (sizeof(T) == 4u)
                    v[c] = static_cast<uint32_t>(raw);
                else if constexpr (sizeof(T) == 2u)
                    v[c] = Signed ? static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(raw))) : static_cast<uint32_t>(raw);
                else
                    v[c] = Signed ? static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(raw))) : static_cast<uint32_t>(raw);
            }
            // built from registers: a load of four words just stored would pay a failed store forward per vector
            if constexpr (Comps == 1u)
                out[i] = _mm_set1_epi32(static_cast<int>(v[0]));
            else
                out[i] = _mm_set_epi32(static_cast<int>(v[3]), static_cast<int>(v[2]), static_cast<int>(v[1]), static_cast<int>(v[0]));
        }
    }

    void decodeUnpackV45(const uint8_t *src, uint32_t count, __m128i *out)
    {
        for (uint32_t i = 0u; i < count; ++i, src += 2u)
        {
            uint16_t p;
            std::memcpy(&p, src, 2u);
            out[i] = _mm_set_epi32((p >> 15) << 7, ((p >> 10) & 0x1F) << 3, ((p >> 5) & 0x1F) << 3, (p & 0x1F) << 3);
        }
    }

    bool decodeUnpackAny(uint32_t vl, uint32_t comps, bool zeroExtend, const uint8_t *src, uint32_t count, __m128i *out)
    {
        switch ((vl << 4) | comps)
        {
        case 0x01: decodeUnpack<1u, uint32_t, false>(src, count, out); return true;
        case 0x02: decodeUnpack<2u, uint32_t, false>(src, count, out); return true;
        case 0x03: decodeUnpack<3u, uint32_t, false>(src, count, out); return true;
        case 0x04: decodeUnpack<4u, uint32_t, false>(src, count, out); return true;
        case 0x11: zeroExtend ? decodeUnpack<1u, uint16_t, false>(src, count, out) : decodeUnpack<1u, uint16_t, true>(src, count, out); return true;
        case 0x12: zeroExtend ? decodeUnpack<2u, uint16_t, false>(src, count, out) : decodeUnpack<2u, uint16_t, true>(src, count, out); return true;
        case 0x13: zeroExtend ? decodeUnpack<3u, uint16_t, false>(src, count, out) : decodeUnpack<3u, uint16_t, true>(src, count, out); return true;
        case 0x14: zeroExtend ? decodeUnpack<4u, uint16_t, false>(src, count, out) : decodeUnpack<4u, uint16_t, true>(src, count, out); return true;
        case 0x21: zeroExtend ? decodeUnpack<1u, uint8_t, false>(src, count, out) : decodeUnpack<1u, uint8_t, true>(src, count, out); return true;
        case 0x22: zeroExtend ? decodeUnpack<2u, uint8_t, false>(src, count, out) : decodeUnpack<2u, uint8_t, true>(src, count, out); return true;
        case 0x23: zeroExtend ? decodeUnpack<3u, uint8_t, false>(src, count, out) : decodeUnpack<3u, uint8_t, true>(src, count, out); return true;
        case 0x24: zeroExtend ? decodeUnpack<4u, uint8_t, false>(src, count, out) : decodeUnpack<4u, uint8_t, true>(src, count, out); return true;
        case 0x34: decodeUnpackV45(src, count, out); return true;
        default: return false;
        }
    }

    // CL >= WL: every write has a source vector and the destination skips CL - WL vectors per block.
    void writeUnpack(uint8_t *vuData, const __m128i *decoded, uint32_t count, uint32_t vuAddr, uint32_t cl, uint32_t wl, uint32_t comps,
                     bool scalarBroadcast, bool canAdd, uint32_t mode, bool masked, uint32_t mask, uint32_t *row, const uint32_t *col)
    {
        const auto all = _mm_set1_epi32(-1);
        const auto fieldMask = [](uint32_t bits) {
            return _mm_set_epi32(bits & 8u ? -1 : 0, bits & 4u ? -1 : 0, bits & 2u ? -1 : 0, bits & 1u ? -1 : 0);
        };
        const __m128i present = scalarBroadcast ? all : fieldMask((1u << comps) - 1u); // fields the format does not carry keep memory
        const uint32_t addMode = canAdd && (mode == 1u || mode == 2u) ? mode : 0u;
        __m128i raw[4], useRow[4], useCol[4], protect[4];
        for (uint32_t cycle = 0u; cycle < 4u; ++cycle)
        {
            uint32_t bits[4] = {};
            for (uint32_t field = 0u; field < 4u && masked; ++field)
                bits[(mask >> (((cycle * 4u) + field) * 2u)) & 3u] |= 1u << field;
            raw[cycle] = masked ? fieldMask(bits[0]) : all;
            useRow[cycle] = fieldMask(bits[1]);
            useCol[cycle] = _mm_and_si128(fieldMask(bits[2]), _mm_set1_epi32(static_cast<int>(col[cycle])));
            protect[cycle] = fieldMask(bits[3]);
        }
        if (!masked && addMode == 0u && (scalarBroadcast || comps == 4u))
        {
            // the common case: every field comes from the stream
            uint32_t cycle = 0u, base = 0u;
            for (uint32_t i = 0u; i < count; ++i)
            {
                _mm_storeu_si128(reinterpret_cast<__m128i *>(vuData + ((vuAddr + base + cycle) & 0x3FFu) * 16u), decoded[i]);
                if (++cycle == wl)
                {
                    cycle = 0u;
                    base += cl;
                }
            }
            return;
        }
        __m128i rowValue = _mm_loadu_si128(reinterpret_cast<const __m128i *>(row));
        uint32_t cyclePos = 0u, blockBase = 0u;
        for (uint32_t i = 0u; i < count; ++i)
        {
            const uint32_t destVec = (vuAddr + blockBase + cyclePos) & 0x3FFu;
            __m128i *dest = reinterpret_cast<__m128i *>(vuData + destVec * 16u);
            const uint32_t c = cyclePos > 3u ? 3u : cyclePos;
            const __m128i old = _mm_loadu_si128(dest);
            __m128i value = _mm_or_si128(_mm_and_si128(decoded[i], present), _mm_andnot_si128(present, old));
            if (addMode != 0u)
            {
                value = _mm_add_epi32(value, rowValue);
                if (addMode == 2u)
                    rowValue = _mm_or_si128(_mm_and_si128(value, raw[c]), _mm_andnot_si128(raw[c], rowValue));
            }
            value = _mm_and_si128(value, raw[c]);
            if (masked)
                value = _mm_or_si128(_mm_or_si128(value, _mm_and_si128(rowValue, useRow[c])), _mm_or_si128(useCol[c], _mm_and_si128(old, protect[c])));
            _mm_storeu_si128(dest, value);
            if (++cyclePos == wl)
            {
                cyclePos = 0u;
                blockBase += cl;
            }
        }
        _mm_storeu_si128(reinterpret_cast<__m128i *>(row), rowValue);
    }

    uint32_t pendingGifImageQwc(const uint8_t *data, uint32_t sizeBytes)
    {
        if (!data || sizeBytes < 16u)
            return 0u;

        uint32_t offset = 0u;
        while (offset + 16u <= sizeBytes)
        {
            uint64_t tagLo = 0u;
            std::memcpy(&tagLo, data + offset, sizeof(tagLo));
            offset += 16u;

            const uint32_t nloop = static_cast<uint32_t>(tagLo & 0x7FFFu);
            const uint8_t flg = static_cast<uint8_t>((tagLo >> 58) & 0x3u);
            uint32_t nreg = static_cast<uint32_t>((tagLo >> 60) & 0xFu);
            if (nreg == 0u)
                nreg = 16u;

            uint64_t payloadBytes = 0u;
            if (flg == 0u) // PACKED
            {
                payloadBytes = static_cast<uint64_t>(nloop) * nreg * 16ull;
            }
            else if (flg == 1u) // REGLIST, padded to a quadword
            {
                payloadBytes = static_cast<uint64_t>(nloop) * nreg * 8ull;
                payloadBytes = (payloadBytes + 15ull) & ~15ull;
            }
            else if (flg == kGifFmtImage || flg == 3u) // FLG=3 ("disabled" in the GS manual) behaves exactly like IMAGE: NLOOP qwords of image data.
            {
                payloadBytes = static_cast<uint64_t>(nloop) * 16ull;
                const uint64_t availableBytes = sizeBytes - offset;
                if (payloadBytes > availableBytes)
                {
                    return static_cast<uint32_t>((payloadBytes - availableBytes) / 16ull);
                }
            }
            else
            {
                return 0u;
            }

            if (payloadBytes > static_cast<uint64_t>(sizeBytes - offset))
                return 0u;
            offset += static_cast<uint32_t>(payloadBytes);
        }

        return 0u;
    }
}

void PS2Memory::processVIF0Data(uint32_t srcPhys, uint32_t sizeBytes)
{
    if (sizeBytes == 0u || srcPhys >= PS2_RAM_SIZE)
        return;

    const uint64_t requestedEnd = static_cast<uint64_t>(srcPhys) + static_cast<uint64_t>(sizeBytes);
    if (requestedEnd > static_cast<uint64_t>(PS2_RAM_SIZE))
        sizeBytes = PS2_RAM_SIZE - srcPhys;

    processVIF0Data(m_rdram + srcPhys, sizeBytes);
}

void PS2Memory::processVIF0Data(const uint8_t *data, uint32_t sizeBytes)
{
    if (sizeBytes == 0u)
        return;

    uint32_t pos = 0;
    while (pos + 4 <= sizeBytes)
    {
        uint32_t cmd = 0u;
        std::memcpy(&cmd, data + pos, sizeof(cmd));
        pos += 4u;

        const uint8_t opcode = static_cast<uint8_t>((cmd >> 24) & 0x7Fu);
        const uint16_t imm = static_cast<uint16_t>(cmd & 0xFFFFu);
        const uint8_t num = static_cast<uint8_t>((cmd >> 16) & 0xFFu);
        const bool irq = (cmd & 0x80000000u) != 0u;

        vif0_regs.code = cmd;
        vif0_regs.num = num;
        if (irq)
            vif0_regs.stat |= (1u << 11);

        if (opcode == VIF_NOP)
        {
            continue;
        }
        else if (opcode == VIF_STCYCL)
        {
            vif0_regs.cycle = imm;
            continue;
        }
        else if (opcode == VIF_ITOP)
        {
            vif0_regs.itops = imm & 0x3FFu;
            continue;
        }
        else if (opcode == VIF_STMOD)
        {
            vif0_regs.mode = imm & 3u;
            continue;
        }
        else if (opcode == VIF_MARK)
        {
            vif0_regs.mark = imm;
            vif0_regs.stat |= (1u << 6);
            continue;
        }
        else if (opcode == VIF_FLUSHE || opcode == VIF_FLUSH || opcode == VIF_FLUSHA)
        {
            continue;
        }
        else if (opcode == VIF_STMASK)
        {
            if (pos + 4u > sizeBytes)
                break;
            std::memcpy(&vif0_regs.mask, data + pos, sizeof(vif0_regs.mask));
            pos += 4u;
            continue;
        }
        else if (opcode == VIF_STROW)
        {
            if (pos + 16u > sizeBytes)
                break;
            std::memcpy(vif0_regs.row, data + pos, 16u);
            pos += 16u;
            continue;
        }
        else if (opcode == VIF_STCOL)
        {
            if (pos + 16u > sizeBytes)
                break;
            std::memcpy(vif0_regs.col, data + pos, 16u);
            pos += 16u;
            continue;
        }
        else if (opcode == VIF_MPG)
        {
            const uint32_t destAddr = static_cast<uint32_t>(imm & 0x1FFu) * 8u;
            const uint32_t instructionCount = (num == 0u) ? 256u : static_cast<uint32_t>(num);
            const uint32_t mpgBytes = instructionCount * 8u;
            uint32_t copyBytes = 0u;
            if (m_vu0Code && destAddr < PS2_VU0_CODE_SIZE && mpgBytes > 0u)
            {
                copyBytes = mpgBytes;
                if (destAddr + copyBytes > PS2_VU0_CODE_SIZE)
                    copyBytes = PS2_VU0_CODE_SIZE - destAddr;
                if (pos + copyBytes <= sizeBytes)
                {
                    std::memcpy(m_vu0Code + destAddr, data + pos, copyBytes);
                    markVU0CodeModified();
                }
            }

            pos += mpgBytes;
            if (pos > sizeBytes)
                break;
            continue;
        }
        else if ((opcode & 0x60u) == 0x60u)
        {
            const uint8_t vn = static_cast<uint8_t>((opcode >> 2) & 0x3u);
            const uint8_t vl = static_cast<uint8_t>(opcode & 0x3u);
            const int components = static_cast<int>(vn) + 1;
            int bitsPerComponent = 32;
            switch (vl)
            {
            case 0:
                bitsPerComponent = 32;
                break;
            case 1:
                bitsPerComponent = 16;
                break;
            case 2:
                bitsPerComponent = 8;
                break;
            case 3:
                bitsPerComponent = (vn == 3u) ? 4 : 16;
                break;
            default:
                break;
            }
            const int bitsPerVector = (vl == 3u && vn == 3u) ? 16 : (components * bitsPerComponent);
            uint32_t bytesPerVector = static_cast<uint32_t>((bitsPerVector + 7) / 8);
            const uint32_t writeVectorCount = (num == 0u) ? 256u : static_cast<uint32_t>(num);
            uint32_t cl = vif0_regs.cycle & 0xFFu;
            uint32_t wl = (vif0_regs.cycle >> 8) & 0xFFu;
            // CL=0 with WL>0 is a filling write that consumes no source vectors
            // (data = CL * (NUM / WL) + min(NUM % WL, CL)). An unset CYCLE
            // register (WL=0) keeps the straight 1:1 transfer.
            if (wl == 0u)
            {
                wl = 1u;
                if (cl == 0u)
                    cl = 1u;
            }
            uint32_t sourceVectorCount = writeVectorCount;
            if (cl < wl)
            {
                const uint32_t fullBlocks = writeVectorCount / wl;
                uint32_t remainder = writeVectorCount % wl;
                if (remainder > cl)
                    remainder = cl;
                sourceVectorCount = fullBlocks * cl + remainder;
            }
            uint32_t totalBytes = sourceVectorCount * bytesPerVector;
            totalBytes = (totalBytes + 3u) & ~3u;

            if (m_vu0Data && pos + totalBytes <= sizeBytes && vl == 0u)
            {
                uint32_t vuAddr = static_cast<uint32_t>(imm & 0x3FFu);
                if ((imm & 0x8000u) != 0u)
                    vuAddr = (vuAddr + (vif0_regs.tops & 0x3FFu)) & 0x3FFu;
                const uint8_t *srcBase = data + pos;
                uint32_t srcIndex = 0u;
                for (uint32_t writeIndex = 0; writeIndex < writeVectorCount; ++writeIndex)
                {
                    const uint32_t cyclePos = writeIndex % wl;
                    const bool sourceAvailable = (cl >= wl) || (cyclePos < cl);
                    uint32_t destVec = (cl >= wl) ? ((vuAddr + (writeIndex / wl) * cl + cyclePos) & 0x3FFu)
                                                  : ((vuAddr + writeIndex) & 0x3FFu);
                    const uint32_t destOff = destVec * 16u;
                    if (destOff + 16u > PS2_VU0_DATA_SIZE)
                    {
                        if (sourceAvailable && srcIndex < sourceVectorCount)
                            ++srcIndex;
                        continue;
                    }
                    if (!sourceAvailable || srcIndex >= sourceVectorCount)
                        continue;
                    const uint8_t *srcVec = srcBase + srcIndex * bytesPerVector;
                    ++srcIndex;
                    uint32_t lanes[4] = {0u, 0u, 0u, 0u};
                    std::memcpy(lanes, m_vu0Data + destOff, sizeof(lanes));
                    const uint32_t limit = (components > 4) ? 4u : static_cast<uint32_t>(components);
                    for (uint32_t c = 0; c < limit; ++c)
                    {
                        uint32_t scalar = 0u;
                        std::memcpy(&scalar, srcVec + c * 4u, sizeof(scalar));
                        lanes[c] = scalar;
                    }
                    _mm_storeu_si128(reinterpret_cast<__m128i *>(m_vu0Data + destOff), _mm_loadu_si128(reinterpret_cast<const __m128i *>(lanes)));
                }
            }
            pos += totalBytes;
            if (pos > sizeBytes)
                break;
            continue;
        }
        else
        {
            break;
        }
    }
}

void PS2Memory::processVIF1Data(uint32_t srcPhys, uint32_t sizeBytes)
{
    if (sizeBytes == 0u || srcPhys >= PS2_RAM_SIZE)
        return;

    const uint64_t requestedEnd = static_cast<uint64_t>(srcPhys) + static_cast<uint64_t>(sizeBytes);
    if (requestedEnd > static_cast<uint64_t>(PS2_RAM_SIZE))
        sizeBytes = PS2_RAM_SIZE - srcPhys;

    processVIF1Data(m_rdram + srcPhys, sizeBytes);
}

void PS2Memory::processVIF1Data(const uint8_t *data, uint32_t sizeBytes)
{
    if (sizeBytes == 0u)
        return;
    if (m_vif1Carry.empty())
    {
        processVIF1Stream(data, sizeBytes);
        return;
    }
    std::vector<uint8_t> stream;
    stream.swap(m_vif1Carry);
    stream.insert(stream.end(), data, data + sizeBytes);
    processVIF1Stream(stream.data(), static_cast<uint32_t>(stream.size()));
}

void PS2Memory::processVIF1Stream(const uint8_t *data, uint32_t sizeBytes)
{
    uint32_t pos = 0;
    uint32_t cmdStart = 0;
    // Keep an incomplete command, from its VIFcode on, for the next transfer.
    auto carry = [&](uint32_t from)
    {
        static uint32_t carryLogs = 0;
        if (carryLogs++ < 16u)
            std::cerr << "[vif1:carry] bytes=" << (sizeBytes - from) << " cmd=0x" << std::hex
                      << vif1_regs.code << std::dec << std::endl;
        m_vif1Carry.assign(data + from, data + sizeBytes);
    };

    while (pos + 4 <= sizeBytes)
    {
        if (m_vif1PendingPath2ImageQwc != 0u)
        {
            // A GIF IMAGE can span distinct VIF DIRECT commands. Consume the
            // next DMA tag's NOP/DIRECT VIFcodes, not their bytes as texels.
            // Retain legacy raw continuation support when no such header exists.
            if (sizeBytes - pos >= 8u) {
                uint32_t first = 0u, direct = 0u;
                std::memcpy(&first, data + pos, 4u);
                std::memcpy(&direct, data + pos + 4u, 4u);
                const uint32_t opcode = (direct >> 24u) & 0x7fu;
                const uint32_t directQw = (direct & 0xffffu) ? (direct & 0xffffu) : 65536u;
                if (first == 0u && (opcode == VIF_DIRECT || opcode == VIF_DIRECTHL) &&
                    directQw == m_vif1PendingPath2ImageQwc) {
                    pos += 8u;
                    vif1_regs.code = direct;
                    vif1_regs.num = (direct >> 16u) & 0xffu;
                    if (direct & 0x80000000u) vif1_regs.stat |= 1u << 11u;
                    m_vif1PendingPath2DirectHl = opcode == VIF_DIRECTHL;
                }
            }
            const char *trace = std::getenv("PS2X_VIF_TRACE");
            static uint32_t continuationLogs = 0;
            if (trace && std::strcmp(trace, "1") == 0 && continuationLogs++ < 24u) {
                std::cerr << "[ff-vif-image-cont] pending=" << m_vif1PendingPath2ImageQwc
                          << " bytes=" << sizeBytes - pos << " pos=" << pos << " words=" << std::hex;
                for (uint32_t index = 0; index < 12u && pos + (index + 1u) * 4u <= sizeBytes; ++index) {
                    uint32_t word = 0;
                    std::memcpy(&word, data + pos + index * 4u, 4u);
                    std::cerr << word << ',';
                }
                std::cerr << std::dec << std::endl;
            }
            const uint32_t availableQw = (sizeBytes - pos) / 16u;
            if (availableQw == 0u)
            {
                break;
            }

            const uint32_t chunkQw = std::min<uint32_t>(m_vif1PendingPath2ImageQwc, availableQw);
            std::vector<uint8_t> imagePacket(16u + static_cast<size_t>(chunkQw) * 16u, 0u);
            const uint64_t imageTag =
                static_cast<uint64_t>(chunkQw & 0x7FFFu) |
                ((m_vif1PendingPath2ImageQwc == chunkQw) ? (1ull << 15) : 0ull) |
                (static_cast<uint64_t>(kGifFmtImage) << 58);
            std::memcpy(imagePacket.data(), &imageTag, sizeof(imageTag));
            std::memcpy(imagePacket.data() + 16u, data + pos, static_cast<size_t>(chunkQw) * 16u);
            submitGifPacket(GifPathId::Path2, imagePacket.data(), static_cast<uint32_t>(imagePacket.size()), true, m_vif1PendingPath2DirectHl);

            pos += chunkQw * 16u;
            m_vif1PendingPath2ImageQwc -= chunkQw;
            if (m_vif1PendingPath2ImageQwc == 0u)
            {
                m_vif1PendingPath2DirectHl = false;
            }
            continue;
        }

        cmdStart = pos;
        uint32_t cmd;
        memcpy(&cmd, data + pos, 4);
        pos += 4;

        uint8_t opcode = (cmd >> 24) & 0x7F;
        uint16_t imm = cmd & 0xFFFF;
        uint8_t num = (cmd >> 16) & 0xFF;
        const bool irq = (cmd & 0x80000000u) != 0u;

        {
            // Desync diagnostic: on the first impossible VIFcode, print the recent
            // command history (code, offset in transfer, transfer size, cycle).
            struct Recent { uint32_t cmd, offset, size, cycle, serial; };
            static std::array<Recent, 48> recent{};
            static uint32_t recentCount = 0, serial = 0, reports = 0;
            static const uint8_t *lastData = nullptr;
            if (data != lastData || cmdStart == 0u) { ++serial; lastData = data; }
            const bool unpackOk = (opcode & 0x60u) == 0x60u && ((opcode & 3u) != 3u || ((opcode >> 2) & 3u) == 3u);
            const bool known = opcode <= 0x07u || opcode == 0x10u || opcode == 0x11u || opcode == 0x13u || opcode == 0x14u ||
                               opcode == 0x15u || opcode == 0x17u || opcode == 0x20u || opcode == 0x30u || opcode == 0x31u ||
                               opcode == 0x4Au || opcode == 0x50u || opcode == 0x51u || unpackOk;
            if (!known && reports < 3u && std::getenv("PS2X_VIF_CMD_TRACE"))
            {
                ++reports;
                std::cerr << "[vif1:desync] bad=0x" << std::hex << cmd << " at=" << std::dec << cmdStart << "/" << sizeBytes << std::endl;
                if (reports == 1u)
                {
                    const uint32_t from = cmdStart > 4096u ? cmdStart - 4096u : 0u;
                    const uint32_t to = std::min(sizeBytes, cmdStart + 8192u);
                    if (FILE *file = std::fopen("analysis/vif1-desync.bin", "wb"))
                    {
                        std::fwrite(data + from, 1, to - from, file);
                        std::fclose(file);
                    }
                    std::cerr << "[vif1:desync] dumped transfer bytes " << from << ".." << to << std::endl;
                }
                for (uint32_t i = 0; i < 48u; ++i)
                {
                    const Recent &r = recent[(recentCount + i) % 48u];
                    if (r.cmd || r.size)
                        std::cerr << "[vif1:desync] code=0x" << std::hex << r.cmd << " cycle=0x" << r.cycle << std::dec
                                  << " at=" << r.offset << "/" << r.size << " xfer=" << r.serial << std::endl;
                }
            }
            recent[recentCount++ % 48u] = Recent{cmd, cmdStart, sizeBytes, vif1_regs.cycle, serial};
        }

        // Track most-recent command for VIFn_CODE emulation.
        vif1_regs.code = cmd;
        vif1_regs.num = num;
        if (irq)
        {
            vif1_regs.stat |= (1u << 11); // INT
            // The I bit raises INTC cause 5 (VIF1). Games use it to start PATH3 transfers in step with the VIF1 list
            // (MotorStorm AE kicks its texture uploads from that handler). The VIF does not stall here: the handler's
            // FBRST.STC release is accepted whenever it comes. PS2X_VIF1_IRQ=0 disables.
            static const bool vifIrq = [] { const char *e = std::getenv("PS2X_VIF1_IRQ"); return !(e && e[0] == '0'); }();
            if (vifIrq)
                stallVif1ForInterrupt();
        }

        if (opcode == VIF_NOP)
        {
            continue;
        }
        else if (opcode == VIF_STCYCL)
        {
            vif1_regs.cycle = imm;
            continue;
        }
        else if (opcode == VIF_OFFSET)
        {
            // VIF double-buffer setup. OFFSET clears DBF and resets TOPS to BASE.
            // Do not rewrite BASE from the previous TOPS value.
            vif1_regs.ofst = imm & 0x3FFu;
            vif1_regs.tops = vif1_regs.base & 0x3FFu;
            vif1_regs.stat &= ~(1u << 7); // clear DBF
            static const bool dbfInvert = std::getenv("PS2X_VIF_DBF_INVERT") != nullptr; // diagnostic: start on the other buffer
            if (dbfInvert)
            {
                vif1_regs.tops = (vif1_regs.base + vif1_regs.ofst) & 0x3FFu;
                vif1_regs.stat |= (1u << 7);
            }
            if (vifSeqLogOn())
                std::fprintf(stderr, "[vifseq] OFFSET %x (base %x) tops=%x\n", vif1_regs.ofst, vif1_regs.base, vif1_regs.tops);
            continue;
        }
        else if (opcode == VIF_BASE)
        {
            // BASE only updates the base register. TOPS changes on OFFSET/MSCAL.
            vif1_regs.base = imm & 0x3FFu;
            if (vifSeqLogOn())
                std::fprintf(stderr, "[vifseq] BASE %x\n", vif1_regs.base);
            continue;
        }
        else if (opcode == VIF_ITOP)
        {
            // ITOP VIFcode writes pending ITOPS; VU XITOP observes it after MSCAL/MSCNT.
            vif1_regs.itops = imm & 0x3FFu;
            continue;
        }
        else if (opcode == VIF_STMOD)
        {
            vif1_regs.mode = imm & 3u;
            continue;
        }
        else if (opcode == VIF_MSKPATH3)
        {
            // VIF command docs: MSKPATH3 uses IMMEDIATE bit 15.
            const bool wasMasked = m_path3Masked;
            m_path3Masked = (imm & 0x8000u) != 0u;
            if (wasMasked && !m_path3Masked)
                flushMaskedPath3Packets(true, true);
            continue;
        }
        else if (opcode == VIF_MARK)
        {
            vif1_regs.mark = imm;
            vif1_regs.stat |= (1u << 6); // MRK
            continue;
        }
        else if (opcode == VIF_FLUSHE || opcode == VIF_FLUSH || opcode == VIF_FLUSHA)
        {
            if (opcode == VIF_FLUSHA)
                flushMaskedPath3Packets();
            continue;
        }
        else if (opcode == VIF_MSCAL || opcode == VIF_MSCALF)
        {
            uint32_t startPC = (uint32_t)imm * 8u;
            if (vifSeqLogOn())
                std::fprintf(stderr, "[vifseq] MSCAL %x top=%x dbf=%u -> next tops=%x\n", startPC, vif1_regs.tops & 0x3FFu,
                             (vif1_regs.stat >> 7) & 1u, ((vif1_regs.stat >> 7) & 1u) ? vif1_regs.base : (vif1_regs.base + vif1_regs.ofst) & 0x3FFu);

            const uint32_t runTop = vif1_regs.tops & 0x3FFu;
            const uint32_t runItop = vif1_regs.itops & 0x3FFu;
            vif1_regs.top = runTop;
            vif1_regs.itop = runItop;

            const bool dbf = (vif1_regs.stat & (1u << 7)) != 0u;
            if (dbf)
                vif1_regs.tops = vif1_regs.base & 0x3FFu;
            else
                vif1_regs.tops = (vif1_regs.base + vif1_regs.ofst) & 0x3FFu;
            vif1_regs.stat ^= (1u << 7); // toggle DBF

            if (m_vu1MscalCallback)
                m_vu1MscalCallback(startPC, runTop, runItop);
            continue;
        }
        else if (opcode == VIF_MSCNT)
        {
            const uint32_t runTop = vif1_regs.tops & 0x3FFu;
            const uint32_t runItop = vif1_regs.itops & 0x3FFu;
            vif1_regs.top = runTop;
            vif1_regs.itop = runItop;

            const bool dbf = (vif1_regs.stat & (1u << 7)) != 0u;
            if (dbf)
                vif1_regs.tops = vif1_regs.base & 0x3FFu;
            else
                vif1_regs.tops = (vif1_regs.base + vif1_regs.ofst) & 0x3FFu;
            vif1_regs.stat ^= (1u << 7); // toggle DBF

            if (m_vu1MscntCallback)
                m_vu1MscntCallback(runTop, runItop);
            continue;
        }
        else if (opcode == VIF_STMASK)
        {
            if (pos + 4 > sizeBytes)
            {
                carry(cmdStart);
                return;
            }
            uint32_t maskValue = 0;
            std::memcpy(&maskValue, data + pos, sizeof(maskValue));
            vif1_regs.mask = maskValue;
            pos += 4;
            continue;
        }
        else if (opcode == VIF_STROW)
        {
            if (pos + 16 > sizeBytes)
            {
                carry(cmdStart);
                return;
            }
            std::memcpy(vif1_regs.row, data + pos, 16);
            pos += 16;
            continue;
        }
        else if (opcode == VIF_STCOL)
        {
            if (pos + 16 > sizeBytes)
            {
                carry(cmdStart);
                return;
            }
            std::memcpy(vif1_regs.col, data + pos, 16);
            pos += 16;
            continue;
        }
        else if (opcode == VIF_MPG)
        {
            const uint32_t destAddr = (uint32_t)imm * 8u;
            const uint32_t instructionCount = (num == 0u) ? 256u : static_cast<uint32_t>(num);
            const uint32_t mpgBytes = instructionCount * 8u;
            if (pos + mpgBytes > sizeBytes)
            {
                carry(cmdStart);
                return;
            }
            if (m_vu1Code && destAddr < PS2_VU1_CODE_SIZE)
            {
                // Games send their microprograms again and again: unchanged code changes nothing.
                const uint32_t copyBytes = std::min(mpgBytes, PS2_VU1_CODE_SIZE - destAddr);
                if (std::memcmp(m_vu1Code + destAddr, data + pos, copyBytes) != 0)
                {
                    std::memcpy(m_vu1Code + destAddr, data + pos, copyBytes);
                    markVU1CodeModified();
                }
            }
            pos += mpgBytes;
            continue;
        }
        else if (opcode == VIF_DIRECT || opcode == VIF_DIRECTHL)
        {
            uint32_t qwCount = imm;
            if (qwCount == 0)
                qwCount = 65536;
            if (static_cast<uint64_t>(pos) + qwCount * 16ull > sizeBytes)
            {
                carry(cmdStart);
                return;
            }

            if (qwCount > 0)
            {
                const bool directHl = (opcode == VIF_DIRECTHL);
                submitGifPacket(GifPathId::Path2, data + pos, qwCount * 16, true, directHl);

                const uint32_t pendingImageQw = pendingGifImageQwc(data + pos, qwCount * 16u);
                if (pendingImageQw != 0u)
                {
                    m_vif1PendingPath2ImageQwc = pendingImageQw;
                    m_vif1PendingPath2DirectHl = directHl;
                }
            }

            pos += qwCount * 16;
            continue;
        }
        else if ((opcode & 0x60) == 0x60)
        {
            uint8_t vn = (opcode >> 2) & 0x3;
            uint8_t vl = opcode & 0x3;
            const bool maskEnable = (opcode & 0x10u) != 0u;
            int components = vn + 1;
            int bitsPerComponent = 32;
            switch (vl)
            {
            case 0:
                bitsPerComponent = 32;
                break;
            case 1:
                bitsPerComponent = 16;
                break;
            case 2:
                bitsPerComponent = 8;
                break;
            case 3:
                bitsPerComponent = (vn == 3) ? 4 : 16;
                break;
            default:
                break;
            }
            int bitsPerVector = (vl == 3 && vn == 3) ? 16 : (components * bitsPerComponent);
            uint32_t bytesPerVector = (bitsPerVector + 7) / 8;
            // UNPACK semantics: NUM is 8-bit and NUM==0 means 256 vectors (writes).
            const uint32_t writeVectorCount = (num == 0u) ? 256u : static_cast<uint32_t>(num);

            // STCYCL controls write cycles for UNPACK.
            uint32_t cl = vif1_regs.cycle & 0xFFu;
            uint32_t wl = (vif1_regs.cycle >> 8) & 0xFFu;
            // CL=0 with WL>0 is a filling write that consumes no source vectors
            // (data = CL * (NUM / WL) + min(NUM % WL, CL)). An unset CYCLE
            // register (WL=0) keeps the straight 1:1 transfer.
            if (wl == 0u)
            {
                wl = 1u;
                if (cl == 0u)
                    cl = 1u;
            }

            uint32_t sourceVectorCount = writeVectorCount;
            if (cl < wl)
            {
                const uint32_t fullBlocks = writeVectorCount / wl;
                uint32_t remainder = writeVectorCount % wl;
                if (remainder > cl)
                    remainder = cl;
                sourceVectorCount = fullBlocks * cl + remainder;
            }

            uint32_t totalBytes = sourceVectorCount * bytesPerVector;
            totalBytes = (totalBytes + 3) & ~3u;

            uint32_t vuAddr = (uint32_t)imm & 0x3FFu;
            if ((imm & 0x8000u) != 0u)
                vuAddr = (vuAddr + (vif1_regs.tops & 0x3FFu)) & 0x3FFu;

            const bool zeroExtend = (imm & 0x4000u) != 0u;
            {
                static const bool keepHistory = std::getenv("GOW_VU1_KICKPRIM") != nullptr;
                if (keepHistory)
                {
                    UnpackHistoryEntry &h = g_unpackHistory[g_unpackHistoryNext++ & 255u];
                    h.vuStart = vuAddr;
                    h.vuCount = writeVectorCount;
                    h.tops = vif1_regs.tops & 0x3FFu;
                    h.opcode = opcode;
                    std::memset(h.bytes, 0, sizeof(h.bytes));
                    std::memcpy(h.bytes, data + pos, std::min<uint32_t>(64u, sizeBytes - pos));
                }
            }
            if (vifSeqLogOn())
            {
                float maxAbs = 0.0f;
                if (opcode == 0x6Cu && m_rdram)
                    for (uint32_t w = 0; w < std::min<uint32_t>(sourceVectorCount * 4u, (sizeBytes - pos) / 4u); ++w)
                    {
                        float f;
                        std::memcpy(&f, data + pos + w * 4u, 4);
                        if (f == f && std::fabs(f) > maxAbs && std::fabs(f) < 1.0e30f)
                            maxAbs = std::fabs(f);
                    }
                const unsigned src = (m_rdram && data + pos >= m_rdram && data + pos < m_rdram + PS2_RAM_SIZE) ? static_cast<unsigned>(data + pos - m_rdram) : 0u;
                std::fprintf(stderr, "[vifseq] UNPACK op=%02x num=%u imm=%04x -> vu %x..%x (tops %x) cl=%u wl=%u src=%x max=%g\n", opcode, num, imm, vuAddr,
                             vuAddr + writeVectorCount - 1u, vif1_regs.tops & 0x3FFu, cl, wl, src, maxAbs);
            }
            {
                // PS2X_VIF_UNPACKLOG=<seconds>:<count>: from <seconds> after the first UNPACK, logs <count> UNPACK
                // commands with their destination, cycle/mode/mask and the first source words as floats
                static const char *spec = std::getenv("PS2X_VIF_UNPACKLOG");
                static const double at = spec ? std::atof(spec) : -1.0;
                static const int maxLogs = spec && std::strchr(spec, ':') ? std::atoi(std::strchr(spec, ':') + 1) : 200;
                static const auto t0 = std::chrono::steady_clock::now();
                static int logs = 0;
                // PS2X_VIF_UNPACK_ADDR=<hex qword>: only UNPACKs whose destination range covers that VU address
                static const long onlyAddr = std::getenv("PS2X_VIF_UNPACK_ADDR") ? std::strtol(std::getenv("PS2X_VIF_UNPACK_ADDR"), nullptr, 16) : -1;
                const bool addrMatch = onlyAddr < 0 || (static_cast<uint32_t>(onlyAddr) >= vuAddr && static_cast<uint32_t>(onlyAddr) < vuAddr + writeVectorCount);
                if (at >= 0.0 && logs < maxLogs && addrMatch && pos + totalBytes <= sizeBytes &&
                    std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() >= at)
                {
                    ++logs;
                    std::fprintf(stderr, "[vif1:unpack] op=%02x vn=%u vl=%u num=%u addr=0x%x (imm=%04x tops=0x%x) cl=%u wl=%u mode=%u mask=%08x usn=%d bytes=%u:",
                                 opcode, vn, vl, num, vuAddr, imm, vif1_regs.tops & 0x3FFu, cl, wl, vif1_regs.mode, maskEnable ? vif1_regs.mask : 0u,
                                 zeroExtend ? 1 : 0, totalBytes);
                    if (m_rdram && data + pos >= m_rdram && data + pos < m_rdram + PS2_RAM_SIZE)
                        std::fprintf(stderr, " src=0x%x", static_cast<unsigned>(data + pos - m_rdram));
                    for (uint32_t w = 0; w < std::min<uint32_t>(totalBytes / 4u, 16u); ++w)
                    {
                        float f;
                        std::memcpy(&f, data + pos + w * 4u, 4);
                        std::fprintf(stderr, " %.4g", f);
                    }
                    std::fprintf(stderr, "\n");
                }
            }

            if (pos + totalBytes > sizeBytes)
            {
                carry(cmdStart);
                return;
            }
            static const bool unpackTrace = std::getenv("PS2X_VIF_CMD_TRACE") != nullptr;
            alignas(16) __m128i decodedVectors[256];
            if (m_vu1Data && cl >= wl && !unpackTrace &&
                decodeUnpackAny(vl, static_cast<uint32_t>(components), zeroExtend, data + pos, writeVectorCount, decodedVectors))
            {
                writeUnpack(m_vu1Data, decodedVectors, writeVectorCount, vuAddr, cl, wl, static_cast<uint32_t>(components), components == 1,
                            vl != 3u || vn != 3u, vif1_regs.mode & 3u, maskEnable, vif1_regs.mask, vif1_regs.row, vif1_regs.col);
            }
            else if (m_vu1Data) // CL=0 fills still write with no source bytes
            {
                const uint8_t *srcBase = data + pos;
                uint32_t srcIndex = 0u;
                for (uint32_t writeIndex = 0; writeIndex < writeVectorCount; ++writeIndex)
                {
                    const uint32_t cyclePos = writeIndex % wl;
                    const bool sourceAvailable = (cl >= wl) || (cyclePos < cl);

                    uint32_t destVec = 0;
                    if (cl >= wl)
                    {
                        destVec = (vuAddr + (writeIndex / wl) * cl + cyclePos) & 0x3FFu;
                    }
                    else
                    {
                        destVec = (vuAddr + writeIndex) & 0x3FFu;
                    }

                    uint32_t destOff = destVec * 16u;
                    if (destOff + 16u > PS2_VU1_DATA_SIZE)
                    {
                        if (sourceAvailable && srcIndex < sourceVectorCount)
                            ++srcIndex;
                        continue;
                    }

                    uint32_t lanes[4] = {0u, 0u, 0u, 0u};
                    std::memcpy(lanes, m_vu1Data + destOff, sizeof(lanes));
                    uint32_t decompressed[4] = {lanes[0], lanes[1], lanes[2], lanes[3]};
                    bool decoded = false;

                    const uint8_t *srcVec = nullptr;
                    if (sourceAvailable && srcIndex < sourceVectorCount)
                    {
                        srcVec = srcBase + srcIndex * bytesPerVector;
                        ++srcIndex;
                        decoded = true;
                    }

                    auto extend16 = [&](uint16_t raw) -> uint32_t
                    {
                        if (zeroExtend)
                            return static_cast<uint32_t>(raw);
                        return static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(raw)));
                    };

                    auto extend8 = [&](uint8_t raw) -> uint32_t
                    {
                        if (zeroExtend)
                            return static_cast<uint32_t>(raw);
                        return static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(raw)));
                    };

                    bool handledFormat = true;
                    if (!decoded)
                    {
                        handledFormat = false;
                    }
                    else if (vl == 0u)
                    {
                        if (components == 1)
                        {
                            uint32_t scalar = 0;
                            std::memcpy(&scalar, srcVec, sizeof(scalar));
                            decompressed[0] = scalar;
                            decompressed[1] = scalar;
                            decompressed[2] = scalar;
                            decompressed[3] = scalar;
                        }
                        else
                        {
                            const uint32_t limit = (components > 4) ? 4u : static_cast<uint32_t>(components);
                            for (uint32_t c = 0; c < limit; ++c)
                            {
                                uint32_t scalar = 0;
                                std::memcpy(&scalar, srcVec + c * 4u, sizeof(scalar));
                                decompressed[c] = scalar;
                            }
                        }
                    }
                    else if (vl == 1u)
                    {
                        if (components == 1)
                        {
                            uint16_t raw = 0;
                            std::memcpy(&raw, srcVec, sizeof(raw));
                            const uint32_t scalar = extend16(raw);
                            decompressed[0] = scalar;
                            decompressed[1] = scalar;
                            decompressed[2] = scalar;
                            decompressed[3] = scalar;
                        }
                        else
                        {
                            const uint32_t limit = (components > 4) ? 4u : static_cast<uint32_t>(components);
                            for (uint32_t c = 0; c < limit; ++c)
                            {
                                uint16_t raw = 0;
                                std::memcpy(&raw, srcVec + c * 2u, sizeof(raw));
                                decompressed[c] = extend16(raw);
                            }
                        }
                    }
                    else if (vl == 2u)
                    {
                        if (components == 1)
                        {
                            const uint32_t scalar = extend8(srcVec[0]);
                            decompressed[0] = scalar;
                            decompressed[1] = scalar;
                            decompressed[2] = scalar;
                            decompressed[3] = scalar;
                        }
                        else
                        {
                            const uint32_t limit = (components > 4) ? 4u : static_cast<uint32_t>(components);
                            for (uint32_t c = 0; c < limit; ++c)
                            {
                                decompressed[c] = extend8(srcVec[c]);
                            }
                        }
                    }
                    else if (vl == 3u && vn == 3u)
                    {
                        // V4-5: packed color-like format in a single 16-bit value.
                        uint16_t packed = 0;
                        std::memcpy(&packed, srcVec, sizeof(packed));
                        // V4-5 expands RGBA5551 to 8-bit lanes: RGB << 3, A << 7.
                        decompressed[0] = (packed & 0x1Fu) << 3;
                        decompressed[1] = ((packed >> 5) & 0x1Fu) << 3;
                        decompressed[2] = ((packed >> 10) & 0x1Fu) << 3;
                        decompressed[3] = ((packed >> 15) & 0x01u) << 7;
                    }
                    else
                    {
                        handledFormat = false;
                    }

                    // Unknown compressed format fallback: preserve legacy raw-copy behavior.
                    if (!handledFormat && decoded && !maskEnable && (vif1_regs.mode == 0u || vif1_regs.mode == 3u))
                    {
                        uint32_t copyBytes = (bytesPerVector < 16u) ? bytesPerVector : 16u;
                        std::memcpy(m_vu1Data + destOff, srcVec, copyBytes);
                        continue;
                    }

                    const bool canAdd = (vl != 3u || vn != 3u);
                    const uint32_t mode = vif1_regs.mode & 3u;
                    const uint32_t colIdx = (cyclePos > 3u) ? 3u : cyclePos;
                    const uint32_t maskCycle = (cyclePos > 3u) ? 3u : cyclePos;

                    for (uint32_t field = 0u; field < 4u; ++field)
                    {
                        uint32_t maskSpec = 0u;
                        if (maskEnable)
                        {
                            const uint32_t shift = ((maskCycle * 4u) + field) * 2u;
                            maskSpec = (vif1_regs.mask >> shift) & 0x3u;
                        }

                        // In fill-write cycles with suspended source reads, treat raw-data selections as row-fill.
                        if (!decoded && maskSpec == 0u)
                            maskSpec = 1u;

                        uint32_t writeVal = lanes[field];
                        if (maskSpec == 0u)
                        {
                            if (handledFormat)
                            {
                                writeVal = decompressed[field];
                                if (canAdd && (mode == 1u || mode == 2u))
                                {
                                    writeVal = writeVal + vif1_regs.row[field];
                                    if (mode == 2u)
                                        vif1_regs.row[field] = writeVal;
                                }
                            }
                        }
                        else if (maskSpec == 1u)
                        {
                            writeVal = vif1_regs.row[field];
                        }
                        else if (maskSpec == 2u)
                        {
                            writeVal = vif1_regs.col[colIdx];
                        }
                        else
                        {
                            continue; // write-protect
                        }

                        lanes[field] = writeVal;
                    }

                    std::memcpy(m_vu1Data + destOff, lanes, sizeof(lanes));
                    static const bool cmdTrace = std::getenv("PS2X_VIF_CMD_TRACE") != nullptr;
                    if (cmdTrace && destVec >= 0x3e0u)
                    {
                        static uint32_t cmdLogs = 0;
                        if (cmdLogs++ < 96u)
                            std::cerr << "[vif1:unpack-cmd] vu=0x" << std::hex << destVec << " op=0x" << static_cast<uint32_t>(opcode)
                                      << " num=" << static_cast<uint32_t>(num) << " imm=0x" << imm << " cycle=0x" << vif1_regs.cycle
                                      << " mode=" << vif1_regs.mode << " mask=" << (maskEnable ? vif1_regs.mask : 0u)
                                      << " lanes=" << lanes[0] << ',' << lanes[1] << ',' << lanes[2] << ',' << lanes[3]
                                      << std::dec << std::endl;
                    }
                }
            }
            pos += totalBytes;
            continue;
        }
        else
        {
            continue;
        }
    }
    if (pos < sizeBytes && m_vif1PendingPath2ImageQwc == 0u)
        carry(pos); // a VIFcode cut by the transfer boundary

    // No following remask in this scheduling slice: allow PATH3 to run freely.
    flushMaskedPath3Packets();
}
