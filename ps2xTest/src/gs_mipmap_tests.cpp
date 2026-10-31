#include "MiniTest.h"
#include "runtime/gs/gs_cpu_backend.h"
#include "runtime/gs/gs_mipmap.h"
#include <cmath>
#include <vector>

namespace
{
    constexpr uint32_t colors[] = {0x800000ffu, 0x80ff0000u, 0x8000ff00u};
    constexpr uint32_t bases[] = {0x2000u, 0x2100u, 0x2200u};

    uint64_t tex1(unsigned mmin, unsigned maximum, int k = 0, bool constant = false)
    {
        return (static_cast<uint64_t>(k & 0xfff) << 32u) | (mmin << 6u) | (maximum << 2u) | (constant ? 1u : 0u);
    }

    struct Fixture
    {
        std::vector<uint8_t> vram = std::vector<uint8_t>(4u * 1024u * 1024u);
        GSCpuBackend backend;
        GSPrimitiveBatch batch{};
        Fixture()
        {
            backend.Initialize(vram.data(), static_cast<uint32_t>(vram.size()));
            batch.vertexCount = 3;
            batch.state.prim.type = GS_PRIM_TRIANGLE;
            batch.state.prim.tme = true;
            batch.state.colclamp = 1;
            batch.state.textureWidth = batch.state.textureHeight = 16;
            auto &ctx = batch.state.context;
            ctx.frame.fbw = 1;
            ctx.frame.psm = GS_PSM_CT32;
            ctx.scissor.x1 = ctx.scissor.y1 = 3;
            ctx.test = 1ull << 17u; // ZTST=ALWAYS; exercise sampling rather than rejecting every pixel.
            ctx.zbuf.zmask = true;
            ctx.tex0.tbp0 = bases[0]; ctx.tex0.tbw = 1;
            ctx.tex0.tw = ctx.tex0.th = 4;
            ctx.tex0.psm = GS_PSM_CT32; ctx.tex0.tfx = 1; ctx.tex0.tcc = 1;
            ctx.miptbp1 = bases[1] | (1ull << 14u) | (static_cast<uint64_t>(bases[2]) << 20u) | (1ull << 34u);
            batch.vertices[1].x = 4; batch.vertices[2].y = 4;
            for (auto &vertex : batch.vertices) vertex.a = 128;
            for (unsigned level = 0; level < 3; ++level)
                for (unsigned y = 0; y < (16u >> level); ++y)
                    for (unsigned x = 0; x < (16u >> level); ++x)
                        backend.WriteVram(GS_PSM_CT32, bases[level], 1, x, y, colors[level]);
        }
        uint32_t render(float q)
        {
            for (auto &vertex : batch.vertices) { vertex.q = q; vertex.s = vertex.t = q * 0.125f; }
            backend.Submit(batch);
            return backend.ReadVram(GS_PSM_CT32, 0, 1, 0, 0);
        }
    };
}

void register_gs_mipmap_tests()
{
    MiniTest::Case("GSMipmap", [](TestCase &tc)
    {
        tc.Run("Q selects separately resident mip images and MXL zero preserves level zero", [](TestCase &t)
        {
            Fixture f;
            f.batch.state.context.tex1 = tex1(2, 2);
            t.Equals(f.render(0.5f), colors[1], "Q=1/2 should sample resident mip1");
            t.Equals(f.render(-0.25f), colors[2], "absolute Q=1/4 should sample resident mip2");
            f.batch.state.context.tex1 = tex1(2, 0);
            t.Equals(f.render(0.01f), colors[0], "MXL=0 keeps the existing base-level/Boku behavior");
        });
        tc.Run("constant LOD, signed K and L scaling select the intended level", [](TestCase &t)
        {
            Fixture f;
            f.batch.state.context.tex1 = tex1(2, 2, 16, true);
            t.Equals(f.render(1.0f), colors[1], "LCM uses K/16 regardless of Q");
            f.batch.state.context.tex1 = tex1(2, 2, -16);
            t.Equals(f.render(0.25f), colors[1], "negative K lowers dynamic LOD");
            f.batch.state.context.tex1 = tex1(2, 2) | (1ull << 19u);
            t.Equals(f.render(0.5f), colors[2], "L multiplies log2 LOD by two");
        });
        tc.Run("nearest mip rounds and trilinear blends adjacent resident colors", [](TestCase &t)
        {
            Fixture f;
            f.batch.state.context.tex1 = tex1(2, 2, 12, true);
            t.Equals(f.render(1), colors[1], "LOD .75 rounds to mip1");
            f.batch.state.context.tex1 = tex1(3, 2, 8, true);
            t.Equals(f.render(1), 0x80800080u, "LOD .5 mixes red base and blue mip1");
            f.batch.state.context.tex1 = tex1(5, 2, 8, true);
            t.Equals(f.render(1), 0x80800080u, "linear/trilinear also blends the two levels");
        });
        tc.Run("UV coordinates and per-level nearest or linear filtering", [](TestCase &t)
        {
            Fixture f;
            f.batch.state.prim.fst = true;
            for (auto &vertex : f.batch.vertices) { vertex.u = 32; vertex.v = 16; }
            for (unsigned y = 0; y < 8; ++y)
            {
                f.backend.WriteVram(GS_PSM_CT32, bases[1], 1, 0, y, colors[0]);
                f.backend.WriteVram(GS_PSM_CT32, bases[1], 1, 1, y, colors[1]);
            }
            f.batch.state.context.tex1 = tex1(2, 2, 16, true);
            t.Equals(f.render(1), colors[1], "base U=2 shifts to mip1 U=1");
            f.batch.state.context.tex1 = tex1(4, 2, 16, true);
            t.Equals(f.render(1), 0x80800080u, "linear mip filter samples half between adjacent texels");
        });
        tc.Run("all six MIPTBP slots and region clamp bounds scale independently", [](TestCase &t)
        {
            GSTex0Reg base{}; base.tbp0 = 77; base.tbw = 4; base.tw = 8; base.th = 7; base.cbp = 123;
            const uint64_t first = 101ull | (2ull << 14u) | (102ull << 20u) | (3ull << 34u) | (103ull << 40u) | (4ull << 54u);
            const uint64_t second = 104ull | (5ull << 14u) | (105ull << 20u) | (6ull << 34u) | (106ull << 40u) | (7ull << 54u);
            for (unsigned level = 1; level <= 6; ++level)
            {
                const auto texture = GSMip::LevelTexture(base, first, second, level);
                t.Equals(texture.tbp0, 100u + level, "each level uses its own 14-bit base");
                t.Equals(static_cast<unsigned>(texture.tbw), level + 1u, "each level uses its own six-bit width");
                t.Equals(texture.cbp, base.cbp, "mip selection preserves the already-loaded CLUT");
            }
            const uint64_t clamp = 10u | (12ull << 4u) | (28ull << 14u) | (20ull << 24u) | (44ull << 34u);
            t.Equals(GSMip::ScaleClamp(clamp, 2), 10ull | (3ull << 4u) | (7ull << 14u) | (5ull << 24u) | (11ull << 34u),
                "region clamp masks and bounds shift while wrap modes remain unchanged");
            t.Equals(GSMip::Select(tex1(2, 7), 0.0f).level0, 6u, "LOD is bounded to the six available mip slots");
        });
    });
}
