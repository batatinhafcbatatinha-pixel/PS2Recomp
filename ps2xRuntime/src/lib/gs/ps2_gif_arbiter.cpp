#include "runtime/gs/ps2_gif_arbiter.h"
#include <algorithm>
#include <cstring>
#include <cstdio>

bool Ps2GifOrderTraceEnabled();
void Ps2TraceGifOrder(const char *stage, unsigned path, const uint8_t *data, uint32_t size,
                      const void *owner, size_t pendingGif, size_t pendingVif, const char *extra);

GifArbiter::GifArbiter(ProcessPacketFn processFn)
    : m_processFn(std::move(processFn))
{
}

bool GifArbiter::isImagePacket(const uint8_t *data, uint32_t sizeBytes)
{
    if (!data || sizeBytes < 16u)
        return false;

    uint64_t tagLo = 0;
    std::memcpy(&tagLo, data, sizeof(tagLo));
    const uint8_t flg = static_cast<uint8_t>((tagLo >> 58) & 0x3u);
    return flg >= 2u; // IMAGE, or FLG 3 which operates like IMAGE
}

void GifArbiter::submit(GifPathId pathId, const uint8_t *data, uint32_t sizeBytes, bool path2DirectHl)
{
    if (!data || sizeBytes < 16 || !m_processFn)
        return;

    GifArbiterPacket pkt;
    pkt.pathId = pathId;
    pkt.path2DirectHl = (pathId == GifPathId::Path2) && path2DirectHl;
    pkt.path3Image = (pathId == GifPathId::Path3) && isImagePacket(data, sizeBytes);
    pkt.data.resize(sizeBytes);
    std::memcpy(pkt.data.data(), data, sizeBytes);
    if (Ps2GifOrderTraceEnabled())
    {
        char detail[128];
        std::snprintf(detail, sizeof(detail), "{\"queue_index\":%zu,\"direct_hl\":%s,\"image\":%s}",
            m_queue.size(), pkt.path2DirectHl ? "true" : "false", pkt.path3Image ? "true" : "false");
        Ps2TraceGifOrder("arbiter-enqueue", static_cast<unsigned>(pathId), data, sizeBytes, this, 0u, 0u, detail);
    }
    m_queue.push_back(std::move(pkt));
}

void GifArbiter::drain()
{
    if (!m_processFn)
        return;

    if (Ps2GifOrderTraceEnabled())
    for (size_t i = 0; i < m_queue.size(); ++i)
    {
        const auto &pkt = m_queue[i];
        char detail[96]; std::snprintf(detail, sizeof(detail), "{\"index\":%zu,\"queue_size\":%zu}", i, m_queue.size());
        Ps2TraceGifOrder("arbiter-before-sort", static_cast<unsigned>(pkt.pathId), pkt.data.data(),
            static_cast<uint32_t>(pkt.data.size()), this, 0u, 0u, detail);
    }
    std::stable_sort(m_queue.begin(), m_queue.end(),
                     [](const GifArbiterPacket &a, const GifArbiterPacket &b)
                     {
                         // DIRECTHL cannot preempt PATH3 IMAGE transfers.
                         if (a.path2DirectHl != b.path2DirectHl || a.path3Image != b.path3Image)
                         {
                             if (a.path3Image && b.path2DirectHl)
                                 return true;
                             if (a.path2DirectHl && b.path3Image)
                                 return false;
                         }
                         return pathPriority(a.pathId) < pathPriority(b.pathId);
                     });

    for (size_t i = 0; i < m_queue.size(); ++i)
    {
        auto &pkt = m_queue[i];
        if (!pkt.data.empty())
        {
            char detail[96]; std::snprintf(detail, sizeof(detail), "{\"index\":%zu,\"queue_size\":%zu}", i, m_queue.size());
            Ps2TraceGifOrder("arbiter-dispatch", static_cast<unsigned>(pkt.pathId), pkt.data.data(),
                static_cast<uint32_t>(pkt.data.size()), this, 0u, 0u, detail);
            m_processFn(pkt.data.data(), static_cast<uint32_t>(pkt.data.size()));
        }
    }
    m_queue.clear();
}

uint8_t GifArbiter::pathPriority(GifPathId id)
{
    return static_cast<uint8_t>(id);
}
