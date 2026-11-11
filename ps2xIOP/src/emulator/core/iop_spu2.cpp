#include "iop_spu2.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <cstdio>

namespace ps2x::iop::detail
{
    namespace
    {
        // Core register offsets (core 1 = +0x400)
        constexpr uint32_t kAttr = 0x19A, kIrqaHi = 0x19C, kIrqaLo = 0x19E, kKonLo = 0x1A0, kKonHi = 0x1A2,
                           kKoffLo = 0x1A4, kKoffHi = 0x1A6, kTsaHi = 0x1A8, kTsaLo = 0x1AA, kData = 0x1AC,
                           kAdmas = 0x1B0, kEndxLo = 0x340, kEndxHi = 0x342;
        constexpr uint32_t kIrqInfo = 0x7C2;
        // Mixer routing and volumes (SPU2 register map as documented publicly and implemented in PCSX2, GPL-3.0):
        // per core VMIXL/VMIXR (+2: voices 16-23) send voices to the dry output, MMIX gates the core's inputs, and in
        // the common area each core has MVOL (master), EVOL (effect return), AVOL (external input: core 0's output
        // into core 1) and BVOL (sound data input: ADMA). Core 1's output is the console's output.
        constexpr uint32_t kVmixl = 0x188, kVmixr = 0x190, kMmix = 0x198;
        constexpr uint32_t kCommonCoreStride = 0x28; // core 1's volume block: 0x788 (core 0's: 0x760)
        constexpr uint32_t kMvol = 0x0, kAvol = 0x8, kBvol = 0xC; // byte offsets inside a core's volume block (L, R+2)
        // MMIX bits: dry gates of the external input (core 0 -> core 1), the sound data input (ADMA) and the voices.
        constexpr uint16_t kMmixDryExtR = 0x004, kMmixDryExtL = 0x008, kMmixDryInpR = 0x040, kMmixDryInpL = 0x080,
                           kMmixDrySndR = 0x400, kMmixDrySndL = 0x800;
        // The routed mix below (VMIX/MMIX gates, MVOL/AVOL/BVOL) is the default; tried on The Sopranos (2026-11-10) it
        // plays sound but its gameplay clipping remains (likely specific to that game). PS2X_SPU2_OLD_MIX=1: the previous
        // mix (every voice and ADMA at full level, no master volume).
        const bool g_oldMix = [] { const char *e = std::getenv("PS2X_SPU2_OLD_MIX"); return e && e[0] == '1'; }();
        bool g_mixSeen = false; // the game wrote the mixer registers: until then the old mix (nothing goes silent)
        int g_mixLogLeft = 48;  // the first mixer register writes go to the log
        // A 15-bit volume register (bit 14 = sign) as a factor of 0x4000; sweep mode (bit 15) is taken at full level.
        int32_t volumeFactor(uint16_t vol) { return (vol & 0x8000u) ? 0x3FFF : static_cast<int32_t>(static_cast<int16_t>(vol << 1)) / 2; }
        // AVOL/BVOL are plain signed 16-bit levels (0x7FFF = full; Sopranos writes core 1 AVOL = 0x7FFF), no sweep bit.
        int32_t levelFactor(uint16_t vol) { return static_cast<int32_t>(static_cast<int16_t>(vol)) / 2; }
        constexpr int32_t kFilter[5][2] = {{0, 0}, {60, 0}, {115, -52}, {98, -55}, {122, -60}};

        // SPSC ring of interleaved stereo frames for the host audio thread.
        constexpr uint32_t kRingFrames = 1u << 15;
        int16_t g_ring[kRingFrames * 2];
        std::atomic<uint32_t> g_write{0}, g_read{0};

        uint32_t wordAddress(uint16_t hi, uint16_t lo) { return ((((uint32_t)hi & 0xFu) << 16) | lo) * 2u; }

        // PS2X_SPU2_LOG=1: one line per emulated second with key-on/IRQ/register traffic (diagnostics only).
        const bool g_log = [] { const char *e = std::getenv("PS2X_SPU2_LOG"); return e && e[0] == '1'; }();
        struct Stats { uint32_t konCalls = 0, konVoices = 0, koff = 0, irqs = 0, reads = 0, writes = 0, dma = 0, admaBytes = 0, ticks = 0; uint32_t readAt[0x400] = {}, writeAt[0x400] = {}; } g_stats;
        // the three busiest register offsets (byte offset into the 0x800 window) of a histogram
        void printTop(const char *label, const uint32_t (&h)[0x400])
        {
            uint32_t best[3] = {0, 0, 0}, at[3] = {0, 0, 0};
            for (uint32_t i = 0; i < 0x400u; ++i)
                for (int k = 0; k < 3; ++k)
                    if (h[i] > best[k])
                    {
                        for (int j = 2; j > k; --j) { best[j] = best[j - 1]; at[j] = at[j - 1]; }
                        best[k] = h[i];
                        at[k] = i * 2u;
                        break;
                    }
            std::fprintf(stderr, " %s", label);
            for (int k = 0; k < 3 && best[k]; ++k)
                std::fprintf(stderr, " %03x:%u", at[k], best[k]);
        }
    }

    bool IopSpu2::enabled()
    {
        static const bool on = [] { const char *e = std::getenv("PS2X_SPU2"); return !(e && e[0] == '0'); }();
        return on;
    }

    uint32_t IopSpu2::readOutput(int16_t *dst, uint32_t frames)
    {
        uint32_t r = g_read.load(std::memory_order_acquire);
        const uint32_t w = g_write.load(std::memory_order_acquire);
        uint32_t n = 0;
        for (; n < frames && r != w; ++n, ++r)
        {
            dst[n * 2] = g_ring[(r % kRingFrames) * 2];
            dst[n * 2 + 1] = g_ring[(r % kRingFrames) * 2 + 1];
        }
        g_read.store(r, std::memory_order_release);
        if (n < frames)
            std::memset(dst + n * 2, 0, (frames - n) * 4u);
        return n;
    }

    IopSpu2::IopSpu2() : m_ram(kRamBytes, 0u) {}
}

namespace ps2x::iop
{
    uint32_t spu2ReadOutput(int16_t *interleavedStereo, uint32_t frames)
    {
        return detail::IopSpu2::readOutput(interleavedStereo, frames);
    }
    bool spu2Enabled() { return detail::IopSpu2::enabled(); }
}

namespace ps2x::iop::detail
{

    uint32_t IopSpu2::irqAddress(int core) const
    {
        const auto &r = m_core[core].reg;
        return wordAddress(r[kIrqaHi >> 1], r[kIrqaLo >> 1]);
    }

    bool IopSpu2::irqEnabled(int core) const { return (m_core[core].reg[kAttr >> 1] & 0x40u) != 0u; }

    void IopSpu2::checkIrq(int core, uint32_t addr, uint32_t len)
    {
        for (int c = 0; c < 2; ++c) // either core's IRQ address watches every access to sound RAM
        {
            (void)core;
            if (!irqEnabled(c))
                continue;
            const uint32_t irqa = irqAddress(c);
            if (irqa >= addr && irqa < addr + len)
            {
                m_common[(kIrqInfo - 0x760u) >> 1] |= static_cast<uint16_t>(4u << c);
                m_irqPending = true;
                ++g_stats.irqs;
            }
        }
    }

    bool IopSpu2::takeIrq()
    {
        const bool p = m_irqPending;
        m_irqPending = false;
        return p;
    }

    bool IopSpu2::admaActive(int core) const { return (m_core[core].reg[kAdmas >> 1] & (1u << core)) != 0u; }

    void IopSpu2::dmaWrite(int core, const uint8_t *data, uint32_t bytes)
    {
        Core &c = m_core[core];
        ++g_stats.dma;
        if (admaActive(core))
        {
            g_stats.admaBytes += bytes;
            // ADMA: blocks of 512 bytes left, 512 bytes right (256 samples each), into the core's PCM input
            for (uint32_t off = 0; off + 1024u <= bytes; off += 1024u)
            {
                for (uint32_t i = 0; i < 256u; ++i)
                {
                    int16_t l, r;
                    std::memcpy(&l, data + off + i * 2u, 2);
                    std::memcpy(&r, data + off + 512u + i * 2u, 2);
                    c.admaL.push_back(l);
                    c.admaR.push_back(r);
                }
            }
            const size_t maxFifo = 48000u; // one second; drop the oldest beyond that
            if (c.admaL.size() - c.admaRead > maxFifo)
                c.admaRead = static_cast<uint32_t>(c.admaL.size() - maxFifo);
            if (c.admaRead > 65536u)
            {
                c.admaL.erase(c.admaL.begin(), c.admaL.begin() + c.admaRead);
                c.admaR.erase(c.admaR.begin(), c.admaR.begin() + c.admaRead);
                c.admaRead = 0;
            }
            return;
        }
        const uint32_t start = c.tsa % kRamBytes;
        for (uint32_t i = 0; i < bytes; ++i)
            m_ram[(start + i) % kRamBytes] = data[i];
        checkIrq(core, start, bytes);
        c.tsa = (start + bytes) % kRamBytes;
    }

    bool IopSpu2::read16(uint32_t offset, uint16_t &value)
    {
        ++g_stats.reads;
        ++g_stats.readAt[(offset >> 1) & 0x3FFu];
        if (offset >= 0x760u)
        {
            if (offset == kIrqInfo)
            {
                value = m_common[(kIrqInfo - 0x760u) >> 1];
                return true;
            }
            return false;
        }
        const int core = offset >= 0x400u ? 1 : 0;
        const uint32_t off = offset & 0x3FFu;
        Core &c = m_core[core];
        if (off < 0x180u && (off & 0xFu) == 0xAu)
        {
            value = static_cast<uint16_t>(c.voices[off >> 4].envelope);
            return true;
        }
        if (off >= 0x1C0u && off < 0x1C0u + 24u * 0xCu)
        {
            const uint32_t v = (off - 0x1C0u) / 0xCu, field = (off - 0x1C0u) % 0xCu;
            const uint32_t word = c.voices[v].nax / 2u;
            if (field == 8u) { value = static_cast<uint16_t>((word >> 16) & 0xFu); return true; }
            if (field == 0xAu) { value = static_cast<uint16_t>(word & 0xFFFFu); return true; }
            return false;
        }
        if (off == kEndxLo) { value = static_cast<uint16_t>(c.endx & 0xFFFFu); return true; }
        if (off == kEndxHi) { value = static_cast<uint16_t>(c.endx >> 16); return true; }
        if (off == kTsaHi) { value = static_cast<uint16_t>((c.tsa / 2u) >> 16); return true; }
        if (off == kTsaLo) { value = static_cast<uint16_t>((c.tsa / 2u) & 0xFFFFu); return true; }
        return false;
    }

    void IopSpu2::write16(uint32_t offset, uint16_t value)
    {
        ++g_stats.writes;
        ++g_stats.writeAt[(offset >> 1) & 0x3FFu];
        if (offset >= 0x760u)
        {
            m_common[((offset - 0x760u) >> 1) & 0x3Fu] = value;
            const uint32_t rel = (offset - 0x760u) % kCommonCoreStride;
            if (offset < 0x760u + 2u * kCommonCoreStride && (rel < 0x4u || (rel >= kAvol && rel < kBvol + 0x4u)))
            {
                g_mixSeen = true;
                if (g_mixLogLeft > 0 && g_mixLogLeft--)
                    std::fprintf(stderr, "[spu2-mix] %03x = %04x\n", offset, value);
            }
            return;
        }
        const int core = offset >= 0x400u ? 1 : 0;
        const uint32_t off = offset & 0x3FFu;
        Core &c = m_core[core];
        c.reg[off >> 1] = value;
        if (off >= kVmixl && off < kMmix + 2u)
        {
            g_mixSeen = true;
            if (g_mixLogLeft > 0 && g_mixLogLeft--)
                std::fprintf(stderr, "[spu2-mix] core%d %03x = %04x\n", core, off, value);
        }
        if (off >= 0x1C0u && off < 0x1C0u + 24u * 0xCu)
        {
            const uint32_t v = (off - 0x1C0u) / 0xCu, field = (off - 0x1C0u) % 0xCu;
            Voice &voice = c.voices[v];
            const uint16_t *r = &c.reg[(0x1C0u + v * 0xCu) >> 1];
            if (field <= 2u)
                voice.ssa = wordAddress(r[0], r[1]);
            else if (field <= 6u)
            {
                voice.lsa = wordAddress(r[2], r[3]);
                voice.lsaSetBySoftware = true;
            }
            return;
        }
        switch (off)
        {
        case kKonLo: keyOn(core, value); break;
        case kKonHi: keyOn(core, static_cast<uint32_t>(value & 0xFFu) << 16); break;
        case kKoffLo: keyOff(core, value); break;
        case kKoffHi: keyOff(core, static_cast<uint32_t>(value & 0xFFu) << 16); break;
        // A write clears its own half only (voices 0-15 / 16-23), whatever the value: MultiStream polls and clears
        // the two halves separately, and clearing all 24 bits on the first write lost end flags set in between.
        case kEndxLo: c.endx &= 0xFF0000u; break;
        case kEndxHi: c.endx &= 0x00FFFFu; break;
        case kTsaHi: case kTsaLo: c.tsa = wordAddress(c.reg[kTsaHi >> 1], c.reg[kTsaLo >> 1]); break;
        case kData:
            m_ram[c.tsa % kRamBytes] = static_cast<uint8_t>(value);
            m_ram[(c.tsa + 1u) % kRamBytes] = static_cast<uint8_t>(value >> 8);
            checkIrq(core, c.tsa, 2);
            c.tsa = (c.tsa + 2u) % kRamBytes;
            break;
        case kAttr:
            if ((value & 0x40u) == 0u)
                m_common[(kIrqInfo - 0x760u) >> 1] &= static_cast<uint16_t>(~(4u << core));
            break;
        default: break;
        }
    }

    void IopSpu2::keyOn(int core, uint32_t mask)
    {
        Core &c = m_core[core];
        if (mask)
            ++g_stats.konCalls;
        for (int v = 0; v < 24; ++v)
        {
            if (!(mask & (1u << v)))
                continue;
            ++g_stats.konVoices;
            Voice &voice = c.voices[v];
            voice.nax = voice.ssa;
            if (!voice.lsaSetBySoftware)
                voice.lsa = voice.ssa;
            voice.lsaSetBySoftware = false;
            voice.counter = 0;
            voice.sampleIndex = 28;
            voice.prev1 = voice.prev2 = 0;
            voice.last = voice.older = 0;
            voice.envelope = 0;
            voice.envPhase = 1;
            voice.envCounter = 0;
            c.endx &= ~(1u << v);
        }
    }

    void IopSpu2::keyOff(int core, uint32_t mask)
    {
        if (mask)
            ++g_stats.koff;
        for (int v = 0; v < 24; ++v)
            if (mask & (1u << v))
            {
                Voice &voice = m_core[core].voices[v];
                if (voice.envPhase != 0)
                {
                    voice.envPhase = 4;
                    voice.envCounter = 0;
                }
            }
    }

    void IopSpu2::decodeBlock(int core, Voice &v)
    {
        const uint32_t a = v.nax % kRamBytes;
        const uint8_t header = m_ram[a], flags = m_ram[(a + 1u) % kRamBytes];
        int shift = header & 0xF;
        if (shift > 12)
            shift = 9;
        int filter = (header >> 4) & 7;
        if (filter > 4)
            filter = 0;
        if ((flags & 4u) && !v.lsaSetBySoftware)
            v.lsa = a;
        v.blockFlags = flags;
        for (int i = 0; i < 28; ++i)
        {
            const uint8_t byte = m_ram[(a + 2u + static_cast<uint32_t>(i / 2)) % kRamBytes];
            const int nibble = (i & 1) ? (byte >> 4) : (byte & 0xF);
            int32_t s = (static_cast<int32_t>(static_cast<int16_t>(nibble << 12)) >> shift);
            s += (v.prev1 * kFilter[filter][0] + v.prev2 * kFilter[filter][1] + 32) >> 6;
            s = std::clamp(s, -32768, 32767);
            v.decoded[i] = static_cast<int16_t>(s);
            v.prev2 = v.prev1;
            v.prev1 = s;
        }
        checkIrq(core, a, 16);
        v.sampleIndex = 0;
    }

    void IopSpu2::stepEnvelope(int core, int vi, Voice &v)
    {
        const uint16_t adsr1 = m_core[core].reg[(vi * 0x10 + 6) >> 1];
        const uint16_t adsr2 = m_core[core].reg[(vi * 0x10 + 8) >> 1];
        int shift = 0, step = 0;
        bool exponential = false, decrease = false;
        switch (v.envPhase)
        {
        case 1: // attack
            exponential = (adsr1 & 0x8000u) != 0;
            shift = (adsr1 >> 10) & 0x1F;
            step = 7 - ((adsr1 >> 8) & 3);
            break;
        case 2: // decay
            exponential = true;
            decrease = true;
            shift = ((adsr1 >> 4) & 0xF) << 2;
            step = -8;
            break;
        case 3: // sustain
            exponential = (adsr2 & 0x8000u) != 0;
            decrease = (adsr2 & 0x4000u) != 0;
            shift = (adsr2 >> 8) & 0x1F;
            step = decrease ? -8 + ((adsr2 >> 6) & 3) : 7 - ((adsr2 >> 6) & 3);
            break;
        case 4: // release
            exponential = (adsr2 & 0x20u) != 0;
            decrease = true;
            shift = (adsr2 & 0x1F) << 2;
            step = -8;
            break;
        default:
            return;
        }
        if (v.envCounter > 0)
        {
            --v.envCounter;
            return;
        }
        int cycles = 1 << std::max(0, shift - 11);
        int32_t delta = step << std::max(0, 11 - shift);
        if (exponential && !decrease && v.envelope > 0x6000)
            cycles *= 4;
        if (exponential && decrease)
            delta = (delta * v.envelope) >> 15;
        v.envCounter = cycles - 1;
        v.envelope = std::clamp(v.envelope + delta, 0, 0x7FFF);
        if (v.envPhase == 1 && v.envelope >= 0x7FFF)
        {
            v.envPhase = 2;
            v.envCounter = 0;
        }
        else if (v.envPhase == 2)
        {
            const int32_t sustain = ((adsr1 & 0xF) + 1) * 0x800;
            if (v.envelope <= sustain)
            {
                v.envPhase = 3;
                v.envCounter = 0;
            }
        }
        else if (v.envPhase == 4 && v.envelope == 0)
            v.envPhase = 0;
    }

    void IopSpu2::tick()
    {
        if (g_log && ++g_stats.ticks >= 48000u)
        {
            std::fprintf(stderr, "[spu2] kon=%u/%u koff=%u irq=%u rd=%u wr=%u dma=%u adma=%u",
                         g_stats.konCalls, g_stats.konVoices, g_stats.koff, g_stats.irqs, g_stats.reads, g_stats.writes, g_stats.dma, g_stats.admaBytes);
            for (int core = 0; core < 2; ++core)
            {
                const Core &c = m_core[core];
                uint32_t active = 0;
                int first = -1;
                for (int vi = 0; vi < 24; ++vi)
                    if (c.voices[vi].envPhase != 0) { active |= 1u << vi; if (first < 0) first = vi; }
                std::fprintf(stderr, " | c%d act=%06x endx=%06x attr=%04x admas=%x irqa=%05x fifo=%u", core, active, c.endx,
                             c.reg[kAttr >> 1], c.reg[kAdmas >> 1], irqAddress(core), static_cast<unsigned>(c.admaL.size() - c.admaRead));
                if (first >= 0)
                    std::fprintf(stderr, " v%d nax=%05x lsa=%05x pitch=%04x env=%04x", first, c.voices[first].nax, c.voices[first].lsa,
                                 c.reg[(first * 0x10 + 4) >> 1], c.voices[first].envelope);
            }
            printTop("R", g_stats.readAt);
            printTop("W", g_stats.writeAt);
            std::fprintf(stderr, "\n");
            g_stats = Stats{};
        }
        int32_t outL = 0, outR = 0;
        const bool routed = g_mixSeen && !g_oldMix;
        int32_t extL = 0, extR = 0; // core 0's output, the external input of core 1
        for (int core = 0; core < 2; ++core)
        {
            Core &c = m_core[core];
            const uint32_t vmixl = c.reg[kVmixl >> 1] | (static_cast<uint32_t>(c.reg[(kVmixl + 2u) >> 1]) << 16);
            const uint32_t vmixr = c.reg[kVmixr >> 1] | (static_cast<uint32_t>(c.reg[(kVmixr + 2u) >> 1]) << 16);
            int32_t sndL = 0, sndR = 0, inpL = 0, inpR = 0;
            for (int vi = 0; vi < 24; ++vi)
            {
                Voice &v = c.voices[vi];
                if (v.envPhase == 0)
                    continue;
                if (v.sampleIndex >= 28)
                    decodeBlock(core, v);
                const int16_t sample = v.decoded[v.sampleIndex];
                stepEnvelope(core, vi, v);
                const int32_t enveloped = (static_cast<int32_t>(sample) * v.envelope) >> 15;
                auto volume = [&](uint32_t off) {
                    const uint16_t vol = c.reg[(vi * 0x10 + off) >> 1];
                    return (vol & 0x8000u) ? 0x3FFF : static_cast<int32_t>(static_cast<int16_t>(vol << 1)) / 2;
                };
                const int32_t voiceL = (enveloped * volume(0)) >> 14, voiceR = (enveloped * volume(2)) >> 14;
                if (!routed)
                {
                    outL += voiceL;
                    outR += voiceR;
                }
                else
                {
                    if (vmixl & (1u << vi))
                        sndL += voiceL;
                    if (vmixr & (1u << vi))
                        sndR += voiceR;
                }

                uint32_t pitch = c.reg[(vi * 0x10 + 4) >> 1];
                if (pitch > 0x3FFFu)
                    pitch = 0x3FFFu;
                v.counter += pitch;
                while (v.counter >= 0x1000u)
                {
                    v.counter -= 0x1000u;
                    if (++v.sampleIndex >= 28)
                    {
                        // end of a block: loop flags decide the next address
                        if (v.blockFlags & 1u)
                        {
                            c.endx |= 1u << vi;
                            v.nax = v.lsa;
                            if (!(v.blockFlags & 2u))
                            {
                                v.envelope = 0;
                                v.envPhase = 0;
                            }
                        }
                        else
                            v.nax = (v.nax + 16u) % kRamBytes;
                        if (v.envPhase == 0)
                            break;
                        decodeBlock(core, v);
                    }
                }
            }
            if (c.admaRead < c.admaL.size())
            {
                inpL = c.admaL[c.admaRead];
                inpR = c.admaR[c.admaRead];
                ++c.admaRead;
                if (!routed)
                {
                    outL += inpL;
                    outR += inpR;
                }
            }
            if (routed)
            {
                const uint16_t mmix = c.reg[kMmix >> 1];
                const uint32_t vb = static_cast<uint32_t>(core) * kCommonCoreStride; // this core's volume block
                const auto common = [&](uint32_t off) { return m_common[((vb + off) >> 1) & 0x3Fu]; };
                int32_t dryL = 0, dryR = 0;
                if (mmix & kMmixDrySndL)
                    dryL += sndL;
                if (mmix & kMmixDrySndR)
                    dryR += sndR;
                if (mmix & kMmixDryInpL)
                    dryL += (inpL * levelFactor(common(kBvol))) >> 14;
                if (mmix & kMmixDryInpR)
                    dryR += (inpR * levelFactor(common(kBvol + 2u))) >> 14;
                if (core == 1)
                {
                    if (mmix & kMmixDryExtL)
                        dryL += (extL * levelFactor(common(kAvol))) >> 14;
                    if (mmix & kMmixDryExtR)
                        dryR += (extR * levelFactor(common(kAvol + 2u))) >> 14;
                }
                dryL = std::clamp(dryL, -32768, 32767);
                dryR = std::clamp(dryR, -32768, 32767);
                const int32_t coreL = (dryL * volumeFactor(common(kMvol))) >> 14;
                const int32_t coreR = (dryR * volumeFactor(common(kMvol + 2u))) >> 14;
                if (core == 0)
                {
                    extL = coreL;
                    extR = coreR;
                }
                else
                {
                    outL = coreL;
                    outR = coreR;
                }
            }
        }
        const uint32_t w = g_write.load(std::memory_order_relaxed);
        g_ring[(w % kRingFrames) * 2] = static_cast<int16_t>(std::clamp(outL, -32768, 32767));
        g_ring[(w % kRingFrames) * 2 + 1] = static_cast<int16_t>(std::clamp(outR, -32768, 32767));
        g_write.store(w + 1u, std::memory_order_release);
        uint32_t r = g_read.load(std::memory_order_acquire);
        if (w + 1u - r > kRingFrames - 1024u) // nobody draining fast enough: keep the newest audio
            g_read.compare_exchange_strong(r, w + 1u - (kRingFrames / 2u));
    }

    void IopSpu2::advanceTo(uint64_t cycle)
    {
        if (!m_started)
        {
            m_started = true;
            m_cycle = cycle;
            return;
        }
        if (cycle <= m_cycle)
            return;
        uint64_t samples = (cycle - m_cycle) / kCyclesPerSample;
        if (samples > 48000u) // a long jump (idle skip): run at most one second of audio
        {
            m_cycle = cycle - 48000u * kCyclesPerSample;
            samples = 48000u;
        }
        for (uint64_t i = 0; i < samples; ++i)
            tick();
        m_cycle += samples * kCyclesPerSample;
    }
}
