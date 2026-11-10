#include "runtime/gs/gs_gpu_backend.h"
#include "runtime/gs/gs_cpu_backend.h"
#include "runtime/gs/ps2_gs_common.h"
#include "runtime/gs/ps2_gs_memory.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cmath>
#include <cctype>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <functional>
#include <future>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>


// Diagnostics: host time the producer spends blocked on the GS thread, by reason,
// reported every 5 s as [gs:sync] (synchronous jobs) and lock= (producer lock waits).
void ps2ProfilerRegisterCurrentThread(const char *name);

namespace GsSyncStats
{
    struct Entry { const char *reason; std::atomic<uint64_t> calls{0}, nanos{0}; };
    inline Entry g_entries[10];
    inline std::atomic<uint64_t> g_lockWaitNanos{0}, g_lockWaits{0};
    inline void report()
    {
        static std::atomic<int64_t> last{0};
        const int64_t now = std::chrono::steady_clock::now().time_since_epoch().count();
        int64_t prev = last.load();
        if (now - prev < 5'000'000'000ll || !last.compare_exchange_strong(prev, now))
            return;
        std::fprintf(stderr, "[gs:sync] lock=%llums/%llu", (unsigned long long)(g_lockWaitNanos.exchange(0) / 1000000u),
                     (unsigned long long)g_lockWaits.exchange(0));
        for (Entry &e : g_entries)
            if (e.reason && e.calls)
                std::fprintf(stderr, " %s=%llums/%llu", e.reason, (unsigned long long)(e.nanos.exchange(0) / 1000000u),
                             (unsigned long long)e.calls.exchange(0));
        std::fputc('\n', stderr);
    }
    struct Scope
    {
        Entry *entry = nullptr;
        std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
        explicit Scope(const char *reason)
        {
            for (Entry &e : g_entries)
                if (e.reason == reason || !e.reason) { e.reason = reason; entry = &e; break; }
        }
        ~Scope()
        {
            if (!entry) return;
            entry->calls++;
            entry->nanos += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();
            report();
        }
    };
    // Producer lock with its wait time counted.
    inline std::unique_lock<std::mutex> lockTimed(std::mutex &m)
    {
        std::unique_lock<std::mutex> lock(m, std::try_to_lock);
        if (!lock.owns_lock())
        {
            const auto start = std::chrono::steady_clock::now();
            lock.lock();
            g_lockWaits++;
            g_lockWaitNanos += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();
        }
        return lock;
    }
}

#include "gs_gpu_shaders.inl"

namespace GSMem
{
    u32 PageTableEntry(u32 layout, u32 block, u32 y, u32 x);
}

// raylib links GLFW statically; only the calls needed for an offscreen context.
extern "C"
{
    struct GLFWwindow;
    struct GLFWmonitor;
    typedef void (*GLFWglproc)(void);
    void glfwDefaultWindowHints(void);
    void glfwWindowHint(int hint, int value);
    GLFWwindow *glfwCreateWindow(int width, int height, const char *title, GLFWmonitor *monitor, GLFWwindow *share);
    void glfwDestroyWindow(GLFWwindow *window);
    void glfwMakeContextCurrent(GLFWwindow *window);
    GLFWwindow *glfwGetCurrentContext(void);
    GLFWglproc glfwGetProcAddress(const char *procname);
}

namespace
{
    thread_local GSGpuBackend *s_mainThreadNativeBackend = nullptr;
    constexpr int kGlfwVisible = 0x00020004;
    constexpr int kGlfwClientApi = 0x00022001;
    constexpr int kGlfwOpenGlApi = 0x00030001;
    constexpr int kGlfwContextVersionMajor = 0x00022002;
    constexpr int kGlfwContextVersionMinor = 0x00022003;
    constexpr int kGlfwOpenGlProfile = 0x00022008;
    constexpr int kGlfwOpenGlCoreProfile = 0x00032001;

#ifdef _WIN32
#define GSGL_APIENTRY __stdcall
#else
#define GSGL_APIENTRY
#endif
    using GLenum = unsigned int;
    using GLuint = unsigned int;
    using GLint = int;
    using GLsizei = int;
    using GLbitfield = unsigned int;
    using GLchar = char;
    using GLintptr = std::ptrdiff_t;
    using GLsizeiptr = std::ptrdiff_t;
    using GLuint64 = unsigned long long;
    using GLsync = struct __GLsync *;
    constexpr GLenum GL_SYNC_GPU_COMMANDS_COMPLETE = 0x9117, GL_STREAM_READ = 0x88E1;
    constexpr GLenum GL_PIXEL_UNPACK_BUFFER = 0x88EC, GL_RGBA = 0x1908, GL_UNSIGNED_BYTE = 0x1401;
    constexpr GLuint64 GL_TIMEOUT_IGNORED = ~0ull;
    constexpr GLbitfield GL_SYNC_FLUSH_COMMANDS_BIT = 0x1;

    constexpr GLenum GL_SHADER_STORAGE_BUFFER = 0x90D2;
    constexpr GLenum GL_COMPUTE_SHADER = 0x91B9;
    constexpr GLenum GL_VERTEX_SHADER = 0x8B31;
    constexpr GLenum GL_FRAGMENT_SHADER = 0x8B30;
    constexpr GLenum GL_COMPILE_STATUS = 0x8B81;
    constexpr GLenum GL_LINK_STATUS = 0x8B82;
    constexpr GLenum GL_DYNAMIC_STORAGE_BIT = 0x0100;
    constexpr GLenum GL_STREAM_DRAW = 0x88E0;
    constexpr GLbitfield GL_ALL_BARRIER_BITS = 0xFFFFFFFFu;
    // GL_SHADER_STORAGE_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT; PS2X_GS_FULL_BARRIERS=1 restores all bits.
    const GLbitfield kStorageBarrier = std::getenv("PS2X_GS_FULL_BARRIERS") ? 0xFFFFFFFFu : (0x2000u | 0x200u);
    constexpr GLenum GL_TRIANGLES = 0x0004;
    constexpr GLenum GL_FRAMEBUFFER = 0x8D40;
    constexpr GLenum GL_FRAMEBUFFER_DEFAULT_WIDTH = 0x9310;
    constexpr GLenum GL_FRAMEBUFFER_DEFAULT_HEIGHT = 0x9311;
    constexpr GLenum GL_EXTENSIONS = 0x1F03;
    constexpr GLenum GL_NUM_EXTENSIONS = 0x821D;
    constexpr GLenum GL_VERSION = 0x1F02;
    constexpr GLenum GL_RENDERER = 0x1F01;
    constexpr GLenum GL_DEPTH_TEST = 0x0B71;
    constexpr GLenum GL_BLEND = 0x0BE2;
    constexpr GLenum GL_CULL_FACE = 0x0B44;
    constexpr GLenum GL_SCISSOR_TEST = 0x0C11;
    constexpr GLenum GL_TEXTURE_2D = 0x0DE1, GL_RGBA8 = 0x8058;
    constexpr GLenum GL_TEXTURE_MIN_FILTER = 0x2801, GL_TEXTURE_MAG_FILTER = 0x2800;
    constexpr GLenum GL_TEXTURE_WRAP_S = 0x2802, GL_TEXTURE_WRAP_T = 0x2803;
    constexpr GLenum GL_CLAMP_TO_EDGE = 0x812F, GL_NEAREST = 0x2600, GL_WRITE_ONLY = 0x88B9;

#define GSGL_FUNCTIONS(X)                                                                         \
    X(const unsigned char *, GetString, (GLenum))                                                 \
    X(const unsigned char *, GetStringi, (GLenum, GLuint))                                        \
    X(void, GetIntegerv, (GLenum, GLint *))                                                       \
    X(GLuint, CreateShader, (GLenum))                                                             \
    X(void, ShaderSource, (GLuint, GLsizei, const GLchar *const *, const GLint *))                \
    X(void, CompileShader, (GLuint))                                                              \
    X(void, GetShaderiv, (GLuint, GLenum, GLint *))                                               \
    X(void, GetShaderInfoLog, (GLuint, GLsizei, GLsizei *, GLchar *))                             \
    X(GLuint, CreateProgram, ())                                                                  \
    X(void, AttachShader, (GLuint, GLuint))                                                       \
    X(void, LinkProgram, (GLuint))                                                                \
    X(void, GetProgramiv, (GLuint, GLenum, GLint *))                                              \
    X(void, GetProgramInfoLog, (GLuint, GLsizei, GLsizei *, GLchar *))                            \
    X(void, DeleteShader, (GLuint))                                                               \
    X(void, UseProgram, (GLuint))                                                                 \
    X(void, CreateBuffers, (GLsizei, GLuint *))                                                   \
    X(void, NamedBufferStorage, (GLuint, GLsizeiptr, const void *, GLbitfield))                   \
    X(void, NamedBufferData, (GLuint, GLsizeiptr, const void *, GLenum))                          \
    X(void, NamedBufferSubData, (GLuint, GLintptr, GLsizeiptr, const void *))                     \
    X(void, GetNamedBufferSubData, (GLuint, GLintptr, GLsizeiptr, void *))                        \
    X(void, CopyNamedBufferSubData, (GLuint, GLuint, GLintptr, GLintptr, GLsizeiptr))             \
    X(GLsync, FenceSync, (GLenum, GLbitfield))                                                    \
    X(GLenum, ClientWaitSync, (GLsync, GLbitfield, GLuint64))                                     \
    X(void, DeleteSync, (GLsync))                                                                 \
    X(void, BindBufferBase, (GLenum, GLuint, GLuint))                                             \
    X(void, DispatchCompute, (GLuint, GLuint, GLuint))                                            \
    X(void, MemoryBarrier, (GLbitfield))                                                          \
    X(void, CreateVertexArrays, (GLsizei, GLuint *))                                              \
    X(void, BindVertexArray, (GLuint))                                                            \
    X(void, CreateFramebuffers, (GLsizei, GLuint *))                                              \
    X(void, NamedFramebufferParameteri, (GLuint, GLenum, GLint))                                  \
    X(void, BindFramebuffer, (GLenum, GLuint))                                                    \
    X(void, Viewport, (GLint, GLint, GLsizei, GLsizei))                                           \
    X(void, DrawArrays, (GLenum, GLint, GLsizei))                                                 \
    X(void, Disable, (GLenum))                                                                    \
    X(void, CreateTextures, (GLenum, GLsizei, GLuint *))                                          \
    X(void, TextureStorage2D, (GLuint, GLsizei, GLenum, GLsizei, GLsizei))                         \
    X(void, TextureParameteri, (GLuint, GLenum, GLint))                                          \
    X(void, BindImageTexture, (GLuint, GLuint, GLint, unsigned char, GLint, GLenum, GLenum))       \
    X(void, DeleteTextures, (GLsizei, const GLuint *))                                            \
    X(void, BindBuffer, (GLenum, GLuint)) \
    X(void, TextureSubImage2D, (GLuint, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void *)) \
    X(void, WaitSync, (GLsync, GLbitfield, GLuint64)) \
    X(void, Flush, ()) \
    X(void, Finish, ())                                                                               X(GLenum, GetError, ())                                                                           X(GLenum, CheckNamedFramebufferStatus, (GLuint, GLenum))

    struct GlApi
    {
#define GSGL_DECLARE(ret, name, args) ret(GSGL_APIENTRY *name) args = nullptr;
        GSGL_FUNCTIONS(GSGL_DECLARE)
#undef GSGL_DECLARE

        bool load()
        {
            bool ok = true;
#define GSGL_LOAD(ret, name, args)                                                                       \
    name = reinterpret_cast<ret(GSGL_APIENTRY *) args>(glfwGetProcAddress("gl" #name));               \
    if (!name)                                                                                           \
    {                                                                                                    \
        std::cerr << "[gs:gpu] missing GL function gl" #name << std::endl;                               \
        ok = false;                                                                                      \
    }
            GSGL_FUNCTIONS(GSGL_LOAD)
#undef GSGL_LOAD
            return ok;
        }
    };

    // std430 mirrors of the GLSL Prim/State structs (gs_gpu_shaders.inl).
    struct GpuPrim
    {
        float x[3];
        float y[3];
        uint32_t z[3];
        uint32_t rgba[3];
        float s[3];
        float t[3];
        float q[3];
        uint32_t uv[3];
        uint32_t fog;
        uint32_t state;
        uint32_t type;
        int32_t bbox[4];
        uint32_t pad;
    };
    static_assert(sizeof(GpuPrim) == 32 * 4, "GpuPrim must match the GLSL layout");

    // A few disjoint VRAM byte ranges (a batch's frame, Z and render targets, or the textures it samples). One
    // interval spanning them all made every CLUT load inside it look like a hazard. Full: the closest two merge.
    struct RangeSet
    {
        static constexpr uint32_t kMax = 8u;
        uint64_t lo[kMax] = {}, hi[kMax] = {};
        uint32_t count = 0u;
        void clear() { count = 0u; }
        bool overlaps(uint64_t a, uint64_t b) const
        {
            for (uint32_t i = 0; i < count; ++i)
                if (a < hi[i] && lo[i] < b)
                    return true;
            return false;
        }
        void add(uint64_t a, uint64_t b)
        {
            for (uint32_t i = 0; i < count; ++i)
                if (a <= hi[i] && lo[i] <= b) // overlapping or touching: grow it
                {
                    lo[i] = std::min(lo[i], a);
                    hi[i] = std::max(hi[i], b);
                    return;
                }
            if (count < kMax)
            {
                lo[count] = a;
                hi[count] = b;
                ++count;
                return;
            }
            uint32_t best = 0u;
            uint64_t bestGrow = ~0ull;
            for (uint32_t i = 0; i < count; ++i)
            {
                const uint64_t grow = (std::max(hi[i], b) - std::min(lo[i], a)) - (hi[i] - lo[i]);
                if (grow < bestGrow)
                    bestGrow = grow, best = i;
            }
            lo[best] = std::min(lo[best], a);
            hi[best] = std::max(hi[best], b);
        }
    };

    struct GpuState
    {
        uint32_t fbp, fbw, fpsm, fbmsk;
        uint32_t zbp, zpsm, zmask, flags;
        uint32_t test, alphaMode, alphaFix, fba;
        int32_t scx0, scy0, scx1, scy1;
        uint32_t tbp0, tbw, tpsm, tcc;
        uint32_t tfx, cpsm, csm, csa;
        uint32_t wrapU, wrapV, minU, maxU;
        uint32_t minV, maxV, ta0, ta1;
        uint32_t aem, fogColor, texW, texH;
        int32_t ofx, ofy;
        uint32_t tex1Lo, tex1Hi;
        uint32_t mip1Lo, mip1Hi, mip2Lo, mip2Hi;
        uint32_t pad0, pad1, pad2, pad3;
    };
    static_assert(sizeof(GpuState) == 48 * 4, "GpuState must match the GLSL layout");

    constexpr uint32_t kFlagIip = 1u, kFlagTme = 2u, kFlagFge = 4u, kFlagAbe = 8u, kFlagFst = 16u, kFlagPabe = 32u, kFlagLinear = 64u;
    constexpr uint32_t kVramSize = 4u * 1024u * 1024u;
    constexpr size_t kMaxBatchPrims = 1u << 16u;

    uint32_t packRgba(const GSVertex &v)
    {
        return static_cast<uint32_t>(v.r) | (static_cast<uint32_t>(v.g) << 8u) |
               (static_cast<uint32_t>(v.b) << 16u) | (static_cast<uint32_t>(v.a) << 24u);
    }

    uint8_t clampU8i(int v)
    {
        return static_cast<uint8_t>(std::clamp(v, 0, 255));
    }

    GpuState makeState(const GSDrawState &st)
    {
        const GSContext &ctx = st.context;
        GpuState s{};
        s.fbp = GSInternal::framePageBaseToBlock(ctx.frame.fbp);
        s.fbw = std::max<uint32_t>(ctx.frame.fbw, 1u);
        s.fpsm = ctx.frame.psm;
        s.fbmsk = ctx.frame.fbmsk;
        s.zbp = GSInternal::framePageBaseToBlock(ctx.zbuf.zbp);
        s.zpsm = ctx.zbuf.psm;
        s.zmask = ctx.zbuf.zmask ? 1u : 0u;
        s.flags = (st.prim.iip ? kFlagIip : 0u) | (st.prim.tme ? kFlagTme : 0u) | (st.prim.fge ? kFlagFge : 0u) |
                  (st.prim.abe ? kFlagAbe : 0u) | (st.prim.fst ? kFlagFst : 0u) | (st.pabe ? kFlagPabe : 0u) |
                  (st.linearFilter ? kFlagLinear : 0u);
        s.test = static_cast<uint32_t>(ctx.test);
        s.alphaMode = static_cast<uint32_t>(ctx.alpha & 0xFFu);
        s.alphaFix = static_cast<uint32_t>((ctx.alpha >> 32u) & 0xFFu);
        s.fba = static_cast<uint32_t>(ctx.fba & 1u);
        s.scx0 = ctx.scissor.x0;
        s.scy0 = ctx.scissor.y0;
        s.scx1 = ctx.scissor.x1;
        s.scy1 = ctx.scissor.y1;
        s.tbp0 = ctx.tex0.tbp0;
        s.tbw = ctx.tex0.tbw;
        s.tpsm = ctx.tex0.psm;
        s.tcc = ctx.tex0.tcc;
        s.tfx = ctx.tex0.tfx;
        s.cpsm = ctx.tex0.cpsm;
        s.csm = ctx.tex0.csm;
        s.csa = ctx.tex0.csa;
        s.wrapU = static_cast<uint32_t>(ctx.clamp & 0x3u);
        s.wrapV = static_cast<uint32_t>((ctx.clamp >> 2u) & 0x3u);
        s.minU = static_cast<uint32_t>((ctx.clamp >> 4u) & 0x3FFu);
        s.maxU = static_cast<uint32_t>((ctx.clamp >> 14u) & 0x3FFu);
        s.minV = static_cast<uint32_t>((ctx.clamp >> 24u) & 0x3FFu);
        s.maxV = static_cast<uint32_t>((ctx.clamp >> 34u) & 0x3FFu);
        s.ta0 = st.texa.ta0;
        s.ta1 = st.texa.ta1;
        s.aem = st.texa.aem ? 1u : 0u;
        s.fogColor = static_cast<uint32_t>(st.fogR) | (static_cast<uint32_t>(st.fogG) << 8u) | (static_cast<uint32_t>(st.fogB) << 16u);
        s.texW = std::max<uint32_t>(st.textureWidth, 1u);
        s.texH = std::max<uint32_t>(st.textureHeight, 1u);
        s.ofx = ctx.xyoffset.ofx >> 4;
        s.ofy = ctx.xyoffset.ofy >> 4;
        s.tex1Lo = static_cast<uint32_t>(ctx.tex1);
        s.tex1Hi = static_cast<uint32_t>(ctx.tex1 >> 32u);
        s.mip1Lo = static_cast<uint32_t>(ctx.miptbp1);
        s.mip1Hi = static_cast<uint32_t>(ctx.miptbp1 >> 32u);
        s.mip2Lo = static_cast<uint32_t>(ctx.miptbp2);
        s.mip2Hi = static_cast<uint32_t>(ctx.miptbp2 >> 32u);
        return s;
    }

    void fillVertex(GpuPrim &p, uint32_t i, const GSVertex &v)
    {
        p.x[i] = v.x;
        p.y[i] = v.y;
        p.z[i] = static_cast<uint32_t>(v.z);
        p.rgba[i] = packRgba(v);
        p.s[i] = v.s;
        p.t[i] = v.t;
        p.q[i] = v.q;
        p.uv[i] = static_cast<uint32_t>(v.u) | (static_cast<uint32_t>(v.v) << 16u);
        p.fog |= static_cast<uint32_t>(v.fog) << (i * 8u);
    }

    bool insideScissor(const GSContext &ctx, int x, int y)
    {
        return !(x < ctx.scissor.x0 || x > ctx.scissor.x1 || y < ctx.scissor.y0 || y > ctx.scissor.y1);
    }
}

// On-demand primitive capture: with PS2X_GS_TRACE set, create analysis/gs-trace.trigger to log the
// next 3000 submitted primitives to analysis/gs-trace.txt (same format as the CPU backend).
extern std::atomic<uint32_t> g_gsDebugVu1Pc, g_gsDebugVu1Top;
static void traceSubmit(const GSPrimitiveBatch &batch)
{
    static const bool allowed = std::getenv("PS2X_GS_TRACE") != nullptr;
    if (!allowed)
        return;
    static uint32_t countdown = 0, remaining = 0;
    static FILE *file = nullptr;
    if (remaining == 0u && countdown++ % 4096u == 0u && std::filesystem::exists("analysis/gs-trace.trigger"))
    {
        std::filesystem::remove("analysis/gs-trace.trigger");
        file = std::fopen("analysis/gs-trace.txt", "w");
        remaining = file ? static_cast<uint32_t>(std::max(3000, std::atoi(std::getenv("PS2X_GS_TRACE")))) : 0u;
    }
    if (remaining == 0u)
        return;
    const GSDrawState &state = batch.state;
    const GSContext &ctx = state.context;
    std::fprintf(file, "type=%u tme=%u abe=%u fst=%u fbp=%u psm=%x fbmsk=%x zbp=%u zpsm=%x zmsk=%u alpha=%llx test=%llx tex0=%u/%u/%x tfx=%u tcc=%u v=",
                 static_cast<unsigned>(state.prim.type), state.prim.tme, state.prim.abe, state.prim.fst, ctx.frame.fbp, ctx.frame.psm,
                 ctx.frame.fbmsk, ctx.zbuf.zbp, static_cast<unsigned>(ctx.zbuf.psm), ctx.zbuf.zmask ? 1u : 0u, static_cast<unsigned long long>(ctx.alpha),
                 static_cast<unsigned long long>(ctx.test), ctx.tex0.tbp0, ctx.tex0.tbw, ctx.tex0.psm, ctx.tex0.tfx, ctx.tex0.tcc);
    for (uint32_t i = 0; i < batch.vertexCount; ++i)
    {
        const GSVertex &v = batch.vertices[i];
        std::fprintf(file, "(%.1f,%.1f,%.0f rgba=%u,%u,%u,%u st=%.3f,%.3f,%.3f uv=%u,%u)", v.x, v.y, v.z, v.r, v.g, v.b, v.a,
                     v.s, v.t, v.q, v.u, v.v);
    }
    std::fprintf(file, " ofs=%d,%d sc=%d,%d-%d,%d vu=%x top=%x", ctx.xyoffset.ofx >> 4, ctx.xyoffset.ofy >> 4, ctx.scissor.x0, ctx.scissor.y0,
                 ctx.scissor.x1, ctx.scissor.y1, g_gsDebugVu1Pc.load(std::memory_order_relaxed), g_gsDebugVu1Top.load(std::memory_order_relaxed));
    // TEST[14:15] is DATE/DATM; TEST[16:18] is ZTE/ZTST. Keep the raw value above.
    std::fprintf(file, " fbw=%u fba=%u pabe=%u ate=%u atst=%u afail=%u date=%u datm=%u zte=%u ztst=%u tw=%u th=%u cbp=%u cpsm=%x csm=%u csa=%u cld=%u\n",
                 ctx.frame.fbw, static_cast<unsigned>(ctx.fba & 1u), static_cast<unsigned>(state.pabe),
                 static_cast<unsigned>(ctx.test & 1u), static_cast<unsigned>((ctx.test >> 1u) & 7u),
                 static_cast<unsigned>((ctx.test >> 12u) & 3u), static_cast<unsigned>((ctx.test >> 14u) & 1u),
                 static_cast<unsigned>((ctx.test >> 15u) & 1u), static_cast<unsigned>((ctx.test >> 16u) & 1u),
                 static_cast<unsigned>((ctx.test >> 17u) & 3u), static_cast<unsigned>(ctx.tex0.tw), static_cast<unsigned>(ctx.tex0.th),
                 ctx.tex0.cbp, static_cast<unsigned>(ctx.tex0.cpsm), static_cast<unsigned>(ctx.tex0.csm),
                 static_cast<unsigned>(ctx.tex0.csa), static_cast<unsigned>(ctx.tex0.cld));
    if (--remaining == 0u)
    {
        std::fclose(file);
        file = nullptr;
    }
}

struct GSGpuBackend::Impl
{
    // Created on the main thread; its context is used only by the GS thread.
    GLFWwindow *window = nullptr;
    GLFWwindow *sharedMainContext = nullptr;
    std::thread worker;
    GlApi gl;

    std::mutex queueMutex;
    std::condition_variable queueCv;
    struct Job
    {
        uint64_t id = 0;
        std::function<void()> work;
        std::string label;
        std::string metadata;
    };
    std::deque<Job> queue;
    bool stopping = false;
    bool requireHardware = false;

    // Producer state: everything the frontend submits, in order.
    std::mutex producerMutex;
    std::vector<GpuPrim> prims;
    // Deferred TEXFLUSH: the batch is cut only when a textured primitive reads VRAM the batch has drawn into
    // (batchWrite: its frame and Z buffers). Cutting at every TEXFLUSH left batches of a few
    // primitives, each followed by a GPU-wide barrier. PS2X_GS_TEXFLUSH_SPLIT=1 cuts at every TEXFLUSH again.
    bool texBarrierPending = false;
    // Bytes of VRAM the pending batch samples as textures: an upload or copy into them (or into what it draws) must
    // cut the batch first; any other one may run before the batch's draw (it touches nothing the batch uses).
    RangeSet batchRead;
    // CLUT ring: every load fills the next slot (a copy of the current one, then the loaded entries), states point at
    // their slot, so a CLUT load no longer cuts the batch. Cut only when the ring would reuse a slot the batch reads.
    static constexpr uint32_t kClutSlots = 64u, kClutSlotWords = 512u;
    uint32_t clutSlot = 0u, clutLoadsInBatch = 0u;
    bool overlapsBatch(uint64_t lo, uint64_t hi) const
    {
        return batchWrite.overlaps(lo, hi) || batchRead.overlaps(lo, hi);
    }
    // PS2X_GS_RB_ORDER=1: how many draw batches a GS->EE download still waits for after the last one that wrote
    // its rectangle (the readback drains the whole queue; few after = the rectangle is drawn late in the frame).
    uint64_t rbBatches = 0, rbLastHit = 0, rbSamples = 0, rbAfterSum = 0, rbTotalSum = 0, rbFrameStart = 0;
    uint64_t rbLo = 0, rbHi = 0;
    uint64_t flushReasons[16] = {}; // batch cuts by cause (PS2X_GS_RB_ORDER report)
    RangeSet batchWrite;
    std::vector<GpuState> states;
    GSTransferCommand transfer{};
    GSTransferSnapshot transferState{};
    bool localToHostActive = false;
    uint32_t clutCbp[2]{};

    // Read-back copy in the frontend's local memory, served by the CPU backend
    // for presentation, local-to-host transfers and debug reads.
    uint8_t *shadow = nullptr;
    uint32_t shadowSize = 0;
    std::mutex shadowMutex;
    GSCpuBackend cpu;

    // GL objects (GS thread only).
    GLuint vramBuffer = 0, tableBuffer = 0, clutBuffer = 0, primBuffer = 0, stateBuffer = 0;
    GLuint stagingBuffer = 0, paramBuffer = 0, debugBuffer = 0;
    GLuint drawProgram = 0, uploadProgram = 0, copyProgram = 0, clutProgram = 0;
    GLuint scanoutProgram = 0, scanoutBuffer = 0, scanoutPresenceBuffer = 0;
    // Texture handoff: the last present's picture is in scanoutTexture (copied on the GPU, no readback); the main
    // thread waits for handoffFence on the GPU before drawing it. handoffFrames counts the frames handed off.
    GLsync handoffFence = nullptr;
    uint64_t lastPresenceKey = 0;  // presentGpu: display registers whose FBP-0 picture was non-black lately
    uint32_t presenceStreak = 0;
    std::mutex handoffMutex;
    uint64_t handoffFrames = 0;
    GLuint scanoutTexture = 0; // the latest handed-off picture (one of scanoutTextures)
    // Three in rotation: the GS thread writes one the window is not drawing (a single texture made the driver
    // serialize the two contexts: the GS thread waited for the window's draws and swaps).
    std::array<GLuint, 3> scanoutTextures{};
    uint32_t scanoutNext = 0;
    uint32_t nativePresentationWidth = 0, nativePresentationHeight = 0;
    GLuint vao = 0, fbo = 0;

    ~Impl()
    {
        {
            std::lock_guard<std::mutex> lock(queueMutex);
            stopping = true;
        }
        queueCv.notify_all();
        queueSpaceCv.notify_all();
        if (worker.joinable())
            worker.join();
        if (window)
            glfwDestroyWindow(window);
    }

    // Bounded: a producer that runs ahead of the GS thread by kMaxQueuedJobs waits
    // (counted as queue-full in [gs:sync]) instead of growing the queue without limit.
    static constexpr size_t kMaxQueuedJobs = 2048u;
    std::condition_variable queueSpaceCv;
    void enqueue(std::function<void()> job, const char *label = "job", std::string metadata = "{}")
    {
        {
            std::unique_lock<std::mutex> lock(queueMutex);
            if (queue.size() >= kMaxQueuedJobs && std::this_thread::get_id() != worker.get_id())
            {
                GsSyncStats::Scope scope("queue-full");
                queueSpaceCv.wait(lock, [this]() { return stopping || queue.size() < kMaxQueuedJobs / 2u; });
            }
            // Assigned under the queue lock: IDs follow execution order for each Impl.
            static std::atomic<uint64_t> nextJobId{0};
            queue.push_back(Job{++nextJobId, std::move(job), label, std::move(metadata)});
        }
        queueCv.notify_one();
    }

    // Caller holds producerMutex.
    void flushPrimsLocked(uint32_t reason = 0u)
    {
        if (prims.empty())
        {
            texBarrierPending = false;
            clutLoadsInBatch = 0u;
            return;
        }
        ++flushReasons[std::min<uint32_t>(reason, 15u)];
        std::string metadata = "{}";
        static const bool capturing = std::getenv("PS2X_GS_CAPTURE_DIR") != nullptr;
        if (capturing)
        {
            metadata = "{\"primitive_count\":" + std::to_string(prims.size()) + ",\"states\":[";
            for (size_t i = 0; i < states.size(); ++i)
            {
                const GpuState &s = states[i];
                if (i) metadata += ",";
                metadata += "{\"tbp0\":" + std::to_string(s.tbp0) + ",\"tbw\":" + std::to_string(s.tbw) +
                    ",\"tpsm\":" + std::to_string(s.tpsm) + ",\"fbp\":" + std::to_string(s.fbp) +
                    ",\"zbp\":" + std::to_string(s.zbp) + ",\"flags\":" + std::to_string(s.flags) + "}";
            }
            metadata += "]}";
        }
        ++rbBatches;
        if (rbHi > rbLo && batchWrite.overlaps(rbLo, rbHi))
            rbLastHit = rbBatches;
        enqueue([this, p = std::move(prims), s = std::move(states)]() { drawBatch(p, s); }, "draw", std::move(metadata));
        prims.clear();
        states.clear();
        texBarrierPending = false;
        batchWrite.clear();
        batchRead.clear();
        clutLoadsInBatch = 0u;
    }

    // Caller holds producerMutex; runs job on the GS thread after all prior work.
    void runSyncLocked(const char *reason, std::function<void()> job, std::string metadata = "{}")
    {
        GsSyncStats::Scope scope(reason);
        flushPrimsLocked(1);
        std::promise<void> done;
        std::future<void> finished = done.get_future();
        enqueue([&]() {
            job();
            done.set_value();
        }, reason, std::move(metadata));
        finished.wait();
    }

    uint32_t addState(const GSDrawState &st)
    {
        GpuState s = makeState(st);
        s.pad0 = clutSlot * kClutSlotWords;
        if (states.empty() || std::memcmp(&states.back(), &s, sizeof(s)) != 0)
            states.push_back(s);
        return static_cast<uint32_t>(states.size() - 1u);
    }

    void pushPrim(const GpuPrim &p)
    {
        prims.push_back(p);
        if (prims.size() >= kMaxBatchPrims)
            flushPrimsLocked(2);
    }

    // Diagnostic metadata is opt-in; the original parameter order is preserved.
    static std::string journalParams(const uint32_t *params, size_t count, uint32_t inputBytes = 0)
    {
        static const bool capturing = std::getenv("PS2X_GS_CAPTURE_DIR") != nullptr;
        if (!capturing) return {};
        std::string result = "{\"input_bytes\":" + std::to_string(inputBytes) + ",\"params\":[";
        for (size_t i = 0; i < count; ++i)
        {
            if (i) result += ",";
            result += std::to_string(params[i]);
        }
        return result + "]}";
    }

    // ---- GS thread ----
    // These readbacks run only on the context owner, after the first armed draw.
    // FNV-1a covers exactly the two investigated 64KiB banks, never mutating VRAM.
    FILE *journalFile = nullptr;
    unsigned journalJobs = 0;
    const std::chrono::steady_clock::time_point journalStart = std::chrono::steady_clock::now();
    bool journalEnabled() const
    {
        return captureUploads && journalJobs < 2000u && std::getenv("PS2X_GS_CAPTURE_DIR");
    }

    void journalJob(const Job &job, const char *phase)
    {
        if (!journalFile)
        {
            char name[96];
            std::snprintf(name, sizeof(name), "/jobs-%p.jsonl", static_cast<void *>(this));
            const std::string path = std::string(std::getenv("PS2X_GS_CAPTURE_DIR")) + name;
            journalFile = std::fopen(path.c_str(), "w");
            if (!journalFile) return;
        }
        std::array<uint8_t, 65536> bytes{};
        uint64_t hashes[2]{};
        gl.MemoryBarrier(GL_ALL_BARRIER_BITS);
        for (unsigned bank = 0; bank < 2; ++bank)
        {
            gl.GetNamedBufferSubData(vramBuffer, bank == 0 ? 0x2a0000 : 0x2cc000, bytes.size(), bytes.data());
            uint64_t hash = 14695981039346656037ull;
            for (uint8_t byte : bytes) { hash ^= byte; hash *= 1099511628211ull; }
            hashes[bank] = hash;
        }
        const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - journalStart).count();
        std::fprintf(journalFile,
            "{\"job\":%llu,\"phase\":\"%s\",\"label\":\"%s\",\"impl\":\"%p\",\"vram_buffer\":%u,"
            "\"elapsed\":%.9f,\"bank_2a00_fnv1a64\":\"%016llx\",\"bank_2cc0_fnv1a64\":\"%016llx\","
            "\"captured_uploads\":%u,\"metadata\":%s}\n",
            static_cast<unsigned long long>(job.id), phase, job.label.c_str(), static_cast<void *>(this), vramBuffer,
            elapsed, static_cast<unsigned long long>(hashes[0]), static_cast<unsigned long long>(hashes[1]),
            capturedUploads, job.metadata.c_str());
        std::fflush(journalFile);
    }


    void workerMain(std::promise<bool> &ready)
    {
        ps2ProfilerRegisterCurrentThread("gl");
        glfwMakeContextCurrent(window);
        const bool ok = gl.load() && checkCapabilities() && createObjects();
        ready.set_value(ok);
        if (!ok)
        {
            glfwMakeContextCurrent(nullptr);
            return;
        }
        for (;;)
        {
            Job job;
            {
                std::unique_lock<std::mutex> lock(queueMutex);
                queueCv.wait(lock, [this]() { return stopping || !queue.empty(); });
                if (queue.empty())
                    break;
                job = std::move(queue.front());
                queue.pop_front();
                if (queue.size() == kMaxQueuedJobs / 2u - 1u)
                    queueSpaceCv.notify_all();
            }
            const bool tracedBefore = journalEnabled();
            if (tracedBefore) { ++journalJobs; journalJob(job, "before"); }
            job.work();
            // The first qualifying draw arms capture inside its job, so its
            // first record has phase=armed-after (no claimed pre-job snapshot).
            if (tracedBefore || journalEnabled())
            {
                if (!tracedBefore) ++journalJobs;
                journalJob(job, tracedBefore ? "after" : "armed-after");
            }
        }
        if (journalFile) { std::fclose(journalFile); journalFile = nullptr; }
        gl.Finish();
        for (GLuint &texture : scanoutTextures)
            if (texture) gl.DeleteTextures(1, &texture);
        glfwMakeContextCurrent(nullptr);
    }

    bool checkCapabilities()
    {
        const char *version = reinterpret_cast<const char *>(gl.GetString(GL_VERSION));
        const char *renderer = reinterpret_cast<const char *>(gl.GetString(GL_RENDERER));
        if (requireHardware) {
            std::string lower = renderer ? renderer : "";
            std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return char(std::tolower(c)); });
            for (const char *software : {"llvmpipe", "softpipe", "swiftshader", "software rasterizer", "gdi generic"})
                if (lower.find(software) != std::string::npos) {
                    std::cerr << "[gs:gpu] hardware rasterizer required; rejected software GL renderer: " << lower << std::endl;
                    return false;
                }
        }
        GLint count = 0;
        gl.GetIntegerv(GL_NUM_EXTENSIONS, &count);
        bool interlock = false;
        for (GLint i = 0; i < count; ++i)
        {
            const char *name = reinterpret_cast<const char *>(gl.GetStringi(GL_EXTENSIONS, static_cast<GLuint>(i)));
            if (name && std::strcmp(name, "GL_ARB_fragment_shader_interlock") == 0)
                interlock = true;
        }
        std::cerr << "[gs:gpu] renderer=" << (renderer ? renderer : "?") << " version=" << (version ? version : "?")
                  << " interlock=" << interlock << std::endl;
        if (!interlock)
            std::cerr << "[gs:gpu] GL_ARB_fragment_shader_interlock is required for ordered GS blending" << std::endl;
        return interlock;
    }

    GLuint compile(GLenum type, const std::string &source, const char *name)
    {
        const GLuint shader = gl.CreateShader(type);
        const char *text = source.c_str();
        gl.ShaderSource(shader, 1, &text, nullptr);
        gl.CompileShader(shader);
        GLint status = 0;
        gl.GetShaderiv(shader, GL_COMPILE_STATUS, &status);
        if (!status)
        {
            std::vector<char> log(16384, 0);
            gl.GetShaderInfoLog(shader, static_cast<GLsizei>(log.size()), nullptr, log.data());
            std::cerr << "[gs:gpu] " << name << " compile failed:\n" << log.data() << std::endl;
            gl.DeleteShader(shader);
            return 0;
        }
        return shader;
    }

    GLuint link(std::initializer_list<GLuint> shaders, const char *name)
    {
        for (GLuint shader : shaders)
            if (!shader)
                return 0;
        const GLuint program = gl.CreateProgram();
        for (GLuint shader : shaders)
            gl.AttachShader(program, shader);
        gl.LinkProgram(program);
        for (GLuint shader : shaders)
            gl.DeleteShader(shader);
        GLint status = 0;
        gl.GetProgramiv(program, GL_LINK_STATUS, &status);
        if (!status)
        {
            std::vector<char> log(16384, 0);
            gl.GetProgramInfoLog(program, static_cast<GLsizei>(log.size()), nullptr, log.data());
            std::cerr << "[gs:gpu] " << name << " link failed:\n" << log.data() << std::endl;
            return 0;
        }
        return program;
    }

    bool createObjects()
    {
        const std::string version = "#version 460 core\n";
        const std::string interlock = "#extension GL_ARB_fragment_shader_interlock : require\n";
        drawProgram = link({compile(GL_VERTEX_SHADER, version + kGsGlslDrawTypes + kGsDrawVertex, "draw.vert"),
                            compile(GL_FRAGMENT_SHADER, version + interlock + kGsGlslCommon + kGsGlslDrawTypes + kGsDrawFragment, "draw.frag")},
                           "draw");
        uploadProgram = link({compile(GL_COMPUTE_SHADER, version + kGsGlslCommon + kGsUploadCompute, "upload.comp")}, "upload");
        copyProgram = link({compile(GL_COMPUTE_SHADER, version + kGsGlslCommon + kGsCopyCompute, "copy.comp")}, "copy");
        clutProgram = link({compile(GL_COMPUTE_SHADER, version + kGsGlslCommon + kGsClutCompute, "clut.comp")}, "clut");
        if (!drawProgram || !uploadProgram || !copyProgram || !clutProgram)
            return false;
#if PS2X_GPU_COMPUTE_SCANOUT
        const std::string handoff = PS2X_GPU_TEXTURE_HANDOFF ? "#define GS_NATIVE_TEXTURE_SCANOUT 1\n" : "";
        scanoutProgram = link({compile(GL_COMPUTE_SHADER, version + handoff + kGsGlslCommon + kGsScanoutCompute, "scanout.comp")}, "scanout");
        if (!scanoutProgram) return false;
#endif

        // Page lookup tables, packed as 16-bit entries (layouts as in GSMem::PageTableEntry).
        static constexpr uint32_t kPW[8] = {64, 64, 64, 64, 64, 64, 128, 128};
        static constexpr uint32_t kPH[8] = {32, 32, 64, 64, 64, 64, 64, 128};
        std::vector<uint16_t> tables;
        tables.reserve(1441792u);
        for (uint32_t layout = 0; layout < 8u; ++layout)
            for (uint32_t block = 0; block < 32u; ++block)
                for (uint32_t y = 0; y < kPH[layout]; ++y)
                    for (uint32_t x = 0; x < kPW[layout]; ++x)
                        tables.push_back(static_cast<uint16_t>(GSMem::PageTableEntry(layout, block, y, x)));

        GLuint buffers[7]{};
        gl.CreateBuffers(7, buffers);
        vramBuffer = buffers[0];
        tableBuffer = buffers[1];
        clutBuffer = buffers[2];
        primBuffer = buffers[3];
        stateBuffer = buffers[4];
        stagingBuffer = buffers[5];
        paramBuffer = buffers[6];
        std::vector<uint8_t> zeros(kVramSize, 0u);
        gl.NamedBufferStorage(vramBuffer, kVramSize, zeros.data(), GL_DYNAMIC_STORAGE_BIT);
        gl.NamedBufferStorage(tableBuffer, static_cast<GLsizeiptr>(tables.size() * sizeof(uint16_t)), tables.data(), 0);
        gl.NamedBufferStorage(clutBuffer, kClutSlots * kClutSlotWords * sizeof(uint32_t), zeros.data(), GL_DYNAMIC_STORAGE_BIT);
        gl.NamedBufferData(primBuffer, sizeof(GpuPrim), zeros.data(), GL_STREAM_DRAW);
        gl.NamedBufferData(stateBuffer, sizeof(GpuState), zeros.data(), GL_STREAM_DRAW);
        gl.NamedBufferData(stagingBuffer, 16, zeros.data(), GL_STREAM_DRAW);
        gl.NamedBufferData(paramBuffer, 64, zeros.data(), GL_STREAM_DRAW);

        gl.CreateBuffers(1, &debugBuffer);
        gl.NamedBufferStorage(debugBuffer, 8 * sizeof(uint32_t), zeros.data(), GL_DYNAMIC_STORAGE_BIT);
#if PS2X_GPU_COMPUTE_SCANOUT
        gl.CreateBuffers(1, &scanoutBuffer);
        gl.NamedBufferStorage(scanoutBuffer, 640u * 512u * 4u, nullptr, GL_DYNAMIC_STORAGE_BIT);
        gl.CreateBuffers(1, &scanoutPresenceBuffer);
        gl.NamedBufferStorage(scanoutPresenceBuffer, 3u * sizeof(uint32_t), zeros.data(), GL_DYNAMIC_STORAGE_BIT);
#if PS2X_GPU_TEXTURE_HANDOFF
        for (GLuint &texture : scanoutTextures)
        {
            gl.CreateTextures(GL_TEXTURE_2D, 1, &texture);
            gl.TextureStorage2D(texture, 1, GL_RGBA8, 640, 512);
            gl.TextureParameteri(texture, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            gl.TextureParameteri(texture, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            gl.TextureParameteri(texture, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            gl.TextureParameteri(texture, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        }
        scanoutTexture = scanoutTextures[0];
#endif
#endif
        gl.CreateVertexArrays(1, &vao);
        gl.CreateFramebuffers(1, &fbo);
        gl.NamedFramebufferParameteri(fbo, GL_FRAMEBUFFER_DEFAULT_WIDTH, 2048);
        gl.NamedFramebufferParameteri(fbo, GL_FRAMEBUFFER_DEFAULT_HEIGHT, 2048);
        std::cerr << "[gs:gpu] fbo status=0x" << std::hex << gl.CheckNamedFramebufferStatus(fbo, GL_FRAMEBUFFER) << std::dec << std::endl;
        gl.Disable(GL_DEPTH_TEST);
        gl.Disable(GL_BLEND);
        gl.Disable(GL_CULL_FACE);
        gl.Disable(GL_SCISSOR_TEST);
        bindCommon();
        return true;
    }

    void bindCommon()
    {
        gl.BindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, vramBuffer);
        gl.BindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, tableBuffer);
        gl.BindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, clutBuffer);
    }

    bool captureUploads = false;
    unsigned capturedUploads = 0;
    void drawBatch(const std::vector<GpuPrim> &p, const std::vector<GpuState> &s)
    {
        // Opt-in, bounded texture investigation capture, on the owning GL thread.
        // Read before rasterization so decoded texels can be separated from scanout.
        static const char *captureDir = std::getenv("PS2X_GS_CAPTURE_DIR");
        // Opt in for affine STQ menus (Q=1); retain the original perspective-only filter by default.
        static const bool captureAnyQ = std::getenv("PS2X_GS_CAPTURE_ANY_Q") != nullptr;
        static const double captureAt = std::getenv("PS2X_GS_CAPTURE_AT") ? std::atof(std::getenv("PS2X_GS_CAPTURE_AT")) : 85.0;
        static const auto captureStart = std::chrono::steady_clock::now();
        static unsigned captureCount = 0;
        const double captureTime = captureDir ? std::chrono::duration<double>(std::chrono::steady_clock::now() - captureStart).count() : 0.0;
        if (captureDir && captureCount < 80u && captureTime >= captureAt && !s.empty() && !p.empty() &&
            (s[0].flags & kFlagTme) && !(s[0].flags & kFlagFst) && p[0].type == 2u &&
            (captureAnyQ || (std::abs(p[0].q[0]) > 1.0e-8f && std::abs(p[0].q[0]) < 0.5f)))
        {
            captureUploads = true;
            const std::string prefix = std::string(captureDir) + "/gs-" + std::to_string(captureCount++);
            auto save = [&](const char *suffix, const void *data, size_t size) {
                if (FILE *file = std::fopen((prefix + suffix).c_str(), "wb")) {
                    std::fwrite(data, 1, size, file); std::fclose(file);
                }
            };
            std::vector<uint8_t> memory(kVramSize);
            gl.MemoryBarrier(GL_ALL_BARRIER_BITS);
            gl.GetNamedBufferSubData(vramBuffer, 0, memory.size(), memory.data());
            save("-vram.bin", memory.data(), memory.size());
            std::array<uint32_t, 512> palette{};
            gl.GetNamedBufferSubData(clutBuffer, 0, sizeof(palette), palette.data());
            save("-clut.bin", palette.data(), sizeof(palette));
            save("-prims.bin", p.data(), p.size() * sizeof(GpuPrim));
            save("-states.bin", s.data(), s.size() * sizeof(GpuState));
            if (captureCount == 1u) {
                if (FILE *layout = std::fopen((std::string(captureDir) + "/layout.json").c_str(), "w")) {
                    std::fprintf(layout, "{\"primitive_bytes\":%zu,\"state_bytes\":%zu}\n", sizeof(GpuPrim), sizeof(GpuState));
                    std::fclose(layout);
                }
                memory.resize(1441792u * sizeof(uint16_t));
                gl.GetNamedBufferSubData(tableBuffer, 0, memory.size(), memory.data());
                save("-tables.bin", memory.data(), memory.size());
            }
            if (FILE *file = std::fopen((prefix + "-time.txt").c_str(), "w")) {
                std::fprintf(file, "%.6f\n", captureTime); std::fclose(file);
            }
        }
        gl.NamedBufferData(primBuffer, static_cast<GLsizeiptr>(p.size() * sizeof(GpuPrim)), p.data(), GL_STREAM_DRAW);
        gl.NamedBufferData(stateBuffer, static_cast<GLsizeiptr>(s.size() * sizeof(GpuState)), s.data(), GL_STREAM_DRAW);
        gl.UseProgram(drawProgram);
        bindCommon();
        gl.BindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, primBuffer);
        gl.BindBufferBase(GL_SHADER_STORAGE_BUFFER, 4, stateBuffer);
        gl.BindBufferBase(GL_SHADER_STORAGE_BUFFER, 7, debugBuffer);
        gl.BindVertexArray(vao);
        gl.BindFramebuffer(GL_FRAMEBUFFER, fbo);
        gl.Viewport(0, 0, 2048, 2048);
        gl.DrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(p.size() * 6u));
        // The next draw or dispatch reads VRAM as a storage buffer: that is all it waits for. Readbacks, copies and the
        // scanout issue their own full barrier.
        gl.MemoryBarrier(kStorageBarrier);
        checkErrors("draw", static_cast<uint32_t>(p.size()));
        if (workLogs < 24u)
        {
            uint32_t counters[8]{};
            gl.GetNamedBufferSubData(debugBuffer, 0, sizeof(counters), counters);
            std::cerr << "[gs:gpu] frag invoked=" << counters[0] << " covered=" << counters[1] << " reached=" << counters[2]
                      << " fbwrites=" << counters[3] << " ztst0=" << counters[4] << " prim0 type=" << p[0].type
                      << " bbox=" << p[0].bbox[0] << "," << p[0].bbox[1] << "-" << p[0].bbox[2] << "," << p[0].bbox[3]
                      << " test=0x" << std::hex << s[p[0].state].test << " fbp=0x" << s[p[0].state].fbp << std::dec << std::endl;
        }
    }

    void dispatch(GLuint program, const void *params, size_t paramBytes, uint32_t invocations, uint32_t localSize)
    {
        if (invocations == 0u)
            return;
        gl.NamedBufferData(paramBuffer, static_cast<GLsizeiptr>(paramBytes), params, GL_STREAM_DRAW);
        gl.UseProgram(program);
        bindCommon();
        gl.BindBufferBase(GL_SHADER_STORAGE_BUFFER, 5, stagingBuffer);
        gl.BindBufferBase(GL_SHADER_STORAGE_BUFFER, 6, paramBuffer);
        gl.DispatchCompute((invocations + localSize - 1u) / localSize, 1, 1);
        gl.MemoryBarrier(kStorageBarrier);
        checkErrors(program == uploadProgram ? "upload" : (program == clutProgram ? "clut" : "copy"), invocations);
    }

    // Bounded diagnostics for bring-up: GL errors and work counts.
    uint32_t errorLogs = 0, workLogs = 0, readbackLogs = 0;
    bool readbackPrimed = false; // one synchronous readback before PS2X_GS_ASYNC_READBACK serves stale ones
    void checkErrors(const char *what, uint32_t count)
    {
        const GLenum error = gl.GetError();
        if (error != 0u && errorLogs++ < 16u)
            std::cerr << "[gs:gpu] GL error 0x" << std::hex << error << std::dec << " after " << what << std::endl;
        if (workLogs++ < 24u)
            std::cerr << "[gs:gpu] " << what << " count=" << count << std::endl;
    }

    // PS2X_GS_ASYNC_READBACK: the VRAM is copied on the GPU into a staging buffer behind the queued work (fence), and
    // collected into the shadow at the next request: neither the GL thread nor the EE waits for the GPU to drain.
    GLuint readbackStage = 0;
    GLsync readbackFence = nullptr;
    uint32_t asyncStart = ~0u, asyncEnd = 0; // producer side: the rectangle the last request staged
    uint32_t stagedStart = 0, stagedEnd = 0; // the VRAM bytes the pending stage copy holds (same offsets in the stage)
    // [start, end): the pages the transfer reads (whole VRAM when it wraps); only they are copied and collected.
    // collect=false: the previous stage is dropped (a synchronous readback just refreshed the shadow).
    void readbackAsync(uint32_t start, uint32_t end, bool collect = true)
    {
        if (!shadow)
            return;
        const GLsizeiptr size = std::min<uint32_t>(shadowSize, kVramSize);
        if (!readbackStage)
        {
            gl.CreateBuffers(1, &readbackStage);
            gl.NamedBufferData(readbackStage, size, nullptr, GL_STREAM_READ);
        }
        if (readbackFence)
        {
            // Not waited for: while the GPU has not reached the previous copy, it stays pending and this request is
            // served from the older shadow (waiting here kept the GL thread from feeding the GPU).
            const GLenum status = gl.ClientWaitSync(readbackFence, GL_SYNC_FLUSH_COMMANDS_BIT, 0u);
            if (status == 0x911Bu /* GL_TIMEOUT_EXPIRED */ && collect)
                return;
            gl.DeleteSync(readbackFence);
            readbackFence = nullptr;
            if (status == 0x911Bu)
                collect = false; // a synchronous readback refreshed the shadow; the old stage is dropped
            std::lock_guard<std::mutex> lock(shadowMutex);
            if (collect && stagedEnd > stagedStart)
                gl.GetNamedBufferSubData(readbackStage, stagedStart, stagedEnd - stagedStart, shadow + stagedStart);
        }
        end = std::min<uint32_t>(end, static_cast<uint32_t>(size));
        if (start >= end)
            start = 0u, end = static_cast<uint32_t>(size);
        gl.MemoryBarrier(GL_ALL_BARRIER_BITS);
        gl.CopyNamedBufferSubData(vramBuffer, readbackStage, start, start, end - start);
        stagedStart = start;
        stagedEnd = end;
        readbackFence = gl.FenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0u);
    }

    // Only the bytes [offset, offset + bytes) of the VRAM (a GS->EE transfer reads a few pages, not 4 MB).
    void readbackShadowRange(uint32_t offset, uint32_t bytes)
    {
        const uint32_t size = std::min<uint32_t>(shadowSize, kVramSize);
        if (!shadow || offset >= size)
            return;
        bytes = std::min(bytes, size - offset);
        gl.MemoryBarrier(GL_ALL_BARRIER_BITS);
        std::lock_guard<std::mutex> lock(shadowMutex);
        gl.GetNamedBufferSubData(vramBuffer, offset, bytes, shadow + offset);
    }

    void readbackShadow()
    {
        if (!shadow)
            return;
        gl.MemoryBarrier(GL_ALL_BARRIER_BITS);
        std::lock_guard<std::mutex> lock(shadowMutex);
        gl.GetNamedBufferSubData(vramBuffer, 0, std::min<uint32_t>(shadowSize, kVramSize), shadow);
        if ((readbackLogs++ % 60u) == 0u && readbackLogs < 4000u)
        {
            uint32_t nonzero = 0;
            for (uint32_t i = 0; i < std::min<uint32_t>(shadowSize, kVramSize); i += 4u)
                { uint32_t word = 0; std::memcpy(&word, shadow + i, 4); nonzero += word != 0u; }
            std::cerr << "[gs:gpu] readback nonzero_words=" << nonzero << std::endl;
            if (readbackLogs > 900u && std::getenv("PS2X_GS_VRAM_DUMP"))
                if (FILE *file = std::fopen(std::getenv("PS2X_GS_VRAM_DUMP"), "wb"))
                {
                    std::fwrite(shadow, 1, std::min<uint32_t>(shadowSize, kVramSize), file);
                    std::fclose(file);
                }
        }
    }

    void uploadShadow()
    {
        if (!shadow)
            return;
        std::lock_guard<std::mutex> lock(shadowMutex);
        gl.NamedBufferSubData(vramBuffer, 0, std::min<uint32_t>(shadowSize, kVramSize), shadow);
        gl.MemoryBarrier(GL_ALL_BARRIER_BITS);
    }

    void uploadPixels(std::vector<uint8_t> bytes, const uint32_t params[8])
    {
        const char *captureDir = captureUploads && capturedUploads < 16u ? std::getenv("PS2X_GS_CAPTURE_DIR") : nullptr;
        std::string prefix;
        std::vector<uint8_t> capturedVram;
        auto saveCapture = [&](const char *suffix, const void *data, size_t size) {
            if (FILE *file = std::fopen((prefix + suffix).c_str(), "wb")) {
                std::fwrite(data, 1, size, file); std::fclose(file);
            }
        };
        if (captureDir) {
            prefix = std::string(captureDir) + "/upload-" + std::to_string(capturedUploads++);
            saveCapture("-input.bin", bytes.data(), bytes.size());
            saveCapture("-params.bin", params, 8 * sizeof(uint32_t));
            capturedVram.resize(kVramSize);
            gl.MemoryBarrier(GL_ALL_BARRIER_BITS);
            gl.GetNamedBufferSubData(vramBuffer, 0, capturedVram.size(), capturedVram.data());
            saveCapture("-before.bin", capturedVram.data(), capturedVram.size());
        }
        bytes.resize((bytes.size() + 3u) & ~size_t(3u), 0u);
        gl.NamedBufferData(stagingBuffer, static_cast<GLsizeiptr>(std::max<size_t>(bytes.size(), 4u)), bytes.data(), GL_STREAM_DRAW);
        dispatch(uploadProgram, params, 8 * sizeof(uint32_t), params[1], 64u);
        if (captureDir) {
            gl.GetNamedBufferSubData(vramBuffer, 0, capturedVram.size(), capturedVram.data());
            saveCapture("-after.bin", capturedVram.data(), capturedVram.size());
        }
    }

    void copyLocal(const uint32_t params[15])
    {
        const uint32_t total = params[1];
        std::vector<uint32_t> scratch(std::max<uint32_t>(total, 1u), 0u);
        gl.NamedBufferData(stagingBuffer, static_cast<GLsizeiptr>(scratch.size() * 4u), scratch.data(), GL_STREAM_DRAW);
        uint32_t pass[15];
        std::memcpy(pass, params, sizeof(pass));
        pass[0] = 0u;
        dispatch(copyProgram, pass, sizeof(pass), total, 64u);
        pass[0] = 1u;
        dispatch(copyProgram, pass, sizeof(pass), total, 64u);
    }

#if PS2X_GPU_COMPUTE_SCANOUT
#include "gs_gpu_scanout.inl"
#endif
};

GSGpuBackend::GSGpuBackend() : m_impl(std::make_unique<Impl>()) {}

GSGpuBackend::~GSGpuBackend() {
    if (s_mainThreadNativeBackend == this) s_mainThreadNativeBackend = nullptr;
}

bool GSGpuBackend::PrepareNativePresentation() {
#if PS2X_GPU_TEXTURE_HANDOFF
    auto *backend = s_mainThreadNativeBackend;
    if (backend && glfwGetCurrentContext() == backend->m_impl->sharedMainContext) {
        // The main context waits on the GPU for the copy into scanoutTexture (no CPU stall, no readback).
        auto &impl = *backend->m_impl;
        // The main context's earlier draws of the texture finish before the GS thread may overwrite it.
        impl.gl.Finish();
        std::lock_guard<std::mutex> lock(impl.handoffMutex);
        if (impl.handoffFence)
            impl.gl.WaitSync(impl.handoffFence, 0u, GL_TIMEOUT_IGNORED);
        return impl.handoffFrames != 0u;
    }
#endif
    return false;
}

bool GSGpuBackend::GetNativePresentation(NativePresentation &out) {
    out = {};
#if PS2X_GPU_TEXTURE_HANDOFF
    auto *backend = s_mainThreadNativeBackend;
    if (backend && glfwGetCurrentContext() == backend->m_impl->sharedMainContext) {
        auto &impl = *backend->m_impl;
        std::lock_guard<std::mutex> lock(impl.handoffMutex);
        out.texture = impl.scanoutTexture;
        out.sequence = impl.handoffFrames;
        out.width = impl.nativePresentationWidth;
        out.height = impl.nativePresentationHeight;
        return out.texture && out.width && out.height;
    }
#endif
    return false;
}

std::unique_ptr<GSRasterBackend> GSGpuBackend::Create(bool requireHardware)
{
    std::unique_ptr<GSGpuBackend> backend(new GSGpuBackend());
    Impl &impl = *backend->m_impl;
    impl.requireHardware = requireHardware;
#if PS2X_GPU_TEXTURE_HANDOFF
    impl.sharedMainContext = glfwGetCurrentContext();
    if (!impl.sharedMainContext) {
        std::cerr << "[gs:gpu] shared texture handoff requires the main window context on this thread" << std::endl;
        return nullptr;
    }
#endif
    glfwDefaultWindowHints();
    glfwWindowHint(kGlfwVisible, 0);
    glfwWindowHint(kGlfwClientApi, kGlfwOpenGlApi);
    glfwWindowHint(kGlfwContextVersionMajor, 4);
    glfwWindowHint(kGlfwContextVersionMinor, 6);
    glfwWindowHint(kGlfwOpenGlProfile, kGlfwOpenGlCoreProfile);
    impl.window = glfwCreateWindow(16, 16, "ps2x-gs", nullptr, impl.sharedMainContext);
    glfwDefaultWindowHints();
    if (!impl.window)
    {
        std::cerr << "[gs:gpu] could not create an OpenGL 4.6 core context" << std::endl;
        return nullptr;
    }
    std::promise<bool> ready;
    std::future<bool> result = ready.get_future();
    impl.worker = std::thread([&impl, &ready]() { impl.workerMain(ready); });
    if (!result.get())
        return nullptr; // ~Impl joins the GS thread and destroys the window
#if PS2X_GPU_TEXTURE_HANDOFF
    s_mainThreadNativeBackend = backend.get();
#endif
    return backend;
}

void GSGpuBackend::Initialize(uint8_t *vram, uint32_t vramSize)
{
    Impl &impl = *m_impl;
    std::lock_guard<std::mutex> lock(impl.producerMutex);
    impl.shadow = vram;
    impl.shadowSize = vramSize;
    if (vram)
        impl.cpu.Initialize(vram, vramSize);
    // The external allocation is the hand-off format: make it the GPU contents.
    impl.runSyncLocked("init", [&impl]() { impl.uploadShadow(); }, "{\"action\":\"uploadShadow\"}");
    impl.transfer = {};
    impl.transfer.direction = 3u;
    impl.transferState = {};
    impl.transferState.direction = 3u;
    impl.localToHostActive = false;
    impl.clutCbp[0] = impl.clutCbp[1] = 0u;
}

void GSGpuBackend::Reset()
{
    Impl &impl = *m_impl;
    std::lock_guard<std::mutex> lock(impl.producerMutex);
    impl.flushPrimsLocked(3);
    impl.transfer = {};
    impl.transfer.direction = 3u;
    impl.transferState = {};
    impl.transferState.direction = 3u;
    impl.localToHostActive = false;
    impl.clutCbp[0] = impl.clutCbp[1] = 0u;
    impl.cpu.Reset();
    const uint32_t zeros[512]{};
    impl.clutSlot = 0u;
    impl.enqueue([&impl, zeros]() {
        impl.gl.NamedBufferSubData(impl.clutBuffer, 0, sizeof(zeros), zeros);
        impl.gl.MemoryBarrier(GL_ALL_BARRIER_BITS);
    }, "reset");
}

void GSGpuBackend::Submit(const GSPrimitiveBatch &batch)
{
    if (batch.vertexCount == 0u)
        return;
    Impl &impl = *m_impl;
    const GSDrawState &state = batch.state;
    const GSContext &ctx = state.context;
    const int ofx = ctx.xyoffset.ofx >> 4;
    const int ofy = ctx.xyoffset.ofy >> 4;
    const auto lock = GsSyncStats::lockTimed(impl.producerMutex);
    traceSubmit(batch);
    {
        // Deferred TEXFLUSH (see texBarrierPending): a textured primitive that reads what this batch drew cuts it.
        const auto pageRows = [](uint32_t lines) { return (static_cast<uint64_t>(lines) + 31u) / 32u + 1u; };
        if (state.prim.tme)
        {
            const GSTex0Reg &t = ctx.tex0;
            const bool mipmapped = ((ctx.tex1 >> 2) & 7u) != 0u;
            uint64_t lo = static_cast<uint64_t>(t.tbp0) * 256u;
            uint64_t hi = lo + static_cast<uint64_t>(std::max<uint32_t>(t.tbw, 1u)) * 8192u * pageRows(1u << std::min<uint32_t>(t.th, 11u));
            if (mipmapped || hi > kVramSize)
                lo = 0u, hi = kVramSize; // mip levels anywhere: the whole VRAM
            if (impl.texBarrierPending && (mipmapped || impl.batchWrite.overlaps(lo, hi)))
                impl.flushPrimsLocked(4);
            impl.batchRead.add(lo, hi);
        }
        // Frame and Z buffers start on a page: a row of fbw pages holds 32 lines (32-bit) or 64 (16-bit formats).
        const uint32_t lines = static_cast<uint32_t>(ctx.scissor.y1) + 1u;
        const auto bufferSpan = [&](uint32_t psm) {
            const bool sixteen = (psm & 0xFu) == 0x2u || (psm & 0xFu) == 0xAu;
            const uint32_t perRow = sixteen ? 64u : 32u;
            return static_cast<uint64_t>(std::max<uint32_t>(ctx.frame.fbw, 1u)) * 8192u * ((lines + perRow - 1u) / perRow);
        };
        const uint64_t frameLo = static_cast<uint64_t>(ctx.frame.fbp) * 8192u;
        const uint64_t zLo = static_cast<uint64_t>(ctx.zbuf.zbp) * 8192u;
        impl.batchWrite.add(frameLo, frameLo + bufferSpan(ctx.frame.psm));
        if (!ctx.zbuf.zmask)
            impl.batchWrite.add(zLo, zLo + bufferSpan(ctx.zbuf.psm));
    }

    switch (state.prim.type)
    {
    case GS_PRIM_SPRITE:
    {
        const GSVertex &v0 = batch.vertices[0];
        const GSVertex &v1 = batch.vertices[1];
        int x0 = static_cast<int>(v0.x) - ofx, y0 = static_cast<int>(v0.y) - ofy;
        int x1 = static_cast<int>(v1.x) - ofx, y1 = static_cast<int>(v1.y) - ofy;
        if (x0 > x1)
            std::swap(x0, x1);
        if (y0 > y1)
            std::swap(y0, y1);
        const int ux1 = x0 + std::max(1, x1 - x0) - 1;
        const int uy1 = y0 + std::max(1, y1 - y0) - 1;
        if (ux1 < ctx.scissor.x0 || x0 > ctx.scissor.x1 || uy1 < ctx.scissor.y0 || y0 > ctx.scissor.y1)
            return;
        GpuPrim p{};
        fillVertex(p, 0, v0);
        fillVertex(p, 1, v1);
        p.type = 1u;
        p.bbox[0] = std::clamp<int>(x0, ctx.scissor.x0, ctx.scissor.x1);
        p.bbox[1] = std::clamp<int>(y0, ctx.scissor.y0, ctx.scissor.y1);
        p.bbox[2] = std::clamp<int>(ux1, ctx.scissor.x0, ctx.scissor.x1);
        p.bbox[3] = std::clamp<int>(uy1, ctx.scissor.y0, ctx.scissor.y1);
        p.state = impl.addState(state);
        impl.pushPrim(p);
        break;
    }
    case GS_PRIM_TRIANGLE:
    case GS_PRIM_TRISTRIP:
    case GS_PRIM_TRIFAN:
    {
        const GSVertex &v0 = batch.vertices[0];
        const GSVertex &v1 = batch.vertices[1];
        const GSVertex &v2 = batch.vertices[2];
        const float fx0 = v0.x - static_cast<float>(ofx), fy0 = v0.y - static_cast<float>(ofy);
        const float fx1 = v1.x - static_cast<float>(ofx), fy1 = v1.y - static_cast<float>(ofy);
        const float fx2 = v2.x - static_cast<float>(ofx), fy2 = v2.y - static_cast<float>(ofy);
        const float denom = (fy1 - fy2) * (fx0 - fx2) + (fx2 - fx1) * (fy0 - fy2);
        if (std::fabs(denom) < 0.001f)
            return;
        GpuPrim p{};
        fillVertex(p, 0, v0);
        fillVertex(p, 1, v1);
        fillVertex(p, 2, v2);
        p.type = 2u;
        p.bbox[0] = std::clamp(static_cast<int>(std::floor(std::min({fx0, fx1, fx2}))), static_cast<int>(ctx.scissor.x0), static_cast<int>(ctx.scissor.x1));
        p.bbox[2] = std::clamp(static_cast<int>(std::ceil(std::max({fx0, fx1, fx2}))), static_cast<int>(ctx.scissor.x0), static_cast<int>(ctx.scissor.x1));
        p.bbox[1] = std::clamp(static_cast<int>(std::floor(std::min({fy0, fy1, fy2}))), static_cast<int>(ctx.scissor.y0), static_cast<int>(ctx.scissor.y1));
        p.bbox[3] = std::clamp(static_cast<int>(std::ceil(std::max({fy0, fy1, fy2}))), static_cast<int>(ctx.scissor.y0), static_cast<int>(ctx.scissor.y1));
        p.state = impl.addState(state);
        impl.pushPrim(p);
        break;
    }
    case GS_PRIM_LINE:
    case GS_PRIM_LINESTRIP:
    {
        // Same Bresenham walk as GSCpuBackend::DrawLine, emitted as final points.
        const GSVertex &v0 = batch.vertices[0];
        const GSVertex &v1 = batch.vertices[1];
        int x0 = static_cast<int>(v0.x) - ofx, y0 = static_cast<int>(v0.y) - ofy;
        const int x1 = static_cast<int>(v1.x) - ofx, y1 = static_cast<int>(v1.y) - ofy;
        const int dx = std::abs(x1 - x0), dy = -std::abs(y1 - y0);
        const int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
        int err = dx + dy;
        const int totalSteps = std::max(1, std::max(std::abs(x1 - x0), std::abs(y1 - y0)));
        const uint32_t stateIndex = impl.addState(state);
        for (int step = 0;; ++step)
        {
            const float t = static_cast<float>(step) / static_cast<float>(totalSteps);
            if (insideScissor(ctx, x0, y0))
            {
                GpuPrim p{};
                uint8_t r = v1.r, g = v1.g, b = v1.b, a = v1.a;
                if (state.prim.iip)
                {
                    r = clampU8i(static_cast<int>(v0.r + (v1.r - v0.r) * t));
                    g = clampU8i(static_cast<int>(v0.g + (v1.g - v0.g) * t));
                    b = clampU8i(static_cast<int>(v0.b + (v1.b - v0.b) * t));
                    a = clampU8i(static_cast<int>(v0.a + (v1.a - v0.a) * t));
                }
                p.rgba[0] = static_cast<uint32_t>(r) | (static_cast<uint32_t>(g) << 8u) | (static_cast<uint32_t>(b) << 16u) | (static_cast<uint32_t>(a) << 24u);
                p.z[0] = static_cast<uint32_t>(v0.z + (v1.z - v0.z) * t);
                p.fog = clampU8i(static_cast<int>(v0.fog + (v1.fog - v0.fog) * t));
                p.type = 0u;
                p.bbox[0] = p.bbox[2] = x0;
                p.bbox[1] = p.bbox[3] = y0;
                p.state = stateIndex;
                impl.pushPrim(p);
            }
            if (x0 == x1 && y0 == y1)
                break;
            const int e2 = 2 * err;
            if (e2 >= dy)
            {
                err += dy;
                x0 += sx;
            }
            if (e2 <= dx)
            {
                err += dx;
                y0 += sy;
            }
        }
        break;
    }
    case GS_PRIM_POINT:
    {
        const GSVertex &v = batch.vertices[0];
        const int px = static_cast<int>(v.x) - ofx, py = static_cast<int>(v.y) - ofy;
        if (!insideScissor(ctx, px, py))
            return;
        GpuPrim p{};
        p.rgba[0] = packRgba(v);
        p.z[0] = static_cast<uint32_t>(v.z);
        p.fog = v.fog;
        p.type = 0u;
        p.bbox[0] = p.bbox[2] = px;
        p.bbox[1] = p.bbox[3] = py;
        p.state = impl.addState(state);
        impl.pushPrim(p);
        break;
    }
    default:
        break;
    }
}

void GSGpuBackend::LoadClut(const GSTex0Reg &tex0, const GSTexClutReg &texclut)
{
    const bool fourBit = tex0.psm == GS_PSM_T4 || tex0.psm == GS_PSM_T4HL || tex0.psm == GS_PSM_T4HH;
    const bool eightBit = tex0.psm == GS_PSM_T8 || tex0.psm == GS_PSM_T8H;
    if (!fourBit && !eightBit)
        return;
    Impl &impl = *m_impl;
    std::lock_guard<std::mutex> lock(impl.producerMutex);
    switch (tex0.cld)
    {
    case 1u:
        break;
    case 2u:
        impl.clutCbp[0] = tex0.cbp;
        break;
    case 3u:
        impl.clutCbp[1] = tex0.cbp;
        break;
    case 4u:
        if (impl.clutCbp[0] == tex0.cbp)
            return;
        impl.clutCbp[0] = tex0.cbp;
        break;
    case 5u:
        if (impl.clutCbp[1] == tex0.cbp)
            return;
        impl.clutCbp[1] = tex0.cbp;
        break;
    default:
        return;
    }
    const bool sixteenBit = tex0.cpsm == GS_PSM_CT16 || tex0.cpsm == GS_PSM_CT16S;
    const bool thirtyTwoBit = tex0.cpsm == GS_PSM_CT32 || tex0.cpsm == GS_PSM_CT24;
    if (!sixteenBit && !thirtyTwoBit)
        return;
    // The load reads VRAM at CBP: what the pending batch draws there must be drawn first. The ring is cut before it
    // would reuse a slot the batch still reads.
    {
        const uint64_t cbpLo = static_cast<uint64_t>(tex0.cbp) * 256u;
        static int clutCutLog = std::getenv("PS2X_GS_RB_ORDER") ? 12 : 0;
        if (clutCutLog > 0 && impl.batchWrite.overlaps(cbpLo, cbpLo + 8192u * 2u))
        {
            --clutCutLog;
            std::fprintf(stderr, "[gs:clut-cut] cbp=%u (byte %llx) csm=%u csa=%u writes:", tex0.cbp, (unsigned long long)cbpLo, tex0.csm, tex0.csa);
            for (uint32_t i = 0; i < impl.batchWrite.count; ++i)
                std::fprintf(stderr, " %llx-%llx", (unsigned long long)impl.batchWrite.lo[i], (unsigned long long)impl.batchWrite.hi[i]);
            std::fprintf(stderr, "\n");
        }
        if (impl.batchWrite.overlaps(cbpLo, cbpLo + 8192u * 2u) ||
            impl.clutLoadsInBatch + 1u >= Impl::kClutSlots)
            impl.flushPrimsLocked(5);
    }
    const uint32_t previousSlot = impl.clutSlot;
    impl.clutSlot = (impl.clutSlot + 1u) % Impl::kClutSlots;
    ++impl.clutLoadsInBatch;
    const uint32_t params[10] = {fourBit ? 1u : 0u, sixteenBit ? 1u : 0u, tex0.csm, tex0.cpsm, tex0.cbp, tex0.csa,
                                 texclut.cbw, texclut.cou, texclut.cov, impl.clutSlot * Impl::kClutSlotWords};
    std::array<uint32_t, 10> copy{};
    std::memcpy(copy.data(), params, sizeof(params));
    const uint32_t slotBytes = Impl::kClutSlotWords * sizeof(uint32_t);
    impl.enqueue([&impl, copy, previousSlot, slotBytes]() {
        // entries the load does not write keep the previous CLUT's values
        impl.gl.CopyNamedBufferSubData(impl.clutBuffer, impl.clutBuffer, previousSlot * slotBytes, (copy[9] / Impl::kClutSlotWords) * slotBytes,
                                       slotBytes);
        impl.gl.MemoryBarrier(kStorageBarrier);
        impl.gl.BindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, impl.clutBuffer);
        impl.dispatch(impl.clutProgram, copy.data(), sizeof(uint32_t) * copy.size(), 256u, 256u);
    }, "clut", Impl::journalParams(copy.data(), copy.size()));
}

void GSGpuBackend::BeginTransfer(const GSTransferCommand &command)
{
    Impl &impl = *m_impl;
    std::lock_guard<std::mutex> lock(impl.producerMutex);
    impl.transfer = command;
    impl.transferState.x = command.trxpos.dsax;
    impl.transferState.y = command.trxpos.dsay;
    impl.transferState.totalPixels = static_cast<uint32_t>(command.trxreg.rrw) * static_cast<uint32_t>(command.trxreg.rrh);
    impl.transferState.copiedPixels = 0u;
    impl.transferState.direction = command.direction;
    impl.transferState.localToHostPendingBytes = 0u;
    impl.localToHostActive = false;

    if (command.direction == 2u)
    {
        const uint32_t total = impl.transferState.totalPixels;
        if (total != 0u)
        {
            impl.flushPrimsLocked(6);
            std::array<uint32_t, 15> params = {0u, total, command.trxreg.rrw, command.trxreg.rrh, command.trxpos.dir,
                                               command.bitbltbuf.spsm, command.bitbltbuf.sbp, std::max<uint32_t>(command.bitbltbuf.sbw, 1u),
                                               command.trxpos.ssax, command.trxpos.ssay,
                                               command.bitbltbuf.dpsm, command.bitbltbuf.dbp, std::max<uint32_t>(command.bitbltbuf.dbw, 1u),
                                               command.trxpos.dsax, command.trxpos.dsay};
            impl.enqueue([&impl, params]() { impl.copyLocal(params.data()); }, "copy", Impl::journalParams(params.data(), params.size()));
        }
        impl.transferState.copiedPixels = total;
        impl.transferState.direction = 3u;
    }
    else if (command.direction == 1u)
    {
        // Local-to-host reads the current GPU contents through the CPU backend.
        // PS2X_GS_ASYNC_READBACK=1: serve the previous readback and queue the next one behind the GPU work, instead of
        // draining the GPU first (MotorStorm AE reads a 256x128 image back ~20x/s: 6-15 ms each, in series with the EE).
        static const bool asyncReadback = std::getenv("PS2X_GS_ASYNC_READBACK") != nullptr;
        // The pages the rectangle can touch: from the page row of its first line to the one after its last, a row
        // being sbw pages (64 px wide CT32 pages; narrower formats need fewer, so this covers them). Wraps: all.
        const uint32_t rowPages = std::max<uint32_t>(command.bitbltbuf.sbw, 1u);
        const uint32_t y0 = command.trxpos.ssay, y1 = y0 + std::max<uint32_t>(command.trxreg.rrh, 1u) - 1u;
        const uint64_t start = static_cast<uint64_t>(command.bitbltbuf.sbp & ~31u) * 256u + (y0 / 32u) * rowPages * 8192ull;
        const uint64_t end = static_cast<uint64_t>(command.bitbltbuf.sbp) * 256u + (y1 / 32u + 1u) * rowPages * 8192ull + 8192u;
        static const bool rbOrder = std::getenv("PS2X_GS_RB_ORDER") != nullptr;
        if (rbOrder)
        {
            impl.flushPrimsLocked(6);
            if (impl.rbLo == start && impl.rbHi == end)
            {
                ++impl.rbSamples;
                impl.rbAfterSum += impl.rbBatches - std::max(impl.rbLastHit, impl.rbFrameStart);
                impl.rbTotalSum += impl.rbBatches - impl.rbFrameStart;
            }
            impl.rbLo = start;
            impl.rbHi = end;
            impl.rbFrameStart = impl.rbBatches;
            static auto last = std::chrono::steady_clock::now();
            if (std::chrono::steady_clock::now() - last > std::chrono::seconds(5) && impl.rbSamples)
            {
                last = std::chrono::steady_clock::now();
                std::fprintf(stderr, "[gs:rb-order] downloads=%llu batches/frame=%.0f after-last-write=%.0f range=%llx-%llx\n",
                             (unsigned long long)impl.rbSamples, double(impl.rbTotalSum) / impl.rbSamples,
                             double(impl.rbAfterSum) / impl.rbSamples, (unsigned long long)start, (unsigned long long)end);
                static const char *names[16] = {"other", "sync-job", "max-prims", "reset", "tex-hazard", "clut", "transfer", "upload",
                                                "flush", "texflush", "sync", "present", "", "", "", ""};
                std::fprintf(stderr, "[gs:cuts]");
                for (uint32_t r = 0; r < 16u; ++r)
                    if (impl.flushReasons[r])
                        std::fprintf(stderr, " %s=%llu", names[r], (unsigned long long)impl.flushReasons[r]);
                std::fprintf(stderr, "\n");
                std::fill(std::begin(impl.flushReasons), std::end(impl.flushReasons), 0ull);
                impl.rbSamples = impl.rbAfterSum = impl.rbTotalSum = 0;
            }
        }
        const uint32_t s = end <= kVramSize ? static_cast<uint32_t>(start) : 0u;
        const uint32_t e = end <= kVramSize ? static_cast<uint32_t>(end) : kVramSize;
        // Stale data is served only for the rectangle staged by the previous request (one frame old); another one
        // reads synchronously and is staged from then on.
        const bool sameRange = impl.asyncStart == s && impl.asyncEnd == e;
        impl.asyncStart = s;
        impl.asyncEnd = e;
        if (asyncReadback && impl.readbackPrimed && sameRange)
        {
            impl.flushPrimsLocked(6);
            impl.enqueue([&impl, s, e]() { impl.readbackAsync(s, e); }, "readback-async");
        }
        else
        {
            if (end <= kVramSize)
                impl.runSyncLocked("local-to-host", [&impl, start, end]() {
                    impl.readbackShadowRange(static_cast<uint32_t>(start), static_cast<uint32_t>(end - start)); });
            else
                impl.runSyncLocked("local-to-host", [&impl]() { impl.readbackShadow(); });
            impl.readbackPrimed = true;
            if (asyncReadback) // stage this rectangle for the next request
            {
                impl.flushPrimsLocked(6);
                impl.enqueue([&impl, s, e]() { impl.readbackAsync(s, e, false); }, "readback-async");
            }
        }
        std::lock_guard<std::mutex> shadowLock(impl.shadowMutex);
        impl.cpu.BeginTransfer(command);
        impl.localToHostActive = true;
    }
}

void GSGpuBackend::UploadImage(const uint8_t *data, uint32_t sizeBytes)
{
    Impl &impl = *m_impl;
    const auto lock = GsSyncStats::lockTimed(impl.producerMutex);
    GSTransferSnapshot &ts = impl.transferState;
    const GSTransferCommand &tr = impl.transfer;
    if (!data || sizeBytes == 0u || ts.direction != 0u)
        return;
    if (tr.trxreg.rrw == 0u || tr.trxreg.rrh == 0u || ts.totalPixels == 0u)
        return;

    const uint8_t dpsm = tr.bitbltbuf.dpsm;
    uint32_t bytesPerPixel = 0u;
    switch (dpsm)
    {
    case GS_PSM_CT32: case GS_PSM_Z32: bytesPerPixel = 4u; break;
    case GS_PSM_CT24: case GS_PSM_Z24: bytesPerPixel = 3u; break;
    case GS_PSM_CT16: case GS_PSM_CT16S: case GS_PSM_Z16: case GS_PSM_Z16S: bytesPerPixel = 2u; break;
    case GS_PSM_T8: case GS_PSM_T8H: bytesPerPixel = 1u; break;
    case GS_PSM_T4: case GS_PSM_T4HL: case GS_PSM_T4HH: bytesPerPixel = 0u; break;
    default: return;
    }

    // Mirrors GSCpuBackend::UploadImage: whole pixels only, two per byte for 4-bit.
    const uint32_t start = ts.copiedPixels;
    const uint32_t remaining = ts.totalPixels - start;
    const uint32_t pixels = bytesPerPixel == 0u ? std::min<uint32_t>(remaining, sizeBytes * 2u)
                                                : std::min<uint32_t>(remaining, sizeBytes / bytesPerPixel);
    if (pixels == 0u)
        return;
    const uint32_t usedBytes = bytesPerPixel == 0u ? (pixels + 1u) / 2u : pixels * bytesPerPixel;
    const uint32_t rrw = tr.trxreg.rrw;
    const uint32_t params[8] = {start, pixels, dpsm, tr.bitbltbuf.dbp, std::max<uint32_t>(tr.bitbltbuf.dbw, 1u), rrw,
                                tr.trxpos.dsax, tr.trxpos.dsay};
    std::array<uint32_t, 8> copy{};
    std::memcpy(copy.data(), params, sizeof(params));
    {
        // The pages the upload rectangle can write (32-line page rows of dbw pages: covers the 8/4-bit layouts too).
        const uint64_t rowBytes = static_cast<uint64_t>(std::max<uint32_t>(tr.bitbltbuf.dbw, 1u)) * 8192u;
        const uint64_t lo = static_cast<uint64_t>(tr.bitbltbuf.dbp & ~31u) * 256u + (tr.trxpos.dsay / 32u) * rowBytes;
        const uint64_t hi = static_cast<uint64_t>(tr.bitbltbuf.dbp) * 256u + ((tr.trxpos.dsay + tr.trxreg.rrh) / 32u + 1u) * rowBytes + 8192u;
        if (hi > kVramSize || impl.overlapsBatch(lo, hi))
            impl.flushPrimsLocked(7);
    }
    impl.enqueue([&impl, copy, bytes = std::vector<uint8_t>(data, data + usedBytes)]() mutable {
        impl.uploadPixels(std::move(bytes), copy.data());
    }, "upload", Impl::journalParams(copy.data(), copy.size(), usedBytes));

    ts.copiedPixels = std::min<uint32_t>(ts.totalPixels, start + pixels);
    if (ts.copiedPixels >= ts.totalPixels)
    {
        ts.direction = 3u;
        ts.totalPixels = 0u;
    }
    else
    {
        ts.x = tr.trxpos.dsax + (ts.copiedPixels % rrw);
        ts.y = tr.trxpos.dsay + (ts.copiedPixels / rrw);
    }
}

void GSGpuBackend::Flush()
{
    std::lock_guard<std::mutex> lock(m_impl->producerMutex);
    m_impl->flushPrimsLocked(8);
}

void GSGpuBackend::TextureFlush()
{
    // Later draws must sample what earlier ones rendered: Submit cuts the batch when one reads what it drew.
    std::lock_guard<std::mutex> lock(m_impl->producerMutex);
    static const bool splitAlways = std::getenv("PS2X_GS_TEXFLUSH_SPLIT") != nullptr;
    if (splitAlways)
        m_impl->flushPrimsLocked(9);
    else if (!m_impl->prims.empty())
        m_impl->texBarrierPending = true;
}

void GSGpuBackend::Sync(GSSyncReason)
{
    // Readers that need GPU results wait themselves (Present, ReadVram, ...).
    std::lock_guard<std::mutex> lock(m_impl->producerMutex);
    m_impl->flushPrimsLocked(10);
}

PresentationFrame GSGpuBackend::Present(const GSPresentationRequest &request)
{
    Impl &impl = *m_impl;
#if PS2X_GPU_COMPUTE_SCANOUT
    PresentationFrame frame;
    std::promise<void> done;
    std::future<void> finished = done.get_future();
    {
        // Queued in order under the lock, waited for without it: the frontend keeps submitting while the GS thread catches up.
        std::lock_guard<std::mutex> lock(impl.producerMutex);
        impl.flushPrimsLocked(11);
        impl.enqueue([&]() {
            frame = impl.presentGpu(request);
            done.set_value();
        }, "present-gpu");
    }
    finished.wait();
    std::lock_guard<std::mutex> lock(impl.producerMutex);
    impl.nativePresentationWidth = frame.width;
    impl.nativePresentationHeight = frame.height;
    return frame;
#else
    {
        // Only the buffers the CPU scanout reads come back from the GPU: both read circuits from their read origin, the
        // preferred source, and the drawing contexts' buffers when a circuit shows FBP 0 (the black-FBP0 fallback reads
        // them). A whole-VRAM readback (4 MB) per vblank kept the GS frontend waiting while the EE waited on it
        // (MotorStorm AE). PS2X_GS_PRESENT_FULL=1 reads the whole VRAM as before.
        static const bool fullReadback = std::getenv("PS2X_GS_PRESENT_FULL") != nullptr;
        std::array<std::pair<uint64_t, uint64_t>, 5> ranges{};
        size_t rangeCount = 0;
        bool whole = fullReadback;
        const auto addFrame = [&](uint64_t startBytes, uint32_t fbw, uint32_t lines) {
            const uint64_t rows = (std::min<uint32_t>(lines, 2048u) + 31u) / 32u; // 32-line page rows (16-bit: 64, covered)
            const uint64_t end = startBytes + rows * std::max<uint32_t>(fbw, 1u) * 8192ull;
            if (startBytes >= kVramSize || end > kVramSize)
                whole = true; // wraps around VRAM
            else
                ranges[rangeCount++] = {startBytes, end};
        };
        const auto addCircuit = [&](uint64_t dispfb, uint64_t display) {
            const uint32_t fbp = uint32_t(dispfb & 0x1FFu), fbw = uint32_t((dispfb >> 9) & 0x3Fu);
            const uint32_t dby = uint32_t((dispfb >> 43) & 0x7FFu), dh = uint32_t((display >> 44) & 0x7FFu);
            addFrame(uint64_t(fbp) * 8192u, fbw ? fbw : 10u, dby + dh + 1u);
            return fbp;
        };
        const bool zero1 = addCircuit(request.dispfb1, request.display1) == 0u;
        const bool zero2 = addCircuit(request.dispfb2, request.display2) == 0u;
        if (request.hasPreferredSource) // 256-byte block units, like the CPU backend reads it
            addFrame(uint64_t(request.preferredSource.fbp) * 256u, request.preferredSource.fbw ? request.preferredSource.fbw : 10u, 1024u);
        if (zero1 || zero2)
            for (const GSFrameReg &context : request.contextFrames)
                addFrame(uint64_t(context.fbp) * 8192u, context.fbw ? context.fbw : 10u, 1024u);
        std::lock_guard<std::mutex> lock(impl.producerMutex);
        if (whole)
            impl.runSyncLocked("present-readback", [&impl]() { impl.readbackShadow(); });
        else
            impl.runSyncLocked("present-readback", [&impl, ranges, rangeCount]() {
                for (size_t i = 0; i < rangeCount; ++i)
                    impl.readbackShadowRange(static_cast<uint32_t>(ranges[i].first), static_cast<uint32_t>(ranges[i].second - ranges[i].first));
            });
    }
    std::lock_guard<std::mutex> shadowLock(impl.shadowMutex);
    PresentationFrame frame = impl.cpu.Present(request);
    static uint32_t presentLogs = 0;
    if ((presentLogs++ % 120u) == 0u && presentLogs < 3000u)
    {
        size_t lit = 0;
        for (size_t i = 0; i + 3 < frame.pixels.size(); i += 4)
            lit += (frame.pixels[i] | frame.pixels[i + 1] | frame.pixels[i + 2]) != 0;
        std::cerr << "[gs:gpu] present " << frame.width << "x" << frame.height << " bytes=" << frame.pixels.size()
                  << " lit=" << lit << " pmode=0x" << std::hex << request.pmode << " dispfb1=0x" << request.dispfb1
                  << std::dec << std::endl;
    }
    return frame;
#endif
}

bool GSGpuBackend::ClearFramebuffer(const GSContext &context, uint32_t rgba)
{
    if (context.frame.fbw == 0u)
        return false;
    const uint8_t psm = context.frame.psm;
    if (psm != GS_PSM_CT32 && psm != GS_PSM_CT24 && psm != GS_PSM_CT16 && psm != GS_PSM_CT16S)
        return false;
    // A flat, untested sprite over the scissor rectangle; WritePixel applies FBA,
    // FBMSK and the 16-bit encoding exactly like the CPU clear.
    GSPrimitiveBatch batch{};
    batch.vertexCount = 2;
    batch.state.context = context;
    batch.state.context.test = 1ull << 17u; // ZTST=ALWAYS, alpha test off
    batch.state.context.zbuf.zmask = true;
    batch.state.context.xyoffset = {};
    batch.state.prim.type = GS_PRIM_SPRITE;
    const uint32_t x0 = context.scissor.x0, y0 = context.scissor.y0;
    const uint32_t x1 = std::max<uint32_t>(x0, context.scissor.x1), y1 = std::max<uint32_t>(y0, context.scissor.y1);
    for (GSVertex &v : batch.vertices)
    {
        v.r = static_cast<uint8_t>(rgba);
        v.g = static_cast<uint8_t>(rgba >> 8u);
        v.b = static_cast<uint8_t>(rgba >> 16u);
        v.a = static_cast<uint8_t>(rgba >> 24u);
    }
    batch.vertices[0].x = static_cast<float>(x0);
    batch.vertices[0].y = static_cast<float>(y0);
    batch.vertices[1].x = static_cast<float>(x1 + 1u);
    batch.vertices[1].y = static_cast<float>(y1 + 1u);
    Submit(batch);
    return true;
}

uint32_t GSGpuBackend::ConsumeLocalToHostBytes(uint8_t *dst, uint32_t maxBytes)
{
    Impl &impl = *m_impl;
    std::lock_guard<std::mutex> lock(impl.producerMutex);
    if (!impl.localToHostActive)
        return 0u;
    std::lock_guard<std::mutex> shadowLock(impl.shadowMutex);
    return impl.cpu.ConsumeLocalToHostBytes(dst, maxBytes);
}

uint32_t GSGpuBackend::ReadVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y) const
{
    Impl &impl = *m_impl;
    {
        std::lock_guard<std::mutex> lock(impl.producerMutex);
        impl.runSyncLocked("read-vram", [&impl]() { impl.readbackShadow(); });
    }
    std::lock_guard<std::mutex> shadowLock(impl.shadowMutex);
    return impl.cpu.ReadVram(psm, base, bw, x, y);
}

void GSGpuBackend::WriteVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value)
{
    Impl &impl = *m_impl;
    std::lock_guard<std::mutex> lock(impl.producerMutex);
    const uint32_t journalWrite[6] = {psm, base, bw, x, y, value};
    impl.runSyncLocked("write-vram", [&]() {
        impl.readbackShadow();
        {
            std::lock_guard<std::mutex> shadowLock(impl.shadowMutex);
            impl.cpu.WriteVram(psm, base, bw, x, y, value);
        }
        impl.uploadShadow();
    }, Impl::journalParams(journalWrite, 6));
}

void GSGpuBackend::SnapshotVram(std::vector<uint8_t> &out) const
{
    Impl &impl = *m_impl;
    {
        std::lock_guard<std::mutex> lock(impl.producerMutex);
        impl.runSyncLocked("snapshot", [&impl]() { impl.readbackShadow(); });
    }
    std::lock_guard<std::mutex> shadowLock(impl.shadowMutex);
    if (!impl.shadow)
    {
        out.clear();
        return;
    }
    out.assign(impl.shadow, impl.shadow + impl.shadowSize);
}

GSTransferSnapshot GSGpuBackend::GetTransferSnapshot() const
{
    Impl &impl = *m_impl;
    std::lock_guard<std::mutex> lock(impl.producerMutex);
    if (impl.localToHostActive)
    {
        std::lock_guard<std::mutex> shadowLock(impl.shadowMutex);
        return impl.cpu.GetTransferSnapshot();
    }
    return impl.transferState;
}
