#include "gs_test_support.h"

using namespace GSTest;

namespace
{
    // Boku2 native capture v7, gs-0, primitive0. Pixel (384,192) is strictly
    // interior: weights approx (.25568458,.6470731,.097242296). Their float
    // sum is .99999994, so the old weighted sum turns constant alpha128
    // into127 and incorrectly fails ATEST against128.
    GSPrimitiveBatch capturedTriangle()
    {
        GSPrimitiveBatch batch{};
        batch.vertexCount = 3;
        auto& s = batch.state;
        s.prim.type = GS_PRIM_TRIANGLE;
        s.prim.iip = true;
        s.context.frame.fbp = kOutputPage;
        s.context.frame.fbw = 10;
        s.context.frame.psm = GS_PSM_CT32;
        s.context.zbuf.zbp = 0;
        s.context.zbuf.psm = GS_PSM_Z32;
        s.context.zbuf.zmask = false;
        s.context.scissor.x0 = s.context.scissor.y0 = 0;
        s.context.scissor.x1 = 639;
        s.context.scissor.y1 = 223;
        s.context.xyoffset.ofx = 1728u << 4;
        s.context.xyoffset.ofy = 1936u << 4;
        s.context.test = (1ull << 16) | (1ull << 17); // ZTE, ZTST=ALWAYS.
        const float xs[3] = {2109.375f, 2113.5f, 2114.0625f};
        const float ys[3] = {2128.3125f, 2129.0625f, 2125.25f};
        for (unsigned i = 0; i < 3; ++i)
        {
            auto& v = batch.vertices[i];
            v.x = xs[i];
            v.y = ys[i];
            v.z = 1000;
            v.r = 248;
            v.g = v.b = 0;
            v.a = 128;
            v.q = 1;
        }
        return batch;
    }

    void constantAlpha(unsigned alphaTest)
    {
        BackendFixture f;
        auto batch = capturedTriangle();
        // ATE, ATST=EQUAL or GEQUAL, AREF=128, AFAIL=KEEP.
        batch.state.context.test |= 1ull | (uint64_t(alphaTest) << 1) | (128ull << 4);
        f.backend.Submit(batch);
        expectEqual(f.backend.ReadVram(GS_PSM_CT32, kOutputPage * 32u, 10, 384, 192), kRed,
                    "interior pixel with constant alpha128 must pass ATEST128");
        expectEqual(f.backend.ReadVram(GS_PSM_Z32, 0, 10, 384, 192), 1000,
                    "constant-alpha interpolation must not suppress a valid depth write");
    }

    void constantAlphaEqual() { constantAlpha(4); }
    void constantAlphaGreaterEqual() { constantAlpha(5); }

    void neutralModulation()
    {
        BackendFixture f;
        auto batch = capturedTriangle();
        batch.state.prim.tme = batch.state.prim.fst = true;
        batch.state.context.tex0 = texture();
        batch.state.context.tex0.tfx = 0; // MODULATE; constant128 is the identity.
        batch.state.context.clamp = 5;
        batch.state.textureWidth = batch.state.textureHeight = 1;
        constexpr uint32_t texel = 0x80C86432u;
        f.backend.WriteVram(GS_PSM_CT32, batch.state.context.tex0.tbp0, 2, 0, 0, texel);
        for (auto& vertex : batch.vertices)
            vertex.r = vertex.g = vertex.b = vertex.a = 128;
        f.backend.Submit(batch);
        const uint32_t result = f.backend.ReadVram(GS_PSM_CT32, kOutputPage * 32u, 10, 384, 192);
        expectEqual(result & 0xFFFFFFu, texel & 0xFFFFFFu,
                    "constant Gouraud128 must preserve RGB under MODULATE, not darken by one");
    }

    void genuinelyLowAlphaStillFails()
    {
        BackendFixture f;
        auto batch = capturedTriangle();
        batch.state.context.test |= 1ull | (5ull << 1) | (128ull << 4);
        for (auto& vertex : batch.vertices)
            vertex.a = 127;
        f.backend.Submit(batch);
        expectEqual(f.backend.ReadVram(GS_PSM_CT32, kOutputPage * 32u, 10, 384, 192), 0,
                    "the fix must preserve real alpha127 failure at threshold128");
        expectEqual(f.backend.ReadVram(GS_PSM_Z32, 0, 10, 384, 192), 0,
                    "AFAIL=KEEP still suppresses depth for genuinely failing alpha");
    }
}

int main(int argc, char** argv)
{
    return run(argc, argv, {
        {"constant_alpha_equal", constantAlphaEqual},
        {"constant_alpha_gequal", constantAlphaGreaterEqual},
        {"neutral_modulation", neutralModulation},
        {"low_alpha_still_fails", genuinelyLowAlphaStillFails},
    });
}
