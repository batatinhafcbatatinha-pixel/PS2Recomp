// Memory access for the code written by optimize_runner.py. RAM below this limit is one comparison and a load or store;
// everything else takes the runtime's normal path, out of line. OPT_* also stores the instruction's address there, because
// those instructions no longer store ctx->pc themselves.
#pragma once
// Each guest instruction computes what the console computed: with the labels gone, fast math would fold and reorder across them.
#pragma float_control(precise, on)
#pragma fp_contract(off)
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <emmintrin.h>

namespace ps2opt
{
    constexpr uint32_t kFastLimit = PS2_RAM_SIZE - 16u;

#define PS2OPT_SLOW __declspec(noinline) inline
    PS2OPT_SLOW uint8_t slowRead8(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t a) { return PS2Runtime::isSpecialAddress(a) ? runtime->Load8(rdram, ctx, a) : FAST_READ8(a); }
    PS2OPT_SLOW uint16_t slowRead16(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t a) { return PS2Runtime::isSpecialAddress(a) ? runtime->Load16(rdram, ctx, a) : FAST_READ16(a); }
    PS2OPT_SLOW uint32_t slowRead32(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t a) { return PS2Runtime::isSpecialAddress(a) ? runtime->Load32(rdram, ctx, a) : FAST_READ32(a); }
    PS2OPT_SLOW uint64_t slowRead64(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t a) { return PS2Runtime::isSpecialAddress(a) ? runtime->Load64(rdram, ctx, a) : FAST_READ64(a); }
    PS2OPT_SLOW __m128i slowRead128(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t a) { return PS2Runtime::isSpecialAddress(a) ? runtime->Load128(rdram, ctx, a) : FAST_READ128(a); }
    PS2OPT_SLOW void slowWrite8(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t a, uint8_t v) { if (PS2Runtime::isSpecialAddress(a)) runtime->Store8(rdram, ctx, a, v); else FAST_WRITE8(a, v); }
    PS2OPT_SLOW void slowWrite16(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t a, uint16_t v) { if (PS2Runtime::isSpecialAddress(a)) runtime->Store16(rdram, ctx, a, v); else FAST_WRITE16(a, v); }
    PS2OPT_SLOW void slowWrite32(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t a, uint32_t v) { if (PS2Runtime::isSpecialAddress(a)) runtime->Store32(rdram, ctx, a, v); else FAST_WRITE32(a, v); }
    PS2OPT_SLOW void slowWrite64(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t a, uint64_t v) { if (PS2Runtime::isSpecialAddress(a)) runtime->Store64(rdram, ctx, a, v); else FAST_WRITE64(a, v); }
    PS2OPT_SLOW void slowWrite128(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t a, __m128i v) { if (PS2Runtime::isSpecialAddress(a)) runtime->Store128(rdram, ctx, a, v); else FAST_WRITE128(a, v); }

    PS2OPT_SLOW void noLabel(R5900Context *ctx, uint32_t first, uint32_t last)
    {
        // The entry has no case (the generator's default), and addresses outside the function are aliases of it.
        if (ctx->pc <= first || ctx->pc > last)
            return;
        std::fprintf(stderr, "[opt:missing-label] pc=0x%x in function 0x%x-0x%x ra=0x%x\n", ctx->pc, first, last, static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31])));
        std::fflush(stderr);
        std::abort();
    }

    template <typename T>
    __forceinline T load(const uint8_t *rdram, uint32_t a) { T v; std::memcpy(&v, rdram + a, sizeof(T)); return v; }
    template <typename T>
    __forceinline void store(uint8_t *rdram, uint32_t a, T v) { std::memcpy(rdram + a, &v, sizeof(T)); }
    // LQ, SQ, LQC2 and SQC2 ignore the low four address bits (MTSAB + two LQ + QFSRV relies on it).
    template <>
    __forceinline __m128i load<__m128i>(const uint8_t *rdram, uint32_t a) { return _mm_loadu_si128(reinterpret_cast<const __m128i *>(rdram + (a & ~0xFu))); }
    template <>
    __forceinline void store<__m128i>(uint8_t *rdram, uint32_t a, __m128i v) { _mm_storeu_si128(reinterpret_cast<__m128i *>(rdram + (a & ~0xFu)), v); }
}

#define Ps2OptNoLabel(ctx, first, last) ps2opt::noLabel(ctx, first, last)

#define PS2OPT_READ(T, N, a) ps2opt::read##N(rdram, ctx, runtime, static_cast<uint32_t>(a))
#define PS2OPT_READ_FN(T, N)                                                                                \
    namespace ps2opt {                                                                                      \
    __forceinline T read##N(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t a)             \
    { return a < kFastLimit ? load<T>(rdram, a) : slowRead##N(rdram, ctx, runtime, a); }                    \
    __forceinline T readAt##N(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t pc, uint32_t a) \
    { if (a < kFastLimit) return load<T>(rdram, a); ctx->pc = pc; return slowRead##N(rdram, ctx, runtime, a); } }
PS2OPT_READ_FN(uint8_t, 8)
PS2OPT_READ_FN(uint16_t, 16)
PS2OPT_READ_FN(uint32_t, 32)
PS2OPT_READ_FN(uint64_t, 64)
PS2OPT_READ_FN(__m128i, 128)

#define PS2OPT_WRITE_FN(T, N)                                                                                \
    namespace ps2opt {                                                                                       \
    __forceinline void write##N(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t a, T v)     \
    { if (a < kFastLimit) store<T>(rdram, a, v); else slowWrite##N(rdram, ctx, runtime, a, v); }             \
    __forceinline void writeAt##N(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t pc, uint32_t a, T v) \
    { if (a < kFastLimit) store<T>(rdram, a, v); else { ctx->pc = pc; slowWrite##N(rdram, ctx, runtime, a, v); } } }
PS2OPT_WRITE_FN(uint8_t, 8)
PS2OPT_WRITE_FN(uint16_t, 16)
PS2OPT_WRITE_FN(uint32_t, 32)
PS2OPT_WRITE_FN(uint64_t, 64)
PS2OPT_WRITE_FN(__m128i, 128)

#undef READ8
#undef READ16
#undef READ32
#undef READ64
#undef READ128
#undef WRITE8
#undef WRITE16
#undef WRITE32
#undef WRITE64
#undef WRITE128
#define READ8(a) ps2opt::read8(rdram, ctx, runtime, static_cast<uint32_t>(a))
#define READ16(a) ps2opt::read16(rdram, ctx, runtime, static_cast<uint32_t>(a))
#define READ32(a) ps2opt::read32(rdram, ctx, runtime, static_cast<uint32_t>(a))
#define READ64(a) ps2opt::read64(rdram, ctx, runtime, static_cast<uint32_t>(a))
#define READ128(a) ps2opt::read128(rdram, ctx, runtime, static_cast<uint32_t>(a))
#define WRITE8(a, v) ps2opt::write8(rdram, ctx, runtime, static_cast<uint32_t>(a), static_cast<uint8_t>(v))
#define WRITE16(a, v) ps2opt::write16(rdram, ctx, runtime, static_cast<uint32_t>(a), static_cast<uint16_t>(v))
#define WRITE32(a, v) ps2opt::write32(rdram, ctx, runtime, static_cast<uint32_t>(a), static_cast<uint32_t>(v))
#define WRITE64(a, v) ps2opt::write64(rdram, ctx, runtime, static_cast<uint32_t>(a), static_cast<uint64_t>(v))
#define WRITE128(a, v) ps2opt::write128(rdram, ctx, runtime, static_cast<uint32_t>(a), (v))
#define OPT_READ8(pc, a) ps2opt::readAt8(rdram, ctx, runtime, pc, static_cast<uint32_t>(a))
#define OPT_READ16(pc, a) ps2opt::readAt16(rdram, ctx, runtime, pc, static_cast<uint32_t>(a))
#define OPT_READ32(pc, a) ps2opt::readAt32(rdram, ctx, runtime, pc, static_cast<uint32_t>(a))
#define OPT_READ64(pc, a) ps2opt::readAt64(rdram, ctx, runtime, pc, static_cast<uint32_t>(a))
#define OPT_READ128(pc, a) ps2opt::readAt128(rdram, ctx, runtime, pc, static_cast<uint32_t>(a))
#define OPT_WRITE8(pc, a, v) ps2opt::writeAt8(rdram, ctx, runtime, pc, static_cast<uint32_t>(a), static_cast<uint8_t>(v))
#define OPT_WRITE16(pc, a, v) ps2opt::writeAt16(rdram, ctx, runtime, pc, static_cast<uint32_t>(a), static_cast<uint16_t>(v))
#define OPT_WRITE32(pc, a, v) ps2opt::writeAt32(rdram, ctx, runtime, pc, static_cast<uint32_t>(a), static_cast<uint32_t>(v))
#define OPT_WRITE64(pc, a, v) ps2opt::writeAt64(rdram, ctx, runtime, pc, static_cast<uint32_t>(a), static_cast<uint64_t>(v))
#define OPT_WRITE128(pc, a, v) ps2opt::writeAt128(rdram, ctx, runtime, pc, static_cast<uint32_t>(a), (v))
