// State cache for the GS thread's GL context: replaces entries of its Api table, so every path of the renderer goes
// through it and nothing has to be invalidated by hand. Draws differ in a few values; repeating the rest made the driver
// revalidate its state hundreds of times a frame. Enable/Disable are held until something draws, because the renderer
// turns the scissor on and off around each draw.
namespace GlStateCache
{
    constexpr GLuint kUnknown = 0xFFFFFFFFu;
    constexpr GLenum kCapScissor = 0x0C11, kCapDepth = 0x0B71, kCapBlend = 0x0BE2, kCapCull = 0x0B44;
    constexpr GLenum kArrayBufferTarget = 0x8892, kTexture0Unit = 0x84C0;
    constexpr int kUnits = 16;

    Api real;
    GLuint fbo, vao, arrayBuffer, currentProgram, activeUnit;
    GLuint units[kUnits];
    GLint viewport[4], scissor[4];
    GLenum depthFunc;
    int depthMask, colorMask;
    GLenum blendFunc[4], blendEq[2];
    float blendColor[4];
    bool blendColorKnown;
    int capWanted[4], capApplied[4];

    int capIndex(GLenum cap)
    {
        return cap == kCapScissor ? 0 : cap == kCapDepth ? 1 : cap == kCapBlend ? 2 : cap == kCapCull ? 3 : -1;
    }
    void applyCaps()
    {
        static const GLenum caps[4] = {kCapScissor, kCapDepth, kCapBlend, kCapCull};
        for (int i = 0; i < 4; ++i)
            if (capWanted[i] != capApplied[i] && capWanted[i] >= 0)
            {
                (capWanted[i] ? real.Enable : real.Disable)(caps[i]);
                capApplied[i] = capWanted[i];
            }
    }
    // A job on the GL thread may have bound a texture behind the cache's back.
    inline void forgetBoundTexture()
    {
        if (activeUnit != kUnknown && activeUnit - kTexture0Unit < static_cast<GLuint>(kUnits))
            units[activeUnit - kTexture0Unit] = kUnknown;
    }
    void reset()
    {
        fbo = vao = arrayBuffer = currentProgram = activeUnit = kUnknown;
        for (GLuint &unit : units) unit = kUnknown;
        for (int i = 0; i < 4; ++i) viewport[i] = scissor[i] = -1, capWanted[i] = capApplied[i] = -1, blendFunc[i] = kUnknown;
        depthFunc = blendEq[0] = blendEq[1] = kUnknown;
        depthMask = colorMask = -1;
        blendColorKnown = false;
    }
    void install(Api &api)
    {
        real = api;
        reset();
        api.BindFramebuffer = [](GLenum target, GLuint id) {
            if (target == kFramebuffer) { if (fbo == id) return; fbo = id; }
            else fbo = kUnknown;
            real.BindFramebuffer(target, id);
        };
        api.DeleteFramebuffers = [](GLsizei n, const GLuint *ids) {
            for (GLsizei i = 0; i < n; ++i) if (ids[i] == fbo) fbo = kUnknown;
            real.DeleteFramebuffers(n, ids);
        };
        api.BindVertexArray = [](GLuint id) { if (vao != id) { vao = id; real.BindVertexArray(id); } };
        api.DeleteVertexArrays = [](GLsizei n, const GLuint *ids) {
            for (GLsizei i = 0; i < n; ++i) if (ids[i] == vao) vao = kUnknown;
            real.DeleteVertexArrays(n, ids);
        };
        api.BindBuffer = [](GLenum target, GLuint id) {
            if (target == kArrayBufferTarget) { if (arrayBuffer == id) return; arrayBuffer = id; }
            real.BindBuffer(target, id);
        };
        api.DeleteBuffers = [](GLsizei n, const GLuint *ids) {
            for (GLsizei i = 0; i < n; ++i) if (ids[i] == arrayBuffer) arrayBuffer = kUnknown;
            real.DeleteBuffers(n, ids);
        };
        api.UseProgram = [](GLuint id) { if (currentProgram != id) { currentProgram = id; real.UseProgram(id); } };
        api.DeleteProgram = [](GLuint id) { if (id == currentProgram) currentProgram = kUnknown; real.DeleteProgram(id); };
        api.ActiveTexture = [](GLenum unit) { if (activeUnit != unit) { activeUnit = unit; real.ActiveTexture(unit); } };
        api.BindTexture = [](GLenum target, GLuint id) {
            if (target != kTexture2D || activeUnit == kUnknown || activeUnit - kTexture0Unit >= static_cast<GLuint>(kUnits))
            {
                if (target == kTexture2D) for (GLuint &unit : units) unit = kUnknown;
                real.BindTexture(target, id);
                return;
            }
            GLuint &bound = units[activeUnit - kTexture0Unit];
            if (bound != id) { bound = id; real.BindTexture(target, id); }
        };
        api.DeleteTextures = [](GLsizei n, const GLuint *ids) {
            for (GLsizei i = 0; i < n; ++i)
                for (GLuint &unit : units)
                    if (unit == ids[i]) unit = kUnknown;
            real.DeleteTextures(n, ids);
        };
        api.Viewport = [](GLint x, GLint y, GLsizei w, GLsizei h) {
            if (viewport[0] == x && viewport[1] == y && viewport[2] == w && viewport[3] == h) return;
            viewport[0] = x; viewport[1] = y; viewport[2] = w; viewport[3] = h;
            real.Viewport(x, y, w, h);
        };
        api.Scissor = [](GLint x, GLint y, GLsizei w, GLsizei h) {
            if (scissor[0] == x && scissor[1] == y && scissor[2] == w && scissor[3] == h) return;
            scissor[0] = x; scissor[1] = y; scissor[2] = w; scissor[3] = h;
            real.Scissor(x, y, w, h);
        };
        api.Enable = [](GLenum cap) { const int i = capIndex(cap); if (i >= 0) capWanted[i] = 1; else real.Enable(cap); };
        api.Disable = [](GLenum cap) { const int i = capIndex(cap); if (i >= 0) capWanted[i] = 0; else real.Disable(cap); };
        api.DepthFunc = [](GLenum f) { if (depthFunc != f) { depthFunc = f; real.DepthFunc(f); } };
        api.DepthMask = [](GLboolean m) { if (depthMask != static_cast<int>(m)) { depthMask = m; real.DepthMask(m); } };
        api.ColorMask = [](GLboolean r, GLboolean g, GLboolean b, GLboolean a) {
            const int mask = (r ? 1 : 0) | (g ? 2 : 0) | (b ? 4 : 0) | (a ? 8 : 0);
            if (colorMask != mask) { colorMask = mask; real.ColorMask(r, g, b, a); }
        };
        api.BlendFuncSeparate = [](GLenum a, GLenum b, GLenum c, GLenum d) {
            if (blendFunc[0] == a && blendFunc[1] == b && blendFunc[2] == c && blendFunc[3] == d) return;
            blendFunc[0] = a; blendFunc[1] = b; blendFunc[2] = c; blendFunc[3] = d;
            real.BlendFuncSeparate(a, b, c, d);
        };
        api.BlendEquationSeparate = [](GLenum a, GLenum b) {
            if (blendEq[0] == a && blendEq[1] == b) return;
            blendEq[0] = a; blendEq[1] = b;
            real.BlendEquationSeparate(a, b);
        };
        api.BlendColor = [](float r, float g, float b, float a) {
            if (blendColorKnown && blendColor[0] == r && blendColor[1] == g && blendColor[2] == b && blendColor[3] == a) return;
            blendColorKnown = true;
            blendColor[0] = r; blendColor[1] = g; blendColor[2] = b; blendColor[3] = a;
            real.BlendColor(r, g, b, a);
        };
        api.DrawArrays = [](GLenum mode, GLint first, GLsizei count) { applyCaps(); real.DrawArrays(mode, first, count); };
        api.Clear = [](GLbitfield mask) { applyCaps(); real.Clear(mask); };
        api.BlitFramebuffer = [](GLint a, GLint b, GLint c, GLint d, GLint e, GLint f, GLint g, GLint h, GLbitfield mask, GLenum filter) {
            applyCaps();
            real.BlitFramebuffer(a, b, c, d, e, f, g, h, mask, filter);
        };
    }
}
