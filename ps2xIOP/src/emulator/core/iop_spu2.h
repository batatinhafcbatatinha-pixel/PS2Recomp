#pragma once

// Native SPU2 (PS2 sound processor) model: 2 cores x 24 ADPCM voices, ADSR, loops, ENDX/NAX, IRQ on IRQA,
// AutoDMA (ADMA) PCM input and a stereo 48 kHz mix for the host. Clocked by IOP cycles (768 cycles per sample),
// so voice positions and IRQs follow guest time deterministically.
//
// IopMemory forwards every 16-bit access to 0x1F900000-0x1F9007FF here (read16 returns false for registers the
// model leaves to the plain register file) and DMA channel 4/7 transfers into sound RAM / the ADMA input.

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

namespace ps2x::iop::detail
{
    class IopSpu2
    {
    public:
        static constexpr uint32_t kRamBytes = 2u * 1024u * 1024u;
        static constexpr uint32_t kCyclesPerSample = 768u; // 36.864 MHz / 48 kHz

        IopSpu2();

        // Registers: offset = address - 0x1F900000 (0..0x7FF). write16 always stores the value.
        bool read16(uint32_t offset, uint16_t &value);
        void write16(uint32_t offset, uint16_t value);

        // DMA (IOP RAM -> SPU2). bytes is a multiple of 2.
        void dmaWrite(int core, const uint8_t *data, uint32_t bytes);
        bool admaActive(int core) const;

        void advanceTo(uint64_t cycle);
        bool takeIrq(); // true once per raised IRQ (the caller sets IOP INTC line 9)

        std::vector<uint8_t> &ram() { return m_ram; }

        // Host audio: interleaved stereo s16 at 48 kHz. Returns frames copied (missing frames are zero-filled).
        static uint32_t readOutput(int16_t *dst, uint32_t frames);
        static bool enabled();

    private:
        struct Voice
        {
            uint32_t ssa = 0, lsa = 0, nax = 0; // byte addresses in sound RAM
            uint32_t counter = 0;               // 12-bit fractional pitch counter
            int sampleIndex = 28;               // 0..27 within the decoded block, 28 = need decode
            int16_t decoded[28]{};
            int32_t prev1 = 0, prev2 = 0;
            int32_t envelope = 0;               // 0..0x7FFF
            int envPhase = 0;                   // 0 off, 1 attack, 2 decay, 3 sustain, 4 release
            int envCounter = 0;
            uint8_t blockFlags = 0;
            bool lsaSetBySoftware = false;
            int16_t last = 0, older = 0;        // for interpolation
        };
        struct Core
        {
            std::array<uint16_t, 0x200> reg{}; // shadow of the core's register file (offset/2)
            Voice voices[24];
            uint32_t endx = 0;
            uint32_t tsa = 0;                   // transfer address (bytes)
            std::vector<int16_t> admaL, admaR;  // ADMA input FIFO
            uint32_t admaRead = 0;
        };

        uint16_t &reg(int core, uint32_t off) { return m_core[core].reg[(off & 0x3FFu) >> 1]; }
        uint32_t irqAddress(int core) const;
        bool irqEnabled(int core) const;
        void checkIrq(int core, uint32_t addr, uint32_t len);
        void keyOn(int core, uint32_t mask);
        void keyOff(int core, uint32_t mask);
        void decodeBlock(int core, Voice &v);
        void stepEnvelope(int core, int vi, Voice &v);
        void tick();

        std::vector<uint8_t> m_ram;
        Core m_core[2];
        std::array<uint16_t, 0x40> m_common{}; // 0x760..0x7FF
        uint64_t m_cycle = 0;
        bool m_started = false;
        bool m_irqPending = false;
    };
}
