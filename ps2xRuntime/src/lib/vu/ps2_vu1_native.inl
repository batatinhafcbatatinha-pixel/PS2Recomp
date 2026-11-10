// GOW-Port: native VU1 backend runtime (included at the end of ps2_vu1_core.cpp). See include/runtime/ps2_vu1_native.h.
#include "runtime/ps2_vu1_native.h"

#include <memory>
#include <unordered_map>

namespace
{
    struct NativeEntry
    {
        VU1NativeFn fn;
        uint32_t pairs;
        const VU1TailDesc *tail;
        bool readsEntryFlags; // reads MAC/clip flags written before the block (vu1n_gen.py: bit 16 of pairs)
        std::vector<uint8_t> cyc; // issue cycle of each pair (block entry = 0), filled on first use
    };
    std::unordered_map<uint64_t, NativeEntry> &nativeRegistry()
    {
        static std::unordered_map<uint64_t, NativeEntry> registry;
        return registry;
    }
    bool isBranchLower(uint32_t lower)
    {
        switch (lower >> 25)
        {
        case 0x20: case 0x21: case 0x24: case 0x25: case 0x28: case 0x29: case 0x2C: case 0x2D: case 0x2E: case 0x2F:
            return true;
        default:
            return false;
        }
    }
    // Same rule as tools/vu1/vu1n_gen.py: pairs from pc up to and including the delay slot of the first branch, the pair
    // after an E bit, or a D/T pair; at most 64 pairs; an all-zero pair is invalid (extent 0).
    uint32_t nativeExtent(const uint8_t *code, uint32_t size, uint32_t pc, uint32_t *words)
    {
        uint32_t n = 0;
        for (uint32_t p = pc; p + 8u <= size && n < 64u; p += 8u)
        {
            uint32_t lower, upper;
            std::memcpy(&lower, code + p, 4);
            std::memcpy(&upper, code + p + 4, 4);
            if (lower == 0u && upper == 0u)
                return n == 0u ? 0u : n;
            words[n * 2u] = lower;
            words[n * 2u + 1u] = upper;
            ++n;
            const bool iBit = (upper >> 31) != 0u;
            if (((upper >> 28) & 1u) || ((upper >> 27) & 1u))
                break;
            if ((!iBit && isBranchLower(lower)) || ((upper >> 30) & 1u))
            {
                uint32_t dl, du;
                if (p + 16u <= size)
                {
                    std::memcpy(&dl, code + p + 8u, 4);
                    std::memcpy(&du, code + p + 12u, 4);
                    if (!(dl == 0u && du == 0u))
                    {
                        words[n * 2u] = dl;
                        words[n * 2u + 1u] = du;
                        ++n;
                    }
                }
                break;
            }
        }
        return n;
    }
}

void vu1nRegisterBlock(uint64_t key, VU1NativeFn fn, uint32_t pairs, const VU1TailDesc *tail)
{
    nativeRegistry()[key] = {fn, pairs & 0xFFFFu, tail, (pairs & 0x10000u) != 0u, {}};
}

// Issue cycles of the pairs of a block as the exact engine would see them (operand/pipeline stalls included), assuming
// every pipeline is idle at block entry. Flag readers use them to decide which earlier FMAC/CLIP result is visible.
void VU1Interpreter::nativeTiming(const uint8_t *code, uint32_t pc, uint32_t count, std::vector<uint8_t> &cyc)
{
    VU1Interpreter t(*this);
    constexpr uint64_t base = 1u << 20;
    t.m_cycle = base;
    for (auto &r : t.m_vfReady)
        r.fill(0u);
    t.m_viReady.fill(0u);
    t.m_accReady.fill(0u);
    t.m_fdiv.valid = false;
    for (auto &e : t.m_efu)
        e.valid = false;
    t.m_efuResourceReady = 0;
    t.m_xgkick.active = false;
    cyc.assign(count, 0);
    for (uint32_t i = 0; i < count; ++i)
    {
        const DecodedInstructionPair d = t.decodeInstructionPair(code, pc + i * 8u);
        const uint64_t ready = t.calculatePairReadyCycle(d);
        if (ready > t.m_cycle)
            t.m_cycle = ready;
        cyc[i] = static_cast<uint8_t>(std::min<uint64_t>(t.m_cycle - base, 255u));
        t.markPairWrites(d);
        if (d.lowerUsage.pipeline == PipelineFdiv && !d.lowerUsage.waitQ)
        {
            t.m_fdiv.valid = true;
            t.m_fdiv.readyCycle = t.m_cycle + d.lowerUsage.latency;
        }
        else if (d.lowerUsage.pipeline == PipelineEfu && !d.lowerUsage.waitP)
        {
            t.m_efu[0].valid = true;
            t.m_efu[0].readyCycle = t.m_cycle + d.lowerUsage.latency;
            t.m_efuResourceReady = t.m_cycle + (d.lowerUsage.latency > 0u ? d.lowerUsage.latency - 1u : 0u);
        }
        ++t.m_cycle;
    }
}

// vf0/vi0 are constants: the interpreter's step loop restores them after every pair, single-op callouts must too
// (FlatOut 2 wrote vf0 through one; every later program reading it as zero drew giant dark triangles).
static inline void restoreConstRegs(VU1Interpreter &vu)
{
    VU1State &st = vu.state();
    st.vf[0][0] = 0.0f;
    st.vf[0][1] = 0.0f;
    st.vf[0][2] = 0.0f;
    st.vf[0][3] = 1.0f;
    st.vi[0] = 0;
}

void VU1NativeAccess::callLower(VU1NativeCtx &n, uint32_t pc, uint32_t lower, uint32_t upper)
{
    VU1Interpreter &vu = *n.vu;
    auto &c = *static_cast<VU1Interpreter::StepContext *>(n.context);
    vu.m_state.pc = pc;
    vu.execLower(lower, c.vuData, c.dataSize, *c.gs, c.memory, upper);
    VU1NativeAccess::commitValuesKeepTiming(vu);
    restoreConstRegs(vu);
    VU1NativeAccess::importFlags(vu); // the callout's flag writes join the native blocks' ones
}

void VU1NativeAccess::callEfu(VU1NativeCtx &n, uint32_t rel, uint32_t pc, uint32_t lower, uint32_t upper)
{
    VU1Interpreter &vu = *n.vu;
    auto &c = *static_cast<VU1Interpreter::StepContext *>(n.context);
    (void)pAt(n, rel);
    const uint64_t keep = vu.m_cycle;
    vu.m_cycle = n.start + rel; // the result is queued for its real ready cycle
    vu.m_state.pc = pc;
    vu.execLower(lower, c.vuData, c.dataSize, *c.gs, c.memory, upper);
    vu.m_cycle = keep;
    restoreConstRegs(vu);
}

void VU1NativeAccess::callUpper(VU1NativeCtx &n, uint32_t pc, uint32_t upper)
{
    VU1Interpreter &vu = *n.vu;
    vu.m_state.pc = pc;
    vu.execUpper(upper);
    VU1NativeAccess::commitValuesKeepTiming(vu);
    restoreConstRegs(vu);
    VU1NativeAccess::importFlags(vu); // the callout's flag writes join the native blocks' ones
}

void VU1NativeAccess::xgkick(VU1NativeCtx &n, uint32_t rel, uint32_t qwordAddress)
{
    VU1Interpreter &vu = *n.vu;
    // PATH1 moves two cycles per qword: a following XGKICK stalls until then, and the stall decides what a division in
    // flight has reached by the next MULq (CoD's first vertex of every batch).
    struct KickBusy
    {
        VU1Interpreter &vu;
        uint64_t issue;
        ~KickBusy() { vu.m_native->kickReady = issue + (vu.m_xgkick.totalBytes / 16u) * 2u; }
    } kickBusy{vu, n.start + rel};
    if (vu.m_unit != VU1Interpreter::Unit::VU1 || !vu.m_activeVuData || vu.m_activeVuDataSize < 16u)
        return;
    vu.m_xgkick.reset();
    vu.m_xgkick.active = true;
    vu.m_xgkick.sourceAddress = (qwordAddress * 16u) % vu.m_activeVuDataSize;
    vu.m_xgkick.cycleCredit = 0x40000000u; // the whole packet is read at once (PCSX2 does the same)
    vu.m_xgkick.issueCycle = vu.m_cycle;
    // Fast path: walk the GIFtags in VU memory, then copy the whole packet with at most two memcpy (wrap). Same bytes and
    // end state as progressXgkick's qword loop; anything unusual (bad format, oversize, xgkick trace on) takes that loop.
    const uint32_t size = vu.m_activeVuDataSize;
    if (!g_xgkickTrace && (size & (size - 1u)) == 0u)
    {
        constexpr uint32_t kMax = VU1Interpreter::XgkickPipeline::kBufferSize;
        const uint32_t src = vu.m_xgkick.sourceAddress;
        uint32_t total = 0u;
        bool ok = false;
        for (;;)
        {
            const uint32_t at = (src + total) & (size - 1u);
            if (total > kMax - 16u || at + 8u > size)
                break;
            uint64_t tagLo;
            std::memcpy(&tagLo, vu.m_activeVuData + at, 8);
            const uint32_t nloop = static_cast<uint32_t>(tagLo & 0x7FFFu);
            const uint32_t format = static_cast<uint32_t>((tagLo >> 58) & 0x3u);
            uint32_t nreg = static_cast<uint32_t>((tagLo >> 60) & 0xFu);
            if (nreg == 0u)
                nreg = 16u;
            uint64_t tagBytes = 16u;
            if (format == 0u)
                tagBytes += static_cast<uint64_t>(nloop) * nreg * 16u;
            else if (format == 1u)
                tagBytes += ((static_cast<uint64_t>(nloop) * nreg + 1u) & ~1ull) * 8u;
            else if (format == 2u)
                tagBytes += static_cast<uint64_t>(nloop) * 16u;
            else
                break;
            if (tagBytes > kMax - total)
                break;
            total += static_cast<uint32_t>(tagBytes);
            if ((tagLo >> 15) & 1u)
            {
                ok = true;
                break;
            }
        }
        if (ok)
        {
            uint8_t *dst = vu.m_xgkick.packet.data();
            const uint32_t first = src & (size - 1u);
            const uint32_t head = std::min(total, size - first);
            static const bool kickPrimDiag = std::getenv("GOW_VU1_KICKPRIM") != nullptr; // finishXgkick reports it
            if (head == total && vu.m_activeMemory && !vu.m_verifySilent && !kickPrimDiag)
            {
                // in one piece in VU memory: no copy on the way to the GS queue
                vu.m_xgkick.copiedBytes = total;
                vu.m_xgkick.totalBytes = total;
                vu.m_xgkick.active = false;
                vu.m_activeMemory->submitGifPacket(GifPathId::Path1, vu.m_activeVuData + first, total);
                return;
            }
            std::memcpy(dst, vu.m_activeVuData + first, head);
            for (uint32_t done = head; done < total;)
            {
                const uint32_t chunk = std::min(total - done, size);
                std::memcpy(dst + done, vu.m_activeVuData, chunk);
                done += chunk;
            }
            vu.m_xgkick.copiedBytes = total;
            vu.m_xgkick.totalBytes = total;
            vu.m_xgkick.currentTagEnd = total;
            vu.m_xgkick.currentTagEop = true;
            vu.m_xgkick.cycleCredit = 0x40000000u - (total / 16u) * 2u + 1u;
            vu.finishXgkick();
            return;
        }
    }
    vu.progressXgkick();
}

// Native entry (and after an exact callout inside a block): the exact engine's flag pipeline moves into the rings
// (status / sticky updates, not modelled by the native blocks, are applied at once like before).
void VU1NativeAccess::importFlags(VU1Interpreter &vu)
{
    VU1Interpreter::NativeState &ns = *vu.m_native;
    if (!ns.flagsInRings)
    {
        ns.macRing.reset(vu.m_state.mac);
        ns.clipRing.reset(vu.m_state.clip);
        ns.clipRing.clip = true;
        ns.flagsInRings = true;
    }
    while (vu.m_flagCount != 0u)
    {
        auto &e = vu.m_flagPipeline[vu.m_flagHead];
        vu.m_flagHead = (vu.m_flagHead + 1u) % VU1Interpreter::kMaxFlagEntries;
        --vu.m_flagCount;
        if (e.writesMac)
            ringPush(ns.macRing, e.issueCycle, e.mac);
        if (e.writesClip)
            ringPush(ns.clipRing, e.issueCycle, e.clip);
        if (e.writesStatus)
        {
            const uint32_t current = e.status & 0xFu;
            vu.m_state.status = (vu.m_state.status & 0xFF0u) | current | ((current | e.extraSticky) << 6);
        }
        if (e.writesSticky)
            vu.m_state.status = (vu.m_state.status & 0x03Fu) | (e.status & 0xFC0u);
        e.valid = false;
    }
}

// The exact engine continues: visible flags become its committed values, writers still in flight its flag pipeline.
void VU1NativeAccess::exportFlags(VU1Interpreter &vu)
{
    VU1Interpreter::NativeState &ns = *vu.m_native;
    if (!ns.flagsInRings)
        return;
    ns.flagsInRings = false;
    vu.m_state.mac = ringAt(ns.macRing, vu.m_cycle);
    vu.m_state.clip = ringAt(ns.clipRing, vu.m_cycle);
    // pending writers of both rings (tail records included), oldest first
    struct P { uint64_t issue; uint32_t value; bool clip; };
    P pending[32];
    uint32_t count = 0;
    for (const auto *r : {&ns.macRing, &ns.clipRing})
        for (uint32_t k = 4u; k >= 1u; --k)
        {
            const uint32_t i = (r->next + 4u - k) & 3u;
            if (r->issue[i] + VU1Interpreter::kFmacLatency > vu.m_cycle)
            {
                pending[count++] = {r->issue[i], r->valueOf(i), r == &ns.clipRing};
                if (const VU1FlagTail *t = r->tail[i])
                {
                    const uint32_t n = r->clip ? t->desc->clipCount : t->desc->macCount;
                    for (uint32_t j = 0; j < n; ++j)
                    {
                        const uint8_t pair = r->clip ? t->desc->clipPair[j] : t->desc->macPair[j];
                        const uint64_t issue = t->start + t->cyc[pair];
                        if (issue + VU1Interpreter::kFmacLatency > vu.m_cycle)
                            pending[count++] = {issue, r->clip ? t->c[j] : vu1n::macValRt(t->q[j], t->desc->macMask[j]), r->clip};
                    }
                }
            }
        }
    std::sort(pending, pending + count, [](const P &a, const P &b) { return a.issue < b.issue; });
    for (uint32_t k = 0; k < count; ++k)
    {
        if (vu.m_flagCount >= VU1Interpreter::kMaxFlagEntries)
            break;
        auto &e = vu.m_flagPipeline[(vu.m_flagHead + vu.m_flagCount) % VU1Interpreter::kMaxFlagEntries];
        ++vu.m_flagCount;
        e.mac = pending[k].clip ? 0u : pending[k].value;
        e.clip = pending[k].clip ? pending[k].value : 0u;
        e.status = e.extraSticky = 0u;
        e.writesMac = !pending[k].clip;
        e.writesClip = pending[k].clip;
        e.writesStatus = e.writesSticky = false;
        e.valid = true;
        e.issueCycle = pending[k].issue;
        e.readyCycle = pending[k].issue + VU1Interpreter::kFmacLatency;
    }
}

// Values of results in flight are written at once (a reader would stall until they are ready anyway, so it sees the
// same value), but their timing state stays: the flag queue. Q and P are the exception: their readers do not interlock, so
// divisions and EFU results in flight stay in their entries (readers: VU1NativeAccess::qAt / pAt).
void VU1NativeAccess::commitValuesKeepTiming(VU1Interpreter &vu)
{
    const uint32_t flagHead = vu.m_flagHead, flagCount = vu.m_flagCount;
    const VU1Interpreter::ScalarPipelineEntry fdiv = vu.m_fdiv;
    const auto efu = vu.m_efu;
    vu.m_flagCount = 0u;
    vu.m_fdiv.valid = false;
    for (VU1Interpreter::ScalarPipelineEntry &e : vu.m_efu)
        e.valid = false;
    vu.commitEverythingNow();
    vu.m_flagHead = flagHead;
    vu.m_flagCount = flagCount;
    vu.m_fdiv = fdiv;
    vu.m_efu = efu;
    if (vu.m_fdiv.valid)
        vu.m_nextCommitCycle = std::min(vu.m_nextCommitCycle, vu.m_fdiv.readyCycle);
    for (const VU1Interpreter::ScalarPipelineEntry &e : vu.m_efu)
        if (e.valid)
            vu.m_nextCommitCycle = std::min(vu.m_nextCommitCycle, e.readyCycle);
}

// First use of a block: decoded pairs, the lanes it reads before writing them (with the idle-schedule cycle of that
// first read) and the last write of every lane it writes.
void VU1NativeAccess::prepareBlockTiming(VU1Interpreter &vu, VU1Interpreter::NativeState::BlockTiming &bt, const uint8_t *code, uint32_t pc, uint32_t count,
                                        const uint8_t *idle)
{
    using NS = VU1Interpreter::NativeState;
    bt.idle = idle;
    bt.dec.clear();
    bt.scalarPairs.clear();
    bool read[NS::kLanes] = {}, written[NS::kLanes] = {};
    bool scalarWritten[4] = {};
    uint32_t &maxLat = vu.m_native->maxLat;
    const auto onRead = [&](uint32_t lane, uint8_t at) {
        if (read[lane] || written[lane])
            return;
        read[lane] = true;
        bt.readMask[lane >> 6] |= 1ull << (lane & 63u);
        bt.readAt[lane] = at;
    };
    const auto onWrite = [&](uint32_t lane, uint32_t pair, uint32_t lat) {
        written[lane] = true;
        bt.writeMask[lane >> 6] |= 1ull << (lane & 63u);
        bt.lastPair[lane] = static_cast<uint8_t>(pair);
        bt.lastLat[lane] = static_cast<uint8_t>(lat);
        maxLat = std::max(maxLat, lat);
    };
    const auto onScalarRead = [&](uint32_t k, uint8_t at) {
        if (scalarWritten[k] || (bt.scalarRead & (1u << k)))
            return;
        bt.scalarRead = static_cast<uint8_t>(bt.scalarRead | (1u << k));
        bt.scalarAt[k] = at;
    };
    for (uint32_t i = 0; i < count; ++i)
    {
        const VU1Interpreter::DecodedInstructionPair d = vu.decodeInstructionPair(code, pc + i * 8u);
        bt.dec.push_back(d);
        const uint8_t at = idle[i];
        for (const VU1Interpreter::InstructionUsage *u : {&d.upperUsage, &d.lowerUsage})
        {
            for (uint32_t k = 0; k < u->vfReadCount; ++k)
                for (uint32_t c = 0; c < 4u; ++c)
                    if (u->vfRead[k].lanes & (1u << (3u - c)))
                        onRead(u->vfRead[k].reg * 4u + c, at);
            for (uint32_t r = 1; r < 16u; ++r)
                if (u->viRead & (1u << r))
                    onRead(128u + r, at);
            for (uint32_t c = 0; c < 4u; ++c)
                if (u->accRead & (1u << (3u - c)))
                    onRead(144u + c, at);
        }
        if (d.lowerUsage.pipeline == VU1Interpreter::PipelineFdiv || d.lowerUsage.waitQ)
            onScalarRead(0, at);
        if (d.lowerUsage.pipeline == VU1Interpreter::PipelineEfu)
            onScalarRead(1, at);
        if (d.lowerUsage.waitP)
            onScalarRead(2, at);
        if (d.lowerUsage.pipeline == VU1Interpreter::PipelineXgkick)
            onScalarRead(3, at);
        // writes, in markPairWrites' order (lower VF, upper VF, VI, ACC)
        const auto vfWrite = [&](const VU1Interpreter::InstructionUsage &u, bool lower) {
            const VU1Interpreter::VfAccess w = u.vfWrite;
            if (w.reg == 0u || (lower && d.suppressedLowerVf == w.reg))
                return;
            const uint32_t lat = u.vfLatency != 0u ? u.vfLatency : u.latency;
            for (uint32_t c = 0; c < 4u; ++c)
                if (w.lanes & (1u << (3u - c)))
                    onWrite(w.reg * 4u + c, i, lat);
        };
        vfWrite(d.lowerUsage, true);
        vfWrite(d.upperUsage, false);
        for (uint32_t r = 1; r < 16u; ++r)
            if (d.lowerUsage.viWrite & (1u << r))
                onWrite(128u + r, i, d.lowerUsage.viLatency != 0u ? d.lowerUsage.viLatency : d.lowerUsage.latency);
        for (uint32_t c = 0; c < 4u; ++c)
            if (d.upperUsage.accWrite & (1u << (3u - c)))
                onWrite(144u + c, i, VU1Interpreter::kAccForwardLatency);
        const bool fdivOp = d.lowerUsage.pipeline == VU1Interpreter::PipelineFdiv && !d.lowerUsage.waitQ;
        const bool efuOp = d.lowerUsage.pipeline == VU1Interpreter::PipelineEfu && !d.lowerUsage.waitP;
        if (fdivOp || efuOp)
        {
            bt.scalarPairs.push_back(static_cast<uint8_t>(i));
            scalarWritten[fdivOp ? 0 : 1] = true;
            if (efuOp)
                scalarWritten[2] = true;
        }
    }
}

static inline uint64_t &vu1nLaneWord(VU1Interpreter &vu, uint32_t lane)
{
    return VU1NativeAccess::laneWord(vu, lane);
}

uint64_t &VU1NativeAccess::laneWord(VU1Interpreter &vu, uint32_t lane)
{
    if (lane < 128u)
        return vu.m_vfReady[lane >> 2][lane & 3u];
    if (lane < 144u)
        return vu.m_viReady[lane - 128u];
    return vu.m_accReady[lane - 144u];
}

// Ready cycles of lanes from the blocks in `hist`, newest first: calls ready(lane, cycle) for every lane of `want` whose
// newest writer is one of them and can still be in flight at `now` (cycle 0: surely ready), and removes those lanes from
// `want`. Lanes left in `want` are ready by `now` unless arraysReadyBy > now: then they hold the exact engine's values.
// Blocks that ended maxLat or more cycles before `now` (and all older ones) hold nothing in flight any more.
template <typename F>
void VU1NativeAccess::walkHistory(VU1Interpreter::NativeState &ns, uint64_t now, uint64_t want[3], F &&ready)
{
    using NS = VU1Interpreter::NativeState;
    const uint32_t n = std::min(ns.histCount, NS::kHist);
    uint32_t k = 0;
    for (; k < n; ++k)
    {
        if ((want[0] | want[1] | want[2]) == 0u)
            return; // nothing left to find, here or in the arrays
        const NS::Hist &h = ns.hist[(ns.histNext + NS::kHist - 1u - k) % NS::kHist];
        if (h.end + ns.maxLat <= now)
            break;
        for (uint32_t w = 0; w < 3u; ++w)
        {
            const uint64_t lanes = want[w] & h.bt->writeMask[w];
            want[w] &= ~lanes;
            for (uint64_t bits = lanes; bits != 0u; bits &= bits - 1u)
            {
                const uint32_t lane = w * 64u + static_cast<uint32_t>(std::countr_zero(bits));
                ready(lane, h.start + h.cyc[h.bt->lastPair[lane]] + h.bt->lastLat[lane]);
            }
        }
    }
    if (ns.arraysReadyBy <= now)
        return;
    // Shortly after native entry: lanes written by the older blocks are ready, the others keep the exact engine's values.
    // All blocks since entry are still in the ring here (they span less than the exact engine's latencies, far below kHist).
    for (; k < n; ++k)
    {
        const NS::Hist &h = ns.hist[(ns.histNext + NS::kHist - 1u - k) % NS::kHist];
        for (uint32_t w = 0; w < 3u; ++w)
        {
            const uint64_t lanes = want[w] & h.bt->writeMask[w];
            want[w] &= ~lanes;
            for (uint64_t bits = lanes; bits != 0u; bits &= bits - 1u)
                ready(w * 64u + static_cast<uint32_t>(std::countr_zero(bits)), 0u);
        }
    }
}

// Before the exact engine looks at its readiness arrays: lanes the native blocks wrote that may still be in flight get
// their real ready cycle (the others are ready by now, as every stale value is); the history starts over.
void VU1NativeAccess::syncInflight(VU1Interpreter &vu)
{
    VU1Interpreter::NativeState &ns = *vu.m_native;
    uint64_t want[3] = {~0ull, ~0ull, (1ull << (VU1Interpreter::NativeState::kLanes - 128u)) - 1u};
    uint64_t readyBy = ns.arraysReadyBy;
    walkHistory(ns, vu.m_cycle, want, [&](uint32_t lane, uint64_t ready) {
        vu1nLaneWord(vu, lane) = ready;
        readyBy = std::max(readyBy, ready);
    });
    ns.arraysReadyBy = readyBy;
    ns.histCount = 0u;
}

// Issue cycles (relative to vu.m_cycle) of the block's pairs: the idle table unless a lane it reads is still in flight
// past that read; then the exact engine's stall model, run once per distinct (lane, slack) set and cached.
const uint8_t *VU1NativeAccess::blockTiming(VU1Interpreter &vu, VU1Interpreter::NativeState::Slot &sl, uint32_t &cycId)
{
    using NS = VU1Interpreter::NativeState;
    NS &ns = *vu.m_native;
    NS::BlockTiming &bt = *sl.bt;
    const uint64_t start = vu.m_cycle;
    uint64_t memoKey = 0u;
    if (ns.arraysReadyBy <= start)
    {
        // The run of blocks that ends where this one starts, back to maxLat cycles or to the first block since the arrays
        // were last current (nothing older is in flight then): their tables give every lane's ready cycle. FDIV, EFU and
        // XGKICK are part of the key as the block sees them.
        const uint32_t n = std::min<uint32_t>(std::min<uint32_t>(ns.histCount, NS::kHist), 16u);
        uint64_t key = 0x9E3779B97F4A7C15ull, expected = start;
        bool complete = n == ns.histCount;
        for (uint32_t k = 0; k < n; ++k)
        {
            const NS::Hist &h = ns.hist[(ns.histNext + NS::kHist - 1u - k) % NS::kHist];
            if (h.end != expected)
            {
                complete = false;
                break;
            }
            key = (key ^ reinterpret_cast<uintptr_t>(h.bt)) * 0xFF51AFD7ED558CCDull;
            key = (key ^ h.cycId ^ (key >> 29)) * 0xC4CEB9FE1A85EC53ull;
            expected = h.start;
            if (start - expected >= ns.maxLat)
            {
                complete = true;
                break;
            }
        }
        if (complete)
        {
            if (bt.scalarRead)
            {
                uint64_t ready[4] = {vu.m_fdiv.valid ? vu.m_fdiv.readyCycle : 0u, vu.m_efuResourceReady, 0u, ns.kickReady};
                for (const VU1Interpreter::ScalarPipelineEntry &e : vu.m_efu)
                    if (e.valid)
                        ready[2] = std::max(ready[2], e.readyCycle);
                for (uint32_t k = 0; k < 4u; ++k)
                    if ((bt.scalarRead & (1u << k)) && ready[k] > start + bt.scalarAt[k])
                        key = (key ^ ((k + 1u) << 8 | std::min<uint64_t>(ready[k] - start - bt.scalarAt[k], 255u))) * 0xFF51AFD7ED558CCDull;
            }
            memoKey = key | 1u;
        }
    }
    if (memoKey != 0u)
        for (const NS::Slot::Memo &m : sl.memo)
            if (m.key == memoKey && m.cyc)
            {
                if (m.idx == ~0u)
                {
                    cycId = m.id;
                    return m.cyc;
                }
                // a cache entry replaced since gets a new id (ids are never reused)
                if (m.idx < bt.cache.size() && bt.cache[m.idx].id == m.id)
                {
                    cycId = m.id;
                    return bt.cache[m.idx].cyc.data();
                }
            }
    const auto remember = [&](const uint8_t *cyc, uint32_t id, uint32_t idx) {
        if (memoKey != 0u)
            sl.memo[sl.memoNext++ & 3u] = NS::Slot::Memo{memoKey, cyc, id, idx};
        cycId = id;
        return cyc;
    };
    uint64_t hot[3] = {};
    uint8_t slack[NS::kLanes];
    const auto note = [&](uint32_t lane, uint64_t ready) {
        const uint64_t readAt = start + bt.readAt[lane];
        if (ready > readAt)
        {
            hot[lane >> 6] |= 1ull << (lane & 63u);
            slack[lane] = static_cast<uint8_t>(std::min<uint64_t>(ready - readAt, 255u));
        }
    };
    uint64_t want[3] = {bt.readMask[0], bt.readMask[1], bt.readMask[2]};
    walkHistory(ns, start, want, note);
    if (ns.arraysReadyBy > start) // not written by native blocks since the exact engine ran: its readiness arrays
        for (uint32_t w = 0; w < 3u; ++w)
            for (uint64_t bits = want[w]; bits != 0u; bits &= bits - 1u)
            {
                const uint32_t lane = w * 64u + static_cast<uint32_t>(std::countr_zero(bits));
                note(lane, vu1nLaneWord(vu, lane));
            }
    ns.sig.clear();
    for (uint32_t w = 0; w < 3u; ++w)
        for (uint64_t bits = hot[w]; bits != 0u; bits &= bits - 1u)
        {
            const uint32_t lane = w * 64u + static_cast<uint32_t>(std::countr_zero(bits));
            ns.sig.push_back(static_cast<uint16_t>((lane << 8) | slack[lane]));
        }
    if (bt.scalarRead)
    {
        uint64_t ready[4] = {vu.m_fdiv.valid ? vu.m_fdiv.readyCycle : 0u, vu.m_efuResourceReady, 0u, ns.kickReady};
        for (const VU1Interpreter::ScalarPipelineEntry &e : vu.m_efu)
            if (e.valid)
                ready[2] = std::max(ready[2], e.readyCycle);
        for (uint32_t k = 0; k < 4u; ++k)
            if ((bt.scalarRead & (1u << k)) && ready[k] > start + bt.scalarAt[k])
                ns.sig.push_back(static_cast<uint16_t>(((200u + k) << 8) | std::min<uint64_t>(ready[k] - start - bt.scalarAt[k], 255u)));
    }
    if (ns.sig.empty())
        return remember(bt.idle, 0u, ~0u);
    for (uint32_t k = 0; k < bt.cache.size(); ++k)
        if (bt.cache[k].sig == ns.sig)
            return remember(bt.cache[k].cyc.data(), bt.cache[k].id, k);
    // simulate on the exact engine's pipeline state (synced; this also drops the history, whose timing tables may be
    // cache slots replaced below), then restore it
    syncInflight(vu);
    const uint64_t keepCycle = vu.m_cycle;
    const auto vfReady = vu.m_vfReady;
    const auto viReady = vu.m_viReady;
    const auto accReady = vu.m_accReady;
    const VU1Interpreter::ScalarPipelineEntry fdiv = vu.m_fdiv;
    const auto efu = vu.m_efu;
    const uint64_t efuResource = vu.m_efuResourceReady;
    const bool xgkick = vu.m_xgkick.active;
    vu.m_xgkick.active = false;
    std::vector<uint8_t> cyc(bt.dec.size());
    for (size_t i = 0; i < bt.dec.size(); ++i)
    {
        const VU1Interpreter::DecodedInstructionPair &d = bt.dec[i];
        uint64_t ready = vu.calculatePairReadyCycle(d);
        if (d.lowerUsage.pipeline == VU1Interpreter::PipelineXgkick)
            ready = std::max(ready, ns.kickReady);
        if (ready > vu.m_cycle)
            vu.m_cycle = ready;
        cyc[i] = static_cast<uint8_t>(std::min<uint64_t>(vu.m_cycle - keepCycle, 255u));
        vu.markPairWrites(d);
        if (d.lowerUsage.pipeline == VU1Interpreter::PipelineFdiv && !d.lowerUsage.waitQ)
        {
            vu.m_fdiv.valid = true;
            vu.m_fdiv.readyCycle = vu.m_cycle + d.lowerUsage.latency;
        }
        else if (d.lowerUsage.pipeline == VU1Interpreter::PipelineEfu && !d.lowerUsage.waitP)
        {
            vu.m_efu[0].valid = true;
            vu.m_efu[0].readyCycle = vu.m_cycle + d.lowerUsage.latency;
            vu.m_efuResourceReady = vu.m_cycle + (d.lowerUsage.latency > 0u ? d.lowerUsage.latency - 1u : 0u);
        }
        ++vu.m_cycle;
    }
    vu.m_cycle = keepCycle;
    vu.m_vfReady = vfReady;
    vu.m_viReady = viReady;
    vu.m_accReady = accReady;
    vu.m_fdiv = fdiv;
    vu.m_efu = efu;
    vu.m_efuResourceReady = efuResource;
    vu.m_xgkick.active = xgkick;
    const uint32_t idx = bt.cache.size() < 16u ? static_cast<uint32_t>(bt.cache.size()) : bt.next++ % 16u;
    NS::BlockTiming::Cached &slot = idx == bt.cache.size() ? bt.cache.emplace_back() : bt.cache[idx];
    slot.sig = ns.sig;
    slot.cyc = std::move(cyc);
    slot.id = ns.nextCycId++; // memos of the entry replaced here no longer match (see the lookup above)
    return remember(slot.cyc.data(), slot.id, idx);
}

// After a native block: it joins the history (its results that outlive it are found there when needed).
VU1N_INLINE void VU1NativeAccess::finishBlockTiming(VU1Interpreter &vu, const VU1Interpreter::NativeState::Slot &sl, const uint8_t *cyc,
                                                     uint32_t cycId, uint64_t start)
{
    using NS = VU1Interpreter::NativeState;
    NS &ns = *vu.m_native;
    // the idle table's length is in the slot: most blocks never touch their table's cache line here
    const uint64_t end = start + (cyc == sl.idle ? sl.idleEnd : cyc[sl.pairs - 1u] + 1u);
    NS::Hist &h = ns.hist[ns.histNext];
    h.bt = sl.bt;
    h.cyc = cyc;
    h.start = start;
    h.end = end;
    h.cycId = cycId;
    ns.histNext = (ns.histNext + 1u) % NS::kHist;
    ++ns.histCount;
    vu.m_cycle = end;
}

// The micro memory has a new generation (VIF MPG, EE writes): slots whose extent scan read a changed pair are set up again
// on their next use; the others keep their block (the game swaps one microprogram while the rest stays resident).
void VU1NativeAccess::codeChanged(VU1Interpreter::NativeState &ns, const uint8_t *code)
{
    using NS = VU1Interpreter::NativeState;
    constexpr uint32_t kPairs = sizeof(ns.code) / 8u;
    static_assert(kPairs == sizeof(NS::slots) / sizeof(NS::slots[0]));
    ++ns.codeChanges;
    if (!ns.codeValid)
    {
        for (NS::Slot &sl : ns.slots)
            sl.known = false;
        std::memcpy(ns.code, code, sizeof(ns.code));
        ns.codeValid = true;
        return;
    }
    // changedBefore[i]: changed pairs among 0..i-1 (prefix count), so a slot's range test is one subtraction
    thread_local uint16_t changedBefore[kPairs + 1];
    uint32_t changed = 0;
    for (uint32_t i = 0; i < kPairs; ++i)
    {
        changedBefore[i] = static_cast<uint16_t>(changed);
        changed += std::memcmp(ns.code + i * 8u, code + i * 8u, 8u) != 0;
    }
    changedBefore[kPairs] = static_cast<uint16_t>(changed);
    if (changed == 0u)
        return;
    for (uint32_t i = 0; i < kPairs; ++i)
    {
        NS::Slot &sl = ns.slots[i];
        if (!sl.known)
            continue;
        const uint32_t end = std::min<uint32_t>(kPairs, i + sl.pairs + 1u); // the pair after the block ends the scan
        if (changedBefore[end] != changedBefore[i])
            sl.known = false;
    }
    std::memcpy(ns.code, code, sizeof(ns.code));
}

// The dispatch slot of pc, set up on first use (native block lookup, timing tables).
VU1Interpreter::NativeState::Slot &VU1NativeAccess::slotFor(VU1Interpreter &vu, const uint8_t *code, uint32_t codeSize, uint32_t pc)
{
    VU1Interpreter::NativeState &ns = *vu.m_native;
    VU1Interpreter::NativeState::Slot &sl = ns.slots[pc >> 3];
    if (!sl.known)
    {
        uint32_t words[130]; // 64 pairs and the delay slot of a branch in the last one
        ++ns.slotSetups;
        const uint32_t count = nativeExtent(code, codeSize, pc, words);
        sl = {};
        sl.known = true;
        sl.pairs = static_cast<uint16_t>(count);
        if (count != 0u)
        {
            const auto it = nativeRegistry().find(vu1n::blockKey(pc, words, count * 2u));
            if (it != nativeRegistry().end())
            {
                if (it->second.cyc.empty())
                    vu.nativeTiming(code, pc, count, it->second.cyc);
                VU1Interpreter::NativeState::BlockTiming &bt = ns.timings[it->first];
                if (bt.dec.empty())
                    prepareBlockTiming(vu, bt, code, pc, count, it->second.cyc.data());
                bt.readsEntryFlags = it->second.readsEntryFlags;
                sl.fn = it->second.fn;
                sl.idle = it->second.cyc.data();
                sl.idleEnd = static_cast<uint16_t>(sl.idle[count - 1u] + 1u);
                sl.bt = &bt;
                sl.readsEntryFlags = bt.readsEntryFlags;
                sl.memoable = bt.scalarRead == 0u;
                sl.tail = it->second.tail;
            }
            else if (ns.diag)
                ++ns.missPcs[pc];
        }
    }
    return sl;
}

void VU1Interpreter::commitEverythingNow()
{
    const uint64_t keep = m_cycle;
    m_cycle = ~0ull >> 1;
    m_nextCommitCycle = 0u;
    commitReadyPipelines();
    commitReadyFlags();
    m_cycle = keep;
    m_nextCommitCycle = ~0ull;
}

bool VU1Interpreter::runNative(StepContext &context)
{
    if (context.vuData == nullptr || context.codeSize != (m_unit == Unit::VU1 ? PS2_VU1_CODE_SIZE : PS2_VU0_CODE_SIZE))
        return false;
    if (!m_native)
        m_native = new NativeState();
    NativeState &ns = *m_native;
    if (!ns.enabled)
        return false;
    if (m_state.branchPending || m_state.ebit || m_state.haltAfterDelaySlot || m_stopRequested)
        return false;

    const uint64_t generation = !context.memory ? 0u : m_unit == Unit::VU1 ? context.memory->getVU1CodeGeneration()
                                                                           : context.memory->getVU0CodeGeneration();
    if (generation != ns.generation || !ns.codeValid)
    {
        ns.generation = generation;
        if (m_unit == Unit::VU1)
            VU1NativeAccess::codeChanged(ns, context.vuCode);
        else
        {
            // The slot tables cover the VU1 size: VU0's smaller memory is compared through a zero-padded image.
            static uint8_t padded[PS2_VU1_CODE_SIZE];
            std::memcpy(padded, context.vuCode, PS2_VU0_CODE_SIZE);
            VU1NativeAccess::codeChanged(ns, padded);
        }
    }

    const unsigned savedCsr = _mm_getcsr();
    bool armed = false; // pending exact-engine pipelines are committed only when a native block really runs
    VU1NativeCtx ctx;
    ctx.s = &m_state;
    ctx.data = context.vuData;
    ctx.mask = context.dataSize - 1u;
    ctx.vu = this;
    ctx.context = &context;
    ctx.macRing = &ns.macRing;
    ctx.clipRing = &ns.clipRing;

    // PS2X_VU1N_SKIP=<pc>[,<pc>...]: VU1 blocks starting at these pcs run on the exact engine (a game's native
    // block that GOW_VU1N_VERIFY reports as differing, e.g. FlatOut 2's car shader block 0x7A8).
    static const std::vector<uint32_t> skipPcs = []
    {
        std::vector<uint32_t> list;
        if (const char *env = std::getenv("PS2X_VU1N_SKIP"))
            for (const char *p = env; *p;)
            {
                char *end = nullptr;
                const unsigned long v = std::strtoul(p, &end, 16);
                if (end == p)
                    break;
                list.push_back(static_cast<uint32_t>(v) & ~7u);
                p = *end == ',' ? end + 1 : end;
            }
        return list;
    }();
    const bool skipAny = m_unit == Unit::VU1 && !skipPcs.empty();
    // Steady loops: a block that ran back to back with the same timing table, contiguously, over more than twice the
    // longest latency, sees the same entry state again (its history is only itself) unless the FDIV/EFU/XGKICK in
    // flight are at other distances: then it gets the same table, which blockTiming's memo would hand out after hashing
    // the history. MotorStorm AE's hottest VU1 block is such a loop (an FDIV in flight across iterations).
    // PS2X_VU1N_LOOP_REUSE=0 turns it off.
    static const bool loopReuseOn = [] { const char *e = std::getenv("PS2X_VU1N_LOOP_REUSE"); return !e || e[0] != '0'; }();
    const NativeState::Slot *loopSlot = nullptr;
    const uint8_t *loopCyc = nullptr;
    uint32_t loopCycId = 0u;
    uint64_t loopCycles = 0u;
    std::array<uint64_t, 4> loopScalar{};
    bool loopScalarValid = false; // the streak's FDIV/EFU/XGKICK distances are known (from its second iteration)
    const auto scalarDistances = [&](const NativeState::Slot &s, uint64_t start) {
        std::array<uint64_t, 4> d{};
        const NativeState::BlockTiming &bt = *s.bt;
        if (!bt.scalarRead)
            return d;
        uint64_t ready[4] = {m_fdiv.valid ? m_fdiv.readyCycle : 0u, m_efuResourceReady, 0u, ns.kickReady};
        for (const ScalarPipelineEntry &e : m_efu)
            if (e.valid)
                ready[2] = std::max(ready[2], e.readyCycle);
        for (uint32_t k = 0; k < 4u; ++k)
            if ((bt.scalarRead & (1u << k)) && ready[k] > start + bt.scalarAt[k])
                d[k] = ready[k] - start - bt.scalarAt[k];
        return d;
    };
    bool handled = false;
    for (;;)
    {
        if (m_stopRequested || m_cycle >= context.budgetEnd)
        {
            handled = true;
            break;
        }
        const uint32_t pc = m_state.pc & (context.codeSize - 8u);
        if (pc + 8u > context.codeSize)
            break;
        NativeState::Slot &sl = ns.slots[pc >> 3];
        if (!sl.known)
            VU1NativeAccess::slotFor(*this, context.vuCode, context.codeSize, pc);
        VU1NativeFn fn = sl.fn;
        if (skipAny && fn && std::find(skipPcs.begin(), skipPcs.end(), pc) != skipPcs.end())
            fn = nullptr;
        if (!fn)
        {
            ++ns.missBlocks;
            if (ns.diag)
                ++ns.missHits[pc];
            break;
        }
        if (!armed)
        {
            // Results in flight from the exact engine: values now, timing kept (blockTiming / the flag queue), so the
            // blocks stall on them and see their flags at the same cycles as the exact engine.
            commitReadyPipelines();
            commitReadyFlags();
            VU1NativeAccess::commitValuesKeepTiming(*this);
            VU1NativeAccess::importFlags(*this);
            ns.histCount = 0u; // the exact engine's readiness arrays hold the state in flight
            if (m_cycle != ns.exitCycle) // the exact engine ran since the last native exit
            {
                uint64_t readyBy = 0u;
                for (const auto &r : m_vfReady)
                    for (const uint64_t c : r)
                        readyBy = std::max(readyBy, c);
                for (const uint64_t c : m_viReady)
                    readyBy = std::max(readyBy, c);
                for (const uint64_t c : m_accReady)
                    readyBy = std::max(readyBy, c);
                ns.arraysReadyBy = readyBy;
            }
            ++ns.nativeRuns;
            _mm_setcsr((savedCsr & ~0x6000u) | 0x6000u | 0x8000u | 0x0040u); // round toward zero, FTZ, DAZ
            armed = true;
        }
        if (ns.limit != ~0u)
        {
            if (ns.count >= ns.limit)
                break;
            if (ns.count + 1u == ns.limit)
                ns.snap = m_state;
            ++ns.count;
            ns.trace.push_back(pc);
        }
        const uint64_t start = m_cycle;
        // Values never depend on timing, only flag reads do. A block that reads flags written before it gets its exact
        // issue cycles from the pipeline state at entry (the stalls decide which of those flags it sees); the others use
        // their idle-pipeline table and only hand their results and flags in flight on. A division in flight needs the
        // real cycle count until it retires (EFU too): their readers do not interlock, so stalls decide what they see.
        uint32_t cycId = 0u;
        const uint8_t *cyc = sl.idle;
        std::array<uint64_t, 4> scalar{};
        bool haveScalar = false; // computed only for a block that repeats (the distances cost as much as blockTiming)
        if (sl.readsEntryFlags || m_fdiv.valid || m_efu[0].valid || m_efu[1].valid)
        {
            bool reused = false;
            if (loopReuseOn && &sl == loopSlot)
            {
                scalar = scalarDistances(sl, start);
                haveScalar = true;
                if (loopScalarValid && loopCycles >= 2ull * ns.maxLat + 16u && ns.arraysReadyBy <= start && scalar == loopScalar)
                {
                    cyc = loopCyc;
                    cycId = loopCycId;
                    reused = true;
                }
            }
            if (!reused)
                cyc = VU1NativeAccess::blockTiming(*this, sl, cycId);
        }
        ctx.cyc = cyc;
        ctx.start = start;
        if (sl.tail)
        {
            VU1FlagTail &t = ns.hist[ns.histNext].tail;
            t.desc = sl.tail;
            t.cyc = cyc;
            t.start = start;
            ctx.tail = &t;
        }
        const uint32_t result = fn(ctx);
        VU1NativeAccess::finishBlockTiming(*this, sl, cyc, cycId, start);
        if (&sl == loopSlot && cyc == loopCyc && cycId == loopCycId && haveScalar && loopScalarValid && scalar == loopScalar)
            loopCycles += m_cycle - start;
        else if (&sl == loopSlot && cyc == loopCyc && cycId == loopCycId && haveScalar)
        {
            loopScalar = scalar; // the streak starts here, with known distances
            loopScalarValid = true;
            loopCycles = m_cycle - start;
        }
        else
        {
            loopSlot = &sl;
            loopCyc = cyc;
            loopCycId = cycId;
            loopScalarValid = false;
            loopCycles = m_cycle - start;
        }
        if (m_cycle >= m_nextCommitCycle)
            commitReadyPipelines(); // FDIV/EFU entries kept for timing retire when ready (values are written already)
        ns.nativePairs += sl.pairs;
        ++ns.nativeBlocks;
        m_state.pc = result & (context.codeSize - 1u);
        if (result & kVu1nEnded)
        {
            context.programEnded = true;
            handled = true;
            break;
        }
        if (result & kVu1nBail)
            break;
    }
    m_state.cycles = m_cycle; // only read outside the blocks (diagnostics)
    if (armed)
    {
        VU1NativeAccess::syncInflight(*this); // the exact engine continues: its readiness arrays become current
        VU1NativeAccess::exportFlags(*this);
        ns.exitCycle = m_cycle;
    }
    _mm_setcsr(savedCsr);
    if (ns.diag)
    {
        static auto last = std::chrono::steady_clock::now();
        if (std::chrono::steady_clock::now() - last > std::chrono::seconds(5))
        {
            last = std::chrono::steady_clock::now();
            std::fprintf(stderr, "[vu1n] nativePairs=%llu nativeBlocks=%llu missBlocks=%llu nativeEntries=%llu distinctMissPcs=%zu codeChanges=%llu slotSetups=%llu\n",
                         ns.nativePairs, ns.nativeBlocks, ns.missBlocks, ns.nativeRuns, ns.missPcs.size(), ns.codeChanges, ns.slotSetups);
            ns.nativePairs = ns.nativeBlocks = ns.missBlocks = ns.nativeRuns = ns.codeChanges = ns.slotSetups = 0;
            std::vector<std::pair<unsigned, uint32_t>> top;
            for (const auto &kv : ns.missHits)
                top.emplace_back(kv.second, kv.first);
            std::sort(top.rbegin(), top.rend());
            for (size_t i = 0; i < top.size() && i < 400u; ++i)
                std::fprintf(stderr, "[vu1n-miss] pc=%x hits=%u\n", top[i].second, top[i].first);
            ns.missHits.clear();
        }
    }
    return handled;
}

// GOW_VU1N_RECORD=<file> [GOW_VU1N_RECORD_N=4000, GOW_VU1N_RECORD_STRIDE=7, GOW_VU1N_RECORD_SKIP=0 (runs skipped first, to
// record a later part of a zone run; ~76k runs per second in gameplay)]: every STRIDE-th VU1 program run, the input
// (VU1State, micro memory, data memory) and a bit-exact hash of the native result (state, data memory, GIF packets) are
// appended to <file>. GOW_VU1N_REPLAY=<file>: on the first VU1 run, every record is executed by this build's native
// blocks and its hash compared with the recorded one -> "[vu1n-replay] N records, M differ". Proves that a change of the
// generator / runtime keeps the output bit-identical.
namespace
{
    struct Vu1nRecord
    {
        uint32_t magic;
        uint32_t maxCycles;
        uint64_t hash;
        VU1State state;
        uint8_t code[PS2_VU1_CODE_SIZE];
        uint8_t data[16384];
    };
    uint64_t g_vu1nReplayNs = 0; // time spent in the native runs of the replay (GOW_VU1N_REPLAY_REPS timing)
    inline uint64_t fnvBytes(uint64_t h, const void *p, size_t n)
    {
        const uint8_t *b = static_cast<const uint8_t *>(p);
        for (size_t i = 0; i < n; ++i)
            h = (h ^ b[i]) * 1099511628211ull;
        return h;
    }
}

uint64_t VU1NativeAccess::silentNativeHash(VU1Interpreter &vu, void *contextPtr)
{
    auto &context = *static_cast<VU1Interpreter::StepContext *>(contextPtr);
    const bool silentBefore = vu.m_verifySilent;
    vu.m_verifySilent = true;
    g_vuVerifyCapture = true;
    g_vuVerifyPackets.clear();
    VU1Interpreter::StepContext c = context;
    const auto t0 = std::chrono::steady_clock::now();
    for (;;)
    {
        if (vu.runNative(c))
            break;
        if (!vu.stepInterpreted(c))
            break;
    }
    g_vu1nReplayNs += static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
    vu.flushPipelines();
    g_vuVerifyCapture = false;
    vu.m_verifySilent = silentBefore;
    const VU1State &s = vu.m_state;
    uint64_t h = 1469598103934665603ull;
    h = fnvBytes(h, s.vf, sizeof(s.vf));
    h = fnvBytes(h, s.vi, sizeof(s.vi));
    h = fnvBytes(h, s.acc, sizeof(s.acc));
    h = fnvBytes(h, &s.q, 4);
    h = fnvBytes(h, &s.p, 4);
    h = fnvBytes(h, &s.i, 4);
    h = fnvBytes(h, &s.mac, 4);
    h = fnvBytes(h, &s.clip, 4);
    h = fnvBytes(h, &s.pc, sizeof(s.pc));
    const uint8_t ended = c.programEnded ? 1u : 0u;
    h = fnvBytes(h, &ended, 1);
    h = fnvBytes(h, c.vuData, c.dataSize);
    h = fnvBytes(h, g_vuVerifyPackets.data(), g_vuVerifyPackets.size());
    return h;
}

void VU1NativeAccess::recordReplay(VU1Interpreter &vu, void *contextPtr, uint32_t maxCycles)
{
    auto &context = *static_cast<VU1Interpreter::StepContext *>(contextPtr);
    static const char *recPath = std::getenv("GOW_VU1N_RECORD");
    static const char *repPath = std::getenv("GOW_VU1N_REPLAY");
    if ((!recPath && !repPath) || context.dataSize != 16384u || context.codeSize != PS2_VU1_CODE_SIZE)
        return;
    if (repPath)
    {
        static bool done = false;
        if (done)
            return;
        done = true;
        FILE *f = std::fopen(repPath, "rb");
        if (!f)
        {
            std::fprintf(stderr, "[vu1n-replay] cannot open %s\n", repPath);
            return;
        }
        auto rec = std::make_unique<Vu1nRecord>();
        VU1Interpreter saved(vu);
        unsigned total = 0, differ = 0;
        static const unsigned reps = std::getenv("GOW_VU1N_REPLAY_REPS") ? static_cast<unsigned>(std::atoi(std::getenv("GOW_VU1N_REPLAY_REPS"))) : 1u;
        auto scratch = std::make_unique<Vu1nRecord>();
        uint64_t timedNs = 0;
        while (std::fread(rec.get(), sizeof(Vu1nRecord), 1, f) == 1)
        {
            ++total;
            for (unsigned rep = 0; rep < std::max(reps, 1u); ++rep)
            {
                vu = saved;
                vu.m_state = rec->state;
                if (rep == 0u)
                    vu.m_native->generation = ~0ull - 1u; // micro memory is not the game's: forget the block cache
                std::memcpy(scratch->data, rec->data, sizeof(rec->data));
                VU1Interpreter::StepContext c = context;
                c.vuCode = rec->code;
                c.vuData = scratch->data;
                c.budgetEnd = vu.m_cycle + rec->maxCycles;
                c.programEnded = false;
                vu.m_activeVuData = scratch->data;
                vu.m_activeVuDataSize = 16384u;
                g_vu1nReplayNs = 0;
                const uint64_t h = silentNativeHash(vu, &c);
                if (rep != 0u)
                    timedNs += g_vu1nReplayNs; // warm block cache only
                else if (h != rec->hash && differ++ < 20u)
                    std::fprintf(stderr, "[vu1n-replay] record %u startPc=%x differs\n", total, rec->state.pc);
            }
        }
        if (reps > 1u)
            std::fprintf(stderr, "[vu1n-replay] timing: %.3f ms for %u warm runs\n", timedNs / 1e6, total * (reps - 1u));
        std::fclose(f);
        vu = saved;
        vu.m_native->generation = ~0ull - 1u;
        std::fprintf(stderr, "[vu1n-replay] %u records, %u differ\n", total, differ);
        return;
    }
    static const unsigned limit = std::getenv("GOW_VU1N_RECORD_N") ? static_cast<unsigned>(std::atoi(std::getenv("GOW_VU1N_RECORD_N"))) : 4000u;
    static const unsigned stride = std::getenv("GOW_VU1N_RECORD_STRIDE") ? static_cast<unsigned>(std::atoi(std::getenv("GOW_VU1N_RECORD_STRIDE"))) : 7u;
    static const unsigned long long skip = std::getenv("GOW_VU1N_RECORD_SKIP") ? std::strtoull(std::getenv("GOW_VU1N_RECORD_SKIP"), nullptr, 10) : 0ull;
    static unsigned long long seen = 0;
    static unsigned written = 0;
    if (written >= limit || seen++ < skip || ((seen - skip) % stride) != 0u)
        return;
    static FILE *f = std::fopen(recPath, "wb");
    if (!f)
        return;
    auto rec = std::make_unique<Vu1nRecord>();
    rec->magic = 0x4E315556u;
    rec->maxCycles = maxCycles;
    rec->state = vu.m_state;
    std::memcpy(rec->code, context.vuCode, PS2_VU1_CODE_SIZE);
    std::memcpy(rec->data, context.vuData, 16384u);
    VU1Interpreter saved(vu);
    // same input as the replay: copies of code/data
    VU1Interpreter::StepContext c = context;
    auto code = std::make_unique<Vu1nRecord>();
    std::memcpy(code->code, rec->code, PS2_VU1_CODE_SIZE);
    std::memcpy(code->data, rec->data, 16384u);
    c.vuCode = code->code;
    c.vuData = code->data;
    c.budgetEnd = vu.m_cycle + maxCycles;
    vu.m_native->generation = ~0ull - 1u;
    vu.m_activeVuData = code->data;
    rec->hash = silentNativeHash(vu, &c);
    vu = saved;
    vu.m_native->generation = ~0ull - 1u;
    std::fwrite(rec.get(), sizeof(Vu1nRecord), 1, f);
    if (++written == limit)
        std::fclose(f), std::fprintf(stderr, "[vu1n-record] %u records written\n", written);
    else if (written % 500u == 0u)
        std::fflush(f);
}
