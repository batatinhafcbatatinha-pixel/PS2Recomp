#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <vector>

// Image Processing Unit (MPEG-2 intra decoder of the EE). The input FIFO is fed by TO_IPU DMA (channel 4) or by
// 128-bit stores to IPU_in_FIFO; decoded data leaves through FROM_IPU DMA (channel 3) or IPU_out_FIFO.
// A command that runs out of input stays BUSY and resumes when more data arrives, as on the console: games stream
// the bitstream in chunks and refill only while the IPU reports BUSY with channel 4 idle.
class Ps2Ipu
{
public:
    void reset();

    void writeCmd(uint32_t value);
    void writeCtrl(uint32_t value);
    uint32_t readCmd() const { return m_cmdData; }
    uint32_t readCmdHi() const { return m_busy ? 0x80000000u : 0u; }
    uint32_t readCtrl() const;
    uint32_t readTop() const;
    uint32_t readTopHi() const;
    uint32_t readBp() const;

    void writeInFifo(const uint8_t *data, size_t bytes);
    size_t outputBytes() const { return m_out.size(); }
    size_t takeOutput(uint8_t *dst, size_t bytes);
    bool busy() const { return m_busy; }

    // Runs the current command as far as the input allows.
    void run();

private:
    struct NeedBits {};
    struct Snapshot
    {
        size_t bitPos;
        std::array<int, 3> dcPred;
        int qscaleCode;
    };

    size_t availableBits() const { return m_in.size() * 8u - m_bitPos; }
    uint32_t peek(unsigned bits) const;          // throws NeedBits
    uint32_t get(unsigned bits);                 // throws NeedBits
    void skip(unsigned bits);
    void compact();

    bool execFdec();
    bool execSetIq();
    bool execSetVq();
    bool execIdec();
    void idecMacroblock();
    int getMbai();                               // -1 = start code ahead
    void decodeIntraBlock(int component, int16_t out[64]);
    void outputMacroblock(const uint8_t y[256], const uint8_t cb[64], const uint8_t cr[64]);

    std::vector<uint8_t> m_in;
    size_t m_bitPos = 0;
    std::deque<uint8_t> m_out;

    uint32_t m_cmd = 0, m_cmdData = 0;
    bool m_busy = false, m_cmdStarted = false;
    uint32_t m_ctrl = 0; // IDP, AS, IVF, QST, MP1, PCT as written
    bool m_ecd = false, m_scd = false;

    std::array<uint8_t, 64> m_iqIntra{}, m_iqNonIntra{};
    std::array<uint16_t, 16> m_vq{};
    uint32_t m_th0 = 0, m_th1 = 0;

    // IDEC state
    std::array<int, 3> m_dcPred{};
    int m_qscaleCode = 1;
    uint32_t m_idecMacroblocks = 0;
    bool m_idecFirst = true;
    FILE *m_dump = nullptr;
};
