// GLSL for GSGpuBackend. Local memory lives in one SSBO and is addressed with the
// same page lookup tables as GSMem, so GPU and CPU paths agree byte for byte.
// The draw shader is a per-pixel port of GSCpuBackend (WritePixel, SampleTexture,
// DrawSprite/DrawTriangle/point); fragment shader interlock keeps primitive order.

static const char *kGsGlslCommon = R"GLSL(
layout(std430, binding = 0) coherent buffer Vram { uint vram[]; };
layout(std430, binding = 1) readonly buffer Tables { uint tables[]; };

const uint kPW[8] = uint[8](64u, 64u, 64u, 64u, 64u, 64u, 128u, 128u);
const uint kPH[8] = uint[8](32u, 32u, 64u, 64u, 64u, 64u, 64u, 128u);
const uint kBase[8] = uint[8](0u, 65536u, 131072u, 262144u, 393216u, 524288u, 655360u, 917504u);
const uint kUnpacked[8] = uint[8](32u, 32u, 16u, 16u, 16u, 16u, 8u, 4u);

uint layoutOf(uint psm)
{
    switch (psm)
    {
    case 48u: case 49u: return 1u;
    case 2u: return 2u;
    case 10u: return 3u;
    case 50u: return 4u;
    case 58u: return 5u;
    case 19u: return 6u;
    case 20u: return 7u;
    default: return 0u; // CT32, CT24, T8H, T4HL, T4HH
    }
}

uint bppOf(uint psm)
{
    switch (psm)
    {
    case 1u: case 49u: return 24u;
    case 2u: case 10u: case 50u: case 58u: return 16u;
    case 19u: case 27u: return 8u;
    case 20u: case 36u: case 44u: return 4u;
    default: return 32u;
    }
}

uint bitOffsetOf(uint psm)
{
    return psm == 27u || psm == 36u ? 24u : (psm == 44u ? 28u : 0u);
}

void vramLocate(uint psm, uint bp, uint bw, uint x, uint y, out uint word, out uint bitpos, out uint bpp)
{
    const uint l = layoutOf(psm);
    const uint pw = kPW[l];
    const uint ph = kPH[l];
    const uint page = bp / 32u + (y / ph) * ((bw * 64u) / pw) + x / pw;
    const uint idx = kBase[l] + (((bp % 32u) * ph + (y % ph)) * pw) + (x % pw);
    const uint entry = (tables[idx >> 1u] >> ((idx & 1u) * 16u)) & 0xFFFFu;
    const uint unit = page * (pw * ph) + entry;
    const uint bits = unit * kUnpacked[l] + bitOffsetOf(psm);
    bpp = bppOf(psm);
    const uint storage = bpp >= 24u ? 4u : (bpp == 16u ? 2u : 1u);
    const uint byteAddr = (bits >> 3u) & (0x400000u - storage);
    word = byteAddr >> 2u;
    bitpos = (byteAddr & 3u) * 8u + (bits & 7u);
}

uint vramRead(uint psm, uint bp, uint bw, uint x, uint y)
{
    uint word, bitpos, bpp;
    vramLocate(psm, bp, bw, x, y, word, bitpos, bpp);
    const uint w = vram[word];
    if (bpp == 32u) return w;
    if (bpp == 24u) return w & 0xFFFFFFu;
    const uint mask = bpp == 16u ? 0xFFFFu : (bpp == 8u ? 0xFFu : 0xFu);
    return (w >> bitpos) & mask;
}

void vramWrite(uint psm, uint bp, uint bw, uint x, uint y, uint value)
{
    uint word, bitpos, bpp;
    vramLocate(psm, bp, bw, x, y, word, bitpos, bpp);
    if (bpp == 32u)
    {
        vram[word] = value;
        return;
    }
    if (bpp == 24u)
    {
        atomicAnd(vram[word], 0xFF000000u);
        atomicOr(vram[word], value & 0xFFFFFFu);
        return;
    }
    const uint mask = bpp == 16u ? 0xFFFFu : (bpp == 8u ? 0xFFu : 0xFu);
    atomicAnd(vram[word], ~(mask << bitpos));
    atomicOr(vram[word], (value & mask) << bitpos);
}

uint rgba5551To8888(uint c)
{
    return (((c >> 0u) & 0x1Fu) << 3u) | ((((c >> 5u) & 0x1Fu) << 3u) << 8u) |
           ((((c >> 10u) & 0x1Fu) << 3u) << 16u) | ((((c >> 15u) & 0x1u) << 7u) << 24u);
}

uint rgba8888To5551(uint c)
{
    return (((c >> 0u) & 0xFFu) >> 3u) | ((((c >> 8u) & 0xFFu) >> 3u) << 5u) |
           ((((c >> 16u) & 0xFFu) >> 3u) << 10u) | ((((c >> 24u) & 0xFFu) >> 7u) << 15u);
}
)GLSL";

// Scanout detiling/color conversion/CRT composition. All source reads stay on
// the GPU; output is a host-facing RGBA buffer for the existing window ABI.
static const char *kGsScanoutCompute = R"GLSL(
layout(local_size_x = 64) in;
layout(std430, binding = 5) writeonly buffer Scanout { uint rgbaOutput[]; };
layout(std430, binding = 6) readonly buffer ScanoutParams { uint p[]; };
layout(std430, binding = 7) buffer Presence { uint presence[]; };
bool covered(uint source, uint x, uint y) {
    uint at = 8u + source * 8u;
    return x < p[at + 5u] && y < p[at + 6u];
}
uint sampleDisplay(uint source, uint x, uint y) {
    if (!covered(source, x, y)) return 0u;
    uint at = 8u + source * 8u, psm = p[at];
    uint value = vramRead(psm, p[at+1u], p[at+2u], x+p[at+3u], y+p[at+4u]);
    if (psm == 1u) return value | 0xFF000000u;
    if (psm == 2u || psm == 10u) {
        uvec3 c = uvec3(value, value >> 5u, value >> 10u) & 31u;
        c = (c << 3u) | (c >> 2u);
        return c.x | (c.y << 8u) | (c.z << 16u) | ((value & 0x8000u) != 0u ? 0x80000000u : 0u);
    }
    return value;
}
uint blendChannel(uint a, uint b, uint factor) {
    return uint(clamp(int(b) + ((int(a)-int(b))*int(factor))/255, 0, 255));
}
void main() {
    uint pixel = gl_GlobalInvocationID.x;
    if (pixel >= p[2] * 512u) return;
    uint x = pixel % p[2], y = pixel / p[2];
    if (x >= p[0] || y >= p[1]) { rgbaOutput[pixel] = 0u; return; }
    bool dual = (p[3] & 1u) != 0u;
    uint probe1 = sampleDisplay(0u, x, y);
    if ((probe1 & 0xFFFFFFu) != 0u) atomicOr(presence[1], 1u);
    if (dual && (sampleDisplay(1u, x, y) & 0xFFFFFFu) != 0u) atomicOr(presence[2], 1u);
    uint sy = y;
    if ((p[3] & 16u) != 0u && p[1] >= 2u)
        sy = min(((y >> 1u) << 1u) + ((p[3] & 32u) != 0u ? 1u : 0u), p[1]-1u);
    uint first = sampleDisplay(0u, x, sy), result;
    if (!dual) result = first;
    else {
        result = p[5] | (p[4] << 24u);
        if ((p[3] & 8u) == 0u && covered(1u, x, sy)) result = sampleDisplay(1u, x, sy);
        if (covered(0u, x, sy)) {
            uint factor = (p[3] & 2u) != 0u ? p[4] : min(255u, (first >> 24u) * 2u);
            result = blendChannel(first & 255u, result & 255u, factor) |
                     (blendChannel((first >> 8u) & 255u, (result >> 8u) & 255u, factor) << 8u) |
                     (blendChannel((first >> 16u) & 255u, (result >> 16u) & 255u, factor) << 16u);
        }
    }
    rgbaOutput[pixel] = (result & 0xFFFFFFu) | 0xFF000000u;
}
)GLSL";

static const char *kGsGlslDrawTypes = R"GLSL(
struct Prim
{
    float x[3]; float y[3];
    uint z[3]; uint rgba[3];
    float s[3]; float t[3]; float q[3];
    uint uv[3];
    uint fog; uint state; uint type;
    int bbox[4];
    uint pad;
};

struct State
{
    uint fbp, fbw, fpsm, fbmsk;
    uint zbp, zpsm, zmask, flags;
    uint test, alphaMode, alphaFix, fba;
    int scx0, scy0, scx1, scy1;
    uint tbp0, tbw, tpsm, tcc;
    uint tfx, cpsm, csm, csa;
    uint wrapU, wrapV, minU, maxU;
    uint minV, maxV, ta0, ta1;
    uint aem, fogColor, texW, texH;
    int ofx, ofy; uint pad0, pad1;
};

layout(std430, binding = 3) readonly buffer Prims { Prim prims[]; };
layout(std430, binding = 4) readonly buffer States { State states[]; };
)GLSL";

static const char *kGsDrawVertex = R"GLSL(
flat out uint vPrim;
void main()
{
    const uint prim = uint(gl_VertexID) / 6u;
    const uint corner = uint(gl_VertexID) % 6u;
    const Prim p = prims[prim];
    // Two triangles over the inclusive pixel bounding box.
    const bool right = corner == 1u || corner == 2u || corner == 4u;
    const bool top = corner == 2u || corner == 4u || corner == 5u;
    const float x = float(right ? p.bbox[2] + 1 : p.bbox[0]);
    const float y = float(top ? p.bbox[3] + 1 : p.bbox[1]);
    gl_Position = vec4(x / 1024.0 - 1.0, y / 1024.0 - 1.0, 0.0, 1.0);
    vPrim = prim;
}
)GLSL";

static const char *kGsDrawFragment = R"GLSL(
layout(pixel_interlock_ordered) in;
flat in uint vPrim;
layout(std430, binding = 2) readonly buffer Clut { uint clut[]; };
layout(std430, binding = 7) buffer Debug { uint debugCounters[8]; };

const uint kFlagIip = 1u, kFlagTme = 2u, kFlagFge = 4u, kFlagAbe = 8u, kFlagFst = 16u, kFlagPabe = 32u, kFlagLinear = 64u;

int clampU8(int v) { return clamp(v, 0, 255); }

float fabsQ(float q) { return abs(q) > 1.0e-8 ? q : 1.0; }

uint applyTexa(State s, uint psm, uint texel)
{
    if (psm == 0u)
        return texel;
    const uint r = texel & 0xFFu, g = (texel >> 8u) & 0xFFu, b = (texel >> 16u) & 0xFFu;
    const bool rgbZero = r == 0u && g == 0u && b == 0u;
    uint a = (texel >> 24u) & 0xFFu;
    if (psm == 1u)
        a = (s.aem != 0u && rgbZero) ? 0u : s.ta0;
    else if (psm == 2u || psm == 10u)
        a = (a & 0x80u) != 0u ? s.ta1 : ((s.aem != 0u && rgbZero) ? 0u : s.ta0);
    return (texel & 0x00FFFFFFu) | (a << 24u);
}

int wrapCoord(int c, int size, uint mode, uint minV, uint maxV)
{
    switch (mode & 3u)
    {
    case 0u: return int(uint(c) & uint(size - 1));
    case 1u: return clamp(c, 0, size - 1);
    case 2u: return min(max(c, int(minV)), int(maxV));
    default: return int((uint(c) & minV) | maxV);
    }
}

bool isFourBit(uint psm) { return psm == 20u || psm == 36u || psm == 44u; }
bool isEightBit(uint psm) { return psm == 19u || psm == 27u; }

uint lookupClut(State s, uint index)
{
    const bool sixteenBit = s.cpsm == 2u || s.cpsm == 10u;
    const uint clutBase = (s.csa & (sixteenBit ? 0x1Fu : 0x0Fu)) << 4u;
    const uint sourceIndex = isFourBit(s.tpsm) ? (index & 0x0Fu) : index;
    uint clutIndex = (clutBase + sourceIndex) & (sixteenBit ? 0x1FFu : 0x0FFu);
    if (!sixteenBit && s.csm == 0u && isEightBit(s.tpsm))
        clutIndex = min((sourceIndex & 0xF0u) + clutBase, 240u) + (sourceIndex & 0x0Fu);
    if (s.cpsm == 0u)
        return applyTexa(s, 0u, clut[clutIndex] | (clut[clutIndex + 256u] << 16u));
    if (s.cpsm == 1u)
        return applyTexa(s, 1u, (clut[clutIndex] | (clut[clutIndex + 256u] << 16u)) & 0x00FFFFFFu);
    if (sixteenBit)
        return applyTexa(s, s.cpsm, rgba5551To8888(clut[clutIndex]));
    return 0xFFFF00FFu;
}

uint samplePoint(State s, int su, int sv)
{
    su = wrapCoord(su, int(s.texW), s.wrapU, s.minU, s.maxU);
    sv = wrapCoord(sv, int(s.texH), s.wrapV, s.minV, s.maxV);
    const uint texel = vramRead(s.tpsm, s.tbp0, s.tbw, uint(su), uint(sv));
    switch (s.tpsm)
    {
    case 0u: case 48u: case 1u: case 49u:
        return applyTexa(s, s.tpsm, texel);
    case 2u: case 10u: case 50u: case 58u:
        return applyTexa(s, s.tpsm, rgba5551To8888(texel));
    case 19u: case 27u: case 20u: case 36u: case 44u:
        return lookupClut(s, texel & 0xFFu);
    default:
        return 0xFFFF00FFu;
    }
}

uint lerpChannel(uint c00, uint c10, uint c01, uint c11, float fx, float fy)
{
    precise float top = float(c00) + (float(c10) - float(c00)) * fx;
    precise float bottom = float(c01) + (float(c11) - float(c01)) * fx;
    precise float v = top + (bottom - top) * fy;
    return uint(clampU8(int(floor(v + 0.5))));
}

uint sampleTexture(State s, float sf, float tf, float qf, uint u, uint v)
{
    precise float texUf, texVf;
    if ((s.flags & kFlagFst) != 0u)
    {
        texUf = float(u) / 16.0;
        texVf = float(v) / 16.0;
    }
    else
    {
        precise float invQ = 1.0 / fabsQ(qf);
        texUf = sf * invQ * float(s.texW);
        texVf = tf * invQ * float(s.texH);
    }
    if ((s.flags & kFlagLinear) == 0u)
        return samplePoint(s, int(texUf), int(texVf));
    precise float su = texUf - 0.5, sv = texVf - 0.5;
    const int u0 = int(floor(su)), v0 = int(floor(sv));
    precise float fx = su - float(u0), fy = sv - float(v0);
    const uint c00 = samplePoint(s, u0, v0), c10 = samplePoint(s, u0 + 1, v0);
    const uint c01 = samplePoint(s, u0, v0 + 1), c11 = samplePoint(s, u0 + 1, v0 + 1);
    uint result = 0u;
    for (uint shift = 0u; shift < 32u; shift += 8u)
        result |= lerpChannel((c00 >> shift) & 0xFFu, (c10 >> shift) & 0xFFu, (c01 >> shift) & 0xFFu,
                              (c11 >> shift) & 0xFFu, fx, fy) << shift;
    return result;
}

void combineTexture(State s, inout int r, inout int g, inout int b, inout int a, uint texel)
{
    const int tr = int(texel & 0xFFu), tg = int((texel >> 8u) & 0xFFu), tb = int((texel >> 16u) & 0xFFu), ta = int(texel >> 24u);
    const bool hasAlpha = s.tcc != 0u;
    switch (s.tfx)
    {
    case 0u:
        r = clampU8((tr * r) >> 7); g = clampU8((tg * g) >> 7); b = clampU8((tb * b) >> 7);
        a = hasAlpha ? clampU8((ta * a) >> 7) : a;
        break;
    case 2u:
        r = clampU8(((tr * r) >> 7) + a); g = clampU8(((tg * g) >> 7) + a); b = clampU8(((tb * b) >> 7) + a);
        a = hasAlpha ? clampU8(ta + a) : a;
        break;
    case 3u:
        r = clampU8(((tr * r) >> 7) + a); g = clampU8(((tg * g) >> 7) + a); b = clampU8(((tb * b) >> 7) + a);
        a = hasAlpha ? ta : a;
        break;
    default: // DECAL
        r = tr; g = tg; b = tb;
        a = hasAlpha ? ta : a;
        break;
    }
}

bool passesAlphaTest(uint test, uint alpha)
{
    if ((test & 1u) == 0u)
        return true;
    const uint aref = (test >> 4u) & 0xFFu;
    switch ((test >> 1u) & 7u)
    {
    case 0u: return false;
    case 1u: return true;
    case 2u: return alpha < aref;
    case 3u: return alpha <= aref;
    case 4u: return alpha == aref;
    case 5u: return alpha >= aref;
    case 6u: return alpha > aref;
    default: return alpha != aref;
    }
}

int pickRgb(uint sel, int cs, int cd) { return sel == 0u ? cs : (sel == 1u ? cd : 0); }

void writePixel(State s, uint x, uint y, uint z, int r, int g, int b, int a, int fog)
{
    if ((s.flags & kFlagFge) != 0u)
    {
        const int inverseFog = 255 - fog;
        r = ((fog * r) >> 8) + ((inverseFog * int(s.fogColor & 0xFFu)) >> 8);
        g = ((fog * g) >> 8) + ((inverseFog * int((s.fogColor >> 8u) & 0xFFu)) >> 8);
        b = ((fog * b) >> 8) + ((inverseFog * int((s.fogColor >> 16u) & 0xFFu)) >> 8);
        r &= 0xFF; g &= 0xFF; b &= 0xFF;
    }

    bool writeRgb = true, writeAlpha = true, writeDepth = true;
    if (!passesAlphaTest(s.test, uint(a)))
    {
        switch ((s.test >> 12u) & 3u)
        {
        case 1u: writeDepth = false; break;
        case 2u: writeRgb = false; writeAlpha = false; break;
        case 3u:
            writeDepth = false;
            if (s.fpsm == 0u) writeAlpha = false;
            break;
        default: return;
        }
    }
    const bool writesFb = writeRgb || writeAlpha;
    if (!writesFb && !writeDepth)
        return;

    const uint fpsm = s.fpsm;
    const bool sixteen = fpsm == 2u || fpsm == 10u || fpsm == 50u || fpsm == 58u;
    const bool preserveDestAlpha = writeRgb && !writeAlpha && fpsm == 0u;
    const bool dateNeedsRead = ((s.test >> 14u) & 1u) != 0u && (fpsm == 0u || fpsm == 2u || fpsm == 10u);
    const bool abe = (s.flags & kFlagAbe) != 0u;
    const bool frmw = dateNeedsRead || (writesFb && (s.fbmsk != 0u || abe || preserveDestAlpha));

    uint raw = 0u, fbrgba = 0u;
    if (frmw)
    {
        raw = vramRead(fpsm, s.fbp, s.fbw, x, y);
        fbrgba = raw;
        if (sixteen) fbrgba = rgba5551To8888(fbrgba);
        else if (fpsm == 1u) fbrgba |= 0x80000000u;
    }

    if (((s.test >> 14u) & 1u) != 0u)
    {
        const bool datm = ((s.test >> 15u) & 1u) != 0u;
        if (fpsm == 0u && (((raw >> 31u) & 1u) != 0u) != datm) return;
        if ((fpsm == 2u || fpsm == 10u) && (((raw >> 15u) & 1u) != 0u) != datm) return;
    }

    const uint ztst = (s.test >> 17u) & 3u;
    if (ztst == 0u) { atomicAdd(debugCounters[4], 1u); return; }
    if (ztst >= 2u)
    {
        const uint stored = vramRead(s.zpsm, s.zbp, s.fbw, x, y);
        if (ztst == 2u ? !(z >= stored) : !(z > stored)) return;
    }

    if (writesFb)
    {
        if (abe && !((s.flags & kFlagPabe) != 0u && (a & 0x80) == 0))
        {
            const int dr = int(fbrgba & 0xFFu), dg = int((fbrgba >> 8u) & 0xFFu), db = int((fbrgba >> 16u) & 0xFFu), da = int(fbrgba >> 24u);
            const uint asel = s.alphaMode & 3u, bsel = (s.alphaMode >> 2u) & 3u, csel = (s.alphaMode >> 4u) & 3u, dsel = (s.alphaMode >> 6u) & 3u;
            const int cAlpha = csel == 0u ? a : (csel == 1u ? da : int(s.alphaFix));
            r = clampU8(((pickRgb(asel, r, dr) - pickRgb(bsel, r, dr)) * cAlpha >> 7) + pickRgb(dsel, r, dr));
            g = clampU8(((pickRgb(asel, g, dg) - pickRgb(bsel, g, dg)) * cAlpha >> 7) + pickRgb(dsel, g, dg));
            b = clampU8(((pickRgb(asel, b, db) - pickRgb(bsel, b, db)) * cAlpha >> 7) + pickRgb(dsel, b, db));
        }
        if (writeAlpha && (s.fba & 1u) != 0u && fpsm != 1u)
            a |= 0x80;
        uint pixel = uint(r & 0xFF) | (uint(g & 0xFF) << 8u) | (uint(b & 0xFF) << 16u) | (uint(a & 0xFF) << 24u);
        if (s.fbmsk != 0u)
            pixel = (pixel & ~s.fbmsk) | (fbrgba & s.fbmsk);
        if (preserveDestAlpha)
            pixel = (pixel & 0x00FFFFFFu) | (fbrgba & 0xFF000000u);
        if (sixteen)
            pixel = rgba8888To5551(pixel);
        vramWrite(fpsm, s.fbp, s.fbw, x, y, pixel);
        atomicAdd(debugCounters[3], 1u);
    }

    if (writeDepth && s.zmask == 0u)
        vramWrite(s.zpsm, s.zbp, s.fbw, x, y, z);
}

void main()
{
    const Prim p = prims[vPrim];
    const State s = states[p.state];
    const int x = int(gl_FragCoord.x);
    const int y = int(gl_FragCoord.y);
    bool covered = x >= p.bbox[0] && x <= p.bbox[2] && y >= p.bbox[1] && y <= p.bbox[3];
    atomicAdd(debugCounters[0], 1u);
    if (covered) atomicAdd(debugCounters[1], 1u);
    int r = 0, g = 0, b = 0, a = 0, fog = 0;
    uint z = 0u;

    if (covered && p.type == 0u)
    {
        // Point (and CPU-expanded line steps): attributes are final, no texturing.
        r = int(p.rgba[0] & 0xFFu); g = int((p.rgba[0] >> 8u) & 0xFFu); b = int((p.rgba[0] >> 16u) & 0xFFu); a = int(p.rgba[0] >> 24u);
        z = p.z[0];
        fog = int(p.fog & 0xFFu);
    }
    else if (covered && p.type == 1u)
    {
        // Sprite, following GSCpuBackend::DrawSprite.
        int x0 = int(p.x[0]) - s.ofx, y0 = int(p.y[0]) - s.ofy;
        int x1 = int(p.x[1]) - s.ofx, y1 = int(p.y[1]) - s.ofy;
        if (x0 > x1) { const int t = x0; x0 = x1; x1 = t; }
        if (y0 > y1) { const int t = y0; y0 = y1; y1 = t; }
        const int spanX = max(1, x1 - x0), spanY = max(1, y1 - y0);
        r = int(p.rgba[1] & 0xFFu); g = int((p.rgba[1] >> 8u) & 0xFFu); b = int((p.rgba[1] >> 16u) & 0xFFu); a = int(p.rgba[1] >> 24u);
        z = p.z[1];
        fog = int((p.fog >> 8u) & 0xFFu);
        if ((s.flags & kFlagTme) != 0u)
        {
            precise float u0f, v0f, u1f, v1f;
            if ((s.flags & kFlagFst) != 0u)
            {
                u0f = float((p.uv[0] & 0xFFFFu) >> 4u); v0f = float((p.uv[0] >> 16u) >> 4u);
                u1f = float((p.uv[1] & 0xFFFFu) >> 4u); v1f = float((p.uv[1] >> 16u) >> 4u);
            }
            else
            {
                const float q0 = fabsQ(p.q[0]), q1 = fabsQ(p.q[1]);
                u0f = (p.s[0] / q0) * float(s.texW); v0f = (p.t[0] / q0) * float(s.texH);
                u1f = (p.s[1] / q1) * float(s.texW); v1f = (p.t[1] / q1) * float(s.texH);
            }
            precise float ty = (float(y - y0) + 0.5) / float(spanY);
            precise float tx = (float(x - x0) + 0.5) / float(spanX);
            precise float texVf = v0f + (v1f - v0f) * ty;
            precise float texUf = u0f + (u1f - u0f) * tx;
            uint texel;
            if ((s.flags & kFlagFst) != 0u)
            {
                const uint su = uint(clamp(int((texUf * 16.0) + 0.5), 0, 0xFFFF));
                const uint sv = uint(clamp(int((texVf * 16.0) + 0.5), 0, 0xFFFF));
                texel = sampleTexture(s, 0.0, 0.0, 1.0, su, sv);
            }
            else
            {
                texel = sampleTexture(s, texUf / float(s.texW), texVf / float(s.texH), 1.0, 0u, 0u);
            }
            combineTexture(s, r, g, b, a, texel);
        }
    }
    else if (covered)
    {
        // Triangle, following GSCpuBackend::DrawTriangle.
        precise float fx0 = p.x[0] - float(s.ofx), fy0 = p.y[0] - float(s.ofy);
        precise float fx1 = p.x[1] - float(s.ofx), fy1 = p.y[1] - float(s.ofy);
        precise float fx2 = p.x[2] - float(s.ofx), fy2 = p.y[2] - float(s.ofy);
        precise float denom = (fy1 - fy2) * (fx0 - fx2) + (fx2 - fx1) * (fy0 - fy2);
        const float winding = denom < 0.0 ? -1.0 : 1.0;
        precise float invAbsDenom = 1.0 / abs(denom);
        precise float px = float(x) + 0.5, py = float(y) + 0.5;
        precise float w0 = (((fy1 - fy2) * (px - fx2) + (fx2 - fx1) * (py - fy2)) * winding) * invAbsDenom;
        precise float w1 = (((fy2 - fy0) * (px - fx2) + (fx0 - fx2) * (py - fy2)) * winding) * invAbsDenom;
        precise float w2 = 1.0 - w0 - w1;
        if (w0 < -1.0e-4 || w1 < -1.0e-4 || w2 < -1.0e-4)
        {
            covered = false;
        }
        else
        {
            precise double zd = double(p.z[0]) * double(w0) + double(p.z[1]) * double(w1) + double(p.z[2]) * double(w2);
            z = uint(zd + 0.5lf);
            if ((s.flags & kFlagIip) != 0u)
            {
                for (uint shift = 0u; shift < 32u; shift += 8u)
                {
                    precise float c = float((p.rgba[0] >> shift) & 0xFFu) * w0 + float((p.rgba[1] >> shift) & 0xFFu) * w1 +
                                      float((p.rgba[2] >> shift) & 0xFFu) * w2;
                    const int ci = clampU8(int(c));
                    if (shift == 0u) r = ci; else if (shift == 8u) g = ci; else if (shift == 16u) b = ci; else a = ci;
                }
            }
            else
            {
                r = int(p.rgba[2] & 0xFFu); g = int((p.rgba[2] >> 8u) & 0xFFu); b = int((p.rgba[2] >> 16u) & 0xFFu); a = int(p.rgba[2] >> 24u);
            }
            if ((s.flags & kFlagTme) != 0u)
            {
                uint texel;
                if ((s.flags & kFlagFst) != 0u)
                {
                    precise float fu = float(p.uv[0] & 0xFFFFu) * w0 + float(p.uv[1] & 0xFFFFu) * w1 + float(p.uv[2] & 0xFFFFu) * w2;
                    precise float fv = float(p.uv[0] >> 16u) * w0 + float(p.uv[1] >> 16u) * w1 + float(p.uv[2] >> 16u) * w2;
                    texel = sampleTexture(s, 0.0, 0.0, 1.0, uint(int(fu)) & 0xFFFFu, uint(int(fv)) & 0xFFFFu);
                }
                else
                {
                    precise float is = p.s[0] * w0 + p.s[1] * w1 + p.s[2] * w2;
                    precise float it = p.t[0] * w0 + p.t[1] * w1 + p.t[2] * w2;
                    precise float iq = p.q[0] * w0 + p.q[1] * w1 + p.q[2] * w2;
                    texel = sampleTexture(s, is, it, iq, 0u, 0u);
                }
                combineTexture(s, r, g, b, a, texel);
            }
            precise float f = float(p.fog & 0xFFu) * w0 + float((p.fog >> 8u) & 0xFFu) * w1 + float((p.fog >> 16u) & 0xFFu) * w2;
            fog = clampU8(int(f));
        }
    }

    if (covered) atomicAdd(debugCounters[2], 1u);
    beginInvocationInterlockARB();
    if (covered)
        writePixel(s, uint(x), uint(y), z, r, g, b, a, fog);
    endInvocationInterlockARB();
}
)GLSL";

// Host-to-local transfer: one invocation per pixel of this UploadImage call.
static const char *kGsUploadCompute = R"GLSL(
layout(local_size_x = 64) in;
layout(std430, binding = 5) readonly buffer Staging { uint staging[]; };
layout(std430, binding = 6) readonly buffer Params
{
    uint startPixel, count, psm, dbp, dbw, rrw, dsax, dsay;
};
uint stagingByte(uint i) { return (staging[i >> 2u] >> ((i & 3u) * 8u)) & 0xFFu; }
void main()
{
    const uint i = gl_GlobalInvocationID.x;
    if (i >= count)
        return;
    const uint bpp = bppOf(psm);
    uint value;
    if (bpp == 32u)
        value = stagingByte(i * 4u) | (stagingByte(i * 4u + 1u) << 8u) | (stagingByte(i * 4u + 2u) << 16u) | (stagingByte(i * 4u + 3u) << 24u);
    else if (bpp == 24u)
        value = stagingByte(i * 3u) | (stagingByte(i * 3u + 1u) << 8u) | (stagingByte(i * 3u + 2u) << 16u);
    else if (bpp == 16u)
        value = stagingByte(i * 2u) | (stagingByte(i * 2u + 1u) << 8u);
    else if (bpp == 8u)
        value = stagingByte(i);
    else
        value = (stagingByte(i >> 1u) >> ((i & 1u) * 4u)) & 0xFu;
    const uint pixel = startPixel + i;
    vramWrite(psm, dbp, dbw, dsax + pixel % rrw, dsay + pixel / rrw, value);
}
)GLSL";

// Local-to-local transfer in two passes (read all, then write) per BITBLT.
static const char *kGsCopyCompute = R"GLSL(
layout(local_size_x = 64) in;
layout(std430, binding = 5) buffer Scratch { uint scratch[]; };
layout(std430, binding = 6) readonly buffer Params
{
    uint pass, total, rrw, rrh, dir;
    uint spsm, sbp, sbw, ssax, ssay;
    uint dpsm, dbp, dbw, dsax, dsay;
};
void main()
{
    const uint pixel = gl_GlobalInvocationID.x;
    if (pixel >= total)
        return;
    uint x = pixel % rrw, y = pixel / rrw;
    if ((dir & 2u) != 0u) x = rrw - x - 1u;
    if ((dir & 1u) != 0u) y = rrh - y - 1u;
    if (pass == 0u)
        scratch[pixel] = vramRead(spsm, sbp, sbw, x + ssax, y + ssay);
    else
        vramWrite(dpsm, dbp, dbw, x + dsax, y + dsay, scratch[pixel]);
}
)GLSL";

// CLUT load, following GSCpuBackend::LoadClutUnlocked.
static const char *kGsClutCompute = R"GLSL(
layout(local_size_x = 256) in;
layout(std430, binding = 2) buffer Clut { uint clut[]; };
layout(std430, binding = 6) readonly buffer Params
{
    uint fourBit, sixteenBit, csm, cpsm, cbp, csa, cbw, cou, cov;
};
uint swizzleCsm1(uint index) { return (index & ~0x18u) | ((index & 0x08u) << 1u) | ((index & 0x10u) >> 1u); }
void main()
{
    const uint entry = gl_GlobalInvocationID.x;
    const uint entryCount = fourBit != 0u ? 16u : 256u;
    const uint destinationBase = (csa & (sixteenBit != 0u ? 0x1Fu : 0x0Fu)) << 4u;
    const bool suffix = csm == 0u && sixteenBit == 0u && fourBit == 0u;
    const uint firstEntry = suffix ? destinationBase : 0u;
    if (entry < firstEntry || entry >= entryCount)
        return;
    uint sx, sy, width = 1u;
    if (csm == 0u)
    {
        const uint sourceIndex = swizzleCsm1(entry);
        sx = sourceIndex & 0x0Fu;
        sy = sourceIndex >> 4u;
    }
    else
    {
        width = cbw != 0u ? cbw : 1u;
        sx = (cou << 4u) + entry;
        sy = cov;
    }
    const uint raw = vramRead(cpsm, cbp, width, sx, sy);
    const uint destination = (suffix ? entry : destinationBase + entry) & (sixteenBit != 0u ? 0x1FFu : 0x0FFu);
    if (sixteenBit != 0u)
    {
        clut[destination] = raw & 0xFFFFu;
    }
    else
    {
        clut[destination] = raw & 0xFFFFu;
        clut[destination + 256u] = raw >> 16u;
    }
}
)GLSL";
