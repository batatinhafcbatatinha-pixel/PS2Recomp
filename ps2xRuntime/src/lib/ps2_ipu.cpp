// EE Image Processing Unit: intra-picture MPEG decoding (IDEC) and the bitstream commands around it.
// Tables are the MPEG-1/2 ones (ISO/IEC 13818-2 annex B); colour conversion uses the IPU's BT.601 studio-range
// matrix. Commands the games seen so far do not use (BDEC, VDEC, CSC, PACK) finish at once and are logged.
#include "runtime/ps2_ipu.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>
#include <cstdlib>
#include <cstring>
#include <iostream>

namespace
{
    struct Vlc
    {
        const char *code;
        int a, b; // value(s): run/level, size, increment...
    };

    // B.1 macroblock_address_increment (escape and stuffing handled separately).
    const Vlc kMbai[] = {
        {"1", 1, 0}, {"011", 2, 0}, {"010", 3, 0}, {"0011", 4, 0}, {"0010", 5, 0}, {"00011", 6, 0}, {"00010", 7, 0},
        {"0000111", 8, 0}, {"0000110", 9, 0}, {"00001011", 10, 0}, {"00001010", 11, 0}, {"00001001", 12, 0},
        {"00001000", 13, 0}, {"00000111", 14, 0}, {"00000110", 15, 0}, {"0000010111", 16, 0}, {"0000010110", 17, 0},
        {"0000010101", 18, 0}, {"0000010100", 19, 0}, {"0000010011", 20, 0}, {"0000010010", 21, 0},
        {"00000100011", 22, 0}, {"00000100010", 23, 0}, {"00000100001", 24, 0}, {"00000100000", 25, 0},
        {"00000011111", 26, 0}, {"00000011110", 27, 0}, {"00000011101", 28, 0}, {"00000011100", 29, 0},
        {"00000011011", 30, 0}, {"00000011010", 31, 0}, {"00000011001", 32, 0}, {"00000011000", 33, 0},
    };
    // B.12 / B.13 dct_dc_size.
    const Vlc kDcLum[] = {{"100", 0, 0}, {"00", 1, 0}, {"01", 2, 0}, {"101", 3, 0}, {"110", 4, 0}, {"1110", 5, 0},
                          {"11110", 6, 0}, {"111110", 7, 0}, {"1111110", 8, 0}, {"11111110", 9, 0},
                          {"111111110", 10, 0}, {"111111111", 11, 0}};
    const Vlc kDcChr[] = {{"00", 0, 0}, {"01", 1, 0}, {"10", 2, 0}, {"110", 3, 0}, {"1110", 4, 0}, {"11110", 5, 0},
                          {"111110", 6, 0}, {"1111110", 7, 0}, {"11111110", 8, 0}, {"111111110", 9, 0},
                          {"1111111110", 10, 0}, {"1111111111", 11, 0}};
    // B.14 DCT coefficients, table zero: (run, level), sign bit follows. run -1 = EOB, run -2 = escape.
    const Vlc kCoef0[] = {
        {"10", -1, 0}, {"000001", -2, 0},
        {"11", 0, 1}, {"011", 1, 1}, {"0100", 0, 2}, {"0101", 2, 1}, {"00101", 0, 3}, {"00111", 3, 1},
        {"00110", 4, 1}, {"000110", 1, 2}, {"000111", 5, 1}, {"000101", 6, 1}, {"000100", 7, 1}, {"0000110", 0, 4},
        {"0000100", 2, 2}, {"0000111", 8, 1}, {"0000101", 9, 1}, {"00100110", 0, 5}, {"00100001", 0, 6},
        {"00100101", 1, 3}, {"00100100", 3, 2}, {"00100111", 10, 1}, {"00100011", 11, 1}, {"00100010", 12, 1},
        {"00100000", 13, 1}, {"0000001010", 0, 7}, {"0000001100", 1, 4}, {"0000001011", 2, 3}, {"0000001111", 4, 2},
        {"0000001001", 5, 2}, {"0000001110", 14, 1}, {"0000001101", 15, 1}, {"0000001000", 16, 1},
        {"000000011101", 0, 8}, {"000000011000", 0, 9}, {"000000010011", 0, 10}, {"000000010000", 0, 11},
        {"000000011011", 1, 5}, {"000000010100", 2, 4}, {"000000011100", 3, 3}, {"000000010010", 4, 3},
        {"000000011110", 6, 2}, {"000000010101", 7, 2}, {"000000010001", 8, 2}, {"000000011111", 17, 1},
        {"000000011010", 18, 1}, {"000000011001", 19, 1}, {"000000010111", 20, 1}, {"000000010110", 21, 1},
        {"0000000011010", 0, 12}, {"0000000011001", 0, 13}, {"0000000011000", 0, 14}, {"0000000010111", 0, 15},
        {"0000000010110", 1, 6}, {"0000000010101", 1, 7}, {"0000000010100", 2, 5}, {"0000000010011", 3, 4},
        {"0000000010010", 5, 3}, {"0000000010001", 9, 2}, {"0000000010000", 10, 2}, {"0000000011111", 22, 1},
        {"0000000011110", 23, 1}, {"0000000011101", 24, 1}, {"0000000011100", 25, 1}, {"0000000011011", 26, 1},
        {"00000000011111", 0, 16}, {"00000000011110", 0, 17}, {"00000000011101", 0, 18}, {"00000000011100", 0, 19},
        {"00000000011011", 0, 20}, {"00000000011010", 0, 21}, {"00000000011001", 0, 22}, {"00000000011000", 0, 23},
        {"00000000010111", 0, 24}, {"00000000010110", 0, 25}, {"00000000010101", 0, 26}, {"00000000010100", 0, 27},
        {"00000000010011", 0, 28}, {"00000000010010", 0, 29}, {"00000000010001", 0, 30}, {"00000000010000", 0, 31},
        {"000000000011000", 0, 32}, {"000000000010111", 0, 33}, {"000000000010110", 0, 34},
        {"000000000010101", 0, 35}, {"000000000010100", 0, 36}, {"000000000010011", 0, 37},
        {"000000000010010", 0, 38}, {"000000000010001", 0, 39}, {"000000000010000", 0, 40},
        {"000000000011111", 1, 8}, {"000000000011110", 1, 9}, {"000000000011101", 1, 10},
        {"000000000011100", 1, 11}, {"000000000011011", 1, 12}, {"000000000011010", 1, 13},
        {"000000000011001", 1, 14}, {"0000000000010011", 1, 15}, {"0000000000010010", 1, 16},
        {"0000000000010001", 1, 17}, {"0000000000010000", 1, 18}, {"0000000000010100", 6, 3},
        {"0000000000011010", 11, 2}, {"0000000000011001", 12, 2}, {"0000000000011000", 13, 2},
        {"0000000000010111", 14, 2}, {"0000000000010110", 15, 2}, {"0000000000010101", 16, 2},
        {"0000000000011111", 27, 1}, {"0000000000011110", 28, 1}, {"0000000000011101", 29, 1},
        {"0000000000011100", 30, 1}, {"0000000000011011", 31, 1},
    };

    // B.15 DCT coefficients, table one (intra_vlc_format = 1); long codes are shared with table zero.
    const Vlc kCoef1[] = {
        {"0110", -1, 0}, {"000001", -2, 0}, {"10", 0, 1}, {"010", 1, 1}, {"110", 0, 2}, {"00101", 2, 1},
        {"0111", 0, 3}, {"00111", 3, 1}, {"000110", 4, 1}, {"00110", 1, 2}, {"000111", 5, 1}, {"0000110", 6, 1},
        {"0000100", 7, 1}, {"11100", 0, 4}, {"0000111", 2, 2}, {"0000101", 8, 1}, {"1111000", 9, 1},
        {"11101", 0, 5}, {"000101", 0, 6}, {"1111001", 1, 3}, {"00100110", 3, 2}, {"1111010", 10, 1},
        {"00100001", 11, 1}, {"00100101", 12, 1}, {"00100100", 13, 1}, {"000100", 0, 7}, {"00100111", 1, 4},
        {"11111100", 2, 3}, {"11111101", 4, 2}, {"000000100", 5, 2}, {"000000101", 14, 1}, {"000000111", 15, 1},
        {"0000001101", 16, 1}, {"1111011", 0, 8}, {"1111100", 0, 9}, {"00100011", 0, 10}, {"00100010", 0, 11},
        {"00100000", 1, 5}, {"0000001100", 2, 4}, {"11111010", 0, 12}, {"11111011", 0, 13}, {"11111110", 0, 14},
        {"11111111", 0, 15}, {"000000011100", 3, 3}, {"000000010010", 4, 3}, {"000000011110", 6, 2},
        {"000000010101", 7, 2}, {"000000010001", 8, 2}, {"000000011111", 17, 1}, {"000000011010", 18, 1},
        {"000000011001", 19, 1}, {"000000010111", 20, 1}, {"000000010110", 21, 1}, {"0000000010110", 1, 6},
        {"0000000010101", 1, 7}, {"0000000010100", 2, 5}, {"0000000010011", 3, 4}, {"0000000010010", 5, 3},
        {"0000000010001", 9, 2}, {"0000000010000", 10, 2}, {"0000000011111", 22, 1}, {"0000000011110", 23, 1},
        {"0000000011101", 24, 1}, {"0000000011100", 25, 1}, {"0000000011011", 26, 1}, {"00000000011111", 0, 16},
        {"00000000011110", 0, 17}, {"00000000011101", 0, 18}, {"00000000011100", 0, 19},
        {"00000000011011", 0, 20}, {"00000000011010", 0, 21}, {"00000000011001", 0, 22},
        {"00000000011000", 0, 23}, {"00000000010111", 0, 24}, {"00000000010110", 0, 25},
        {"00000000010101", 0, 26}, {"00000000010100", 0, 27}, {"00000000010011", 0, 28},
        {"00000000010010", 0, 29}, {"00000000010001", 0, 30}, {"00000000010000", 0, 31},
        {"000000000011000", 0, 32}, {"000000000010111", 0, 33}, {"000000000010110", 0, 34},
        {"000000000010101", 0, 35}, {"000000000010100", 0, 36}, {"000000000010011", 0, 37},
        {"000000000010010", 0, 38}, {"000000000010001", 0, 39}, {"000000000010000", 0, 40},
        {"000000000011111", 1, 8}, {"000000000011110", 1, 9}, {"000000000011101", 1, 10},
        {"000000000011100", 1, 11}, {"000000000011011", 1, 12}, {"000000000011010", 1, 13},
        {"000000000011001", 1, 14}, {"0000000000010011", 1, 15}, {"0000000000010010", 1, 16},
        {"0000000000010001", 1, 17}, {"0000000000010000", 1, 18}, {"0000000000010100", 6, 3},
        {"0000000000011010", 11, 2}, {"0000000000011001", 12, 2}, {"0000000000011000", 13, 2},
        {"0000000000010111", 14, 2}, {"0000000000010110", 15, 2}, {"0000000000010101", 16, 2},
        {"0000000000011111", 27, 1}, {"0000000000011110", 28, 1}, {"0000000000011101", 29, 1},
        {"0000000000011100", 30, 1}, {"0000000000011011", 31, 1},
    };

    const uint8_t kZigzag[64] = {0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,
                                 12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6,  7,  14, 21, 28,
                                 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
                                 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};
    const uint8_t kAlternate[64] = {0,  8,  16, 24, 1,  9,  2,  10, 17, 25, 32, 40, 48, 56, 57, 49,
                                    41, 33, 26, 18, 3,  11, 4,  12, 19, 27, 34, 42, 50, 58, 35, 43,
                                    51, 59, 20, 28, 5,  13, 6,  14, 21, 29, 36, 44, 52, 60, 37, 45,
                                    53, 61, 22, 30, 7,  15, 23, 31, 38, 46, 54, 62, 39, 47, 55, 63};
    const int kNonLinearQ[32] = {0,  1,  2,  3,  4,  5,  6,  7,  8,  10, 12, 14, 16, 18,  20,  22,
                                 24, 28, 32, 36, 40, 44, 48, 52, 56, 64, 72, 80, 88, 96, 104, 112};

    // VLC lookup: per code length, the codes of that length.
    struct VlcTable
    {
        struct Entry { uint32_t code; int a, b; };
        std::vector<Entry> byLength[18];
        template <size_t N>
        explicit VlcTable(const Vlc (&src)[N])
        {
            for (const Vlc &v : src)
            {
                const size_t len = std::strlen(v.code);
                uint32_t code = 0;
                for (size_t i = 0; i < len; ++i)
                    code = (code << 1u) | static_cast<uint32_t>(v.code[i] == '1');
                byLength[len].push_back({code, v.a, v.b});
            }
        }
    };

    const VlcTable &mbaiTable() { static const VlcTable t(kMbai); return t; }
    const VlcTable &dcLumTable() { static const VlcTable t(kDcLum); return t; }
    const VlcTable &dcChrTable() { static const VlcTable t(kDcChr); return t; }
    const VlcTable &coefTable() { static const VlcTable t(kCoef0); return t; }
    const VlcTable &coefTable1() { static const VlcTable t(kCoef1); return t; }

    struct IdctTable
    {
        double c[8][8];
        IdctTable()
        {
            for (int x = 0; x < 8; ++x)
                for (int u = 0; u < 8; ++u)
                    c[x][u] = (u == 0 ? std::sqrt(0.125) : 0.5) * std::cos((2 * x + 1) * u * 3.14159265358979323846 / 16.0);
        }
    };
    const IdctTable &idctTable() { static const IdctTable t; return t; }

    uint8_t clamp8(double v)
    {
        const long r = std::lround(v);
        return static_cast<uint8_t>(r < 0 ? 0 : (r > 255 ? 255 : r));
    }

    // PS2X_IPU_OUTDUMP=<dir>: every IDEC's decoded macroblocks go to <dir>/idec_<n>_<macroblocks>.raw (first 64).
    const char *outDumpDir()
    {
        static const char *dir = std::getenv("PS2X_IPU_OUTDUMP");
        return dir;
    }
    std::vector<uint8_t> &outDumpBuffer()
    {
        static std::vector<uint8_t> buf;
        return buf;
    }
    void outDumpFlush(int macroblocks)
    {
        static int n = 0;
        static const int skipCount = std::getenv("PS2X_IPU_OUTDUMP_SKIP") ? std::atoi(std::getenv("PS2X_IPU_OUTDUMP_SKIP")) : -1;
        if (macroblocks == skipCount)
        {
            outDumpBuffer().clear();
            return;
        }
        if (outDumpDir() && n < 64 && !outDumpBuffer().empty())
        {
            char path[1024];
            std::snprintf(path, sizeof(path), "%s/idec_%03d_%d.raw", outDumpDir(), n++, macroblocks);
            if (FILE *f = std::fopen(path, "wb"))
            {
                std::fwrite(outDumpBuffer().data(), 1, outDumpBuffer().size(), f);
                std::fclose(f);
            }
        }
        outDumpBuffer().clear();
    }

    bool ipuLog()
    {
        static const bool on = std::getenv("PS2X_IPU_LOG") != nullptr;
        return on;
    }
}

namespace { uint64_t g_ipuDropped = 0; } // bytes removed from the input FIFO so far (PS2X_IPU_DUMP offsets)

void Ps2Ipu::reset()
{
    g_ipuDropped += m_in.size();
    m_in.clear();
    m_bitPos = 0;
    m_out.clear();
    m_cmd = m_cmdData = 0;
    m_busy = m_cmdStarted = false;
    m_ecd = m_scd = false;
}

uint32_t Ps2Ipu::peek(unsigned bits) const
{
    if (bits == 0u)
        return 0u;
    if (m_bitPos + bits > m_in.size() * 8u)
        throw NeedBits{};
    uint32_t value = 0;
    size_t pos = m_bitPos;
    for (unsigned i = 0; i < bits; ++i, ++pos)
        value = (value << 1u) | ((m_in[pos >> 3u] >> (7u - (pos & 7u))) & 1u);
    return value;
}

uint32_t Ps2Ipu::get(unsigned bits)
{
    const uint32_t value = peek(bits);
    m_bitPos += bits;
    return value;
}

void Ps2Ipu::skip(unsigned bits)
{
    if (m_bitPos + bits > m_in.size() * 8u)
        throw NeedBits{};
    m_bitPos += bits;
}

void Ps2Ipu::compact()
{
    const size_t drop = (m_bitPos / 128u) * 16u;
    if (drop >= 65536u && drop <= m_in.size())
    {
        g_ipuDropped += drop;
        m_in.erase(m_in.begin(), m_in.begin() + static_cast<ptrdiff_t>(drop));
        m_bitPos -= drop * 8u;
    }
}

void Ps2Ipu::writeInFifo(const uint8_t *data, size_t bytes)
{
    static const char *dumpPath = std::getenv("PS2X_IPU_DUMP");
    if (dumpPath && !m_dump)
        m_dump = std::fopen(dumpPath, "wb");
    if (m_dump)
    {
        std::fwrite(data, 1, bytes, m_dump);
        std::fflush(m_dump);
    }
    m_in.insert(m_in.end(), data, data + bytes);
    run();
}

size_t Ps2Ipu::takeOutput(uint8_t *dst, size_t bytes)
{
    const size_t n = std::min(bytes, m_out.size());
    std::copy(m_out.begin(), m_out.begin() + static_cast<ptrdiff_t>(n), dst);
    m_out.erase(m_out.begin(), m_out.begin() + static_cast<ptrdiff_t>(n));
    return n;
}

void Ps2Ipu::writeCtrl(uint32_t value)
{
    if (value & (1u << 30u))
    {
        reset();
        return;
    }
    m_ctrl = value & 0x07F30000u;
}

uint32_t Ps2Ipu::readCtrl() const
{
    const size_t inBytes = m_in.size() > m_bitPos / 8u ? m_in.size() - m_bitPos / 8u : 0u;
    const uint32_t ifc = static_cast<uint32_t>(std::min<size_t>(8u, inBytes / 16u));
    const uint32_t ofc = static_cast<uint32_t>(std::min<size_t>(8u, m_out.size() / 16u));
    return ifc | (ofc << 4u) | (m_ecd ? 1u << 14u : 0u) | (m_scd ? 1u << 15u : 0u) | m_ctrl | (m_busy ? 0x80000000u : 0u);
}

uint32_t Ps2Ipu::readTop() const
{
    if (m_bitPos + 32u > m_in.size() * 8u)
        return 0u;
    return peek(32);
}

uint32_t Ps2Ipu::readTopHi() const
{
    return m_bitPos + 32u > m_in.size() * 8u ? 0x80000000u : 0u;
}

uint32_t Ps2Ipu::readBp() const
{
    const size_t inBytes = m_in.size() > m_bitPos / 8u ? m_in.size() - m_bitPos / 8u : 0u;
    return static_cast<uint32_t>(m_bitPos & 127u) | (static_cast<uint32_t>(std::min<size_t>(8u, inBytes / 16u)) << 8u);
}

void Ps2Ipu::writeCmd(uint32_t value)
{
    if (std::getenv("PS2X_IPU_DUMP"))
    {
        static unsigned logs = 0;
        if (logs++ < 4000u)
            std::fprintf(stderr, "[ipu-cmd] cmd=%08x ctrl=%08x bit=%llu\n", value, m_ctrl,
                         static_cast<unsigned long long>(g_ipuDropped * 8u + m_bitPos));
    }
    m_cmd = value;
    m_busy = true;
    m_cmdStarted = false;
    m_ecd = m_scd = false;
    run();
}

void Ps2Ipu::run()
{
    if (!m_busy)
        return;
    bool done = true;
    switch (m_cmd >> 28u)
    {
    case 0x0: // BCLR: drop the FIFO; BP is the bit position in the data that comes next
        g_ipuDropped += m_in.size();
        m_in.clear();
        m_bitPos = m_cmd & 0x7Fu;
        break;
    case 0x1:
        done = execIdec();
        break;
    case 0x4:
        done = execFdec();
        break;
    case 0x5:
        done = execSetIq();
        break;
    case 0x6:
        done = execSetVq();
        break;
    case 0x9: // SETTH
        m_th0 = m_cmd & 0x1FFu;
        m_th1 = (m_cmd >> 16u) & 0x1FFu;
        break;
    default:
    {
        static unsigned warned = 0;
        if (warned++ < 8u)
            std::cerr << "[ipu] command 0x" << std::hex << m_cmd << std::dec << " not implemented" << std::endl;
        break;
    }
    }
    if (done)
        m_busy = false;
    compact();
}

bool Ps2Ipu::execFdec()
{
    try
    {
        if (!m_cmdStarted)
        {
            skip(m_cmd & 0x3Fu);
            m_cmdStarted = true;
        }
        m_cmdData = peek(32);
        return true;
    }
    catch (const NeedBits &)
    {
        return false;
    }
}

bool Ps2Ipu::execSetIq()
{
    const size_t need = (m_cmdStarted ? 0u : (m_cmd & 0x3Fu)) + 512u;
    if (m_bitPos + need > m_in.size() * 8u)
        return false;
    if (!m_cmdStarted)
    {
        skip(m_cmd & 0x3Fu);
        m_cmdStarted = true;
    }
    std::array<uint8_t, 64> &iq = (m_cmd & (1u << 27u)) ? m_iqNonIntra : m_iqIntra;
    for (int i = 0; i < 64; ++i)
        iq[kZigzag[i]] = static_cast<uint8_t>(get(8)); // matrices arrive in zigzag order
    if (ipuLog())
    {
        static unsigned logs = 0;
        if (logs++ < 16u)
        {
            std::cerr << "[ipu] SETIQ " << ((m_cmd & (1u << 27u)) ? "non-intra" : "intra") << ":";
            for (int i = 0; i < 64; ++i)
                std::cerr << ' ' << static_cast<int>(iq[kZigzag[i]]);
            std::cerr << std::endl;
        }
    }
    return true;
}

bool Ps2Ipu::execSetVq()
{
    if (m_bitPos + 256u > m_in.size() * 8u)
        return false;
    for (int i = 0; i < 16; ++i)
    {
        const uint32_t lo = get(8), hi = get(8);
        m_vq[i] = static_cast<uint16_t>(lo | (hi << 8u));
    }
    return true;
}

int Ps2Ipu::getMbai()
{
    int increment = 0;
    for (;;)
    {
        if (peek(23) == 0u)
        {
            // Start code: leave the bitstream on its 0x000001 prefix (past the zero padding before it), where the
            // game's next FDEC expects it.
            while (peek(24) != 1u)
                skip(1);
            return -1;
        }
        const uint32_t code11 = peek(11);
        if (code11 == 0x0Fu) // stuffing
        {
            skip(11);
            continue;
        }
        if (code11 == 0x08u) // macroblock_escape
        {
            skip(11);
            increment += 33;
            continue;
        }
        break;
    }
    const VlcTable &t = mbaiTable();
    for (unsigned len = 1; len <= 11; ++len)
    {
        const uint32_t v = peek(len);
        for (const auto &e : t.byLength[len])
            if (e.code == v)
            {
                skip(len);
                return increment + e.a;
            }
    }
    m_ecd = true;
    return -2;
}

void Ps2Ipu::decodeIntraBlock(int component, int16_t out[64])
{
    int coef[64] = {};
    // DC
    const VlcTable &dcTable = component == 0 ? dcLumTable() : dcChrTable();
    int size = -1;
    for (unsigned len = 2; len <= 10 && size < 0; ++len)
    {
        const uint32_t v = peek(len);
        for (const auto &e : dcTable.byLength[len])
            if (e.code == v)
            {
                skip(len);
                size = e.a;
                break;
            }
    }
    if (size < 0)
    {
        m_ecd = true;
        throw NeedBits{}; // treated as an error by the caller (ECD set)
    }
    int diff = 0;
    if (size > 0)
    {
        const int raw = static_cast<int>(get(static_cast<unsigned>(size)));
        diff = (raw & (1 << (size - 1))) ? raw : raw - ((1 << size) - 1);
    }
    const int idp = static_cast<int>((m_ctrl >> 16u) & 3u);
    m_dcPred[component] += diff;
    coef[0] = m_dcPred[component] << (3 - idp);

    const bool mpeg1 = (m_ctrl & (1u << 23u)) != 0u;
    const bool alternate = (m_ctrl & (1u << 20u)) != 0u;
    const uint8_t *scan = alternate ? kAlternate : kZigzag;
    // m_qscaleCode holds the quantiser scale itself (as PCSX2's decoder.quantizer_scale): the command's QSC
    // as given, or the doubled / non-linear value of a macroblock's quantiser_scale_code.
    const int qscale = m_qscaleCode;
    const VlcTable &t = ((m_ctrl & (1u << 21u)) && !mpeg1) ? coefTable1() : coefTable();
    int i = 1;
    for (;;)
    {
        int run = 0, level = 0;
        bool found = false;
        for (unsigned len = 2; len <= 16 && !found; ++len)
        {
            const uint32_t v = peek(len);
            for (const auto &e : t.byLength[len])
                if (e.code == v)
                {
                    skip(len);
                    run = e.a;
                    level = e.b;
                    found = true;
                    break;
                }
        }
        if (!found)
        {
            if (peek(16) < 16u) // no code starts with eleven zeros: the IPU ends the block there (PCSX2)
                break;
            m_ecd = true;
            throw NeedBits{};
        }
        if (run == -1)
            break; // EOB
        if (run == -2)
        {
            run = static_cast<int>(get(6));
            if (mpeg1)
            {
                int l = static_cast<int>(get(8));
                if (l == 0)
                    l = static_cast<int>(get(8));
                else if (l == 128)
                    l = static_cast<int>(get(8)) - 256;
                else if (l > 128)
                    l -= 256;
                level = l;
            }
            else
            {
                level = static_cast<int>(get(12));
                if (level & 0x800)
                    level -= 4096;
            }
        }
        else if (get(1))
            level = -level;
        i += run;
        if (i > 63)
            break; // the IPU ends the block without an error (PCSX2)
        const int pos = scan[i];
        const int mag = level < 0 ? -level : level;
        static const bool iqScanIndex = std::getenv("PS2X_IPU_IQ_SCANIDX") != nullptr; // PCSX2: iq[i], not iq[pos]
        const int w = iqScanIndex ? m_iqIntra[kZigzag[i]] : m_iqIntra[pos];
        static const int qmul = std::getenv("PS2X_IPU_QMUL") ? std::atoi(std::getenv("PS2X_IPU_QMUL")) : 1;
        int value = (mag * qscale * qmul * w) >> 4;
        if (mpeg1)
            value = (value - 1) | 1; // oddification
        if (level < 0)
            value = -value;
        coef[pos] = std::clamp(value, -2048, 2047);
        ++i;
    }
    if (!mpeg1)
    {
        int sum = 0;
        for (int k = 0; k < 64; ++k)
            sum += coef[k];
        if ((sum & 1) == 0)
            coef[63] ^= 1;
    }
    for (int k = 0; k < 64; ++k)
        out[k] = static_cast<int16_t>(coef[k]);
}

void Ps2Ipu::idecMacroblock()
{
    // macroblock_type for an intra macroblock: '1' or '01' (+quant)
    // MotorStorm AE (CTRL.PCT = 0 throughout): its MPEG-2 streams (MP1 = 0) carry macroblock_type (+ dct_type)
    // per macroblock, exactly 1120 for a 640x448 picture, while its MPEG-1 streams (MP1 = 1) carry none and parse
    // exactly to their end without it. PS2X_IPU_IDEC_TYPE=always|never overrides.
    static const char *typeMode = std::getenv("PS2X_IPU_IDEC_TYPE");
    const bool mpeg1 = (m_ctrl & (1u << 23u)) != 0u;
    const bool readType = typeMode ? (typeMode[0] == 'a') : !mpeg1;
    bool quant = false;
    bool fieldDct = false;
    if (readType)
    {
        if (get(1) == 0u)
        {
            if (get(1) == 0u)
            {
                m_ecd = true;
                throw NeedBits{};
            }
            quant = true;
        }
        if (m_cmd & (1u << 24u))
            fieldDct = get(1) != 0u;
    }
    if (quant)
    {
        const int code = static_cast<int>(get(5));
        m_qscaleCode = (m_ctrl & (1u << 22u)) ? kNonLinearQ[code] : code << 1;
    }

    int16_t blocks[6][64];
    for (int b = 0; b < 6; ++b)
        decodeIntraBlock(b < 4 ? 0 : (b == 4 ? 1 : 2), blocks[b]);

    const IdctTable &it = idctTable();
    uint8_t pix[6][64];
    for (int b = 0; b < 6; ++b)
    {
        double tmp[8][8];
        for (int y = 0; y < 8; ++y)
            for (int u = 0; u < 8; ++u)
            {
                double s = 0.0;
                for (int v = 0; v < 8; ++v)
                    s += it.c[y][v] * blocks[b][v * 8 + u];
                tmp[y][u] = s;
            }
        for (int y = 0; y < 8; ++y)
            for (int x = 0; x < 8; ++x)
            {
                double s = 0.0;
                for (int u = 0; u < 8; ++u)
                    s += it.c[x][u] * tmp[y][u];
                pix[b][y * 8 + x] = clamp8(s);
            }
    }
    uint8_t y[256];
    for (int b = 0; b < 4; ++b)
        for (int r = 0; r < 8; ++r)
        {
            const int row = fieldDct ? (r * 2 + (b >> 1)) : (r + (b >> 1) * 8);
            std::memcpy(&y[row * 16 + (b & 1) * 8], &pix[b][r * 8], 8);
        }
    outputMacroblock(y, pix[4], pix[5]);
}

void Ps2Ipu::outputMacroblock(const uint8_t y[256], const uint8_t cb[64], const uint8_t cr[64])
{
    const bool rgb16 = (m_cmd & (1u << 27u)) != 0u;
    uint8_t buf[1024];
    size_t n = 0;
    for (int py = 0; py < 16; ++py)
        for (int px = 0; px < 16; ++px)
        {
            const int lum = (static_cast<int>(y[py * 16 + px]) - 16) * 149;
            const int u = static_cast<int>(cb[(py >> 1) * 8 + (px >> 1)]) - 128;
            const int v = static_cast<int>(cr[(py >> 1) * 8 + (px >> 1)]) - 128;
            const int r = std::clamp((lum + 204 * v + 64) >> 7, 0, 255);
            const int g = std::clamp((lum - 50 * u - 104 * v + 64) >> 7, 0, 255);
            const int b = std::clamp((lum + 258 * u + 64) >> 7, 0, 255);
            uint8_t a = 0x80;
            if (r < static_cast<int>(m_th0) && g < static_cast<int>(m_th0) && b < static_cast<int>(m_th0))
                a = 0;
            else if (r < static_cast<int>(m_th1) && g < static_cast<int>(m_th1) && b < static_cast<int>(m_th1))
                a = 0x40;
            if (rgb16)
            {
                const uint16_t p = static_cast<uint16_t>((r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10) | (a ? 0x8000 : 0));
                buf[n++] = static_cast<uint8_t>(p);
                buf[n++] = static_cast<uint8_t>(p >> 8);
            }
            else
            {
                buf[n++] = static_cast<uint8_t>(r);
                buf[n++] = static_cast<uint8_t>(g);
                buf[n++] = static_cast<uint8_t>(b);
                buf[n++] = a;
            }
        }
    m_out.insert(m_out.end(), buf, buf + n);
    if (outDumpDir())
        outDumpBuffer().insert(outDumpBuffer().end(), buf, buf + n);
}

bool Ps2Ipu::execIdec()
{
    static const bool mbaiFirst = std::getenv("PS2X_IPU_IDEC_MBAI_FIRST") != nullptr;
    if (!m_cmdStarted)
    {
        try
        {
            skip(m_cmd & 0x3Fu);
        }
        catch (const NeedBits &)
        {
            return false;
        }
        m_cmdStarted = true;
        const int idp = static_cast<int>((m_ctrl >> 16u) & 3u);
        m_dcPred = {128 << idp, 128 << idp, 128 << idp};
        m_qscaleCode = static_cast<int>((m_cmd >> 16u) & 0x1Fu);
        static const bool qscCode = std::getenv("PS2X_IPU_QSC_CODE") != nullptr; // QSC as a quantiser_scale_code
        if (qscCode)
            m_qscaleCode = (m_ctrl & (1u << 22u)) ? kNonLinearQ[m_qscaleCode] : m_qscaleCode << 1;
        m_idecMacroblocks = 0;
        m_idecFirst = !mbaiFirst;
    }
    for (;;)
    {
        const Snapshot snap{m_bitPos, m_dcPred, m_qscaleCode};
        const size_t outBefore = m_out.size();
        try
        {
            if (!m_idecFirst)
            {
                const int inc = getMbai();
                if (inc < 0)
                {
                    m_scd = inc == -1;
                    outDumpFlush(m_idecMacroblocks);
                    if (ipuLog())
                        std::cerr << "[ipu] IDEC done: " << m_idecMacroblocks << " macroblocks, "
                                  << (inc == -1 ? "start code" : "bad address increment") << " ctrl=0x" << std::hex << m_ctrl
                                  << " cmd=0x" << m_cmd << std::dec << std::endl;
                    return true;
                }
                if (inc > 1)
                {
                    const int idp = static_cast<int>((m_ctrl >> 16u) & 3u);
                    m_dcPred = {128 << idp, 128 << idp, 128 << idp};
                }
            }
            idecMacroblock();
            m_idecFirst = false;
            ++m_idecMacroblocks;
        }
        catch (const NeedBits &)
        {
            m_out.resize(outBefore);
            if (m_ecd)
            {
                if (ipuLog())
                {
                    std::cerr << "[ipu] IDEC error after " << m_idecMacroblocks << " macroblocks ctrl=0x" << std::hex << m_ctrl
                              << " cmd=0x" << m_cmd << std::dec << " mb bits:";
                    for (size_t b = snap.bitPos; b < snap.bitPos + 160u && b < m_in.size() * 8u; ++b)
                        std::cerr << ((m_in[b >> 3u] >> (7u - (b & 7u))) & 1u);
                    std::cerr << " fail at +" << (m_bitPos - snap.bitPos) << std::endl;
                }
                return true;
            }
            m_bitPos = snap.bitPos;
            m_dcPred = snap.dcPred;
            m_qscaleCode = snap.qscaleCode;
            return false;
        }
    }
}
