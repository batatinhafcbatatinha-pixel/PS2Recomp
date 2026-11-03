// The GS thread's GL context, moved to a thread of its own: at 1x and 2x alike, the driver's CPU time was about a third of the
// GS thread, the slowest stage of the pipeline. Entries of the Api table are replaced: calls without pointers and without a
// result go into a queue, in order; every other call waits for the queue and runs on the GL thread. PS2X_GL_ASYNC=0 turns it off.
#if defined(_WIN32)
namespace GlAsync
{
    using Exec = void (*)(const void *);
    constexpr uint64_t kRingBytes = 8u * 1024u * 1024u;

    Api real;
    uint8_t *ring = nullptr;
    alignas(64) std::atomic<uint64_t> writePos{0};
    alignas(64) std::atomic<uint64_t> readPos{0};
    alignas(64) std::atomic<bool> sleeping{false};

    inline void publish(uint64_t position)
    {
        writePos.store(position, std::memory_order_seq_cst);
        if (sleeping.load(std::memory_order_seq_cst))
        {
            sleeping.store(false, std::memory_order_seq_cst);
            sleeping.notify_one();
        }
    }

    // Room for one record: its execute function, its length, its arguments. Producer side only.
    inline void *reserve(Exec exec, size_t argBytes)
    {
        const uint64_t need = 16u + ((argBytes + 7u) & ~static_cast<size_t>(7u));
        uint64_t position = writePos.load(std::memory_order_relaxed);
        const uint64_t index = position % kRingBytes;
        if (index + need > kRingBytes)
        {
            while (position + (kRingBytes - index) - readPos.load(std::memory_order_acquire) > kRingBytes)
                _mm_pause();
            std::memset(ring + index, 0, sizeof(Exec)); // the rest of the ring is skipped
            position += kRingBytes - index;
            publish(position);
        }
        while (position + need - readPos.load(std::memory_order_acquire) > kRingBytes)
            _mm_pause();
        uint8_t *record = ring + position % kRingBytes;
        std::memcpy(record, &exec, sizeof(exec));
        std::memcpy(record + 8u, &need, sizeof(need));
        return record + 16u;
    }

    inline void commit(const void *arguments, size_t argBytes)
    {
        const uint8_t *record = static_cast<const uint8_t *>(arguments) - 16u;
        (void)argBytes;
        uint64_t need;
        std::memcpy(&need, record + 8u, sizeof(need));
        publish(writePos.load(std::memory_order_relaxed) + need);
    }

    template <typename F>
    void callAndWait(F &&function)
    {
        struct Job { std::remove_reference_t<F> *f; std::atomic<bool> done; } job{&function, {false}};
        Job *pointer = &job;
        void *arguments = reserve([](const void *data) {
            Job *j = *static_cast<Job *const *>(data);
            (*j->f)();
            j->done.store(true, std::memory_order_release);
        }, sizeof(pointer));
        std::memcpy(arguments, &pointer, sizeof(pointer));
        commit(arguments, sizeof(pointer));
        for (uint32_t spin = 0; !job.done.load(std::memory_order_acquire); ++spin)
            if (spin < 20000u)
                _mm_pause();
            else
                std::this_thread::yield();
    }

    bool active = false;

    // Work for the GL thread that the GS thread does not wait for. It runs in queue order, with the real table.
    template <typename F>
    void post(F function)
    {
        static_assert(std::is_trivially_copyable_v<F>);
        void *arguments = reserve([](const void *data) { (*static_cast<const F *>(data))(); }, sizeof(F));
        new (arguments) F(function);
        commit(arguments, sizeof(F));
    }

    template <auto Member, typename Signature> struct Sync;
    template <auto Member, typename R, typename... A>
    struct Sync<Member, R (*)(A...)>
    {
        static R call(A... args)
        {
            if constexpr (std::is_void_v<R>)
                callAndWait([&] { (real.*Member)(args...); });
            else
            {
                R result{};
                callAndWait([&] { result = (real.*Member)(args...); });
                return result;
            }
        }
    };

    template <auto Member, typename Signature> struct Queued;
    template <auto Member, typename... A>
    struct Queued<Member, void (*)(A...)>
    {
        using Args = std::tuple<A...>;
        static void call(A... args)
        {
            void *arguments = reserve([](const void *data) { std::apply(real.*Member, *static_cast<const Args *>(data)); }, sizeof(Args));
            new (arguments) Args(args...);
            commit(arguments, sizeof(Args));
        }
    };

    void workerLoop(void *dc, void *context)
    {
        ThreadNaming::SetCurrentThreadName("GSGLThread");
        ps2ProfilerRegisterCurrentThread("gl");
        ThreadPriority::RaiseCurrentThread(ThreadPriority::Level::AboveNormal);
        GSGL::AttachContext(dc, context);
        for (;;)
        {
            uint64_t position = readPos.load(std::memory_order_relaxed);
            uint32_t spin = 0u;
            while (writePos.load(std::memory_order_acquire) == position)
            {
                if (++spin < 4000u)
                {
                    _mm_pause();
                    continue;
                }
                sleeping.store(true, std::memory_order_seq_cst);
                if (writePos.load(std::memory_order_seq_cst) == position)
                    sleeping.wait(true, std::memory_order_seq_cst);
                sleeping.store(false, std::memory_order_seq_cst);
                spin = 0u;
            }
            const uint64_t index = position % kRingBytes;
            uint8_t *record = ring + index;
            Exec exec;
            std::memcpy(&exec, record, sizeof(exec));
            if (!exec)
            {
                readPos.store(position + (kRingBytes - index), std::memory_order_release);
                continue;
            }
            uint64_t need;
            std::memcpy(&need, record + 8u, sizeof(need));
            exec(record + 16u);
            readPos.store(position + need, std::memory_order_release);
        }
    }

    bool install(Api &api)
    {
        static const bool enabled = [] { const char *v = std::getenv("PS2X_GL_ASYNC"); return !v || *v != '0'; }();
        void *dc = nullptr, *context = nullptr;
        if (!enabled || !GSGL::DetachContext(dc, context))
            return false;
        real = api;
        ring = new uint8_t[kRingBytes];
        std::thread(workerLoop, dc, context).detach();
#define GL_SYNC(name) if (api.name) api.name = Sync<&Api::name, decltype(Api::name)>::call;
#define GL_QUEUED(name) if (api.name) api.name = Queued<&Api::name, decltype(Api::name)>::call;
        GL_SYNC(GenBuffers) GL_SYNC(DeleteBuffers) GL_SYNC(BufferData) GL_SYNC(BufferSubData) GL_SYNC(GetBufferSubData)
        GL_SYNC(ClearBufferData) GL_SYNC(BufferStorage) GL_SYNC(MapBufferRange) GL_SYNC(UnmapBuffer) GL_SYNC(GenQueries)
        GL_SYNC(DeleteQueries) GL_SYNC(GetQueryObjectui64v) GL_SYNC(CreateShader) GL_SYNC(ShaderSource) GL_SYNC(CompileShader)
        GL_SYNC(GetShaderiv) GL_SYNC(GetShaderInfoLog) GL_SYNC(DeleteShader) GL_SYNC(CreateProgram) GL_SYNC(AttachShader)
        GL_SYNC(LinkProgram) GL_SYNC(GetProgramiv) GL_SYNC(GetProgramInfoLog) GL_SYNC(ProgramParameteri) GL_SYNC(GetProgramBinary)
        GL_SYNC(ProgramBinary) GL_SYNC(DeleteProgram) GL_SYNC(GetUniformLocation) GL_SYNC(Uniform1uiv) GL_SYNC(FenceSync)
        GL_SYNC(ClientWaitSync) GL_SYNC(Finish) GL_SYNC(GetError) GL_SYNC(GetString) GL_SYNC(GenTextures) GL_SYNC(DeleteTextures)
        GL_SYNC(TexImage2D) GL_SYNC(TexSubImage2D) GL_SYNC(GenFramebuffers) GL_SYNC(DeleteFramebuffers) GL_SYNC(GenVertexArrays)
        GL_SYNC(DeleteVertexArrays) GL_SYNC(CheckFramebufferStatus) GL_SYNC(VertexAttribPointer) GL_SYNC(VertexAttribIPointer)
        GL_SYNC(DrawBuffers) GL_SYNC(ReadPixels) GL_SYNC(MaxShaderCompilerThreads) GL_SYNC(BeginQuery) GL_SYNC(EndQuery)
        GL_QUEUED(BindBuffer) GL_QUEUED(BindBufferBase) GL_QUEUED(BindBufferRange) GL_QUEUED(CopyBufferSubData) GL_QUEUED(QueryCounter)
        GL_QUEUED(UseProgram) GL_QUEUED(Uniform1ui) GL_QUEUED(Uniform4ui) GL_QUEUED(DispatchCompute) GL_QUEUED(MemBarrier)
        GL_QUEUED(DeleteSync) GL_QUEUED(Flush) GL_QUEUED(BindTexture) GL_QUEUED(TexParameteri) GL_QUEUED(BindFramebuffer)
        GL_QUEUED(FramebufferParameteri) GL_QUEUED(Viewport) GL_QUEUED(DrawArrays) GL_QUEUED(BindVertexArray) GL_QUEUED(Enable)
        GL_QUEUED(Disable) GL_QUEUED(ActiveTexture) GL_QUEUED(FramebufferTexture2D) GL_QUEUED(BlendFuncSeparate)
        GL_QUEUED(BlendEquationSeparate) GL_QUEUED(BlendColor) GL_QUEUED(ColorMask) GL_QUEUED(DepthMask) GL_QUEUED(DepthFunc)
        GL_QUEUED(ClearColor) GL_QUEUED(ClearDepth) GL_QUEUED(Clear) GL_QUEUED(Scissor) GL_QUEUED(Uniform1i) GL_QUEUED(Uniform1f)
        GL_QUEUED(Uniform2f) GL_QUEUED(Uniform4f) GL_QUEUED(Uniform4i) GL_QUEUED(EnableVertexAttribArray) GL_QUEUED(BlitFramebuffer)
        GL_QUEUED(TextureBarrier) GL_QUEUED(CopyImageSubData) GL_QUEUED(PixelStorei)
#undef GL_SYNC
#undef GL_QUEUED
        active = true;
        return true;
    }
}
#else
namespace GlAsync
{
    constexpr bool active = false;
    inline Api real;
    inline bool install(Api &) { return false; }
    template <typename F> void post(F function) { function(); }
    template <typename F> void callAndWait(F &&function) { function(); }
}
#endif
