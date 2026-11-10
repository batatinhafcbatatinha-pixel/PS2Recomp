// Included inside GSGpuBackend::Impl. Source selection follows the existing
// display ABI, while every detile, color conversion, CRT blend and field pixel
// is computed on the host GPU. Only RGBA output/presence bits cross to the CPU.
PresentationFrame presentGpu(const GSPresentationRequest &request)
{
    constexpr uint32_t stride = 640u, maxHeight = 512u;
    struct Source {
        GSFrameReg frame{};
        uint32_t base = 0, x = 0, y = 0, width = 0, height = 0;
        bool valid = false;
    };
    auto formatSupported = [](uint32_t psm) {
        return psm == GS_PSM_CT32 || psm == GS_PSM_CT24 || psm == GS_PSM_CT16 || psm == GS_PSM_CT16S;
    };
    auto decode = [&](uint64_t dispfb, uint64_t display, bool enabled) {
        Source source{};
        source.frame.fbp = uint32_t(dispfb & 0x1FFu);
        source.frame.fbw = uint32_t((dispfb >> 9) & 0x3Fu);
        source.frame.psm = uint8_t((dispfb >> 15) & 0x1Fu);
        source.base = source.frame.fbp * 32u;
        source.x = uint32_t((dispfb >> 32) & 0x7FFu);
        source.y = uint32_t((dispfb >> 43) & 0x7FFu);
        const uint32_t dw = uint32_t((display >> 32) & 0xFFFu);
        const uint32_t dh = uint32_t((display >> 44) & 0x7FFu);
        const uint32_t magh = uint32_t((display >> 23) & 0xFu);
        source.width = (dw + 1u) / (magh + 1u);
        source.height = dh + 1u;
        if ((request.smode2 & 3u) == 3u && source.height > 1u)
            source.height /= 2u; // field mode: DH counts half-lines (see gs_hw_backend.cpp)
        if (source.width < 64u || source.height < 64u) {
            source.width = 640u;
            source.height = 448u;
        }
        source.width = std::min(source.width, stride);
        source.height = std::min(source.height, maxHeight);
        source.valid = enabled && (source.frame.fbw || dw || dh || magh);
        return source;
    };
    Source first = decode(request.dispfb1, request.display1, (request.pmode & 1u) != 0u);
    Source second = decode(request.dispfb2, request.display2, (request.pmode & 2u) != 0u);
    if (!first.valid && !second.valid)
        return {};
    if (!first.valid)
        std::swap(first, second);
    bool dual = first.valid && second.valid;
    if (!formatSupported(first.frame.psm))
        return {};
    if (dual && !formatSupported(second.frame.psm))
        dual = false; // CPU ABI also falls through to the first CRT in this case.
    const GSFrameReg displayFrame = first.frame;
    PresentationFrame result{};
    result.width = dual ? std::max(first.width, second.width) : first.width;
    result.height = dual ? std::max(first.height, second.height) : first.height;
    result.displayFbp = displayFrame.fbp;
    if (!dual && request.hasPreferredSource && request.preferredDestFbp == first.frame.fbp &&
        (request.preferredSource.fbw || request.preferredSource.fbp != first.frame.fbp) &&
        formatSupported(request.preferredSource.psm)) {
        first.frame = request.preferredSource;
        first.base = first.frame.fbp; // preferred source uses 256-byte block units
        first.x = first.y = 0;
        result.usedPreferred = true;
    }
    std::array<uint32_t, 32> params{};
    params[0] = result.width;
    params[1] = result.height;
    params[2] = stride;
    params[3] = (dual ? 1u : 0u) | (((request.pmode >> 5) & 7u) << 1u);
    // Interlaced frame mode (INT=1, FFMD=0) holds a whole progressive frame; showing one field per vblank (bob) made
    // every game bounce half a line up and down at 60 Hz, like an old TV. PS2X_GS_BOB=1 restores that CRT emulation.
    static const bool bob = [] { const char *e = std::getenv("PS2X_GS_BOB"); return e && e[0] == '1'; }();
    if (bob && (request.smode2 & 1u) && !(request.smode2 & 2u))
        params[3] |= 16u | ((request.vsyncTick & 1u) ? 32u : 0u);
    params[4] = uint32_t((request.pmode >> 8) & 0xFFu);
    params[5] = uint32_t(request.bgcolor & 0xFFFFFFu);
    auto setSource = [&](uint32_t slot, const Source &source) {
        const uint32_t at = 8u + slot * 8u;
        params[at + 0] = source.frame.psm;
        params[at + 1] = source.base;
        params[at + 2] = source.frame.fbw ? source.frame.fbw : stride / 64u;
        params[at + 3] = source.x;
        params[at + 4] = source.y;
        params[at + 5] = source.width;
        params[at + 6] = source.height;
    };
    setSource(0u, first);
    setSource(1u, second);
    std::array<uint32_t, 3> presence{};
    // The presence bits only matter to the black-FBP0 fallback below; reading them back waits for the GPU.
    // Once the same display registers showed FBP 0 non-black 8 presents in a row, it is taken as non-black until
    // they change (a game that flips to FBP 0 every other frame otherwise drained the GPU on each of those presents).
    const uint64_t presenceKey = request.dispfb1 * 0x9E3779B97F4A7C15ull ^ request.dispfb2 ^ (request.pmode << 48);
    bool needPresence = (first.frame.fbp == 0u && !result.usedPreferred) || (dual && second.frame.fbp == 0u);
    const bool presenceAssumed = needPresence && presenceKey == lastPresenceKey && presenceStreak >= 8u;
    if (presenceAssumed)
        needPresence = false;
    auto render = [&]() {
        presence.fill(0u);
        gl.NamedBufferSubData(scanoutPresenceBuffer, 0, sizeof(presence), presence.data());
        gl.NamedBufferData(paramBuffer, sizeof(params), params.data(), GL_STREAM_DRAW);
        bindCommon();
        gl.UseProgram(scanoutProgram);
        gl.BindBufferBase(GL_SHADER_STORAGE_BUFFER, 5, scanoutBuffer);
        gl.BindBufferBase(GL_SHADER_STORAGE_BUFFER, 6, paramBuffer);
        gl.BindBufferBase(GL_SHADER_STORAGE_BUFFER, 7, scanoutPresenceBuffer);
        gl.DispatchCompute((stride * maxHeight + 63u) / 64u, 1u, 1u);
        gl.MemoryBarrier(GL_ALL_BARRIER_BITS);
        if (needPresence)
            gl.GetNamedBufferSubData(scanoutPresenceBuffer, 0, sizeof(presence), presence.data());
    };
    render();
    if (presenceAssumed)
        presence[1] = presence[2] = 1u;
    else if (needPresence)
    {
        const bool nonblack = (first.frame.fbp != 0u || presence[1]) && (!dual || second.frame.fbp != 0u || presence[2]);
        presenceStreak = presenceKey == lastPresenceKey && nonblack ? presenceStreak + 1u : (nonblack ? 1u : 0u);
        lastPresenceKey = presenceKey;
    }
    // Preserve the existing black-FBP0 workaround using GPU presence bits;
    // it never invokes the CPU rasterizer or decodes pixels on the host.
    auto selectNonblack = [&](Source &source, uint32_t slot) {
        if (source.frame.fbp || (slot == 0u && result.usedPreferred) || presence[slot + 1u])
            return;
        const Source initial = source;
        for (const GSFrameReg &candidate : request.contextFrames) {
            if (!formatSupported(candidate.psm) ||
                (candidate.fbp == initial.frame.fbp && candidate.fbw == initial.frame.fbw && candidate.psm == initial.frame.psm))
                continue;
            source.frame = candidate;
            source.base = candidate.fbp * 32u;
            source.x = source.y = 0;
            setSource(slot, source);
            render();
            if (presence[slot + 1u])
                return;
        }
        source = initial;
        setSource(slot, source);
        render();
    };
    selectNonblack(first, 0u);
    if (dual)
        selectNonblack(second, 1u);
    result.sourceFbp = first.frame.fbp;
#if PS2X_GPU_TEXTURE_HANDOFF
    // PS2X_GS_PRESENT_READBACK=1: always read the picture back (the old path).
    static const bool readbackAlways = std::getenv("PS2X_GS_PRESENT_READBACK") != nullptr;
    if (sharedMainContext && !request.forceReadback && !readbackAlways)
    {
        const GLuint target = scanoutTextures[scanoutNext];
        scanoutNext = (scanoutNext + 1u) % static_cast<uint32_t>(scanoutTextures.size());
        gl.BindBuffer(GL_PIXEL_UNPACK_BUFFER, scanoutBuffer);
        gl.TextureSubImage2D(target, 0, 0, 0, static_cast<GLsizei>(stride), static_cast<GLsizei>(maxHeight), GL_RGBA,
                             GL_UNSIGNED_BYTE, nullptr);
        gl.BindBuffer(GL_PIXEL_UNPACK_BUFFER, 0u);
        {
            std::lock_guard<std::mutex> lock(handoffMutex);
            if (handoffFence)
                gl.DeleteSync(handoffFence);
            handoffFence = gl.FenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0u);
            scanoutTexture = target;
            ++handoffFrames;
        }
        gl.Flush(); // the fence must reach the GPU before another context waits on it
        result.handedOff = true;
        checkErrors("scanout-handoff", result.width * result.height);
        return result;
    }
#endif
    result.pixels.resize(stride * maxHeight * 4u);
    gl.GetNamedBufferSubData(scanoutBuffer, 0, static_cast<GLsizeiptr>(result.pixels.size()), result.pixels.data());
    checkErrors("scanout", result.width * result.height);
    return result;
}
